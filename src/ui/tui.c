#define _DEFAULT_SOURCE
#include "prism/sim.h"
#include "prism/workload.h"
#include "prism/sim_api.h"
#include "prism/metrics.h"
#include <ncurses.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define PAIR_HEADER    1
#define PAIR_RUNNING   2
#define PAIR_READY     3
#define PAIR_BLOCKED   4
#define PAIR_TERM      5
#define PAIR_FRAME     6
#define PAIR_ALERT     7

#define MIN_ROWS            22
#define MIN_COLS            90
#define GANTT_COLUMNS       30
#define MEMORY_COLUMN_X     60
#define MAX_FRAMES_SHOWN    12
#define FRAMES_PER_ROW      4
#define MAX_RESOURCES_SHOWN 4
#define EVENT_LOG_LINES     5
#define EVENT_LOG_HEIGHT    7

#define HISTORY_SIZE        500
#define COMPARE_MAX_TICKS   50000
#define DEFAULT_DELAY_MS    250

typedef struct {
    sim_metrics_t metrics[SCHED_COUNT];
    bool          finished[SCHED_COUNT];
} compare_t;

typedef struct {
    sim_t     *sim;
    sim_t     *initial;
    sim_t     *scratch;
    sim_t     *history;
    int        history_head;
    int        history_count;

    bool       running;
    bool       paused;
    int        delay_ms;
    sim_pid_t  selected;
    bool       show_compare;
    bool       compare_stale;
    compare_t  compare;
} tui_t;

static void *alloc_or_die(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Fatal error: Failed to allocate simulation history buffer\n");
        exit(1);
    }
    return ptr;
}

static void init_colors(void) {
    if (!has_colors()) return;

    start_color();
    use_default_colors();
    init_pair(PAIR_HEADER,  COLOR_CYAN,    -1);
    init_pair(PAIR_RUNNING, COLOR_GREEN,   -1);
    init_pair(PAIR_READY,   COLOR_YELLOW,  -1);
    init_pair(PAIR_BLOCKED, COLOR_RED,     -1);
    init_pair(PAIR_TERM,    COLOR_WHITE,   -1);
    init_pair(PAIR_FRAME,   COLOR_MAGENTA, -1);
    init_pair(PAIR_ALERT,   COLOR_WHITE,   COLOR_RED);
}

static void compare_run(compare_t *cmp, const sim_t *base, sim_t *scratch) {
    for (int i = 0; i < SCHED_COUNT; i++) {
        bool prepared = sim_prepare_variant(scratch, base, i, SIM_VARIANT_KEEP,
                                            SIM_VARIANT_KEEP, SIM_VARIANT_KEEP) == SIM_OK;
        cmp->finished[i] = prepared && sim_run_bounded(scratch, COMPARE_MAX_TICKS);
        sim_metrics_compute(scratch, &cmp->metrics[i]);
    }
}

static double best_avg_wait(const compare_t *cmp) {
    double best = -1;
    for (int i = 0; i < SCHED_COUNT; i++) {
        bool better = best < 0 || cmp->metrics[i].avg_wait < best;
        if (cmp->finished[i] && better) best = cmp->metrics[i].avg_wait;
    }
    return best;
}

static void draw_compare_row(int row, int policy, const compare_t *cmp, bool current) {
    const sim_metrics_t *m = &cmp->metrics[policy];
    const char *name = sched_get_ops((sched_policy_t)policy)->name;
    char marker = current ? '>' : ' ';

    if (cmp->finished[policy]) {
        mvprintw(row, 1, "%c %-11s %9.2f %9.2f %9.2f %9u %8u %7u %7.1f %6" PRIu64,
                 marker, name, m->avg_wait, m->avg_turnaround, m->avg_response, m->max_wait,
                 m->ctx_switches, m->page_faults, m->cpu_util, m->makespan);
    } else {
        mvprintw(row, 1, "%c %-11s did not finish within %d ticks", marker, name, COMPARE_MAX_TICKS);
    }
}

static void draw_compare(const sim_t *s, const compare_t *cmp, int y, int max_y) {
    double best = best_avg_wait(cmp);

    attron(A_BOLD);
    mvprintw(y, 1, "SCHEDULER COMPARISON on this scenario (%d cores, %d frames, %s)   [C] back to processes",
             s->num_cores, s->configured_frames, repl_get_ops(s->repl_policy)->name);
    attroff(A_BOLD);
    mvprintw(y + 1, 1, "  %-11s %9s %9s %9s %9s %8s %7s %7s %6s",
             "Scheduler", "Avg wait", "Avg turn", "Avg resp", "Max wait", "Switches", "Faults", "Util %", "Ticks");
    mvhline(y + 2, 1, '-', 86);

    for (int i = 0; i < SCHED_COUNT && y + 3 + i < max_y; i++) {
        bool current = (int)s->sched_policy == i;
        bool is_best = cmp->finished[i] && cmp->metrics[i].avg_wait <= best;

        if (current) attron(A_REVERSE);
        if (is_best) attron(COLOR_PAIR(PAIR_RUNNING) | A_BOLD);
        draw_compare_row(y + 3 + i, i, cmp, current);
        if (is_best) attroff(COLOR_PAIR(PAIR_RUNNING) | A_BOLD);
        if (current) attroff(A_REVERSE);
    }
}

static void draw_too_small(int max_y, int max_x) {
    attron(A_BOLD | COLOR_PAIR(PAIR_ALERT));
    mvprintw(max_y / 2 - 1, 2, " PRISM :: TERMINAL WINDOW TOO SMALL (%dx%d) ", max_x, max_y);
    attroff(A_BOLD | COLOR_PAIR(PAIR_ALERT));
    mvprintw(max_y / 2 + 1, 2, "Please expand your terminal window to at least 95x24 for full layout.");
    mvprintw(max_y / 2 + 3, 2, "Press [Q] to quit or resize your window.");
}

static void draw_header(const sim_t *s, bool paused, int max_x) {
    attron(A_BOLD | COLOR_PAIR(PAIR_HEADER));
    mvprintw(0, 1, "PRISM :: PROCESS, RESOURCE & MEMORY SIMULATOR");
    attroff(A_BOLD | COLOR_PAIR(PAIR_HEADER));

    mvprintw(0, 52, "[Tick: %4" PRIu64 "] [Sched: %s] [Repl: %s] [%s]",
             s->tick, sched_get_ops(s->sched_policy)->name, repl_get_ops(s->repl_policy)->name,
             paused ? "PAUSED" : "RUNNING");
    mvprintw(1, 1, "SPACE run  S step  B back  M sched  P repl  C compare  K kill  D -RAM  R reset  Q quit");
    mvhline(2, 0, ACS_HLINE, max_x);
}

static void draw_core_timeline(const sim_t *s, int core) {
    int first = (int)s->gantt_count - GANTT_COLUMNS;
    if (first < 0) first = 0;

    for (int t = first; t < (int)s->gantt_count; t++) {
        sim_pid_t pid = s->gantt_history[t % PRISM_GANTT_HISTORY][core];
        if (pid == -1) {
            printw(".");
        } else {
            printw("%c", 'A' + (pid % 26));
        }
    }
    printw("|");
}

static int draw_cores(const sim_t *s, int max_x) {
    mvprintw(3, 1, "CPU CORES (Count: %d):", s->num_cores);

    for (int c = 0; c < s->num_cores; c++) {
        int row = 4 + c;
        sim_pid_t pid = s->running_pid[c];
        if (pid == -1) {
            mvprintw(row, 3, "Core %d: [ IDLE ]", c);
        } else {
            attron(COLOR_PAIR(PAIR_RUNNING) | A_BOLD);
            mvprintw(row, 3, "Core %d: [ RUN PID %d (%s) ]", c, pid, s->procs[pid].name);
            attroff(COLOR_PAIR(PAIR_RUNNING) | A_BOLD);
        }
        mvprintw(row, 36, "|");
        draw_core_timeline(s, c);
    }

    int divider_row = 5 + s->num_cores;
    mvhline(divider_row, 0, ACS_HLINE, max_x);
    return divider_row + 1;
}

static int process_color(proc_state_t state) {
    switch (state) {
        case PROC_RUNNING:       return PAIR_RUNNING;
        case PROC_BLOCKED_IO:
        case PROC_BLOCKED_FAULT: return PAIR_BLOCKED;
        case PROC_TERMINATED:    return PAIR_TERM;
        default:                 return PAIR_READY;
    }
}

static int draw_process_table(const sim_t *s, sim_pid_t selected, int y, int max_y) {
    mvprintw(y, 1, "%-4s %-12s %-8s %-5s %-5s %-5s %-5s %-6s",
             "PID", "Name", "State", "Wait", "CPU", "I/O", "Fault", "Faults");
    mvhline(y + 1, 1, '-', 57);

    int row = y + 2;
    for (int i = 0; i < s->num_processes && row < max_y - 8; i++, row++) {
        const pcb_t *p = &s->procs[i];
        int color = process_color(p->state);
        bool highlighted = p->pid == selected;

        if (highlighted) attron(A_REVERSE);
        attron(COLOR_PAIR(color));
        mvprintw(row, 1, "%-4d %-12.12s %-8s %-5u %-5u %-5u %-5u %-6u",
                 p->pid, p->name, proc_state_str(p->state),
                 p->total_wait_time, p->total_cpu_time, p->total_io_time,
                 p->total_fault_time, p->num_page_faults);
        attroff(COLOR_PAIR(color));
        if (highlighted) attroff(A_REVERSE);
    }
    return row;
}

static int draw_frames(const sim_t *s, int y) {
    int x = MEMORY_COLUMN_X;
    mvprintw(y, x, "PHYSICAL FRAMES (%d Total | Faults: %u):", s->total_frames, s->total_page_faults);
    mvhline(y + 1, x, '-', 40);

    int first_row = y + 2;
    for (int f = 0; f < s->total_frames && f < MAX_FRAMES_SHOWN; f++) {
        const frame_entry_t *frame = &s->frames[f];
        int col = x + (f % FRAMES_PER_ROW) * 10;
        int row = first_row + f / FRAMES_PER_ROW;

        if (frame->occupied && frame->owner_pid >= 0) {
            attron(COLOR_PAIR(PAIR_FRAME));
            mvprintw(row, col, "[F%02d:P%d]", f, frame->owner_pid);
            attroff(COLOR_PAIR(PAIR_FRAME));
        } else {
            mvprintw(row, col, "[F%02d: - ]", f);
        }
    }
    return first_row + (s->total_frames > 8 ? 3 : 2) + 1;
}

static void draw_resources(const sim_t *s, int y) {
    int x = MEMORY_COLUMN_X;
    mvprintw(y, x, "RESOURCES & DEADLOCK ENGINE:");
    mvhline(y + 1, x, '-', 40);

    if (s->deadlock_detected) {
        attron(COLOR_PAIR(PAIR_BLOCKED) | A_BOLD);
        mvprintw(y + 2, x, "[! DEADLOCK DETECTED (WFG Cycle) !]");
        attroff(COLOR_PAIR(PAIR_BLOCKED) | A_BOLD);
    } else {
        attron(COLOR_PAIR(PAIR_RUNNING));
        mvprintw(y + 2, x, "[Status: Healthy (Acyclic WFG)]");
        attroff(COLOR_PAIR(PAIR_RUNNING));
    }

    int row = y + 3;
    for (int r = 0; r < s->num_resources && r < MAX_RESOURCES_SHOWN; r++, row++) {
        const sim_resource_t *res = &s->resources[r];
        mvprintw(row, x, "R%d (%s): %d/%d avail | Held by P%d",
                 r, res->name, res->available_instances, res->total_instances, res->owner_pid);
    }
    mvprintw(row, x, "IPC Queue: %d pending messages", s->ipc_msg_count);
}

static void draw_event_log(const sim_t *s, int content_bottom, int max_y, int max_x) {
    int y = max_y - EVENT_LOG_HEIGHT;
    if (y <= content_bottom) return;

    mvhline(y, 0, ACS_HLINE, max_x);
    mvprintw(y, 2, " RECENT EVENT LOG (Hash: 0x%016" PRIx64 ") ", s->log_hash);

    int first = (int)s->event_count - EVENT_LOG_LINES;
    if (first < 0) first = 0;

    int row = y + 1;
    for (int e = first; e < (int)s->event_count && row < max_y; e++, row++) {
        const sim_event_t *ev = &s->event_log[e];
        mvprintw(row, 2, "[T:%04" PRIu64 "] %-12s PID:%-2d Core:%-2d Val1:%-3d Val2:%-3d",
                 ev->tick, event_type_str(ev->type), ev->pid, ev->core_id, ev->detail1, ev->detail2);
    }
}

static void draw_dashboard(const tui_t *t) {
    const sim_t *s = t->sim;
    int max_y, max_x;

    erase();
    getmaxyx(stdscr, max_y, max_x);

    if (max_y < MIN_ROWS || max_x < MIN_COLS) {
        draw_too_small(max_y, max_x);
        refresh();
        return;
    }

    draw_header(s, t->paused, max_x);
    int y = draw_cores(s, max_x);

    int content_bottom;
    if (t->show_compare) {
        draw_compare(s, &t->compare, y, max_y);
        content_bottom = y + 3 + SCHED_COUNT;
    } else {
        content_bottom = draw_process_table(s, t->selected, y, max_y);
        int resources_y = draw_frames(s, y);
        draw_resources(s, resources_y);
    }
    draw_event_log(s, content_bottom, max_y, max_x);

    refresh();
}

static void history_push(tui_t *t) {
    sim_snapshot(t->sim, &t->history[t->history_head]);
    t->history_head = (t->history_head + 1) % HISTORY_SIZE;
    if (t->history_count < HISTORY_SIZE) t->history_count++;
}

static void history_back(tui_t *t) {
    if (t->history_count == 0) return;
    t->history_head = (t->history_head + HISTORY_SIZE - 1) % HISTORY_SIZE;
    t->history_count--;
    sim_restore(t->sim, &t->history[t->history_head]);
}

static void cycle_scheduler(sim_t *sim) {
    sched_policy_t next = (sched_policy_t)((sim->sched_policy + 1) % SCHED_COUNT);
    sim_set_sched(sim, sched_get_ops(next)->key, 0);
}

static void cycle_replacement(sim_t *sim) {
    repl_policy_t next = (repl_policy_t)((sim->repl_policy + 1) % REPL_COUNT);
    sim_set_repl(sim, repl_get_ops(next)->key);
}

static void reset_to_start(tui_t *t) {
    sim_restore(t->sim, t->initial);
    t->history_head = 0;
    t->history_count = 0;
    t->paused = true;
    t->compare_stale = true;
}

static bool handle_key(tui_t *t, int key) {
    switch (key) {
        case 'q': case 'Q': t->running = false; break;
        case ' ':           t->paused = !t->paused; break;
        case 's': case 'S': return true;
        case 'b': case 'B': history_back(t); break;
        case 'm': case 'M': cycle_scheduler(t->sim); break;
        case 'p': case 'P':
            cycle_replacement(t->sim);
            t->compare_stale = true;
            break;
        case 'c': case 'C': t->show_compare = !t->show_compare; break;
        case 'k': case 'K': sim_inject_kill(t->sim, t->selected); break;
        case 'd': case 'D':
            if (t->sim->total_frames > 2) sim_inject_shrink_ram(t->sim, t->sim->total_frames - 2);
            break;
        case KEY_UP:
            if (t->selected > 0) t->selected--;
            break;
        case KEY_DOWN:
            if (t->selected + 1 < t->sim->num_processes) t->selected++;
            break;
        case '+':
            if (t->delay_ms > 50) t->delay_ms -= 50;
            break;
        case '-':
            if (t->delay_ms < 1000) t->delay_ms += 50;
            break;
        case 'r': case 'R': reset_to_start(t); break;
    }
    return false;
}

static void init_curses(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();
}

static void run_loop(tui_t *t) {
    while (t->running) {
        if (t->show_compare && t->compare_stale) {
            compare_run(&t->compare, t->sim, t->scratch);
            t->compare_stale = false;
        }
        draw_dashboard(t);

        timeout(t->delay_ms);
        int key = getch();
        bool step = key == ERR ? !t->paused : handle_key(t, key);

        if (step && !sim_is_done(t->sim)) {
            history_push(t);
            sim_step(t->sim);
        }
    }
}

int main(int argc, char **argv) {
    const char *workload_path = argc > 1 ? argv[1] : "workloads/mixed_demo.txt";

    tui_t t = {
        .sim = alloc_or_die(sizeof(sim_t)),
        .initial = alloc_or_die(sizeof(sim_t)),
        .scratch = alloc_or_die(sizeof(sim_t)),
        .history = alloc_or_die(HISTORY_SIZE * sizeof(sim_t)),
        .running = true,
        .paused = true,
        .delay_ms = DEFAULT_DELAY_MS,
        .compare_stale = true,
    };

    sim_init(t.sim, 2, 6, 3, SCHED_MLFQ, REPL_LRU, 42);
    if (!workload_load_file(workload_path, t.sim, NULL)) {
        workload_generate_mixed(t.sim, 5, 42);
    }
    sim_snapshot(t.sim, t.initial);

    init_curses();
    run_loop(&t);
    endwin();

    free(t.sim);
    free(t.initial);
    free(t.scratch);
    free(t.history);
    return 0;
}
