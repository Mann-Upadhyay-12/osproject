#ifndef PRISM_SIM_API_H
#define PRISM_SIM_API_H

#include "prism/sim.h"

enum {
    SIM_OK          =  0,
    SIM_ERR_FULL    = -1,
    SIM_ERR_INVALID = -2,
    SIM_ERR_STATE   = -3,
    SIM_ERR_NO_PID  = -4,
    SIM_ERR_NAME    = -5,
    SIM_ERR_IO      = -6
};

const char *sim_strerror(int err);

typedef struct {
    char      name[PRISM_MAX_NAME_LEN];
    tick_t    arrival;
    uint16_t  priority;
    uint16_t  num_bursts;
    burst_t   bursts[PRISM_MAX_BURSTS];
    uint16_t  num_page_refs;
    uint16_t  page_refs[PRISM_MAX_PAGE_REFS];
} proc_spec_t;

void proc_spec_init(proc_spec_t *spec, const char *name);
int  proc_spec_add_burst(proc_spec_t *spec, burst_kind_t kind, uint16_t duration);
int  proc_spec_add_ref(proc_spec_t *spec, uint16_t vpage);
int  proc_spec_validate(const proc_spec_t *spec, char *msg, size_t msglen);

int  sim_spawn_process(sim_t *s, const proc_spec_t *spec);
int  sim_remove_process(sim_t *s, sim_pid_t pid);
int  sim_update_process(sim_t *s, sim_pid_t pid, const proc_spec_t *spec);
int  sim_get_process_spec(const sim_t *s, sim_pid_t pid, proc_spec_t *out);

void sim_reset(sim_t *s);

int  sim_set_sched(sim_t *s, const char *name, int quantum);
int  sim_set_repl(sim_t *s, const char *name);
int  sim_set_frames(sim_t *s, int nframes);
int  sim_set_cores(sim_t *s, int ncores);
int  sim_set_disk_latency(sim_t *s, int ticks);

bool sim_run_until(sim_t *s, tick_t t);
#define PRISM_STEP_EVENT_MAX_TICKS 100000
bool sim_step_event(sim_t *s);
bool sim_event_is_notable(event_type_t type);

void sim_set_observer(sim_t *s, sim_observer_fn fn, void *user);

#endif
