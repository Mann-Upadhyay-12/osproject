#include "prism/sched.h"
#include "prism/sim.h"
#include "prism/text.h"
#include <stddef.h>

#define NO_PROCESS  (-1)
#define NO_CORE     (-1)
#define MAIN_QUEUE  0

static void no_init(sim_t *s) {
    (void)s;
}

static void no_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    (void)s;
    (void)core_id;
    (void)running_pid;
}

static void no_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static void preempt(sim_t *s, int core_id, sim_pid_t pid, int32_t detail1, int32_t detail2) {
    pcb_t *p = &s->procs[pid];
    p->state = PROC_READY;
    p->assigned_core = NO_CORE;
    s->running_pid[core_id] = NO_PROCESS;
    sim_log_event(s, EVENT_PROC_PREEMPT, pid, core_id, detail1, detail2);
}

static bool quantum_expired(pcb_t *p) {
    if (p->quantum_remaining > 0) p->quantum_remaining--;
    return p->quantum_remaining == 0;
}

static bool has_ready(const sim_t *s, int queue_idx) {
    return s->ready_count[queue_idx] > 0;
}

typedef bool (*runs_before_fn)(const pcb_t *candidate, const pcb_t *best);

static sim_pid_t take_best_ready(sim_t *s, runs_before_fn runs_before) {
    sim_pid_t best = NO_PROCESS;
    for (sim_pid_t pid = s->ready_head[MAIN_QUEUE]; pid != NO_PROCESS; pid = s->procs[pid].next_proc) {
        if (best == NO_PROCESS || runs_before(&s->procs[pid], &s->procs[best])) best = pid;
    }
    if (best != NO_PROCESS) sim_queue_remove(s, MAIN_QUEUE, best);
    return best;
}

static const burst_t *current_cpu_burst(const pcb_t *p) {
    if (p->curr_burst_idx >= p->num_bursts) return NULL;

    const burst_t *burst = &p->bursts[p->curr_burst_idx];
    return burst->kind == BURST_CPU ? burst : NULL;
}

static uint16_t cpu_burst_length(const pcb_t *p) {
    const burst_t *burst = current_cpu_burst(p);
    return burst ? burst->duration : 0;
}

static uint16_t cpu_burst_left(const pcb_t *p) {
    const burst_t *burst = current_cpu_burst(p);
    return burst ? burst->remaining : 0;
}

static void fcfs_on_ready(sim_t *s, sim_pid_t pid) {
    sim_queue_push(s, MAIN_QUEUE, pid);
}

static sim_pid_t fcfs_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    return sim_queue_pop(s, MAIN_QUEUE);
}

static void rr_init(sim_t *s) {
    if (s->default_quantum == 0) s->default_quantum = 2;
}

static void rr_on_ready(sim_t *s, sim_pid_t pid) {
    s->procs[pid].quantum_remaining = s->default_quantum;
    sim_queue_push(s, MAIN_QUEUE, pid);
}

static sim_pid_t rr_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    sim_pid_t pid = sim_queue_pop(s, MAIN_QUEUE);
    if (pid != NO_PROCESS) s->procs[pid].quantum_remaining = s->default_quantum;
    return pid;
}

static void rr_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    if (!quantum_expired(p)) return;

    if (!has_ready(s, MAIN_QUEUE)) {
        p->quantum_remaining = s->default_quantum;
        return;
    }
    preempt(s, core_id, running_pid, 0, 0);
    rr_on_ready(s, running_pid);
}

static bool shorter_burst(const pcb_t *candidate, const pcb_t *best) {
    uint16_t a = cpu_burst_length(candidate);
    uint16_t b = cpu_burst_length(best);
    return a < b || (a == b && candidate->arrival_tick < best->arrival_tick);
}

static sim_pid_t sjf_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    return take_best_ready(s, shorter_burst);
}

static bool less_time_left(const pcb_t *candidate, const pcb_t *best) {
    uint16_t a = cpu_burst_left(candidate);
    uint16_t b = cpu_burst_left(best);
    return a < b || (a == b && candidate->arrival_tick < best->arrival_tick);
}

static void srtf_preempt_for(sim_t *s, sim_pid_t newcomer) {
    uint16_t newcomer_left = cpu_burst_left(&s->procs[newcomer]);

    for (int c = 0; c < s->num_cores; c++) {
        sim_pid_t running = s->running_pid[c];
        if (running == NO_PROCESS) continue;

        uint16_t running_left = cpu_burst_left(&s->procs[running]);
        if (newcomer_left < running_left) {
            preempt(s, c, running, running_left, newcomer_left);
            sim_queue_push(s, MAIN_QUEUE, running);
            return;
        }
    }
}

static void srtf_on_ready(sim_t *s, sim_pid_t pid) {
    sim_queue_push(s, MAIN_QUEUE, pid);
    srtf_preempt_for(s, pid);
}

static sim_pid_t srtf_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    return take_best_ready(s, less_time_left);
}

static void srtf_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    uint16_t running_left = cpu_burst_left(&s->procs[running_pid]);

    for (sim_pid_t pid = s->ready_head[MAIN_QUEUE]; pid != NO_PROCESS; pid = s->procs[pid].next_proc) {
        uint16_t candidate_left = cpu_burst_left(&s->procs[pid]);
        if (candidate_left < running_left) {
            preempt(s, core_id, running_pid, running_left, candidate_left);
            sim_queue_push(s, MAIN_QUEUE, running_pid);
            return;
        }
    }
}

static const uint16_t MLFQ_QUANTA[PRISM_MLFQ_QUEUES] = {2, 4, 8};
#define MLFQ_BOOST_PERIOD 50
#define MLFQ_TOP_LEVEL    0
#define MLFQ_LOWEST_LEVEL (PRISM_MLFQ_QUEUES - 1)

static void mlfq_init(sim_t *s) {
    s->mlfq_last_boost_tick = 0;
}

static void mlfq_boost_all(sim_t *s) {
    for (int level = 1; level < PRISM_MLFQ_QUEUES; level++) {
        while (has_ready(s, level)) {
            sim_pid_t pid = sim_queue_pop(s, level);
            s->procs[pid].quantum_remaining = MLFQ_QUANTA[MLFQ_TOP_LEVEL];
            sim_queue_push(s, MLFQ_TOP_LEVEL, pid);
        }
    }
    for (int i = 0; i < s->num_processes; i++) {
        s->procs[i].priority = MLFQ_TOP_LEVEL;
    }
}

static void mlfq_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    if (p->priority > MLFQ_LOWEST_LEVEL) p->priority = MLFQ_LOWEST_LEVEL;

    p->quantum_remaining = MLFQ_QUANTA[p->priority];
    sim_queue_push(s, p->priority, pid);
}

static sim_pid_t mlfq_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    for (int level = 0; level < PRISM_MLFQ_QUEUES; level++) {
        sim_pid_t pid = sim_queue_pop(s, level);
        if (pid != NO_PROCESS) {
            s->procs[pid].quantum_remaining = MLFQ_QUANTA[level];
            return pid;
        }
    }
    return NO_PROCESS;
}

static void mlfq_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    if (s->tick - s->mlfq_last_boost_tick >= MLFQ_BOOST_PERIOD) {
        s->mlfq_last_boost_tick = s->tick;
        mlfq_boost_all(s);
    }

    pcb_t *p = &s->procs[running_pid];
    if (!quantum_expired(p)) return;

    if (p->priority < MLFQ_LOWEST_LEVEL) p->priority++;
    preempt(s, core_id, running_pid, p->priority, 0);
    mlfq_on_ready(s, running_pid);
}

#define ADAPTIVE_NORMAL        0
#define ADAPTIVE_THRASHING     1
#define ADAPTIVE_NORMAL_SLICE  3
#define ADAPTIVE_THRASH_SLICE  1
#define ADAPTIVE_AGING_LIMIT   15

static bool is_thrashing(const pcb_t *p) {
    return p->num_page_faults > 5 && p->total_fault_time > p->total_cpu_time * 2;
}

static void adaptive_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    bool thrashing = is_thrashing(p);
    p->priority = thrashing ? ADAPTIVE_THRASHING : ADAPTIVE_NORMAL;
    p->quantum_remaining = thrashing ? ADAPTIVE_THRASH_SLICE : ADAPTIVE_NORMAL_SLICE;
    sim_queue_push(s, p->priority, pid);
}

static void adaptive_age_one_thrasher(sim_t *s) {
    for (sim_pid_t pid = s->ready_head[ADAPTIVE_THRASHING]; pid != NO_PROCESS; pid = s->procs[pid].next_proc) {
        if (s->procs[pid].total_wait_time > ADAPTIVE_AGING_LIMIT) {
            sim_queue_remove(s, ADAPTIVE_THRASHING, pid);
            s->procs[pid].priority = ADAPTIVE_NORMAL;
            sim_queue_push(s, ADAPTIVE_NORMAL, pid);
            return;
        }
    }
}

static sim_pid_t adaptive_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    adaptive_age_one_thrasher(s);

    if (has_ready(s, ADAPTIVE_NORMAL)) return sim_queue_pop(s, ADAPTIVE_NORMAL);
    return sim_queue_pop(s, ADAPTIVE_THRASHING);
}

static void adaptive_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    if (!quantum_expired(p)) return;

    bool anyone_waiting = has_ready(s, ADAPTIVE_NORMAL) || has_ready(s, ADAPTIVE_THRASHING);
    if (!anyone_waiting) {
        p->quantum_remaining = ADAPTIVE_NORMAL_SLICE;
        return;
    }
    preempt(s, core_id, running_pid, 0, 0);
    adaptive_on_ready(s, running_pid);
}

static const uint32_t CFS_WEIGHTS[] = {1024, 820, 640, 480, 320};
#define CFS_NUM_WEIGHTS    5
#define CFS_SLICE_TICKS    2
#define CFS_VRUNTIME_SCALE 100ULL
#define CFS_WAKEUP_CREDIT  (CFS_SLICE_TICKS * CFS_VRUNTIME_SCALE)

static void cfs_init(sim_t *s) {
    s->cfs_min_vruntime = 0;
}

static void cfs_update_min_vruntime(sim_t *s) {
    uint64_t smallest = UINT64_MAX;

    for (int c = 0; c < s->num_cores; c++) {
        sim_pid_t running = s->running_pid[c];
        if (running != NO_PROCESS && s->procs[running].vruntime < smallest) {
            smallest = s->procs[running].vruntime;
        }
    }
    for (sim_pid_t pid = s->ready_head[MAIN_QUEUE]; pid != NO_PROCESS; pid = s->procs[pid].next_proc) {
        if (s->procs[pid].vruntime < smallest) smallest = s->procs[pid].vruntime;
    }

    bool found = smallest != UINT64_MAX;
    if (found && smallest > s->cfs_min_vruntime) s->cfs_min_vruntime = smallest;
}

static uint64_t cfs_vruntime_floor(const sim_t *s, const pcb_t *p) {
    uint64_t floor = s->cfs_min_vruntime;
    bool has_run_before = p->total_cpu_time > 0;
    if (!has_run_before) return floor;
    return floor > CFS_WAKEUP_CREDIT ? floor - CFS_WAKEUP_CREDIT : 0;
}

static void cfs_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    cfs_update_min_vruntime(s);

    uint64_t floor = cfs_vruntime_floor(s, p);
    if (p->vruntime < floor) p->vruntime = floor;

    p->quantum_remaining = CFS_SLICE_TICKS;
    sim_queue_push(s, MAIN_QUEUE, pid);
}

static bool smaller_vruntime(const pcb_t *candidate, const pcb_t *best) {
    return candidate->vruntime < best->vruntime;
}

static sim_pid_t cfs_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    sim_pid_t pid = take_best_ready(s, smaller_vruntime);
    if (pid != NO_PROCESS) s->procs[pid].quantum_remaining = CFS_SLICE_TICKS;
    return pid;
}

static uint64_t cfs_vruntime_per_tick(const pcb_t *p) {
    uint32_t weight_idx = p->priority < CFS_NUM_WEIGHTS ? p->priority : CFS_NUM_WEIGHTS - 1;
    return (1024ULL * CFS_VRUNTIME_SCALE) / CFS_WEIGHTS[weight_idx];
}

static void cfs_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    p->vruntime += cfs_vruntime_per_tick(p);
    cfs_update_min_vruntime(s);

    if (!quantum_expired(p)) return;

    if (!has_ready(s, MAIN_QUEUE)) {
        p->quantum_remaining = CFS_SLICE_TICKS;
        return;
    }
    preempt(s, core_id, running_pid, (int32_t)(p->vruntime / CFS_VRUNTIME_SCALE), 0);
    sim_queue_push(s, MAIN_QUEUE, running_pid);
}

#define BFS_RR_INTERVAL    2
#define BFS_LATENCY_TARGET 10

static uint64_t bfs_deadline(const sim_t *s, const pcb_t *p) {
    return s->tick + (uint64_t)(BFS_LATENCY_TARGET * (p->priority + 1));
}

static void bfs_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    p->quantum_remaining = BFS_RR_INTERVAL;
    p->virtual_deadline = bfs_deadline(s, p);
    sim_queue_push(s, MAIN_QUEUE, pid);
}

static bool earlier_deadline(const pcb_t *candidate, const pcb_t *best) {
    return candidate->virtual_deadline < best->virtual_deadline;
}

static sim_pid_t bfs_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    sim_pid_t pid = take_best_ready(s, earlier_deadline);
    if (pid != NO_PROCESS) s->procs[pid].quantum_remaining = BFS_RR_INTERVAL;
    return pid;
}

static void bfs_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    if (!quantum_expired(p)) return;

    if (!has_ready(s, MAIN_QUEUE)) {
        p->quantum_remaining = BFS_RR_INTERVAL;
        return;
    }
    p->virtual_deadline = bfs_deadline(s, p);
    preempt(s, core_id, running_pid, (int32_t)p->virtual_deadline, 0);
    sim_queue_push(s, MAIN_QUEUE, running_pid);
}

static const sched_ops_t SCHEDULERS[SCHED_COUNT] = {
    [SCHED_FCFS] = {
        .policy = SCHED_FCFS, .name = "FCFS", .key = "fcfs",
        .init = no_init, .on_ready = fcfs_on_ready, .pick_next = fcfs_pick_next,
        .on_tick = no_tick, .on_block = no_block
    },
    [SCHED_RR] = {
        .policy = SCHED_RR, .name = "RoundRobin", .key = "rr",
        .init = rr_init, .on_ready = rr_on_ready, .pick_next = rr_pick_next,
        .on_tick = rr_on_tick, .on_block = no_block
    },
    [SCHED_SJF] = {
        .policy = SCHED_SJF, .name = "SJF", .key = "sjf",
        .init = no_init, .on_ready = fcfs_on_ready, .pick_next = sjf_pick_next,
        .on_tick = no_tick, .on_block = no_block
    },
    [SCHED_SRTF] = {
        .policy = SCHED_SRTF, .name = "SRTF", .key = "srtf",
        .init = no_init, .on_ready = srtf_on_ready, .pick_next = srtf_pick_next,
        .on_tick = srtf_on_tick, .on_block = no_block
    },
    [SCHED_MLFQ] = {
        .policy = SCHED_MLFQ, .name = "MLFQ", .key = "mlfq",
        .init = mlfq_init, .on_ready = mlfq_on_ready, .pick_next = mlfq_pick_next,
        .on_tick = mlfq_on_tick, .on_block = no_block
    },
    [SCHED_ADAPTIVE] = {
        .policy = SCHED_ADAPTIVE, .name = "Adaptive", .key = "adaptive",
        .init = no_init, .on_ready = adaptive_on_ready, .pick_next = adaptive_pick_next,
        .on_tick = adaptive_on_tick, .on_block = no_block
    },
    [SCHED_CFS] = {
        .policy = SCHED_CFS, .name = "CFS", .key = "cfs",
        .init = cfs_init, .on_ready = cfs_on_ready, .pick_next = cfs_pick_next,
        .on_tick = cfs_on_tick, .on_block = no_block
    },
    [SCHED_BFS] = {
        .policy = SCHED_BFS, .name = "BFS", .key = "bfs",
        .init = no_init, .on_ready = bfs_on_ready, .pick_next = bfs_pick_next,
        .on_tick = bfs_on_tick, .on_block = no_block
    },
};

const sched_ops_t *sched_get_ops(sched_policy_t policy) {
    bool known = policy >= 0 && policy < SCHED_COUNT;
    return &SCHEDULERS[known ? policy : SCHED_FCFS];
}

bool sched_lookup(const char *name, sched_policy_t *out) {
    for (int i = 0; i < SCHED_COUNT; i++) {
        const sched_ops_t *ops = &SCHEDULERS[i];
        if (text_equals_ignore_case(name, ops->key) || text_equals_ignore_case(name, ops->name)) {
            if (out) *out = ops->policy;
            return true;
        }
    }
    return false;
}
