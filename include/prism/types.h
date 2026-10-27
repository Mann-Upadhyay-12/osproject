#ifndef PRISM_TYPES_H
#define PRISM_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PRISM_MAX_PROCESSES      64
#define PRISM_MAX_BURSTS         64
#define PRISM_MAX_PAGE_REFS      256
#define PRISM_MAX_CORES          4
#define PRISM_MAX_FRAMES         128
#define PRISM_MAX_VIRTUAL_PAGES  64
#define PRISM_MAX_EVENT_LOG      8192
#define PRISM_MAX_NAME_LEN       32
#define PRISM_GANTT_HISTORY      512

/* Extensions Limits */
#define PRISM_MAX_RESOURCES      16
#define PRISM_MAX_MSG_QUEUE      32
#define PRISM_MAX_CHECKPOINTS    8

typedef uint64_t tick_t;
typedef int32_t  sim_pid_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_NEW,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED_IO,
    PROC_BLOCKED_FAULT,
    PROC_BLOCKED_RESOURCE,
    PROC_BLOCKED_IPC,
    PROC_TERMINATED
} proc_state_t;

typedef enum {
    BURST_CPU = 0,
    BURST_IO,
    BURST_ACQUIRE,    /* resource request */
    BURST_RELEASE,    /* resource release */
    BURST_IPC_SEND,   /* send message to target PID */
    BURST_IPC_RECV,   /* receive message */
    BURST_CHECKPOINT  /* capture checkpoint */
} burst_kind_t;

typedef struct {
    uint8_t  kind;          /* burst_kind_t */
    uint16_t duration;      /* duration in ticks, or resource/target id */
    uint16_t remaining;     /* ticks remaining in current burst */
    int16_t  target_id;     /* resource_id or target_pid */
    int16_t  extra_data;    /* message payload or count */
} burst_t;

typedef enum {
    SCHED_FCFS = 0,
    SCHED_RR,
    SCHED_SJF,
    SCHED_SRTF,
    SCHED_MLFQ,
    SCHED_ADAPTIVE,
    SCHED_CFS,
    SCHED_BFS,
    SCHED_COUNT
} sched_policy_t;

typedef enum {
    REPL_FIFO = 0,
    REPL_LRU,
    REPL_CLOCK,
    REPL_OPT,
    REPL_COUNT
} repl_policy_t;

typedef enum {
    EVENT_NONE = 0,
    EVENT_PROC_ARRIVE,
    EVENT_PROC_DISPATCH,
    EVENT_PROC_PREEMPT,
    EVENT_PROC_BLOCK_IO,
    EVENT_PROC_BLOCK_FAULT,
    EVENT_PROC_WAKE_IO,
    EVENT_PROC_WAKE_FAULT,
    EVENT_PROC_TERMINATE,
    EVENT_PAGE_ACCESS,
    EVENT_PAGE_FAULT,
    EVENT_PAGE_EVICT,
    EVENT_RAM_SHRUNK,
    EVENT_PROC_KILLED,
    /* Resource & Deadlock Events */
    EVENT_RESOURCE_REQUEST,
    EVENT_RESOURCE_ACQUIRED,
    EVENT_RESOURCE_RELEASED,
    EVENT_RESOURCE_BLOCKED,
    EVENT_DEADLOCK_DETECTED,
    EVENT_DEADLOCK_RESOLVED,
    /* IPC Events */
    EVENT_IPC_SEND,
    EVENT_IPC_RECV,
    EVENT_IPC_BLOCKED,
    /* Checkpoint & Rollback Events */
    EVENT_CHECKPOINT_SAVED,
    EVENT_CHECKPOINT_RESTORED,
    EVENT_FAULT_INJECTED
} event_type_t;

typedef struct {
    tick_t       tick;
    event_type_t type;
    sim_pid_t    pid;
    int32_t      core_id;
    int32_t      detail1;    /* e.g., page number, frame, resource_id */
    int32_t      detail2;    /* e.g., victim frame, target pid */
} sim_event_t;

#endif /* PRISM_TYPES_H */
