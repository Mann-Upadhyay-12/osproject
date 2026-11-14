#ifndef PRISM_METRICS_H
#define PRISM_METRICS_H

/*
 * Run metrics and algorithm comparison helpers.
 *
 * sim_metrics_compute() is the one definition of "average waiting time",
 * "turnaround", etc. used by every frontend, so the GUI's comparison table,
 * its live metrics panel and the tests all agree on the numbers.
 *
 * sim_prepare_variant() builds an independent, rewound copy of a scenario
 * with different policies, for side-by-side comparison. The copy has no
 * observer, so running it never disturbs the base simulation or its UI.
 */

#include "prism/sim.h"

typedef struct {
    int      processes;        /* non-removed processes */
    int      completed;        /* processes that terminated */
    double   avg_wait;         /* ticks spent READY, averaged over completed */
    double   avg_turnaround;   /* finish - arrival */
    double   avg_response;     /* first dispatch - arrival */
    uint32_t max_wait;         /* longest READY time of any process: starvation indicator */
    double   cpu_util;         /* busy core-ticks / (ticks * cores), percent */
    double   throughput;       /* completed processes per 100 ticks */
    tick_t   makespan;         /* ticks elapsed (finish time of the last process when done) */
    uint32_t ctx_switches;     /* dispatches */
    uint32_t page_accesses;
    uint32_t page_faults;
    double   fault_rate;       /* percent of page references that faulted */
    bool     finished;         /* every process completed */
} sim_metrics_t;

void sim_metrics_compute(const sim_t *s, sim_metrics_t *m);

/* "Keep the base scenario's setting" for sim_prepare_variant arguments. */
#define SIM_VARIANT_KEEP (-1)

/*
 * Copy base into dst, detach the observer, rewind to tick 0 and apply the
 * given policies / quantum / frame count (SIM_VARIANT_KEEP keeps the base's).
 * base may be mid-run; it is not modified. Returns SIM_OK or a SIM_ERR_* code.
 */
int sim_prepare_variant(sim_t *dst, const sim_t *base, int sched, int repl, int quantum, int frames);

/* Run until done or until max_ticks have elapsed; true if every process finished. */
bool sim_run_bounded(sim_t *s, tick_t max_ticks);

#endif /* PRISM_METRICS_H */
