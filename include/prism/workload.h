#ifndef PRISM_WORKLOAD_H
#define PRISM_WORKLOAD_H

#include "prism/sim.h"

/*
 * Workload format:
 * # Comments start with '#'
 * cores 1
 * frames 4
 * disk_latency 5
 * sched rr
 * repl lru
 * quantum 2
 * seed 42
 *
 * proc P1 arrival=0 bursts=C10,I5,C15 pages=0,1,2,3,0,1,4
 * proc P2 arrival=2 bursts=C5,I10,C10 pages=loop:0:4:5
 * proc P3 arrival=4 bursts=C20 pages=random:0:7:20
 */

typedef struct {
    int             cores;
    int             frames;
    int             disk_latency;
    sched_policy_t  sched;
    repl_policy_t   repl;
    uint16_t        quantum;
    uint64_t        seed;
    bool            has_cores;
    bool            has_frames;
    bool            has_disk_latency;
    bool            has_sched;
    bool            has_repl;
    bool            has_quantum;
    bool            has_seed;
} workload_config_t;

/* Parse a workload file into a simulation instance */
bool workload_load_file(const char *filepath, sim_t *sim, workload_config_t *cfg);

/* Generate synthetic workloads */
bool workload_generate_mixed(sim_t *sim, int num_procs, uint64_t seed);
bool workload_generate_belady(sim_t *sim);
bool workload_generate_thrashing(sim_t *sim, int num_procs);

#endif /* PRISM_WORKLOAD_H */
