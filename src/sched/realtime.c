#include "sched_common.h"
#include "prism/realtime.h"
#include <math.h>

#define BACKGROUND_KEY UINT64_MAX

typedef uint64_t (*rt_key_fn)(const pcb_t *p);

static bool is_periodic(const pcb_t *p) {
    return p->period > 0;
}

static uint64_t rm_key(const pcb_t *p) {
    return is_periodic(p) ? p->period : BACKGROUND_KEY;
}

static uint64_t edf_key(const pcb_t *p) {
    return is_periodic(p) ? p->job_deadline : BACKGROUND_KEY;
}

static bool runs_before(rt_key_fn key, const pcb_t *candidate, const pcb_t *best) {
    uint64_t a = key(candidate), b = key(best);
    if (a != b) return a < b;
    if (candidate->arrival_tick != best->arrival_tick) return candidate->arrival_tick < best->arrival_tick;
    return candidate->pid < best->pid;
}

static bool rm_runs_before(const pcb_t *candidate, const pcb_t *best) {
    return runs_before(rm_key, candidate, best);
}

static bool edf_runs_before(const pcb_t *candidate, const pcb_t *best) {
    return runs_before(edf_key, candidate, best);
}

static int32_t key_detail(uint64_t key) {
    return key == BACKGROUND_KEY ? -1 : (int32_t)key;
}

static bool any_core_idle(const sim_t *s) {
    for (int c = 0; c < s->num_cores; c++) {
        if (s->running_pid[c] == NO_PROCESS) return true;
    }
    return false;
}

static int core_running_latest(const sim_t *s, rt_key_fn key) {
    int worst = NO_CORE;
    for (int c = 0; c < s->num_cores; c++) {
        sim_pid_t pid = s->running_pid[c];
        if (pid == NO_PROCESS) continue;
        if (worst == NO_CORE || runs_before(key, &s->procs[s->running_pid[worst]], &s->procs[pid])) worst = c;
    }
    return worst;
}

static void preempt_for_newcomer(sim_t *s, sim_pid_t newcomer, rt_key_fn key) {
    sim_queue_push(s, MAIN_QUEUE, newcomer);
    if (any_core_idle(s)) return;

    int core = core_running_latest(s, key);
    if (core == NO_CORE) return;

    sim_pid_t running = s->running_pid[core];
    if (!runs_before(key, &s->procs[newcomer], &s->procs[running])) return;

    sched_preempt(s, core, running, key_detail(key(&s->procs[running])), key_detail(key(&s->procs[newcomer])));
    sim_queue_push(s, MAIN_QUEUE, running);
}

static void preempt_if_outranked(sim_t *s, int core_id, sim_pid_t running, rt_key_fn key) {
    for (sim_pid_t pid = s->ready_head[MAIN_QUEUE]; pid != NO_PROCESS; pid = s->procs[pid].next_proc) {
        if (!runs_before(key, &s->procs[pid], &s->procs[running])) continue;

        sched_preempt(s, core_id, running, key_detail(key(&s->procs[running])), key_detail(key(&s->procs[pid])));
        sim_queue_push(s, MAIN_QUEUE, running);
        return;
    }
}

void rm_on_ready(sim_t *s, sim_pid_t pid) {
    preempt_for_newcomer(s, pid, rm_key);
}

sim_pid_t rm_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    return sched_take_best_ready(s, rm_runs_before);
}

void rm_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    preempt_if_outranked(s, core_id, running_pid, rm_key);
}

void edf_on_ready(sim_t *s, sim_pid_t pid) {
    preempt_for_newcomer(s, pid, edf_key);
}

sim_pid_t edf_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    return sched_take_best_ready(s, edf_runs_before);
}

void edf_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    preempt_if_outranked(s, core_id, running_pid, edf_key);
}

uint32_t rt_job_demand(const pcb_t *p) {
    uint32_t demand = 0;
    for (int b = 0; b < p->num_bursts; b++) {
        if (p->bursts[b].kind == BURST_CPU) demand += p->bursts[b].duration;
    }
    return demand;
}

uint64_t rt_priority_key(const sim_t *s, const pcb_t *p) {
    return s->sched_policy == SCHED_EDF ? edf_key(p) : rm_key(p);
}

static uint64_t gcd(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static uint64_t capped_lcm(uint64_t a, uint64_t b) {
    uint64_t lcm = a / gcd(a, b) * b;
    return lcm > RT_HYPERPERIOD_LIMIT ? 0 : lcm;
}

void rt_analyze(const sim_t *s, rt_analysis_t *out) {
    *out = (rt_analysis_t){0};
    out->hyperperiod = 1;

    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        if (!is_periodic(p) || p->state == PROC_UNUSED) continue;

        double demand = rt_job_demand(p);
        out->tasks++;
        out->utilization += demand / p->period;
        out->density += demand / (p->relative_deadline < p->period ? p->relative_deadline : p->period);
        if (out->hyperperiod) out->hyperperiod = capped_lcm(out->hyperperiod, p->period);
    }

    if (out->tasks == 0) {
        out->hyperperiod = 0;
        return;
    }
    out->rm_bound = out->tasks * (pow(2.0, 1.0 / out->tasks) - 1.0);
    out->rm_guaranteed = out->utilization <= out->rm_bound + 1e-12;
    out->edf_feasible = out->density <= 1.0 + 1e-12;
}
