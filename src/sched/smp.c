#include "sched_common.h"

#define SMP_BALANCE_INTERVAL 4
#define SMP_IMBALANCE        2

static bool allowed_on(const pcb_t *p, int core) {
    return p->affinity_mask == 0 || (p->affinity_mask & (1u << core)) != 0;
}

static int core_load(const sim_t *s, int core) {
    return s->ready_count[core] + (s->running_pid[core] != NO_PROCESS ? 1 : 0);
}

static int least_loaded_core(const sim_t *s, const pcb_t *p) {
    int best = NO_CORE;
    for (int c = 0; c < s->num_cores; c++) {
        if (!allowed_on(p, c)) continue;
        if (best == NO_CORE || core_load(s, c) < core_load(s, best)) best = c;
    }
    return best == NO_CORE ? 0 : best;
}

static int home_core(const sim_t *s, const pcb_t *p) {
    bool cache_warm = p->last_core != NO_CORE && p->last_core < s->num_cores && allowed_on(p, p->last_core);
    return cache_warm ? p->last_core : least_loaded_core(s, p);
}

static void enqueue_on(sim_t *s, int core, sim_pid_t pid) {
    s->procs[pid].quantum_remaining = s->default_quantum;
    sim_queue_push(s, core, pid);
}

static void migrate(sim_t *s, sim_pid_t pid, int from, int to) {
    sim_queue_remove(s, from, pid);
    sim_queue_push(s, to, pid);
    sim_log_event(s, EVENT_MIGRATE, pid, to, from, to);
}

static sim_pid_t first_movable(const sim_t *s, int from, int to) {
    for (sim_pid_t pid = s->ready_head[from]; pid != NO_PROCESS; pid = s->procs[pid].next_proc) {
        if (allowed_on(&s->procs[pid], to)) return pid;
    }
    return NO_PROCESS;
}

static int busiest_core_with_work_for(const sim_t *s, int to) {
    int busiest = NO_CORE;
    for (int c = 0; c < s->num_cores; c++) {
        if (c == to || first_movable(s, c, to) == NO_PROCESS) continue;
        if (busiest == NO_CORE || core_load(s, c) > core_load(s, busiest)) busiest = c;
    }
    return busiest;
}

void smp_init(sim_t *s) {
    if (s->default_quantum == 0) s->default_quantum = 2;
    s->last_balance_tick = 0;
}

void smp_on_ready(sim_t *s, sim_pid_t pid) {
    enqueue_on(s, home_core(s, &s->procs[pid]), pid);
}

static sim_pid_t steal_for(sim_t *s, int core) {
    int victim = busiest_core_with_work_for(s, core);
    if (victim == NO_CORE) return NO_PROCESS;

    sim_pid_t pid = first_movable(s, victim, core);
    migrate(s, pid, victim, core);
    return sim_queue_pop(s, core);
}

sim_pid_t smp_pick_next(sim_t *s, int core_id) {
    sim_pid_t pid = sim_queue_pop(s, core_id);
    if (pid == NO_PROCESS) pid = steal_for(s, core_id);
    if (pid != NO_PROCESS) s->procs[pid].quantum_remaining = s->default_quantum;
    return pid;
}

static void balance_once(sim_t *s) {
    int busiest = NO_CORE, idlest = NO_CORE;
    for (int c = 0; c < s->num_cores; c++) {
        if (busiest == NO_CORE || core_load(s, c) > core_load(s, busiest)) busiest = c;
        if (idlest == NO_CORE || core_load(s, c) < core_load(s, idlest)) idlest = c;
    }
    if (busiest == idlest || core_load(s, busiest) - core_load(s, idlest) < SMP_IMBALANCE) return;

    sim_pid_t pid = first_movable(s, busiest, idlest);
    if (pid != NO_PROCESS) migrate(s, pid, busiest, idlest);
}

void smp_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    bool balance_due = s->tick % SMP_BALANCE_INTERVAL == 0 && s->last_balance_tick != s->tick;
    if (balance_due) {
        s->last_balance_tick = s->tick;
        balance_once(s);
    }

    pcb_t *p = &s->procs[running_pid];
    if (p->quantum_remaining > 0) p->quantum_remaining--;
    if (p->quantum_remaining > 0) return;

    if (s->ready_count[core_id] == 0) {
        p->quantum_remaining = s->default_quantum;
        return;
    }
    sched_preempt(s, core_id, running_pid, 0, 0);
    enqueue_on(s, core_id, running_pid);
}
