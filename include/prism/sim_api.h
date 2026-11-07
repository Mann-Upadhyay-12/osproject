#ifndef PRISM_SIM_API_H
#define PRISM_SIM_API_H

/*
 * Interactive simulation API (Milestone 7).
 *
 * Frontends (TUI, GUI) build and edit scenarios through these calls instead
 * of poking sim_t fields, so every edit goes through the same validation and
 * the engine's invariants hold no matter which frontend drives it.
 *
 * Contract:
 *  - pids are stable for the lifetime of a scenario and never reused; a
 *    removed process leaves a hole (state PROC_UNUSED).
 *  - A process spawned mid-run arrives at the current tick at the earliest
 *    ("arrives now"); its arrival tick is kept, so sim_reset() followed by a
 *    rerun reproduces the same run.
 *  - Same scenario + seed => same trace (log_hash), with or without an
 *    observer attached.
 */

#include "prism/sim.h"

/* Error codes: functions return >= 0 on success, one of these on failure. */
enum {
    SIM_OK          =  0,
    SIM_ERR_FULL    = -1,   /* process table full */
    SIM_ERR_INVALID = -2,   /* spec or argument failed validation */
    SIM_ERR_STATE   = -3,   /* operation not allowed in the process's state */
    SIM_ERR_NO_PID  = -4,   /* pid out of range or removed */
    SIM_ERR_NAME    = -5,   /* unknown policy name */
    SIM_ERR_IO      = -6    /* file could not be opened or written */
};

const char *sim_strerror(int err);

/* Everything a user defines about a process; the rest of pcb_t is runtime. */
typedef struct {
    char      name[PRISM_MAX_NAME_LEN];
    tick_t    arrival;
    uint16_t  priority;
    uint16_t  num_bursts;
    burst_t   bursts[PRISM_MAX_BURSTS];      /* kind, duration, target_id, extra_data */
    uint16_t  num_page_refs;
    uint16_t  page_refs[PRISM_MAX_PAGE_REFS];
} proc_spec_t;

void proc_spec_init(proc_spec_t *spec, const char *name);
int  proc_spec_add_burst(proc_spec_t *spec, burst_kind_t kind, uint16_t duration);
int  proc_spec_add_ref(proc_spec_t *spec, uint16_t vpage);
/* SIM_OK or SIM_ERR_INVALID; a human-readable reason goes to msg if given. */
int  proc_spec_validate(const proc_spec_t *spec, char *msg, size_t msglen);

/* Scenario editing */
int  sim_spawn_process(sim_t *s, const proc_spec_t *spec);          /* returns pid */
int  sim_remove_process(sim_t *s, sim_pid_t pid);                   /* NEW or TERMINATED only */
int  sim_update_process(sim_t *s, sim_pid_t pid, const proc_spec_t *spec); /* NEW only */
int  sim_get_process_spec(const sim_t *s, sim_pid_t pid, proc_spec_t *out);

/* Rewind to t=0 keeping the scenario (processes, config, policies, observer). */
void sim_reset(sim_t *s);

/*
 * Policy / config changes.
 * sim_set_sched and sim_set_repl apply immediately, even mid-run: ready
 * processes are migrated into the new scheduler's queues. quantum <= 0 keeps
 * the current quantum. sim_set_frames always resets the run.
 */
int  sim_set_sched(sim_t *s, const char *name, int quantum);
int  sim_set_repl(sim_t *s, const char *name);
int  sim_set_frames(sim_t *s, int nframes);
/* Core count changes always reset (running processes are bound to cores). */
int  sim_set_cores(sim_t *s, int ncores);
/* Applies live: only faults raised after the call use the new latency. */
int  sim_set_disk_latency(sim_t *s, int ticks);

/* Execution. Both return true while the simulation is not finished. */
bool sim_run_until(sim_t *s, tick_t t);
/* Advance until at least one notable event (anything but PAGE_ACCESS) is
 * emitted, or the run finishes, or PRISM_STEP_EVENT_MAX_TICKS pass. */
#define PRISM_STEP_EVENT_MAX_TICKS 100000
bool sim_step_event(sim_t *s);
bool sim_event_is_notable(event_type_t type);

/* Observer: one per sim; pass NULL to detach. */
void sim_set_observer(sim_t *s, sim_observer_fn fn, void *user);

#endif /* PRISM_SIM_API_H */
