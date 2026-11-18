#ifndef PRISM_SCENARIO_H
#define PRISM_SCENARIO_H

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
int  layout_set(layout_t *L, const char *key, float x, float y);
bool layout_get(const layout_t *L, const char *key, float *x, float *y);
bool layout_remove(layout_t *L, const char *key);

int scenario_save(const sim_t *s, const layout_t *L, const char *path);

int scenario_load(sim_t *s, layout_t *L, const char *path);

#endif
