#include "prism/metrics.h"
#include "prism/sim_api.h"
#include <string.h>

void sim_metrics_compute(const sim_t *s, sim_metrics_t *m) {
    memset(m, 0, sizeof(*m));
    double wait = 0, tat = 0, resp = 0;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state == PROC_UNUSED) continue;
        m->processes++;
        if (p->total_wait_time > m->max_wait) m->max_wait = p->total_wait_time;
        if (p->state != PROC_TERMINATED || p->finish_tick < 0) continue;
        m->completed++;
        wait += p->total_wait_time;
        tat += (double)(p->finish_tick - (int64_t)p->arrival_tick);
        /* A process killed before it ever ran has no response time; count it as 0. */
        if (p->start_tick >= 0) resp += (double)(p->start_tick - (int64_t)p->arrival_tick);
    }
    if (m->completed) {
        m->avg_wait = wait / m->completed;
        m->avg_turnaround = tat / m->completed;
        m->avg_response = resp / m->completed;
    }

    uint64_t busy = 0;
    for (int c = 0; c < s->num_cores; c++) busy += s->core_busy_ticks[c];
    m->makespan = s->tick;
    if (s->tick > 0 && s->num_cores > 0) m->cpu_util = 100.0 * (double)busy / ((double)s->tick * s->num_cores);
    if (s->tick > 0) m->throughput = 100.0 * m->completed / (double)s->tick;
    m->ctx_switches = s->total_context_switches;
    m->page_accesses = s->total_page_accesses;
    m->page_faults = s->total_page_faults;
    if (s->total_page_accesses) m->fault_rate = 100.0 * s->total_page_faults / s->total_page_accesses;
    m->finished = sim_is_done(s);
}

int sim_prepare_variant(sim_t *dst, const sim_t *base, int sched, int repl, int quantum, int frames) {
    if (dst == base) return SIM_ERR_INVALID;
    *dst = *base;
    sim_set_observer(dst, NULL, NULL);
    sim_reset(dst);

    if (sched != SIM_VARIANT_KEEP) {
        if (sched < 0 || sched >= SCHED_COUNT) return SIM_ERR_INVALID;
        int rc = sim_set_sched(dst, sched_get_ops((sched_policy_t)sched)->key, quantum > 0 ? quantum : 0);
        if (rc != SIM_OK) return rc;
    } else if (quantum > 0) {
        int rc = sim_set_sched(dst, sched_get_ops(dst->sched_policy)->key, quantum);
        if (rc != SIM_OK) return rc;
    }
    if (repl != SIM_VARIANT_KEEP) {
        if (repl < 0 || repl >= REPL_COUNT) return SIM_ERR_INVALID;
        int rc = sim_set_repl(dst, repl_get_ops((repl_policy_t)repl)->key);
        if (rc != SIM_OK) return rc;
    }
    /* sim_set_frames resets, which is fine: nothing has run yet. */
    if (frames != SIM_VARIANT_KEEP && frames != dst->configured_frames) {
        int rc = sim_set_frames(dst, frames);
        if (rc != SIM_OK) return rc;
    }
    return SIM_OK;
}

bool sim_run_bounded(sim_t *s, tick_t max_ticks) {
    tick_t stop = s->tick + max_ticks;
    while (s->tick < stop && sim_step(s)) {
    }
    return sim_is_done(s);
}
