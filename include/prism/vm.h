#ifndef PRISM_VM_H
#define PRISM_VM_H

#include "prism/types.h"

typedef struct {
    bool       occupied;
    bool       ref_bit;             /* For Clock algorithm */
    bool       dirty_bit;
    sim_pid_t  owner_pid;           /* -1 if empty */
    uint16_t   vpage;
    tick_t     load_tick;           /* For FIFO */
    tick_t     last_access_tick;    /* For LRU */
} frame_entry_t;

struct sim;

typedef struct repl_ops {
    repl_policy_t policy;
    const char   *name;
    void (*init)(struct sim *s);
    void (*on_load)(struct sim *s, sim_pid_t pid, uint16_t vpage, int frame_idx);
    void (*on_access)(struct sim *s, sim_pid_t pid, uint16_t vpage, int frame_idx);
    int  (*pick_victim)(struct sim *s);
} repl_ops_t;

const repl_ops_t *repl_get_ops(repl_policy_t policy);

#endif /* PRISM_VM_H */
