#include "prism/scenario.h"
#include "prism/sim_api.h"
#include "prism/workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

void layout_init(layout_t *L) {
    memset(L, 0, sizeof(*L));
}

static int layout_find(const layout_t *L, const char *key) {
    for (int i = 0; i < L->count; i++) {
        if (strcmp(L->nodes[i].key, key) == 0) return i;
    }
    return -1;
}

int layout_set(layout_t *L, const char *key, float x, float y) {
    if (!key || !*key || strlen(key) >= PRISM_MAX_NAME_LEN || strpbrk(key, " \t\r\n")) {
        return SIM_ERR_INVALID;
    }
    int i = layout_find(L, key);
    if (i < 0) {
        if (L->count >= PRISM_LAYOUT_MAX_NODES) return SIM_ERR_FULL;
        i = L->count++;
        snprintf(L->nodes[i].key, sizeof(L->nodes[i].key), "%s", key);
    }
    L->nodes[i].x = x;
    L->nodes[i].y = y;
    return SIM_OK;
}

bool layout_get(const layout_t *L, const char *key, float *x, float *y) {
    int i = layout_find(L, key);
    if (i < 0) return false;
    if (x) *x = L->nodes[i].x;
    if (y) *y = L->nodes[i].y;
    return true;
}

bool layout_remove(layout_t *L, const char *key) {
    int i = layout_find(L, key);
    if (i < 0) return false;
    L->nodes[i] = L->nodes[--L->count];
    return true;
}

static void write_bursts(FILE *fp, const pcb_t *p) {
    for (int i = 0; i < p->num_bursts; i++) {
        const burst_t *b = &p->bursts[i];
        if (i) fputc(',', fp);
        switch ((burst_kind_t)b->kind) {
            case BURST_CPU:        fprintf(fp, "C%u", b->duration); break;
            case BURST_IO:         fprintf(fp, "I%u", b->duration); break;
            case BURST_ACQUIRE:    fprintf(fp, "A%d", b->target_id); break;
            case BURST_RELEASE:    fprintf(fp, "R%d", b->target_id); break;
            case BURST_IPC_SEND:   fprintf(fp, "S%d", b->target_id); break;
            case BURST_IPC_RECV:   fprintf(fp, "W%d", b->target_id); break;
            case BURST_CHECKPOINT: fputc('K', fp); break;
        }
    }
}

int scenario_save(const sim_t *s, const layout_t *L, const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return SIM_ERR_IO;

    fprintf(fp, "# PRISM scenario (load with prism-cli --workload, prism-tui, or prism-gui)\n");
    fprintf(fp, "cores %d\n", s->num_cores);
    fprintf(fp, "frames %d\n", s->configured_frames > 0 ? s->configured_frames : s->total_frames);
    fprintf(fp, "disk_latency %d\n", s->disk_latency);
    fprintf(fp, "sched %s\n", sched_get_ops(s->sched_policy)->key);
    fprintf(fp, "repl %s\n", repl_get_ops(s->repl_policy)->key);
    fprintf(fp, "quantum %u\n", s->default_quantum);
    fprintf(fp, "seed %" PRIu64 "\n", s->seed);
    for (int r = 0; r < s->num_resources; r++) {
        fprintf(fp, "resource %s %d\n", s->resources[r].name, s->resources[r].total_instances);
    }
    fputc('\n', fp);

    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        if (p->state == PROC_UNUSED) continue;
        fprintf(fp, "proc %s arrival=%" PRIu64, p->name, p->arrival_tick);
        if (p->base_priority) fprintf(fp, " priority=%u", p->base_priority);
        fputs(" bursts=", fp);
        write_bursts(fp, p);
        if (p->num_page_refs) {
            fputs(" pages=", fp);
            for (int r = 0; r < p->num_page_refs; r++) {
                fprintf(fp, r ? ",%u" : "%u", p->page_refs[r]);
            }
        }
        fputc('\n', fp);
    }

    if (L && L->count) {
        fputc('\n', fp);
        for (int i = 0; i < L->count; i++) {
            fprintf(fp, "layout node=%s x=%g y=%g\n", L->nodes[i].key, L->nodes[i].x, L->nodes[i].y);
        }
    }

    bool ok = !ferror(fp);
    if (fclose(fp) != 0) ok = false;
    return ok ? SIM_OK : SIM_ERR_IO;
}

static void load_layout(FILE *fp, layout_t *L, const char *path) {
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (strncmp(line, "layout", 6) != 0) continue;
        char key[PRISM_MAX_NAME_LEN] = "";
        float x = 0, y = 0;
        if (sscanf(line + 6, " node=%31s x=%f y=%f", key, &x, &y) != 3 || layout_set(L, key, x, y) != SIM_OK) {
            fprintf(stderr, "%s:%d: bad layout line (expected: layout node=KEY x=N y=N)\n", path, lineno);
        }
    }
}

int scenario_load(sim_t *s, layout_t *L, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return SIM_ERR_IO;

    sim_observer_fn fn = s->observer;
    void *user = s->observer_user;
    sim_init(s, 1, 4, 3, SCHED_RR, REPL_LRU, 42);
    if (!workload_load_file(path, s, NULL)) {
        fclose(fp);
        return SIM_ERR_IO;
    }
    /* The file set policies by field; run their init hooks like a fresh start. */
    sim_set_policy(s, s->sched_policy, s->repl_policy);
    sim_set_observer(s, fn, user);

    if (L) {
        layout_init(L);
        load_layout(fp, L, path);
    }
    fclose(fp);
    return SIM_OK;
}
