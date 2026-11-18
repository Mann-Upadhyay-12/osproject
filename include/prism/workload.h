#ifndef PRISM_WORKLOAD_H
#define PRISM_WORKLOAD_H

#include "prism/sim.h"
#include "prism/sim_api.h"

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

bool workload_load_file(const char *filepath, sim_t *sim, workload_config_t *cfg);

bool workload_parse_bursts(proc_spec_t *spec, const char *bursts);
bool workload_parse_pages(proc_spec_t *spec, const char *pages, uint64_t seed, int pid);

bool workload_generate_mixed(sim_t *sim, int num_procs, uint64_t seed);
bool workload_generate_belady(sim_t *sim);
bool workload_generate_thrashing(sim_t *sim, int num_procs);

#endif
