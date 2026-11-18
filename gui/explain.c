#include "gui_state.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const algo_info_t g_sched_info[SCHED_COUNT] = {
    [SCHED_FCFS] = {"FCFS (First Come, First Served)", "Run processes in arrival order, each until it blocks or ends.",
        "The ready queue is a plain FIFO. The process at the head gets the CPU and keeps it until its CPU burst ends.",
        "Simple, no starvation, few context switches.",
        "Convoy effect: one long job makes every short job behind it wait. Poor response time.", "no"},
    [SCHED_RR] = {"Round Robin", "FIFO queue, but each turn is limited to a time quantum.",
        "The head of the ready queue runs for at most q ticks. If it is still running when the quantum expires it is "
        "preempted and goes to the back of the queue.",
        "Fair and responsive: every ready process runs within (n-1)*q ticks.",
        "More context switches; average turnaround can be worse than FCFS when q is small. Large q behaves like FCFS.",
        "yes (quantum expiry)"},
    [SCHED_SJF] = {"SJF (Shortest Job First)", "Pick the ready process with the shortest next CPU burst.",
        "Whenever the CPU becomes free, choose the ready process whose next CPU burst is shortest (ties: earlier arrival). "
        "Once started it runs the whole burst.",
        "Minimises average waiting time among non-preemptive policies.",
        "Needs burst lengths in advance; long jobs can starve while short jobs keep arriving.", "no"},
    [SCHED_SRTF] = {"SRTF (Shortest Remaining Time First)", "Preemptive SJF: always run the least remaining CPU time.",
        "Like SJF, but when a process becomes ready with less remaining CPU time than the running one, the running "
        "process is preempted.",
        "Optimal average waiting time; very good response for short jobs.",
        "Starvation of long jobs; more context switches; needs burst lengths in advance.", "yes (shorter job arrives)"},
    [SCHED_MLFQ] = {"MLFQ (Multi-Level Feedback Queue)", "Three queues Q0..Q2 with quanta 2/4/8; learn from behaviour.",
        "New processes start in Q0. Using a whole quantum means CPU-bound, so the process is demoted one level. "
        "Blocking for I/O keeps the level. Higher queues always run first. Every 50 ticks all processes are boosted "
        "back to Q0 so nobody starves.",
        "Favours interactive / I/O-bound processes without knowing burst lengths.",
        "Many parameters to tune; CPU-bound jobs get long waits between boosts.", "yes (quantum expiry, demotion)"},
    [SCHED_ADAPTIVE] = {"Adaptive (aging + thrash aware)", "Two levels; demotes processes that page-fault heavily.",
        "Processes that spend more than twice their CPU time waiting for page faults go to level 1 (quantum 1); "
        "others use level 0 (quantum 3). A level-1 process waiting more than 15 ticks is aged back up.",
        "Lets processes with good locality finish when memory is tight.",
        "Heuristic thresholds; behaves like RR when there are no page faults.", "yes (quantum expiry)"},
    [SCHED_CFS] = {"CFS (Completely Fair Scheduler)", "Run the process that has had the least (weighted) CPU time.",
        "Each process has a virtual runtime that grows while it runs (faster for low priority). The ready process "
        "with the smallest vruntime runs next, for a slice of 2 ticks. Waking processes start at the current minimum.",
        "Fair share over time; good response; no fixed queues.",
        "Many context switches with short slices; no notion of burst length.", "yes (slice end)"},
    [SCHED_BFS] = {"BFS (Earliest Virtual Deadline First)", "Single queue ordered by a virtual deadline.",
        "When a process becomes ready (or uses its 2-tick slice) it gets deadline = now + 10*(priority+1). "
        "The ready process with the earliest deadline runs next.",
        "Simple, low latency, priorities expressed as deadlines.",
        "Many context switches; long jobs keep getting pushed back while new work arrives.", "yes (slice end)"},
    [SCHED_RM] = {"Rate Monotonic", "Fixed priorities: the shorter the period, the higher the priority.",
        "Each periodic task gets a static priority from its period. Whenever a job is released or finishes, the "
        "highest-priority ready job runs, preempting a lower one. Aperiodic work only runs when no job is ready. "
        "With n tasks it is guaranteed to meet every deadline if utilization <= n(2^(1/n) - 1).",
        "Optimal among fixed-priority schedulers; predictable, easy to analyse.",
        "Cannot use the full CPU: sets above the bound may miss deadlines even though EDF would cope.",
        "yes (higher-priority release)"},
    [SCHED_EDF] = {"EDF (Earliest Deadline First)", "Dynamic priorities: the job whose deadline comes first runs.",
        "Every released job has an absolute deadline (release + relative deadline). The ready job with the "
        "earliest one runs and preempts a job with a later deadline. Aperiodic work runs in idle time.",
        "Optimal on one CPU: meets every deadline whenever utilization <= 100%.",
        "Priorities change all the time; under overload misses cascade unpredictably.",
        "yes (earlier deadline released)"},
};

static const algo_info_t g_repl_info[REPL_COUNT] = {
    [REPL_FIFO] = {"FIFO page replacement", "Evict the page that was loaded longest ago.",
        "Frames form a queue in load order. On a fault with no free frame the oldest loaded page is evicted, even "
        "if it is used all the time.",
        "Trivial to implement.",
        "Ignores usage; can suffer Belady's anomaly (more frames -> more faults).", "-"},
    [REPL_LRU] = {"LRU (Least Recently Used)", "Evict the page whose last use is furthest in the past.",
        "Every access stamps the frame with the current tick. The victim is the frame with the oldest stamp: the "
        "recent past predicts the near future (locality).",
        "Close to optimal for programs with locality; never shows Belady's anomaly (stack algorithm).",
        "Needs a timestamp or ordering update on every memory access: expensive in hardware.", "-"},
    [REPL_CLOCK] = {"Clock (Second Chance)", "Approximate LRU with one reference bit per frame.",
        "A hand sweeps the frames in a circle. A frame with reference bit 1 gets a second chance (bit cleared, hand "
        "moves on); the first frame with bit 0 is evicted.",
        "Cheap LRU approximation used by real kernels.",
        "When every page was recently used it degrades to FIFO.", "-"},
    [REPL_OPT] = {"OPT (Belady's optimal)", "Evict the page whose next use is farthest in the future.",
        "Looks ahead in the reference string. Pages never used again go first, otherwise the one needed last.",
        "Fewest possible faults: the yardstick for every other policy.",
        "Needs the future, so it is impossible in a real OS; only for comparison.", "-"},
};

const algo_info_t *explain_sched_info(sched_policy_t policy) {
    bool known = policy >= 0 && policy < SCHED_COUNT;
    return &g_sched_info[known ? policy : 0];
}

const algo_info_t *explain_repl_info(repl_policy_t policy) {
    bool known = policy >= 0 && policy < REPL_COUNT;
    return &g_repl_info[known ? policy : 0];
}

float explain_draw_card(const algo_info_t *a, float x, float y, float w) {
    float top = y;
    ui_text(x, y, 16, UI_TEXT, "%s", a->name);
    y += 22;
    y += ui_text_wrapped(x, y, w, 13, UI_ACCENT, a->summary) + 4;
    y += ui_text_wrapped(x, y, w, 13, UI_TEXT_DIM, a->how) + 4;
    if (a->preemptive[0] != '-') {
        ui_text(x, y, 13, UI_TEXT_DIM, "preemptive: %s", a->preemptive);
        y += 19;
    }
    ui_text(x, y, 13, UI_GOOD, "+");
    y += ui_text_wrapped(x + 14, y, w - 14, 13, UI_TEXT, a->good) + 2;
    ui_text(x, y, 13, UI_BAD, "-");
    y += ui_text_wrapped(x + 14, y, w - 14, 13, UI_TEXT, a->bad);
    return y - top;
}

const char *explain_process_name(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes ? s->procs[pid].name : "?";
}

static int cpu_burst_len(const pcb_t *p, bool remaining) {
    if (p->curr_burst_idx >= p->num_bursts) return 0;

    const burst_t *burst = &p->bursts[p->curr_burst_idx];
    if (burst->kind != BURST_CPU) return 0;
    return remaining ? burst->remaining : burst->duration;
}

static bool scheduling_key(const sim_t *s, const pcb_t *p, char *out, size_t size) {
    switch (s->sched_policy) {
        case SCHED_SJF:      snprintf(out, size, "%d", cpu_burst_len(p, false)); return true;
        case SCHED_SRTF:     snprintf(out, size, "%d", cpu_burst_len(p, true)); return true;
        case SCHED_CFS:      snprintf(out, size, "%.1f", (double)p->vruntime / 100.0); return true;
        case SCHED_BFS:      snprintf(out, size, "t%" PRIu64, (uint64_t)p->virtual_deadline); return true;
        case SCHED_RM:       snprintf(out, size, p->period ? "period %u" : "background", p->period); return true;
        case SCHED_EDF:
            if (p->period) snprintf(out, size, "due t%" PRIu64, (uint64_t)p->job_deadline);
            else snprintf(out, size, "background");
            return true;
        case SCHED_MLFQ:
        case SCHED_ADAPTIVE: snprintf(out, size, "Q%u", p->priority); return true;
        default:             out[0] = '\0'; return false;
    }
}

static void describe_waiting(const sim_t *s, char *out, size_t size) {
    enum { MAX_LISTED = 5 };
    size_t used = 0;
    int listed = 0, waiting = 0;
    out[0] = '\0';

    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        int visited = 0;
        for (int pid = s->ready_head[q]; pid != -1 && visited < PRISM_MAX_PROCESSES; pid = s->procs[pid].next_proc, visited++) {
            waiting++;
            if (listed >= MAX_LISTED || used + 24 >= size) continue;

            char key[24];
            bool has_key = scheduling_key(s, &s->procs[pid], key, sizeof(key));
            const char *separator = listed ? ", " : "";
            used += (size_t)snprintf(out + used, size - used, has_key ? "%s%s=%s" : "%s%s",
                                     separator, s->procs[pid].name, key);
            listed++;
        }
    }
    if (waiting > listed && used + 12 < size) snprintf(out + used, size - used, " +%d more", waiting - listed);
}

static narration_t *next_narration_slot(gui_t *g) {
    if (g->narration_len < GUI_NARR_CAP) {
        return &g->narration[(g->narration_start + g->narration_len++) % GUI_NARR_CAP];
    }
    narration_t *oldest = &g->narration[g->narration_start];
    g->narration_start = (g->narration_start + 1) % GUI_NARR_CAP;
    return oldest;
}

void explain_narrate(gui_t *g, const sim_event_t *ev, const char *fmt, ...) {
    narration_t *e = next_narration_slot(g);
    e->tick = ev->tick;
    e->pid = ev->pid;
    e->type = ev->type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
}

void explain_clear(gui_t *g) {
    g->narration_start = g->narration_len = 0;
    g->narration_scroll = 0;
}

static void dispatch_reason(const sim_t *s, const pcb_t *p, char *why, size_t size) {
    switch (s->sched_policy) {
        case SCHED_FCFS:
            snprintf(why, size, "it is first in the ready queue (FIFO order)");
            break;
        case SCHED_RR:
            snprintf(why, size, "head of the ready queue, for a quantum of %u ticks", s->default_quantum);
            break;
        case SCHED_SJF:
            snprintf(why, size, "shortest next CPU burst (%d ticks)", cpu_burst_len(p, false));
            break;
        case SCHED_SRTF:
            snprintf(why, size, "least remaining CPU time (%d ticks)", cpu_burst_len(p, true));
            break;
        case SCHED_MLFQ:
            snprintf(why, size, "from Q%u, the highest non-empty level (quantum %u)", p->priority, p->quantum_remaining);
            break;
        case SCHED_ADAPTIVE:
            snprintf(why, size, "from level %u (quantum %u)", p->priority, p->quantum_remaining);
            break;
        case SCHED_CFS:
            snprintf(why, size, "smallest virtual runtime (%.1f)", (double)p->vruntime / 100.0);
            break;
        case SCHED_BFS:
            snprintf(why, size, "earliest virtual deadline (t%" PRIu64 ")", (uint64_t)p->virtual_deadline);
            break;
        case SCHED_RM:
            if (p->period) snprintf(why, size, "shortest period among ready jobs (%u)", p->period);
            else snprintf(why, size, "no periodic job is ready, so background work runs");
            break;
        case SCHED_EDF:
            if (p->period) snprintf(why, size, "earliest deadline among ready jobs (t%" PRIu64 ")", (uint64_t)p->job_deadline);
            else snprintf(why, size, "no periodic job is ready, so background work runs");
            break;
        default:
            snprintf(why, size, "chosen by the policy");
            break;
    }
}

static void explain_dispatch(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const pcb_t *p = &s->procs[ev->pid];
    const char *policy = sched_get_ops(s->sched_policy)->name;

    char waiting[160], why[120];
    describe_waiting(s, waiting, sizeof(waiting));
    dispatch_reason(s, p, why, sizeof(why));

    if (waiting[0]) {
        explain_narrate(g, ev, "%s picks %s for core %d: %s. Still waiting: %s.", policy, p->name, ev->core_id, why, waiting);
    } else {
        explain_narrate(g, ev, "%s picks %s for core %d: %s. Nobody else is waiting.", policy, p->name, ev->core_id, why);
    }
}

static void explain_preempt(gui_t *g, const sim_event_t *ev) {
    if (explain_realtime_preempt(g, ev)) return;
    const sim_t *s = g->sim;
    const char *n = explain_process_name(s, ev->pid);
    switch (s->sched_policy) {
        case SCHED_RR:
            explain_narrate(g, ev, "%s used its whole %u-tick quantum: preempted, back to the END of the ready queue.", n, s->default_quantum);
            break;
        case SCHED_MLFQ:
            explain_narrate(g, ev, "%s used its whole quantum, so MLFQ treats it as CPU-bound: now in Q%d (lower priority, longer quantum).",
                 n, ev->detail1);
            break;
        case SCHED_SRTF:
            explain_narrate(g, ev, "%s preempted: a ready process needs only %d ticks, %s still needs %d. Shortest remaining time wins.",
                 n, ev->detail2, n, ev->detail1);
            break;
        case SCHED_CFS:
            explain_narrate(g, ev, "%s's 2-tick slice ended (vruntime now %d); CFS switches to the lowest vruntime.", n, ev->detail1);
            break;
        case SCHED_BFS:
            explain_narrate(g, ev, "%s's 2-tick slice ended; new virtual deadline t%d, so earlier deadlines go first.", n, ev->detail1);
            break;
        default:
            explain_narrate(g, ev, "%s's quantum expired: preempted and back to the ready queue.", n);
            break;
    }
}

static int references_until_next_use(const pcb_t *owner, int vpage) {
    for (int r = owner->curr_page_ref_idx; r < owner->num_page_refs; r++) {
        if (owner->page_refs[r] == vpage) return r - owner->curr_page_ref_idx;
    }
    return -1;
}

static void eviction_reason(const sim_t *s, const sim_event_t *ev, const frame_entry_t *frame, char *why, size_t size) {
    switch (s->repl_policy) {
        case REPL_FIFO:
            snprintf(why, size, "it was loaded longest ago (t%" PRIu64 ")", (uint64_t)frame->load_tick);
            break;
        case REPL_LRU:
            snprintf(why, size, "it is the least recently used (last use t%" PRIu64 ")", (uint64_t)frame->last_access_tick);
            break;
        case REPL_CLOCK:
            snprintf(why, size, "the clock hand found its reference bit at 0 (others got a second chance)");
            break;
        case REPL_OPT: {
            int distance = references_until_next_use(&s->procs[ev->pid], ev->detail1);
            if (distance < 0) snprintf(why, size, "it is never used again");
            else snprintf(why, size, "its next use is farthest away (%d references ahead)", distance);
            break;
        }
        default:
            snprintf(why, size, "chosen by the policy");
            break;
    }
}

static void explain_evict(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    int frame_idx = ev->detail2;
    if (frame_idx < 0 || frame_idx >= s->total_frames) return;

    char why[120];
    eviction_reason(s, ev, &s->frames[frame_idx], why, sizeof(why));
    explain_narrate(g, ev, "RAM is full. %s evicts page %d of %s from frame %d: %s.",
            repl_get_ops(s->repl_policy)->name, ev->detail1, explain_process_name(s, ev->pid), frame_idx, why);
}

void explain_event(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const char *n = explain_process_name(s, ev->pid);
    switch (ev->type) {
        case EVENT_PROC_ARRIVE:
            if (s->sched_policy == SCHED_MLFQ) explain_narrate(g, ev, "%s arrives and enters the top queue Q0.", n);
            else explain_narrate(g, ev, "%s arrives and joins the ready queue.", n);
            break;
        case EVENT_PROC_DISPATCH: explain_dispatch(g, ev); break;
        case EVENT_PROC_PREEMPT:  explain_preempt(g, ev); break;
        case EVENT_PROC_BLOCK_IO:
            explain_narrate(g, ev, "%s finished a CPU burst and starts %d ticks of I/O. It gives up the CPU (blocked, not ready).", n, ev->detail1);
            break;
        case EVENT_PROC_WAKE_IO:
            explain_narrate(g, ev, "%s's I/O is done: back to the ready queue.", n);
            break;
        case EVENT_PAGE_FAULT:
            explain_narrate(g, ev, "%s touches page %d, which is not in any frame: PAGE FAULT.", n, ev->detail1);
            break;
        case EVENT_PROC_BLOCK_FAULT:
            explain_narrate(g, ev, "%s blocks for %d ticks while the disk reads page %d. The CPU is free for others.", n, s->disk_latency, ev->detail1);
            break;
        case EVENT_PAGE_EVICT: explain_evict(g, ev); break;
        case EVENT_PROC_WAKE_FAULT: {
            bool valid_page = ev->pid >= 0 && ev->detail1 >= 0 && ev->detail1 < PRISM_MAX_VIRTUAL_PAGES;
            int frame_idx = valid_page ? s->procs[ev->pid].pt[ev->detail1].frame_idx : -1;
            explain_narrate(g, ev, "Page %d of %s is now in frame %d; %s is ready again and will retry the access.",
                    ev->detail1, n, frame_idx, n);
            break;
        }
        case EVENT_PROC_TERMINATE: {
            const pcb_t *p = &s->procs[ev->pid];
            int64_t turnaround = p->finish_tick - (int64_t)p->arrival_tick;
            explain_narrate(g, ev, "%s finished at t%" PRId64 ". Turnaround = %" PRId64 " - %" PRIu64 " = %" PRId64
                    " ticks; it waited %u ticks in the ready queue.",
                    n, p->finish_tick, p->finish_tick, (uint64_t)p->arrival_tick, turnaround, p->total_wait_time);
            break;
        }
        case EVENT_PROC_KILLED: explain_narrate(g, ev, "%s was killed (fault injection).", n); break;
        case EVENT_PAGE_ACCESS: break;
        case EVENT_SEM_WAIT:
        case EVENT_SEM_BLOCKED:
        case EVENT_SEM_SIGNAL:
        case EVENT_SEM_WAKE:
        case EVENT_STALLED:
            explain_sync_event(g, ev);
            break;
        case EVENT_JOB_RELEASE:
        case EVENT_JOB_COMPLETE:
        case EVENT_DEADLINE_MISS:
            explain_realtime_event(g, ev);
            break;
        default:
            explain_narrate(g, ev, "%s: %s", event_type_str(ev->type), ev->pid >= 0 ? n : "");
            break;
    }
}

static const narration_t *narration_at(const gui_t *g, uint32_t i) {
    return &g->narration[(g->narration_start + i) % GUI_NARR_CAP];
}

static bool starts_new_tick(const gui_t *g, int64_t i) {
    return i == 0 || narration_at(g, (uint32_t)(i - 1))->tick != narration_at(g, (uint32_t)i)->tick;
}

void panel_explain(gui_t *g, Rectangle body) {
    const float font = 13, text_x = 64;
    float text_w = body.width - text_x - 10;

    if (CheckCollisionPointRec(GetMousePosition(), body)) {
        g->narration_scroll += GetMouseWheelMove() * 2;
        if (g->narration_scroll < 0) g->narration_scroll = 0;
        if (g->narration_scroll > (float)g->narration_len) g->narration_scroll = (float)g->narration_len;
    }

    if (g->narration_len == 0) {
        ui_text_wrapped(body.x + 10, body.y + 8, body.width - 20, 14, UI_TEXT_DIM,
                        "Every scheduling and memory decision is explained here as it happens. "
                        "Press Step (N) to advance one decision at a time, Back (B) to undo a tick.");
        return;
    }

    tick_t newest_tick = narration_at(g, g->narration_len - 1)->tick;
    ui_scissor_push(body);
    float y = body.y + body.height - 4;
    for (int64_t i = (int64_t)g->narration_len - 1 - (int64_t)g->narration_scroll; i >= 0 && y > body.y; i--) {
        const narration_t *e = narration_at(g, (uint32_t)i);
        float h = ui_text_wrapped_height(text_w, font, e->text);
        y -= h + 3;

        if (e->tick == newest_tick) {
            DrawRectangleRec((Rectangle){body.x, y - 1, body.width, h + 3}, ColorAlpha(UI_ACCENT, 0.10f));
        }
        if (starts_new_tick(g, i)) {
            ui_text(body.x + 8, y, font, UI_TEXT_DIM, "t%" PRIu64, (uint64_t)e->tick);
            DrawLine((int)body.x + 4, (int)y - 2, (int)(body.x + body.width - 4), (int)y - 2, (Color){44, 49, 60, 255});
        }
        if (e->pid >= 0) DrawRectangle((int)(body.x + text_x - 12), (int)y + 4, 7, 7, proc_color(e->pid));
        ui_text_wrapped(body.x + text_x, y, text_w, font, event_color((event_type_t)e->type), e->text);
    }
    ui_scissor_pop();
}

static uint32_t first_of_latest_tick(const gui_t *g, tick_t tick) {
    enum { MAX_LINES = 3 };
    uint32_t first = g->narration_len;
    while (first > 0 && narration_at(g, first - 1)->tick == tick && g->narration_len - first < MAX_LINES) first--;
    return first;
}

void explain_draw_latest(gui_t *g, Rectangle canvas) {
    bool too_fast = g->playing && g->speed > 6;
    if (g->narration_len == 0 || too_fast) return;

    tick_t tick = narration_at(g, g->narration_len - 1)->tick;
    bool stale = tick + 1 < g->sim->tick;
    if (stale && g->playing) return;

    uint32_t first = first_of_latest_tick(g, tick);
    float w = canvas.width * 0.62f;
    if (w > 760) w = 760;

    float h = 10;
    for (uint32_t i = first; i < g->narration_len; i++) h += ui_text_wrapped_height(w - 40, 14, narration_at(g, i)->text) + 4;

    Rectangle box = {canvas.x + 12, canvas.y + canvas.height - h - 34, w, h + 22};
    DrawRectangleRounded(box, 0.08f, 6, (Color){14, 16, 21, 225});
    DrawRectangleRoundedLinesEx(box, 0.08f, 6, 1, UI_BORDER);
    ui_text(box.x + 10, box.y + 5, 12, UI_ACCENT, "WHAT JUST HAPPENED (tick %" PRIu64 ")", (uint64_t)tick);

    float y = box.y + 24;
    for (uint32_t i = first; i < g->narration_len; i++) {
        const narration_t *e = narration_at(g, i);
        if (e->pid >= 0) DrawRectangle((int)box.x + 12, (int)y + 5, 8, 8, proc_color(e->pid));
        y += ui_text_wrapped(box.x + 28, y, w - 40, 14, UI_TEXT, e->text) + 4;
    }
}
