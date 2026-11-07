#ifndef PRISM_WORKLOAD_H
#define PRISM_WORKLOAD_H

#include "prism/sim.h"
#include "prism/sim_api.h"

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
 * proc P3 arrival=4 bursts=C20 pages=random:0:7:20 priority=2
 *
 * bursts: C<n> cpu, I<n> io, A<r>/R<r> acquire/release resource r,
 *         S<pid> send, W<pid> wait for message, K checkpoint.
 * pages:  explicit list | loop:min:max:times | seq:min:max | random:min:max:count
 * Lines with unknown keywords (e.g. "layout ...", see scenario.h) are ignored.
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

/* Parse a bursts= / pages= value into a spec (appending). Return false if any
 * token was malformed or out of range; valid tokens are still appended. */
bool workload_parse_bursts(proc_spec_t *spec, const char *bursts);
bool workload_parse_pages(proc_spec_t *spec, const char *pages, uint64_t seed, int pid);

/* Generate synthetic workloads */
bool workload_generate_mixed(sim_t *sim, int num_procs, uint64_t seed);
bool workload_generate_belady(sim_t *sim);
bool workload_generate_thrashing(sim_t *sim, int num_procs);

#endif /* PRISM_WORKLOAD_H */
