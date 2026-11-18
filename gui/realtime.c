#include "gui_state.h"
#include "prism/realtime.h"
#include <inttypes.h>
#include <stdio.h>


static const char *rt_key_text(const sim_t *s, int32_t key) {
    if (key < 0) return "background";
    return s->sched_policy == SCHED_EDF ? TextFormat("due t%d", key) : TextFormat("period %d", key);
}

bool explain_realtime_preempt(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    if (s->sched_policy != SCHED_RM && s->sched_policy != SCHED_EDF) return false;

    const char *rule = s->sched_policy == SCHED_EDF ? "an earlier deadline" : "a shorter period (higher fixed priority)";
    explain_narrate(g, ev, "%s (%s) is preempted: a ready job has %s (%s).", explain_process_name(s, ev->pid),
                    rt_key_text(s, ev->detail1), rule, rt_key_text(s, ev->detail2));
    return true;
}

void explain_realtime_event(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const char *n = explain_process_name(s, ev->pid);
    const pcb_t *p = &s->procs[ev->pid];

    switch (ev->type) {
        case EVENT_JOB_RELEASE:
            explain_narrate(g, ev, "%s releases job %d of %u: %u ticks of work, due by t%d.", n, ev->detail1, p->num_jobs,
                            rt_job_demand(p), ev->detail2);
            break;
        case EVENT_JOB_COMPLETE:
            if (ev->detail2 > 0) {
                explain_narrate(g, ev, "%s finishes job %d, %d ticks after its deadline.", n, ev->detail1, ev->detail2);
            } else if (ev->detail1 < p->num_jobs) {
                explain_narrate(g, ev, "%s finishes job %d with %d ticks to spare and sleeps until its next period (t%" PRIu64 ").",
                                n, ev->detail1, -ev->detail2, (uint64_t)(p->job_release + p->period));
            } else {
                explain_narrate(g, ev, "%s finishes its last job (%d) with %d ticks to spare.", n, ev->detail1, -ev->detail2);
            }
            break;
        case EVENT_DEADLINE_MISS:
            explain_narrate(g, ev, "DEADLINE MISS: %s's job %d was due at t%d and is not done. It keeps running, late.", n,
                            ev->detail1, ev->detail2);
            break;
        default:
            break;
    }
}

static void draw_analysis(const sim_t *s, const rt_analysis_t *a, float x, float *y, float w) {
    ui_text(x, *y, 13, UI_ACCENT, "SCHEDULABILITY  (%d periodic task%s)", a->tasks, a->tasks == 1 ? "" : "s");
    *y += 20;
    ui_text(x, *y, 13, UI_TEXT, "utilization U = sum(C / P) = %.3f", a->utilization);
    *y += 18;

    Color rm_color = a->rm_guaranteed ? UI_GOOD : UI_WARN;
    ui_text(x, *y, 13, rm_color, "RM bound n(2^(1/n) - 1) = %.3f: %s", a->rm_bound,
            a->rm_guaranteed ? "U is under it, RM meets every deadline" : "U is over it, RM may miss deadlines");
    *y += 18;
    ui_text(x, *y, 13, a->edf_feasible ? UI_GOOD : UI_BAD, "EDF: %s", a->edf_feasible ? "U <= 1, every deadline can be met"
                                                                                       : "U > 1, overloaded: misses are certain");
    *y += 18;
    if (a->hyperperiod > 0) {
        ui_text_clipped((Rectangle){x, *y, w, 16}, 12, UI_TEXT_DIM,
                        TextFormat("hyperperiod (lcm of periods) = %" PRIu64 " ticks; deadline misses so far: %u",
                                   a->hyperperiod, s->total_deadline_misses));
        *y += 18;
    }
    *y += 6;
}

static void draw_task_header(float x, float y) {
    static const char *headers[] = {"task", "C", "P", "D", "C/P", "job", "due", "misses"};
    const float offsets[] = {0, 110, 150, 190, 230, 290, 350, 420};
    for (int i = 0; i < 8; i++) ui_text(x + offsets[i], y, 12, UI_TEXT_DIM, "%s", headers[i]);
}

static void draw_task_row(const pcb_t *p, float x, float y) {
    DrawRectangleRounded((Rectangle){x, y + 3, 10, 10}, 0.3f, 4, proc_color(p->pid));
    ui_text_clipped((Rectangle){x + 14, y, 92, 16}, 13, UI_TEXT, p->name);

    uint32_t demand = rt_job_demand(p);
    ui_text(x + 110, y, 13, UI_TEXT, "%u", demand);
    ui_text(x + 150, y, 13, UI_TEXT, "%u", p->period);
    ui_text(x + 190, y, 13, UI_TEXT, "%u", p->relative_deadline);
    ui_text(x + 230, y, 13, UI_TEXT, "%.2f", (double)demand / p->period);

    bool finished = p->state == PROC_TERMINATED;
    bool sleeping = p->state == PROC_SLEEPING;
    int current_job = finished ? p->jobs_done : p->jobs_done + 1;
    ui_text(x + 290, y, 13, UI_TEXT, "%d/%u", current_job, p->num_jobs);
    if (finished) ui_text(x + 350, y, 13, UI_TEXT_DIM, "done");
    else if (sleeping) ui_text(x + 350, y, 13, UI_TEXT_DIM, "next t%" PRIu64, (uint64_t)p->job_release);
    else if (p->state != PROC_NEW) ui_text(x + 350, y, 13, p->job_missed ? UI_BAD : UI_TEXT, "t%" PRIu64, (uint64_t)p->job_deadline);
    ui_text(x + 420, y, 13, p->deadline_misses ? UI_BAD : UI_TEXT_DIM, "%u", p->deadline_misses);
}

void panel_realtime(gui_t *g, Rectangle body) {
    const sim_t *s = g->sim;
    rt_analysis_t analysis;
    rt_analyze(s, &analysis);

    ui_scissor_push(body);
    float x = body.x + 10, y = body.y + 8, w = body.width - 20;
    if (analysis.tasks == 0) {
        ui_text_wrapped(x, y, w, 13, UI_TEXT_DIM,
                        "No periodic tasks here. Open rt_rate_monotonic or rt_deadline_miss, or give a process a period "
                        "in the builder. Then pick RateMonotonic or EDF as the scheduler.");
        ui_scissor_pop();
        return;
    }

    draw_analysis(s, &analysis, x, &y, w);
    draw_task_header(x, y);
    y += 18;
    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        if (p->period == 0 || p->state == PROC_UNUSED) continue;
        draw_task_row(p, x, y);
        y += 18;
    }
    ui_scissor_pop();
}
