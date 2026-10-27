#ifndef PRISM_SIM_H
#define PRISM_SIM_H

#include "prism/types.h"
#include "prism/pcb.h"
#include "prism/vm.h"
#include "prism/sched.h"

#define PRISM_MLFQ_QUEUES 3

typedef struct sim {
    tick_t             tick;
    uint64_t           seed;
    uint64_t           prng_state;

    /* Topology */
    int32_t            num_cores;
    sim_pid_t          running_pid[PRISM_MAX_CORES];
    uint32_t           core_busy_ticks[PRISM_MAX_CORES];

    /* Process table */
    int32_t            num_processes;
    pcb_t              procs[PRISM_MAX_PROCESSES];

    /* Scheduling queues (Flat index-based) */
    int16_t            ready_head[PRISM_MLFQ_QUEUES];
    int16_t            ready_tail[PRISM_MLFQ_QUEUES];
    uint16_t           ready_count[PRISM_MLFQ_QUEUES];
    tick_t             mlfq_last_boost_tick;
    uint16_t           default_quantum;
    uint64_t           cfs_min_vruntime;

    /* Policies */
    sched_policy_t     sched_policy;
    repl_policy_t      repl_policy;

    /* Virtual Memory Subsystem */
    int32_t            total_frames;
    int32_t            disk_latency;
    int32_t            clock_hand;
    frame_entry_t      frames[PRISM_MAX_FRAMES];
    uint32_t           total_page_accesses;
    uint32_t           total_page_faults;

    /* Aggregate Statistics */
    uint32_t           completed_processes;
    uint32_t           total_context_switches;

    /* Deterministic Event Log & Hash */
    sim_event_t        event_log[PRISM_MAX_EVENT_LOG];
    uint32_t           event_count;
    uint64_t           log_hash;

    /* Timeline / Gantt history for visualization */
    sim_pid_t          gantt_history[PRISM_GANTT_HISTORY][PRISM_MAX_CORES];
    uint32_t           gantt_count;
} sim_t;

/* Engine lifecycle & execution */
void sim_init(sim_t *s, int num_cores, int num_frames, int disk_latency,
              sched_policy_t sched, repl_policy_t repl, uint64_t seed);
void sim_set_policy(sim_t *s, sched_policy_t sched, repl_policy_t repl);
bool sim_is_done(const sim_t *s);
bool sim_step(sim_t *s);
void sim_run(sim_t *s);

/* Process creation & management */
sim_pid_t sim_add_process(sim_t *s, const char *name, tick_t arrival_tick);
bool sim_add_burst(sim_t *s, sim_pid_t pid, burst_kind_t kind, uint16_t duration);
bool sim_add_page_ref(sim_t *s, sim_pid_t pid, uint16_t vpage);

/* Ready Queue helpers (flat index-based) */
void sim_queue_push(sim_t *s, int queue_idx, sim_pid_t pid);
sim_pid_t sim_queue_pop(sim_t *s, int queue_idx);
bool sim_queue_remove(sim_t *s, int queue_idx, sim_pid_t pid);

/* Event logging & Hash verification */
void sim_log_event(sim_t *s, event_type_t type, sim_pid_t pid, int core, int d1, int d2);
const char *event_type_str(event_type_t type);
const char *proc_state_str(proc_state_t st);

/* Snapshot / Rewind API (Pure memcpy over flat state) */
static inline void sim_snapshot(const sim_t *src, sim_t *dst) {
    *dst = *src;
}

static inline void sim_restore(sim_t *dst, const sim_t *src) {
    *dst = *src;
}

/* Fault injection */
bool sim_inject_kill(sim_t *s, sim_pid_t pid);
bool sim_inject_shrink_ram(sim_t *s, int new_frames);

#endif /* PRISM_SIM_H */
