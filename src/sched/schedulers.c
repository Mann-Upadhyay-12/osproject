#include "prism/sched.h"
#include "prism/sim.h"
#include <assert.h>
#include <stddef.h>

/* =========================================================================
 * 1. First-Come First-Served (FCFS)
 * ========================================================================= */
static void fcfs_init(sim_t *s) {
    (void)s;
}

static void fcfs_on_ready(sim_t *s, sim_pid_t pid) {
    sim_queue_push(s, 0, pid);
}

static sim_pid_t fcfs_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    return sim_queue_pop(s, 0);
}

static void fcfs_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    (void)s;
    (void)core_id;
    (void)running_pid;
    /* FCFS is non-preemptive */
}

static void fcfs_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_fcfs = {
    .policy    = SCHED_FCFS,
    .name      = "FCFS",
    .init      = fcfs_init,
    .on_ready  = fcfs_on_ready,
    .pick_next = fcfs_pick_next,
    .on_tick   = fcfs_on_tick,
    .on_block  = fcfs_on_block
};

/* =========================================================================
 * 2. Round Robin (RR)
 * ========================================================================= */
static void rr_init(sim_t *s) {
    if (s->default_quantum == 0) s->default_quantum = 2;
}

static void rr_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    p->quantum_remaining = s->default_quantum;
    sim_queue_push(s, 0, pid);
}

static sim_pid_t rr_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    sim_pid_t next = sim_queue_pop(s, 0);
    if (next != -1) {
        s->procs[next].quantum_remaining = s->default_quantum;
    }
    return next;
}

static void rr_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    if (p->quantum_remaining > 0) {
        p->quantum_remaining--;
    }

    if (p->quantum_remaining == 0) {
        /* Preemption! Only if someone else is waiting, or rotate back */
        if (s->ready_count[0] > 0) {
            p->state = PROC_READY;
            p->assigned_core = -1;
            s->running_pid[core_id] = -1;
            sim_log_event(s, EVENT_PROC_PREEMPT, running_pid, core_id, 0, 0);
            rr_on_ready(s, running_pid);
        } else {
            /* No one else waiting; renew quantum */
            p->quantum_remaining = s->default_quantum;
        }
    }
}

static void rr_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_rr = {
    .policy    = SCHED_RR,
    .name      = "RoundRobin",
    .init      = rr_init,
    .on_ready  = rr_on_ready,
    .pick_next = rr_pick_next,
    .on_tick   = rr_on_tick,
    .on_block  = rr_on_block
};

/* =========================================================================
 * 3. Multi-Level Feedback Queue (MLFQ)
 * Q0: Quantum 2 (highest)
 * Q1: Quantum 4
 * Q2: Quantum 8 (lowest)
 * Priority boost every 50 ticks to prevent starvation.
 * ========================================================================= */
static const uint16_t g_mlfq_quanta[PRISM_MLFQ_QUEUES] = {2, 4, 8};
#define MLFQ_BOOST_PERIOD 50

static void mlfq_init(sim_t *s) {
    s->mlfq_last_boost_tick = 0;
}

static void mlfq_boost_all(sim_t *s) {
    /* Move all procs from Q1 and Q2 to Q0 */
    for (int q = 1; q < PRISM_MLFQ_QUEUES; q++) {
        while (s->ready_count[q] > 0) {
            sim_pid_t pid = sim_queue_pop(s, q);
            if (pid != -1) {
                s->procs[pid].priority = 0;
                s->procs[pid].quantum_remaining = g_mlfq_quanta[0];
                sim_queue_push(s, 0, pid);
            }
        }
    }
    for (int i = 0; i < s->num_processes; i++) {
        s->procs[i].priority = 0;
    }
}

static void mlfq_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    if (p->priority >= PRISM_MLFQ_QUEUES) {
        p->priority = PRISM_MLFQ_QUEUES - 1;
    }
    p->quantum_remaining = g_mlfq_quanta[p->priority];
    sim_queue_push(s, p->priority, pid);
}

static sim_pid_t mlfq_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        if (s->ready_count[q] > 0) {
            sim_pid_t pid = sim_queue_pop(s, q);
            if (pid != -1) {
                s->procs[pid].quantum_remaining = g_mlfq_quanta[q];
                return pid;
            }
        }
    }
    return -1;
}

static void mlfq_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];

    /* Check periodic priority boost */
    if (s->tick - s->mlfq_last_boost_tick >= MLFQ_BOOST_PERIOD) {
        s->mlfq_last_boost_tick = s->tick;
        mlfq_boost_all(s);
    }

    if (p->quantum_remaining > 0) {
        p->quantum_remaining--;
    }

    if (p->quantum_remaining == 0) {
        /* Demote priority if not at bottom */
        if (p->priority + 1 < PRISM_MLFQ_QUEUES) {
            p->priority++;
        }
        p->state = PROC_READY;
        p->assigned_core = -1;
        s->running_pid[core_id] = -1;
        sim_log_event(s, EVENT_PROC_PREEMPT, running_pid, core_id, p->priority, 0);
        mlfq_on_ready(s, running_pid);
    }
}

static void mlfq_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
    /* I/O or page fault before quantum expires keeps existing priority! */
}

static const sched_ops_t g_sched_mlfq = {
    .policy    = SCHED_MLFQ,
    .name      = "MLFQ",
    .init      = mlfq_init,
    .on_ready  = mlfq_on_ready,
    .pick_next = mlfq_pick_next,
    .on_tick   = mlfq_on_tick,
    .on_block  = mlfq_on_block
};

/* =========================================================================
 * 4. Adaptive Scheduler
 * Aging to prevent starvation + Page-fault-rate awareness to combat thrashing.
 * Processes with severe page faults get briefly demoted/paced so that hot
 * memory jobs can complete their CPU bursts.
 * ========================================================================= */
static void adaptive_init(sim_t *s) {
    (void)s;
}

static void adaptive_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    /* If process has been thrashing (high fault ratio), place in lower queue */
    if (p->num_page_faults > 5 && p->total_fault_time > p->total_cpu_time * 2) {
        p->priority = 1;
    } else {
        p->priority = 0;
    }
    p->quantum_remaining = (p->priority == 0) ? 3 : 1;
    sim_queue_push(s, p->priority, pid);
}

static sim_pid_t adaptive_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    /* Starvation aging: if any ready process in queue 1 waited > 15 ticks, promote it */
    if (s->ready_count[1] > 0) {
        sim_pid_t curr = s->ready_head[1];
        while (curr != -1) {
            if (s->procs[curr].total_wait_time > 15) {
                sim_queue_remove(s, 1, curr);
                s->procs[curr].priority = 0;
                sim_queue_push(s, 0, curr);
                break;
            }
            curr = s->procs[curr].next_proc;
        }
    }

    /* Pick from Q0 first, then Q1 */
    if (s->ready_count[0] > 0) {
        return sim_queue_pop(s, 0);
    }
    if (s->ready_count[1] > 0) {
        return sim_queue_pop(s, 1);
    }
    return -1;
}

static void adaptive_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    if (p->quantum_remaining > 0) {
        p->quantum_remaining--;
    }
    if (p->quantum_remaining == 0) {
        if (s->ready_count[0] > 0 || s->ready_count[1] > 0) {
            p->state = PROC_READY;
            p->assigned_core = -1;
            s->running_pid[core_id] = -1;
            sim_log_event(s, EVENT_PROC_PREEMPT, running_pid, core_id, 0, 0);
            adaptive_on_ready(s, running_pid);
        } else {
            p->quantum_remaining = 3;
        }
    }
}

static void adaptive_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_adaptive = {
    .policy    = SCHED_ADAPTIVE,
    .name      = "Adaptive",
    .init      = adaptive_init,
    .on_ready  = adaptive_on_ready,
    .pick_next = adaptive_pick_next,
    .on_tick   = adaptive_on_tick,
    .on_block  = adaptive_on_block
};

/* =========================================================================
 * 5. Shortest Job First (SJF) - Non-preemptive
 * Chooses the process in READY state with the shortest current CPU burst.
 * Ties broken by earlier arrival time.
 * ========================================================================= */
static uint16_t proc_current_cpu_burst(const pcb_t *p) {
    if (p->curr_burst_idx < p->num_bursts &&
        p->bursts[p->curr_burst_idx].kind == BURST_CPU) {
        return p->bursts[p->curr_burst_idx].duration;
    }
    return 0;
}

static uint16_t proc_remaining_cpu_burst(const pcb_t *p) {
    if (p->curr_burst_idx < p->num_bursts &&
        p->bursts[p->curr_burst_idx].kind == BURST_CPU) {
        return p->bursts[p->curr_burst_idx].remaining;
    }
    return 0;
}

static void sjf_init(sim_t *s) {
    (void)s;
}

static void sjf_on_ready(sim_t *s, sim_pid_t pid) {
    sim_queue_push(s, 0, pid);
}

static sim_pid_t sjf_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    if (s->ready_count[0] == 0) return -1;

    sim_pid_t best_pid = -1;
    uint16_t shortest_burst = 0xFFFF;
    tick_t earliest_arrival = UINT64_MAX;

    sim_pid_t curr = s->ready_head[0];
    while (curr != -1) {
        const pcb_t *p = &s->procs[curr];
        uint16_t b = proc_current_cpu_burst(p);
        if (b < shortest_burst || (b == shortest_burst && p->arrival_tick < earliest_arrival)) {
            shortest_burst = b;
            earliest_arrival = p->arrival_tick;
            best_pid = curr;
        }
        curr = p->next_proc;
    }

    if (best_pid != -1) {
        sim_queue_remove(s, 0, best_pid);
    }
    return best_pid;
}

static void sjf_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    (void)s;
    (void)core_id;
    (void)running_pid;
    /* Non-preemptive */
}

static void sjf_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_sjf = {
    .policy    = SCHED_SJF,
    .name      = "SJF",
    .init      = sjf_init,
    .on_ready  = sjf_on_ready,
    .pick_next = sjf_pick_next,
    .on_tick   = sjf_on_tick,
    .on_block  = sjf_on_block
};

/* =========================================================================
 * 6. Shortest Remaining Time First (SRTF) - Preemptive
 * Preempts the running process if a ready candidate has strictly less
 * remaining time on its current CPU burst.
 * ========================================================================= */
static void srtf_init(sim_t *s) {
    (void)s;
}

static void srtf_check_preemption(sim_t *s, sim_pid_t newly_ready_pid) {
    uint16_t new_rem = proc_remaining_cpu_burst(&s->procs[newly_ready_pid]);

    for (int c = 0; c < s->num_cores; c++) {
        sim_pid_t run_pid = s->running_pid[c];
        if (run_pid != -1) {
            uint16_t run_rem = proc_remaining_cpu_burst(&s->procs[run_pid]);
            if (new_rem < run_rem) {
                /* Preempt currently running process on core c */
                pcb_t *rp = &s->procs[run_pid];
                rp->state = PROC_READY;
                rp->assigned_core = -1;
                s->running_pid[c] = -1;
                sim_log_event(s, EVENT_PROC_PREEMPT, run_pid, c, (int32_t)run_rem, (int32_t)new_rem);
                sim_queue_push(s, 0, run_pid);
                break;
            }
        }
    }
}

static void srtf_on_ready(sim_t *s, sim_pid_t pid) {
    sim_queue_push(s, 0, pid);
    srtf_check_preemption(s, pid);
}

static sim_pid_t srtf_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    if (s->ready_count[0] == 0) return -1;

    sim_pid_t best_pid = -1;
    uint16_t shortest_rem = 0xFFFF;
    tick_t earliest_arrival = UINT64_MAX;

    sim_pid_t curr = s->ready_head[0];
    while (curr != -1) {
        const pcb_t *p = &s->procs[curr];
        uint16_t r = proc_remaining_cpu_burst(p);
        if (r < shortest_rem || (r == shortest_rem && p->arrival_tick < earliest_arrival)) {
            shortest_rem = r;
            earliest_arrival = p->arrival_tick;
            best_pid = curr;
        }
        curr = p->next_proc;
    }

    if (best_pid != -1) {
        sim_queue_remove(s, 0, best_pid);
    }
    return best_pid;
}

static void srtf_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    /* If another ready process has shorter remaining time, trigger preemption */
    if (s->ready_count[0] == 0) return;

    uint16_t run_rem = proc_remaining_cpu_burst(&s->procs[running_pid]);
    sim_pid_t curr = s->ready_head[0];
    while (curr != -1) {
        uint16_t cand_rem = proc_remaining_cpu_burst(&s->procs[curr]);
        if (cand_rem < run_rem) {
            pcb_t *p = &s->procs[running_pid];
            p->state = PROC_READY;
            p->assigned_core = -1;
            s->running_pid[core_id] = -1;
            sim_log_event(s, EVENT_PROC_PREEMPT, running_pid, core_id, (int32_t)run_rem, (int32_t)cand_rem);
            sim_queue_push(s, 0, running_pid);
            return;
        }
        curr = s->procs[curr].next_proc;
    }
}

static void srtf_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_srtf = {
    .policy    = SCHED_SRTF,
    .name      = "SRTF",
    .init      = srtf_init,
    .on_ready  = srtf_on_ready,
    .pick_next = srtf_pick_next,
    .on_tick   = srtf_on_tick,
    .on_block  = srtf_on_block
};

/* =========================================================================
 * 7. Completely Fair Scheduler (CFS)
 * Tracks virtual runtime (vruntime) scaled by priority/nice weight.
 * Normalizes new/waking tasks to min_vruntime to prevent unbounded latency.
 * Slices time with dynamic preemption when candidate vruntime leads by latency delta.
 * ========================================================================= */
static const uint32_t cfs_prio_to_weight[5] = {
    1024,  /* Priority 0: normal nice 0 */
    820,   /* Priority 1 */
    640,   /* Priority 2 */
    480,   /* Priority 3 */
    320    /* Priority 4 */
};

static void cfs_init(sim_t *s) {
    s->cfs_min_vruntime = 0;
}

static void cfs_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    /* If newly entering or waking with stale vruntime, pull to min_vruntime */
    if (p->vruntime < s->cfs_min_vruntime) {
        p->vruntime = s->cfs_min_vruntime;
    }
    p->quantum_remaining = 2; /* latency granularity */
    sim_queue_push(s, 0, pid);
}

static sim_pid_t cfs_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    if (s->ready_count[0] == 0) return -1;

    sim_pid_t best_pid = -1;
    uint64_t min_vrt = UINT64_MAX;

    sim_pid_t curr = s->ready_head[0];
    while (curr != -1) {
        const pcb_t *p = &s->procs[curr];
        if (p->vruntime < min_vrt) {
            min_vrt = p->vruntime;
            best_pid = curr;
        }
        curr = p->next_proc;
    }

    if (best_pid != -1) {
        sim_queue_remove(s, 0, best_pid);
        s->procs[best_pid].quantum_remaining = 2;
        /* Update global min_vruntime */
        if (s->procs[best_pid].vruntime > s->cfs_min_vruntime) {
            s->cfs_min_vruntime = s->procs[best_pid].vruntime;
        }
    }
    return best_pid;
}

static void cfs_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];

    /* Advance vruntime inversely proportional to weight: delta_vruntime = (1024 / weight) */
    uint32_t prio_idx = (p->priority < 5) ? p->priority : 4;
    uint32_t weight = cfs_prio_to_weight[prio_idx];
    uint64_t vruntime_advance = (1024ULL * 100ULL) / (uint64_t)weight; /* scaled by 100 for precision */
    p->vruntime += vruntime_advance;

    if (p->vruntime > s->cfs_min_vruntime) {
        s->cfs_min_vruntime = p->vruntime;
    }

    if (p->quantum_remaining > 0) {
        p->quantum_remaining--;
    }

    if (p->quantum_remaining == 0) {
        if (s->ready_count[0] > 0) {
            p->state = PROC_READY;
            p->assigned_core = -1;
            s->running_pid[core_id] = -1;
            sim_log_event(s, EVENT_PROC_PREEMPT, running_pid, core_id, (int32_t)(p->vruntime / 100), 0);
            sim_queue_push(s, 0, running_pid);
        } else {
            p->quantum_remaining = 2;
        }
    }
}

static void cfs_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_cfs = {
    .policy    = SCHED_CFS,
    .name      = "CFS",
    .init      = cfs_init,
    .on_ready  = cfs_on_ready,
    .pick_next = cfs_pick_next,
    .on_tick   = cfs_on_tick,
    .on_block  = cfs_on_block
};

/* =========================================================================
 * 8. Brainfuck Scheduler (BFS)
 * Single runqueue, earliest effective virtual deadline scheduling.
 * Formula: virtual_deadline = current_tick + (latency_target * (prio + 1))
 * Whenever a process consumes its slice (rr_interval), deadline is updated
 * and lowest deadline is picked next.
 * ========================================================================= */
#define BFS_RR_INTERVAL     2
#define BFS_LATENCY_TARGET 10

static void bfs_init(sim_t *s) {
    (void)s;
}

static void bfs_on_ready(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    p->quantum_remaining = BFS_RR_INTERVAL;
    /* Compute virtual deadline based on priority */
    p->virtual_deadline = s->tick + (uint64_t)(BFS_LATENCY_TARGET * (p->priority + 1));
    sim_queue_push(s, 0, pid);
}

static sim_pid_t bfs_pick_next(sim_t *s, int core_id) {
    (void)core_id;
    if (s->ready_count[0] == 0) return -1;

    sim_pid_t best_pid = -1;
    uint64_t earliest_deadline = UINT64_MAX;

    sim_pid_t curr = s->ready_head[0];
    while (curr != -1) {
        const pcb_t *p = &s->procs[curr];
        if (p->virtual_deadline < earliest_deadline) {
            earliest_deadline = p->virtual_deadline;
            best_pid = curr;
        }
        curr = p->next_proc;
    }

    if (best_pid != -1) {
        sim_queue_remove(s, 0, best_pid);
        s->procs[best_pid].quantum_remaining = BFS_RR_INTERVAL;
    }
    return best_pid;
}

static void bfs_on_tick(sim_t *s, int core_id, sim_pid_t running_pid) {
    pcb_t *p = &s->procs[running_pid];
    if (p->quantum_remaining > 0) {
        p->quantum_remaining--;
    }

    if (p->quantum_remaining == 0) {
        if (s->ready_count[0] > 0) {
            p->state = PROC_READY;
            p->assigned_core = -1;
            s->running_pid[core_id] = -1;
            /* Update virtual deadline on timeslice expiration */
            p->virtual_deadline = s->tick + (uint64_t)(BFS_LATENCY_TARGET * (p->priority + 1));
            sim_log_event(s, EVENT_PROC_PREEMPT, running_pid, core_id, (int32_t)p->virtual_deadline, 0);
            sim_queue_push(s, 0, running_pid);
        } else {
            p->quantum_remaining = BFS_RR_INTERVAL;
        }
    }
}

static void bfs_on_block(sim_t *s, sim_pid_t pid) {
    (void)s;
    (void)pid;
}

static const sched_ops_t g_sched_bfs = {
    .policy    = SCHED_BFS,
    .name      = "BFS",
    .init      = bfs_init,
    .on_ready  = bfs_on_ready,
    .pick_next = bfs_pick_next,
    .on_tick   = bfs_on_tick,
    .on_block  = bfs_on_block
};

/* =========================================================================
 * Scheduler Ops Registry
 * ========================================================================= */
const sched_ops_t *sched_get_ops(sched_policy_t policy) {
    switch (policy) {
        case SCHED_FCFS:     return &g_sched_fcfs;
        case SCHED_RR:       return &g_sched_rr;
        case SCHED_SJF:      return &g_sched_sjf;
        case SCHED_SRTF:     return &g_sched_srtf;
        case SCHED_MLFQ:     return &g_sched_mlfq;
        case SCHED_ADAPTIVE: return &g_sched_adaptive;
        case SCHED_CFS:      return &g_sched_cfs;
        case SCHED_BFS:      return &g_sched_bfs;
        default:             return &g_sched_fcfs;
    }
}
