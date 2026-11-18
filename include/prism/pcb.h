#ifndef PRISM_PCB_H
#define PRISM_PCB_H

#include "prism/types.h"

typedef struct {
    bool     valid;
    bool     referenced;
    bool     modified;
    int16_t  frame_idx;
    tick_t   last_access_tick;
    tick_t   load_tick;
} page_table_entry_t;

typedef struct {
    sim_pid_t          pid;
    char               name[PRISM_MAX_NAME_LEN];
    proc_state_t       state;

    tick_t             arrival_tick;
    int64_t            start_tick;
    int64_t            finish_tick;
    uint16_t           priority;
    uint16_t           base_priority;
    uint16_t           quantum_remaining;
    int16_t            assigned_core;
    uint64_t           vruntime;
    uint64_t           virtual_deadline;

    uint32_t           total_cpu_time;
    uint32_t           total_wait_time;
    uint32_t           total_io_time;
    uint32_t           total_fault_time;
    uint32_t           num_context_switches;
    uint32_t           num_page_faults;

    burst_t            bursts[PRISM_MAX_BURSTS];
    uint16_t           num_bursts;
    uint16_t           curr_burst_idx;

    uint16_t           page_refs[PRISM_MAX_PAGE_REFS];
    uint16_t           num_page_refs;
    uint16_t           curr_page_ref_idx;

    page_table_entry_t pt[PRISM_MAX_VIRTUAL_PAGES];

    uint16_t           io_remaining;
    uint16_t           fault_remaining;
    uint16_t           fault_vpage;
    int16_t            waiting_resource;
    int16_t            waiting_ipc_pid;

    uint16_t           held_resources_mask;

    uint16_t           num_checkpoints;

    int16_t            next_proc;
} pcb_t;

#endif
