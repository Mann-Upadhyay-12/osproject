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

typedef uint64_t tick_t;
typedef int32_t  sim_pid_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_NEW,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED_IO,
    PROC_BLOCKED_FAULT,
    PROC_TERMINATED
} proc_state_t;

typedef enum {
    BURST_CPU = 0,
    BURST_IO
} burst_kind_t;

typedef struct {
    uint8_t  kind;          /* BURST_CPU or BURST_IO */
    uint16_t duration;      /* total burst length in ticks */
    uint16_t remaining;     /* ticks remaining in current burst */
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
    EVENT_PROC_KILLED
} event_type_t;

typedef struct {
    tick_t       tick;
    event_type_t type;
    sim_pid_t    pid;
    int32_t      core_id;
    int32_t      detail1;    /* e.g., page number, frame, quantum */
    int32_t      detail2;    /* e.g., victim frame */
} sim_event_t;

#endif /* PRISM_TYPES_H */
