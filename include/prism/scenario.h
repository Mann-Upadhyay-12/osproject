#ifndef PRISM_SCENARIO_H
#define PRISM_SCENARIO_H

/*
 * Scenario files = the workload format (workload.h) plus optional layout lines
 * that position GUI nodes on the canvas:
 *
 *   layout node=cpu x=640 y=200
 *   layout node=P1  x=120 y=80
 *
 * workload_load_file ignores layout lines, so scenario files still run in
 * prism-cli / prism-tui, and old workload files load here with an empty layout.
 * Node keys are free-form strings owned by the frontend (the GUI uses fixed
 * keys for system nodes and process names for process nodes).
 */

#include "prism/sim.h"

#define PRISM_LAYOUT_MAX_NODES (PRISM_MAX_PROCESSES + 32)

typedef struct {
    char  key[PRISM_MAX_NAME_LEN];
    float x, y;
} layout_node_t;

typedef struct {
    int           count;
    layout_node_t nodes[PRISM_LAYOUT_MAX_NODES];
} layout_t;

void layout_init(layout_t *L);
int  layout_set(layout_t *L, const char *key, float x, float y);   /* SIM_OK / SIM_ERR_FULL / SIM_ERR_INVALID */
bool layout_get(const layout_t *L, const char *key, float *x, float *y);
bool layout_remove(layout_t *L, const char *key);

/*
 * Save the scenario (config, policies, resources, every non-removed process as
 * an explicit script) plus layout. Page references are written out literally,
 * so a generated (random:) string reloads bit-identically. Removed processes
 * are skipped, so pids of later processes shift down on reload.
 * L may be NULL. Returns SIM_OK or SIM_ERR_IO.
 */
int scenario_save(const sim_t *s, const layout_t *L, const char *path);

/*
 * Re-initialise s with prism-cli's defaults (1 core, 4 frames, latency 3, RR,
 * LRU, seed 42), load the file over them, and fill L (may be NULL).
 * The observer attached to s is kept. Returns SIM_OK or SIM_ERR_IO.
 */
int scenario_load(sim_t *s, layout_t *L, const char *path);

#endif /* PRISM_SCENARIO_H */
