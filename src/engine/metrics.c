#include "prism/metrics.h"
#include "prism/sim_api.h"
#include <string.h>

static double percent(double part, double whole) {
    return whole > 0 ? 100.0 * part / whole : 0.0;
}

static void compute_process_averages(const sim_t *s, sim_metrics_t *m) {
    double total_wait = 0, total_turnaround = 0, total_response = 0;

    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state == PROC_UNUSED) continue;

        m->processes++;
        if (p->total_wait_time > m->max_wait) m->max_wait = p->total_wait_time;

        bool completed = p->state == PROC_TERMINATED && p->finish_tick >= 0;
        if (!completed) continue;

        m->completed++;
        total_wait += p->total_wait_time;
        total_turnaround += (double)(p->finish_tick - (int64_t)p->arrival_tick);
        if (p->start_tick >= 0) total_response += (double)(p->start_tick - (int64_t)p->arrival_tick);
    }

    if (m->completed > 0) {
        m->avg_wait = total_wait / m->completed;
        m->avg_turnaround = total_turnaround / m->completed;
        m->avg_response = total_response / m->completed;
    }
}

static uint64_t total_busy_ticks(const sim_t *s) {
    uint64_t busy = 0;
    for (int c = 0; c < s->num_cores; c++) busy += s->core_busy_ticks[c];
    return busy;
}

void sim_metrics_compute(const sim_t *s, sim_metrics_t *m) {
    memset(m, 0, sizeof(*m));
    compute_process_averages(s, m);

    double elapsed = (double)s->tick;
    m->makespan = s->tick;
    m->cpu_util = percent((double)total_busy_ticks(s), elapsed * s->num_cores);
    m->throughput = percent(m->completed, elapsed);
    m->ctx_switches = s->total_context_switches;
    m->deadline_misses = s->total_deadline_misses;
    m->page_accesses = s->total_page_accesses;
    m->page_faults = s->total_page_faults;
    m->fault_rate = percent(s->total_page_faults, s->total_page_accesses);
    m->finished = sim_is_done(s) && !sim_is_stalled(s);
}

static int apply_variant_sched(sim_t *dst, int sched, int quantum) {
    if (sched == SIM_VARIANT_KEEP) {
        if (quantum <= 0) return SIM_OK;
        return sim_set_sched(dst, sched_get_ops(dst->sched_policy)->key, quantum);
    }
    if (sched < 0 || sched >= SCHED_COUNT) return SIM_ERR_INVALID;
    return sim_set_sched(dst, sched_get_ops((sched_policy_t)sched)->key, quantum > 0 ? quantum : 0);
}

static int apply_variant_repl(sim_t *dst, int repl) {
    if (repl == SIM_VARIANT_KEEP) return SIM_OK;
    if (repl < 0 || repl >= REPL_COUNT) return SIM_ERR_INVALID;
    return sim_set_repl(dst, repl_get_ops((repl_policy_t)repl)->key);
}

static int apply_variant_frames(sim_t *dst, int frames) {
    if (frames == SIM_VARIANT_KEEP || frames == dst->configured_frames) return SIM_OK;
    return sim_set_frames(dst, frames);
}

int sim_prepare_variant(sim_t *dst, const sim_t *base, int sched, int repl, int quantum, int frames) {
    if (dst == base) return SIM_ERR_INVALID;

    *dst = *base;
    sim_set_observer(dst, NULL, NULL);
    sim_reset(dst);

    int rc = apply_variant_sched(dst, sched, quantum);
    if (rc == SIM_OK) rc = apply_variant_repl(dst, repl);
    if (rc == SIM_OK) rc = apply_variant_frames(dst, frames);
    return rc;
}

bool sim_run_bounded(sim_t *s, tick_t max_ticks) {
    tick_t stop = s->tick + max_ticks;
    while (s->tick < stop && sim_step(s)) {
    }
    return sim_is_done(s) && !sim_is_stalled(s);
}
