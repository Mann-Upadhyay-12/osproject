#ifndef PRISM_METRICS_H
#define PRISM_METRICS_H

#include "prism/sim.h"

typedef struct {
    int      processes;
    int      completed;
    double   avg_wait;
    double   avg_turnaround;
    double   avg_response;
    uint32_t max_wait;
    double   cpu_util;
    double   throughput;
    tick_t   makespan;
    uint32_t ctx_switches;
    uint32_t page_accesses;
    uint32_t page_faults;
    double   fault_rate;
    uint32_t deadline_misses;
    bool     finished;
} sim_metrics_t;

void sim_metrics_compute(const sim_t *s, sim_metrics_t *m);

#define SIM_VARIANT_KEEP (-1)

int sim_prepare_variant(sim_t *dst, const sim_t *base, int sched, int repl, int quantum, int frames);

bool sim_run_bounded(sim_t *s, tick_t max_ticks);

#endif
