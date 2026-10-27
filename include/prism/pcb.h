#ifndef PRISM_PCB_H
#define PRISM_PCB_H

#include "prism/types.h"

typedef struct {
    bool     valid;
    bool     referenced;
    bool     modified;
    int16_t  frame_idx;         /* index into sim->frames, or -1 */
    tick_t   last_access_tick;
    tick_t   load_tick;
} page_table_entry_t;

typedef struct {
    sim_pid_t          pid;
    char               name[PRISM_MAX_NAME_LEN];
    proc_state_t       state;
    
    /* Scheduling attributes */
    tick_t             arrival_tick;
    int64_t            start_tick;          /* -1 until first dispatch */
    int64_t            finish_tick;         /* -1 until termination */
    uint16_t           priority;            /* 0 is highest */
    uint16_t           quantum_remaining;
    int16_t            assigned_core;       /* -1 if not running */
    uint64_t           vruntime;            /* CFS virtual runtime (fixed-point or scaled) */
    uint64_t           virtual_deadline;    /* BFS virtual deadline */
    
    /* Metrics */
    uint32_t           total_cpu_time;
    uint32_t           total_wait_time;
    uint32_t           total_io_time;
    uint32_t           total_fault_time;
    uint32_t           num_context_switches;
    uint32_t           num_page_faults;
    
    /* Burst script */
    burst_t            bursts[PRISM_MAX_BURSTS];
    uint16_t           num_bursts;
    uint16_t           curr_burst_idx;
    
    /* Page reference script */
    uint16_t           page_refs[PRISM_MAX_PAGE_REFS];
    uint16_t           num_page_refs;
    uint16_t           curr_page_ref_idx;
    
    /* Page Table */
    page_table_entry_t pt[PRISM_MAX_VIRTUAL_PAGES];
    
    /* Blocking state details */
    uint16_t           io_remaining;
    uint16_t           fault_remaining;
    uint16_t           fault_vpage;
    
    /* Intrusive index-based queue links (for flat queues) */
    int16_t            next_proc;
} pcb_t;

#endif /* PRISM_PCB_H */
