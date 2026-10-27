#define _DEFAULT_SOURCE
#include "prism/sim.h"
#include "prism/workload.h"
#include <ncurses.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define COLOR_PAIR_HEADER    1
#define COLOR_PAIR_RUNNING   2
#define COLOR_PAIR_READY     3
#define COLOR_PAIR_BLOCKED   4
#define COLOR_PAIR_TERM      5
#define COLOR_PAIR_FRAME     6
#define COLOR_PAIR_ALERT     7

static void init_tui_colors(void) {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(COLOR_PAIR_HEADER,  COLOR_CYAN,    -1);
    init_pair(COLOR_PAIR_RUNNING, COLOR_GREEN,   -1);
    init_pair(COLOR_PAIR_READY,   COLOR_YELLOW,  -1);
    init_pair(COLOR_PAIR_BLOCKED, COLOR_RED,     -1);
    init_pair(COLOR_PAIR_TERM,    COLOR_WHITE,   -1);
    init_pair(COLOR_PAIR_FRAME,   COLOR_MAGENTA, -1);
    init_pair(COLOR_PAIR_ALERT,   COLOR_WHITE,   COLOR_RED);
}

static void draw_dashboard(const sim_t *s, bool paused, int delay_ms, sim_pid_t selected_proc) {
    (void)delay_ms;
    erase();
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_y;

    /* Top Status Bar */
    attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_HEADER));
    mvprintw(0, 1, "PRISM :: PROCESS, RESOURCE & MEMORY SIMULATOR");
    attroff(A_BOLD | COLOR_PAIR(COLOR_PAIR_HEADER));

    mvprintw(0, 52, "[Tick: %4" PRIu64 "] [Sched: %s] [Repl: %s] [%s]",
             s->tick,
             sched_get_ops(s->sched_policy)->name,
             repl_get_ops(s->repl_policy)->name,
             paused ? "PAUSED" : "RUNNING");

    /* Controls Bar */
    mvprintw(1, 1, "[SPACE] Play/Pause | [S] Step | [B] Rewind | [M] Sched | [P] Repl | [K] Kill | [D] Shrink RAM | [Q] Quit");
    mvhline(2, 0, ACS_HLINE, max_x);

    /* Cores & Gantt Timeline (Lines 3 to 6) */
    mvprintw(3, 1, "CPU CORES (Count: %d):", s->num_cores);
    for (int c = 0; c < s->num_cores; c++) {
        sim_pid_t pid = s->running_pid[c];
        if (pid == -1) {
            mvprintw(4 + c, 3, "Core %d: [ IDLE ]", c);
        } else {
            attron(COLOR_PAIR(COLOR_PAIR_RUNNING) | A_BOLD);
            mvprintw(4 + c, 3, "Core %d: [ RUN PID %d (%s) ]", c, pid, s->procs[pid].name);
            attroff(COLOR_PAIR(COLOR_PAIR_RUNNING) | A_BOLD);
        }

        /* Draw recent Gantt segment */
        mvprintw(4 + c, 36, "|");
        int history_start = (int)s->gantt_count - 30;
        if (history_start < 0) history_start = 0;
        for (int t = history_start; t < (int)s->gantt_count; t++) {
            sim_pid_t hp = s->gantt_history[t % PRISM_GANTT_HISTORY][c];
            if (hp == -1) {
                printw(".");
            } else {
                printw("%c", 'A' + (hp % 26));
            }
        }
        printw("|");
    }

    int next_y = 5 + s->num_cores;
    mvhline(next_y, 0, ACS_HLINE, max_x);
    next_y++;

    /* Process Table (Left side) */
    mvprintw(next_y, 1, "%-4s %-10s %-8s %-5s %-5s %-5s %-5s %-6s",
             "PID", "Name", "State", "Wait", "CPU", "I/O", "Fault", "Faults");
    mvhline(next_y + 1, 1, '-', 55);

    int py = next_y + 2;
    for (int i = 0; i < s->num_processes && py < max_y - 8; i++, py++) {
        const pcb_t *p = &s->procs[i];
        int color = COLOR_PAIR_READY;
        if (p->state == PROC_RUNNING) color = COLOR_PAIR_RUNNING;
        else if (p->state == PROC_BLOCKED_IO || p->state == PROC_BLOCKED_FAULT) color = COLOR_PAIR_BLOCKED;
        else if (p->state == PROC_TERMINATED) color = COLOR_PAIR_TERM;

        if (p->pid == selected_proc) attron(A_REVERSE);
        attron(COLOR_PAIR(color));
        mvprintw(py, 1, "%-4d %-10s %-8s %-5u %-5u %-5u %-5u %-6u",
                 p->pid, p->name, proc_state_str(p->state),
                 p->total_wait_time, p->total_cpu_time, p->total_io_time,
                 p->total_fault_time, p->num_page_faults);
        attroff(COLOR_PAIR(color));
        if (p->pid == selected_proc) attroff(A_REVERSE);
    }

    /* Physical Memory Frames Grid (Right side) */
    int mem_x = 60;
    mvprintw(next_y, mem_x, "PHYSICAL FRAMES (%d Total | Faults: %u):", s->total_frames, s->total_page_faults);
    mvhline(next_y + 1, mem_x, '-', 35);

    int fy = next_y + 2;
    for (int f = 0; f < s->total_frames && f < 16; f++) {
        const frame_entry_t *fr = &s->frames[f];
        int col = mem_x + (f % 4) * 11;
        int row = fy + (f / 4) * 2;

        if (fr->occupied && fr->owner_pid >= 0) {
            attron(COLOR_PAIR(COLOR_PAIR_FRAME));
            mvprintw(row, col, "[F%02d:P%d:v%d]", f, fr->owner_pid, fr->vpage);
            attroff(COLOR_PAIR(COLOR_PAIR_FRAME));
        } else {
            mvprintw(row, col, "[F%02d: --- ]", f);
        }
    }

    /* Event Log (Bottom panel) */
    int log_y = max_y - 7;
    if (log_y > py) {
        mvhline(log_y, 0, ACS_HLINE, max_x);
        mvprintw(log_y, 2, " RECENT EVENT LOG (Hash: 0x%016" PRIx64 ") ", s->log_hash);

        int log_start = (int)s->event_count - 5;
        if (log_start < 0) log_start = 0;
        int l_row = log_y + 1;
        for (int e = log_start; e < (int)s->event_count && l_row < max_y; e++, l_row++) {
            const sim_event_t *ev = &s->event_log[e];
            mvprintw(l_row, 2, "[T:%04" PRIu64 "] %-12s PID:%-2d Core:%-2d Val1:%-3d Val2:%-3d",
                     ev->tick, event_type_str(ev->type), ev->pid, ev->core_id, ev->detail1, ev->detail2);
        }
    }

    refresh();
}

int main(int argc, char **argv) {
    const char *workload_path = "workloads/mixed_demo.txt";
    if (argc > 1) {
        workload_path = argv[1];
    }

    sim_t sim;
    sim_init(&sim, 2, 6, 3, SCHED_MLFQ, REPL_LRU, 42);
    if (!workload_load_file(workload_path, &sim, NULL)) {
        workload_generate_mixed(&sim, 5, 42);
    }

    /* Keep snapshots on heap for rewind to avoid stack overflow */
    #define PRISM_TUI_MAX_HISTORY 500
    sim_t *history = malloc(PRISM_TUI_MAX_HISTORY * sizeof(sim_t));
    if (!history) {
        fprintf(stderr, "Fatal error: Failed to allocate simulation history buffer\n");
        return 1;
    }
    int hist_count = 0;
    sim_snapshot(&sim, &history[hist_count++]);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    init_tui_colors();

    bool running = true;
    bool paused = true;
    int delay_ms = 250;
    sim_pid_t selected_proc = 0;

    while (running) {
        draw_dashboard(&sim, paused, delay_ms, selected_proc);

        int ch = getch();
        if (ch != ERR) {
            switch (ch) {
                case 'q':
                case 'Q':
                    running = false;
                    break;
                case ' ':
                    paused = !paused;
                    break;
                case 's':
                case 'S':
                    if (hist_count < 500) {
                        sim_snapshot(&sim, &history[hist_count++]);
                    }
                    sim_step(&sim);
                    break;
                case 'b':
                case 'B':
                    if (hist_count > 1) {
                        hist_count--;
                        sim_restore(&sim, &history[hist_count - 1]);
                    }
                    break;
                case 'm':
                case 'M':
                    sim_set_policy(&sim, (sim.sched_policy + 1) % SCHED_COUNT, sim.repl_policy);
                    break;
                case 'p':
                case 'P':
                    sim_set_policy(&sim, sim.sched_policy, (sim.repl_policy + 1) % REPL_COUNT);
                    break;
                case 'k':
                case 'K':
                    sim_inject_kill(&sim, selected_proc);
                    break;
                case 'd':
                case 'D':
                    if (sim.total_frames > 2) {
                        sim_inject_shrink_ram(&sim, sim.total_frames - 2);
                    }
                    break;
                case KEY_UP:
                    if (selected_proc > 0) selected_proc--;
                    break;
                case KEY_DOWN:
                    if (selected_proc + 1 < sim.num_processes) selected_proc++;
                    break;
                case '+':
                    if (delay_ms > 50) delay_ms -= 50;
                    break;
                case '-':
                    if (delay_ms < 1000) delay_ms += 50;
                    break;
                case 'r':
                case 'R':
                    if (hist_count > 0) {
                        sim_restore(&sim, &history[0]);
                        hist_count = 1;
                    }
                    break;
            }
        }

        if (!paused && !sim_is_done(&sim)) {
            if (hist_count < 500) {
                sim_snapshot(&sim, &history[hist_count++]);
            }
            sim_step(&sim);
        }

        usleep((useconds_t)(delay_ms * 1000));
    }

    endwin();
    free(history);
    return 0;
}
