#ifndef PRISM_SCHED_H
#define PRISM_SCHED_H

#include "prism/types.h"

struct sim;

typedef struct sched_ops {
    sched_policy_t policy;
    const char    *name;
    void      (*init)(struct sim *s);
    void      (*on_ready)(struct sim *s, sim_pid_t pid);
    sim_pid_t (*pick_next)(struct sim *s, int core_id);
    void      (*on_tick)(struct sim *s, int core_id, sim_pid_t running_pid);
    void      (*on_block)(struct sim *s, sim_pid_t pid);
} sched_ops_t;

const sched_ops_t *sched_get_ops(sched_policy_t policy);

#endif /* PRISM_SCHED_H */
