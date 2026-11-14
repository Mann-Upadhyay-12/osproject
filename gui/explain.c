#include "gui_state.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * "Explain": turns trace events into sentences a student can follow
 * ("SJF dispatches P2: shortest next CPU burst (4). Others: P3=9, P4=5").
 * Sentences are written inside the observer, because that is the only moment
 * the state that justified the decision (the other ready processes, the
 * victim frame's load/use times) is still visible. Pure view: reads sim_t,
 * never changes it.
 */

/* ------------------------------------------------------------------------
 * Algorithm cards
 * ------------------------------------------------------------------------ */

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

const algo_info_t *explain_sched_info(sched_policy_t p) {
    return (p >= 0 && p < SCHED_COUNT) ? &g_sched_info[p] : &g_sched_info[0];
}

const algo_info_t *explain_repl_info(repl_policy_t p) {
    return (p >= 0 && p < REPL_COUNT) ? &g_repl_info[p] : &g_repl_info[0];
}

float explain_draw_card(const algo_info_t *a, float x, float y, float w) {
    float y0 = y;
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
    return y - y0;
}

/* ------------------------------------------------------------------------
 * Narration
 * ------------------------------------------------------------------------ */

static const char *pname(const sim_t *s, int pid) {
    return (pid >= 0 && pid < s->num_processes) ? s->procs[pid].name : "?";
}

static int cpu_burst_len(const pcb_t *p, bool remaining) {
    if (p->curr_burst_idx >= p->num_bursts || p->bursts[p->curr_burst_idx].kind != BURST_CPU) return 0;
    return remaining ? p->bursts[p->curr_burst_idx].remaining : p->bursts[p->curr_burst_idx].duration;
}

/* The value the current scheduler compares, for "why this one" sentences. */
static bool sched_key(const sim_t *s, const pcb_t *p, char *out, size_t n) {
    switch (s->sched_policy) {
        case SCHED_SJF:  snprintf(out, n, "%d", cpu_burst_len(p, false)); return true;
        case SCHED_SRTF: snprintf(out, n, "%d", cpu_burst_len(p, true)); return true;
        case SCHED_CFS:  snprintf(out, n, "%.1f", (double)p->vruntime / 100.0); return true;
        case SCHED_BFS:  snprintf(out, n, "t%" PRIu64, (uint64_t)p->virtual_deadline); return true;
        case SCHED_MLFQ: case SCHED_ADAPTIVE: snprintf(out, n, "Q%u", p->priority); return true;
        default:         out[0] = '\0'; return false;
    }
}

/* "P3=9, P4=5" for up to 5 processes still in the ready queues. */
static void others(const sim_t *s, char *out, size_t n) {
    size_t off = 0;
    int shown = 0, total = 0;
    out[0] = '\0';
    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        int guard = 0;
        for (int pid = s->ready_head[q]; pid != -1 && guard < PRISM_MAX_PROCESSES; pid = s->procs[pid].next_proc, guard++) {
            total++;
            if (shown >= 5 || off + 24 >= n) continue;
            char key[24];
            bool has = sched_key(s, &s->procs[pid], key, sizeof(key));
            off += (size_t)snprintf(out + off, n - off, has ? "%s%s=%s" : "%s%s", shown ? ", " : "", s->procs[pid].name, key);
            shown++;
        }
    }
    if (total > shown && off + 12 < n) snprintf(out + off, n - off, " +%d more", total - shown);
}

static void push(gui_t *g, const sim_event_t *ev, const char *fmt, ...) {
    narr_t *e;
    if (g->narr_len < GUI_NARR_CAP) {
        e = &g->narr[(g->narr_start + g->narr_len) % GUI_NARR_CAP];
        g->narr_len++;
    } else {
        e = &g->narr[g->narr_start];
        g->narr_start = (g->narr_start + 1) % GUI_NARR_CAP;
    }
    e->tick = ev->tick;
    e->pid = ev->pid;
    e->type = ev->type;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
}

void explain_clear(gui_t *g) {
    g->narr_start = g->narr_len = 0;
    g->narr_scroll = 0;
}

static void explain_dispatch(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const pcb_t *p = &s->procs[ev->pid];
    const char *n = p->name;
    const char *alg = sched_get_ops(s->sched_policy)->name;
    char rest[160], why[120];
    others(s, rest, sizeof(rest));
    switch (s->sched_policy) {
        case SCHED_FCFS: snprintf(why, sizeof(why), "it is first in the ready queue (FIFO order)"); break;
        case SCHED_RR:   snprintf(why, sizeof(why), "head of the ready queue, for a quantum of %u ticks", s->default_quantum); break;
        case SCHED_SJF:  snprintf(why, sizeof(why), "shortest next CPU burst (%d ticks)", cpu_burst_len(p, false)); break;
        case SCHED_SRTF: snprintf(why, sizeof(why), "least remaining CPU time (%d ticks)", cpu_burst_len(p, true)); break;
        case SCHED_MLFQ: snprintf(why, sizeof(why), "from Q%u, the highest non-empty level (quantum %u)", p->priority, p->quantum_remaining); break;
        case SCHED_ADAPTIVE: snprintf(why, sizeof(why), "from level %u (quantum %u)", p->priority, p->quantum_remaining); break;
        case SCHED_CFS:  snprintf(why, sizeof(why), "smallest virtual runtime (%.1f)", (double)p->vruntime / 100.0); break;
        case SCHED_BFS:  snprintf(why, sizeof(why), "earliest virtual deadline (t%" PRIu64 ")", (uint64_t)p->virtual_deadline); break;
        default:         snprintf(why, sizeof(why), "chosen by the policy"); break;
    }
    if (rest[0]) push(g, ev, "%s picks %s for core %d: %s. Still waiting: %s.", alg, n, ev->core_id, why, rest);
    else push(g, ev, "%s picks %s for core %d: %s. Nobody else is waiting.", alg, n, ev->core_id, why);
}

static void explain_preempt(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const char *n = pname(s, ev->pid);
    switch (s->sched_policy) {
        case SCHED_RR:
            push(g, ev, "%s used its whole %u-tick quantum: preempted, back to the END of the ready queue.", n, s->default_quantum);
            break;
        case SCHED_MLFQ:
            push(g, ev, "%s used its whole quantum, so MLFQ treats it as CPU-bound: now in Q%d (lower priority, longer quantum).",
                 n, ev->detail1);
            break;
        case SCHED_SRTF:
            push(g, ev, "%s preempted: a ready process needs only %d ticks, %s still needs %d. Shortest remaining time wins.",
                 n, ev->detail2, n, ev->detail1);
            break;
        case SCHED_CFS:
            push(g, ev, "%s's 2-tick slice ended (vruntime now %d); CFS switches to the lowest vruntime.", n, ev->detail1);
            break;
        case SCHED_BFS:
            push(g, ev, "%s's 2-tick slice ended; new virtual deadline t%d, so earlier deadlines go first.", n, ev->detail1);
            break;
        default:
            push(g, ev, "%s's quantum expired: preempted and back to the ready queue.", n);
            break;
    }
}

static void explain_evict(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    int f = ev->detail2;
    if (f < 0 || f >= s->total_frames) return;
    const frame_entry_t *fe = &s->frames[f];   /* not overwritten yet: the engine logs before loading */
    const char *alg = repl_get_ops(s->repl_policy)->name;
    char why[120];
    switch (s->repl_policy) {
        case REPL_FIFO: snprintf(why, sizeof(why), "it was loaded longest ago (t%" PRIu64 ")", (uint64_t)fe->load_tick); break;
        case REPL_LRU:  snprintf(why, sizeof(why), "it is the least recently used (last use t%" PRIu64 ")", (uint64_t)fe->last_access_tick); break;
        case REPL_CLOCK: snprintf(why, sizeof(why), "the clock hand found its reference bit at 0 (others got a second chance)"); break;
        case REPL_OPT: {
            const pcb_t *o = &s->procs[ev->pid];
            int dist = -1;
            for (int r = o->curr_page_ref_idx; r < o->num_page_refs; r++) {
                if (o->page_refs[r] == ev->detail1) { dist = r - o->curr_page_ref_idx; break; }
            }
            if (dist < 0) snprintf(why, sizeof(why), "it is never used again");
            else snprintf(why, sizeof(why), "its next use is farthest away (%d references ahead)", dist);
            break;
        }
        default: snprintf(why, sizeof(why), "chosen by the policy"); break;
    }
    push(g, ev, "RAM is full. %s evicts page %d of %s from frame %d: %s.", alg, ev->detail1, pname(s, ev->pid), f, why);
}

void explain_event(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const char *n = pname(s, ev->pid);
    switch (ev->type) {
        case EVENT_PROC_ARRIVE:
            if (s->sched_policy == SCHED_MLFQ) push(g, ev, "%s arrives and enters the top queue Q0.", n);
            else push(g, ev, "%s arrives and joins the ready queue.", n);
            break;
        case EVENT_PROC_DISPATCH: explain_dispatch(g, ev); break;
        case EVENT_PROC_PREEMPT:  explain_preempt(g, ev); break;
        case EVENT_PROC_BLOCK_IO:
            push(g, ev, "%s finished a CPU burst and starts %d ticks of I/O. It gives up the CPU (blocked, not ready).", n, ev->detail1);
            break;
        case EVENT_PROC_WAKE_IO:
            push(g, ev, "%s's I/O is done: back to the ready queue.", n);
            break;
        case EVENT_PAGE_FAULT:
            push(g, ev, "%s touches page %d, which is not in any frame: PAGE FAULT.", n, ev->detail1);
            break;
        case EVENT_PROC_BLOCK_FAULT:
            push(g, ev, "%s blocks for %d ticks while the disk reads page %d. The CPU is free for others.", n, s->disk_latency, ev->detail1);
            break;
        case EVENT_PAGE_EVICT: explain_evict(g, ev); break;
        case EVENT_PROC_WAKE_FAULT: {
            int f = (ev->pid >= 0 && ev->detail1 >= 0 && ev->detail1 < PRISM_MAX_VIRTUAL_PAGES) ? s->procs[ev->pid].pt[ev->detail1].frame_idx : -1;
            push(g, ev, "Page %d of %s is now in frame %d; %s is ready again and will retry the access.", ev->detail1, n, f, n);
            break;
        }
        case EVENT_PROC_TERMINATE: {
            const pcb_t *p = &s->procs[ev->pid];
            int64_t tat = p->finish_tick - (int64_t)p->arrival_tick;
            push(g, ev, "%s finished at t%" PRId64 ". Turnaround = %" PRId64 " - %" PRIu64 " = %" PRId64 " ticks; it waited %u ticks in the ready queue.",
                 n, p->finish_tick, p->finish_tick, (uint64_t)p->arrival_tick, tat, p->total_wait_time);
            break;
        }
        case EVENT_PROC_KILLED: push(g, ev, "%s was killed (fault injection).", n); break;
        case EVENT_PAGE_ACCESS: break;   /* hits are too frequent to narrate */
        default:
            push(g, ev, "%s: %s", event_type_str(ev->type), ev->pid >= 0 ? n : "");
            break;
    }
}

/* ------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

static const narr_t *narr_at(const gui_t *g, uint32_t i) {
    return &g->narr[(g->narr_start + i) % GUI_NARR_CAP];
}

void panel_explain(gui_t *g, Rectangle body) {
    const float size = 13, textx = 64;
    float w = body.width - textx - 10;
    if (CheckCollisionPointRec(GetMousePosition(), body)) {
        g->narr_scroll += GetMouseWheelMove() * 2;
        if (g->narr_scroll < 0) g->narr_scroll = 0;
        if (g->narr_scroll > (float)g->narr_len) g->narr_scroll = (float)g->narr_len;
    }
    if (g->narr_len == 0) {
        ui_text_wrapped(body.x + 10, body.y + 8, body.width - 20, 14, UI_TEXT_DIM,
                        "Every scheduling and memory decision is explained here as it happens. "
                        "Press Step (N) to advance one decision at a time, Back (B) to undo a tick.");
        return;
    }
    ui_scissor_push(body);
    /* Bottom-up from the newest entry, skipping narr_scroll entries. */
    float y = body.y + body.height - 4;
    for (int64_t i = (int64_t)g->narr_len - 1 - (int64_t)g->narr_scroll; i >= 0 && y > body.y; i--) {
        const narr_t *e = narr_at(g, (uint32_t)i);
        float h = ui_text_wrapped_height(w, size, e->text);
        y -= h + 3;
        bool newest_tick = (e->tick == narr_at(g, g->narr_len - 1)->tick);
        if (newest_tick) DrawRectangleRec((Rectangle){body.x, y - 1, body.width, h + 3}, ColorAlpha(UI_ACCENT, 0.10f));
        /* Label the first sentence of each tick (drawing goes bottom-up). */
        if (i == 0 || narr_at(g, (uint32_t)(i - 1))->tick != e->tick) {
            ui_text(body.x + 8, y, size, UI_TEXT_DIM, "t%" PRIu64, (uint64_t)e->tick);
            DrawLine((int)body.x + 4, (int)y - 2, (int)(body.x + body.width - 4), (int)y - 2, (Color){44, 49, 60, 255});
        }
        if (e->pid >= 0) DrawRectangle((int)(body.x + textx - 12), (int)y + 4, 7, 7, proc_color(e->pid));
        ui_text_wrapped(body.x + textx, y, w, size, event_color((event_type_t)e->type), e->text);
    }
    ui_scissor_pop();
}

void explain_draw_latest(gui_t *g, Rectangle canvas) {
    if (g->narr_len == 0 || (g->playing && g->speed > 6)) return;
    tick_t t = narr_at(g, g->narr_len - 1)->tick;
    if (t + 1 < g->sim->tick && g->playing) return;
    /* Up to 3 sentences from the newest tick, oldest first. */
    uint32_t first = g->narr_len;
    while (first > 0 && narr_at(g, first - 1)->tick == t && g->narr_len - first < 3) first--;
    float w = canvas.width * 0.62f;
    if (w > 760) w = 760;
    float h = 10;
    for (uint32_t i = first; i < g->narr_len; i++) h += ui_text_wrapped_height(w - 40, 14, narr_at(g, i)->text) + 4;
    Rectangle r = {canvas.x + 12, canvas.y + canvas.height - h - 34, w, h + 22};
    DrawRectangleRounded(r, 0.08f, 6, (Color){14, 16, 21, 225});
    DrawRectangleRoundedLinesEx(r, 0.08f, 6, 1, UI_BORDER);
    ui_text(r.x + 10, r.y + 5, 12, UI_ACCENT, "WHAT JUST HAPPENED (tick %" PRIu64 ")", (uint64_t)t);
    float y = r.y + 24;
    for (uint32_t i = first; i < g->narr_len; i++) {
        const narr_t *e = narr_at(g, i);
        if (e->pid >= 0) DrawRectangle((int)r.x + 12, (int)y + 5, 8, 8, proc_color(e->pid));
        y += ui_text_wrapped(r.x + 28, y, w - 40, 14, UI_TEXT, e->text) + 4;
    }
}
