#ifndef PRISM_SCHED_COMMON_H
#define PRISM_SCHED_COMMON_H

#include "prism/sched.h"
#include "prism/sim.h"

#define NO_PROCESS  (-1)
#define NO_CORE     (-1)
#define MAIN_QUEUE  0

typedef bool (*runs_before_fn)(const pcb_t *candidate, const pcb_t *best);

void      sched_preempt(sim_t *s, int core_id, sim_pid_t pid, int32_t detail1, int32_t detail2);
sim_pid_t sched_take_best_ready(sim_t *s, runs_before_fn runs_before);

void      rm_on_ready(sim_t *s, sim_pid_t pid);
sim_pid_t rm_pick_next(sim_t *s, int core_id);
void      rm_on_tick(sim_t *s, int core_id, sim_pid_t running_pid);
void      edf_on_ready(sim_t *s, sim_pid_t pid);
sim_pid_t edf_pick_next(sim_t *s, int core_id);
void      edf_on_tick(sim_t *s, int core_id, sim_pid_t running_pid);

#endif
