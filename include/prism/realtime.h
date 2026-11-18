#ifndef PRISM_REALTIME_H
#define PRISM_REALTIME_H

#include "prism/sim.h"

#define RT_HYPERPERIOD_LIMIT 1000000ull

typedef struct {
    int      tasks;
    double   utilization;
    double   density;
    double   rm_bound;
    bool     rm_guaranteed;
    bool     edf_feasible;
    uint64_t hyperperiod;
} rt_analysis_t;

uint32_t rt_job_demand(const pcb_t *p);
uint64_t rt_priority_key(const sim_t *s, const pcb_t *p);
void     rt_analyze(const sim_t *s, rt_analysis_t *out);

#endif
