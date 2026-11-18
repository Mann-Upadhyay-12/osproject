#ifndef PRISM_SIM_H
#define PRISM_SIM_H

#include "prism/types.h"
#include "prism/pcb.h"
#include "prism/vm.h"
#include "prism/sched.h"

#define PRISM_MLFQ_QUEUES 3

typedef void (*sim_observer_fn)(const sim_event_t *ev, void *user);

typedef struct {
    char           name[PRISM_MAX_NAME_LEN];
    int32_t        total_instances;
    int32_t        available_instances;
    sim_pid_t      owner_pid;
    uint64_t       allocated_mask;
} sim_resource_t;

typedef struct {
    char           name[PRISM_MAX_NAME_LEN];
    int32_t        initial_value;
    int32_t        value;
    int32_t        num_waiters;
    sim_pid_t      waiters[PRISM_MAX_PROCESSES];
    uint32_t       total_waits;
    uint32_t       total_blocks;
} sim_semaphore_t;

typedef struct {
    sim_pid_t      sender_pid;
    sim_pid_t      receiver_pid;
    int16_t        message_id;
    int16_t        data;
    tick_t         timestamp;
} ipc_message_t;

typedef struct {
    bool           valid;
    sim_pid_t      pid;
    uint32_t       checkpoint_id;
    tick_t         saved_tick;
    pcb_t          saved_pcb;
} checkpoint_t;

typedef struct sim {
    tick_t             tick;
    uint64_t           seed;
    uint64_t           prng_state;

    int32_t            num_cores;
    sim_pid_t          running_pid[PRISM_MAX_CORES];
    uint32_t           core_busy_ticks[PRISM_MAX_CORES];

    int32_t            num_processes;
    pcb_t              procs[PRISM_MAX_PROCESSES];

    int16_t            ready_head[PRISM_MLFQ_QUEUES];
    int16_t            ready_tail[PRISM_MLFQ_QUEUES];
    uint16_t           ready_count[PRISM_MLFQ_QUEUES];
    tick_t             mlfq_last_boost_tick;
    uint16_t           default_quantum;
    uint64_t           cfs_min_vruntime;

    sched_policy_t     sched_policy;
    repl_policy_t      repl_policy;

    int32_t            total_frames;
    int32_t            configured_frames;
    int32_t            disk_latency;
    int32_t            clock_hand;
    frame_entry_t      frames[PRISM_MAX_FRAMES];
    uint32_t           total_page_accesses;
    uint32_t           total_page_faults;

    int32_t            num_resources;
    sim_resource_t     resources[PRISM_MAX_RESOURCES];

    int32_t            num_semaphores;
    sim_semaphore_t    semaphores[PRISM_MAX_SEMAPHORES];

    int32_t            ipc_msg_count;
    ipc_message_t      ipc_messages[PRISM_MAX_MSG_QUEUE];

    bool               deadlock_detected;
    sim_pid_t          deadlock_cycle[PRISM_MAX_PROCESSES];
    int32_t            deadlock_cycle_len;

    checkpoint_t       checkpoints[PRISM_MAX_CHECKPOINTS];
    uint32_t           total_checkpoints;

    uint32_t           completed_processes;
    uint32_t           num_removed;
    uint32_t           total_context_switches;
    uint32_t           total_deadlocks_detected;
    uint32_t           total_deadlocks_recovered;

    sim_event_t        event_log[PRISM_MAX_EVENT_LOG];
    uint32_t           event_count;
    uint64_t           log_hash;
    uint64_t           notable_events;

    sim_observer_fn    observer;
    void              *observer_user;

    sim_pid_t          gantt_history[PRISM_GANTT_HISTORY][PRISM_MAX_CORES];
    uint32_t           gantt_count;
} sim_t;

void sim_init(sim_t *s, int num_cores, int num_frames, int disk_latency,
              sched_policy_t sched, repl_policy_t repl, uint64_t seed);
void sim_set_policy(sim_t *s, sched_policy_t sched, repl_policy_t repl);
bool sim_is_done(const sim_t *s);
bool sim_is_stalled(const sim_t *s);
bool sim_step(sim_t *s);
void sim_run(sim_t *s);

sim_pid_t sim_add_process(sim_t *s, const char *name, tick_t arrival_tick);
bool sim_add_burst(sim_t *s, sim_pid_t pid, burst_kind_t kind, uint16_t duration);
bool sim_add_page_ref(sim_t *s, sim_pid_t pid, uint16_t vpage);
void sim_release_frames(sim_t *s, sim_pid_t pid);

void sim_queue_push(sim_t *s, int queue_idx, sim_pid_t pid);
sim_pid_t sim_queue_pop(sim_t *s, int queue_idx);
bool sim_queue_remove(sim_t *s, int queue_idx, sim_pid_t pid);

void sim_log_event(sim_t *s, event_type_t type, sim_pid_t pid, int core, int d1, int d2);
const char *event_type_str(event_type_t type);
const char *proc_state_str(proc_state_t st);

static inline void sim_snapshot(const sim_t *src, sim_t *dst) {
    *dst = *src;
}

static inline void sim_restore(sim_t *dst, const sim_t *src) {
    *dst = *src;
}

int32_t sim_add_resource(sim_t *s, const char *name, int32_t instances);
bool sim_acquire_resource(sim_t *s, sim_pid_t pid, int32_t resource_id);
bool sim_release_resource(sim_t *s, sim_pid_t pid, int32_t resource_id);
void sim_release_all_resources(sim_t *s, sim_pid_t pid);

int32_t sim_add_semaphore(sim_t *s, const char *name, int32_t initial_value);
int32_t sim_find_semaphore(const sim_t *s, const char *name);
bool sim_sem_wait(sim_t *s, sim_pid_t pid, int32_t sem_id);
bool sim_sem_signal(sim_t *s, sim_pid_t pid, int32_t sem_id);
void sim_sem_forget(sim_t *s, sim_pid_t pid);

bool sim_detect_deadlock(sim_t *s);
bool sim_recover_deadlock_terminate(sim_t *s);
bool sim_recover_deadlock_rollback(sim_t *s);

bool sim_ipc_send(sim_t *s, sim_pid_t sender, sim_pid_t receiver, int16_t msg_id, int16_t data);
bool sim_ipc_recv(sim_t *s, sim_pid_t receiver, sim_pid_t expected_sender, int16_t *msg_id_out, int16_t *data_out);

uint32_t sim_checkpoint_create(sim_t *s, sim_pid_t pid);
bool sim_checkpoint_restore(sim_t *s, sim_pid_t pid, uint32_t checkpoint_id);

bool sim_inject_kill(sim_t *s, sim_pid_t pid);
bool sim_inject_shrink_ram(sim_t *s, int new_frames);
bool sim_inject_resource_fail(sim_t *s, int32_t resource_id);

#endif
