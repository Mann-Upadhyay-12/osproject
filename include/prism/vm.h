#ifndef PRISM_VM_H
#define PRISM_VM_H

#include "prism/types.h"

typedef struct {
    bool       occupied;
    bool       ref_bit;
    bool       dirty_bit;
    sim_pid_t  owner_pid;
    uint16_t   vpage;
    tick_t     load_tick;
    tick_t     last_access_tick;
} frame_entry_t;

struct sim;

typedef struct repl_ops {
    repl_policy_t policy;
    const char   *name;
    const char   *key;
    void (*init)(struct sim *s);
    void (*on_load)(struct sim *s, sim_pid_t pid, uint16_t vpage, int frame_idx);
    void (*on_access)(struct sim *s, sim_pid_t pid, uint16_t vpage, int frame_idx);
    int  (*pick_victim)(struct sim *s);
} repl_ops_t;

const repl_ops_t *repl_get_ops(repl_policy_t policy);

bool repl_lookup(const char *name, repl_policy_t *out);

#endif
