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

static bool is_valid_layout_key(const char *key) {
    return key && *key && strlen(key) < PRISM_MAX_NAME_LEN && !strpbrk(key, " \t\r\n");
}

int layout_set(layout_t *L, const char *key, float x, float y) {
    if (!is_valid_layout_key(key)) return SIM_ERR_INVALID;

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

static void write_bursts(FILE *fp, const sim_t *s, const pcb_t *p) {
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
            case BURST_SEM_WAIT:   fprintf(fp, "wait:%s", s->semaphores[b->target_id].name); break;
            case BURST_SEM_SIGNAL: fprintf(fp, "signal:%s", s->semaphores[b->target_id].name); break;
            case BURST_FORK:       fputs("fork", fp); break;
            case BURST_WAIT_CHILD: fputs("wait", fp); break;
        }
    }
}

static void write_settings(FILE *fp, const sim_t *s) {
    int frames = s->configured_frames > 0 ? s->configured_frames : s->total_frames;

    fprintf(fp, "# PRISM scenario (load with prism-cli --workload, prism-tui, or prism-gui)\n");
    fprintf(fp, "cores %d\n", s->num_cores);
    fprintf(fp, "frames %d\n", frames);
    fprintf(fp, "disk_latency %d\n", s->disk_latency);
    fprintf(fp, "sched %s\n", sched_get_ops(s->sched_policy)->key);
    fprintf(fp, "repl %s\n", repl_get_ops(s->repl_policy)->key);
    fprintf(fp, "quantum %u\n", s->default_quantum);
    fprintf(fp, "seed %" PRIu64 "\n", s->seed);
    for (int r = 0; r < s->num_resources; r++) {
        fprintf(fp, "resource %s %d\n", s->resources[r].name, s->resources[r].total_instances);
    }
    for (int i = 0; i < s->num_semaphores; i++) {
        fprintf(fp, "semaphore %s %d\n", s->semaphores[i].name, s->semaphores[i].initial_value);
    }
}

static void write_page_refs(FILE *fp, const pcb_t *p) {
    for (int r = 0; r < p->num_page_refs; r++) {
        fprintf(fp, r ? ",%u" : "%u", p->page_refs[r]);
    }
}

static void write_affinity(FILE *fp, uint8_t mask) {
    fputs(" affinity=", fp);
    bool first = true;
    for (int c = 0; c < PRISM_MAX_CORES; c++) {
        if (!(mask & (1u << c))) continue;
        fprintf(fp, first ? "%d" : ",%d", c);
        first = false;
    }
}

static void write_process(FILE *fp, const sim_t *s, const pcb_t *p) {
    fprintf(fp, "proc %s arrival=%" PRIu64, p->name, p->arrival_tick);
    if (p->base_priority) fprintf(fp, " priority=%u", p->base_priority);
    if (p->period) fprintf(fp, " period=%u jobs=%u", p->period, p->num_jobs);
    if (p->period && p->relative_deadline != p->period) fprintf(fp, " deadline=%u", p->relative_deadline);
    if (p->affinity_mask) write_affinity(fp, p->affinity_mask);
    if (p->parent_pid >= 0 && s->procs[p->parent_pid].state != PROC_UNUSED) fprintf(fp, " parent=%s", s->procs[p->parent_pid].name);

    fputs(" bursts=", fp);
    write_bursts(fp, s, p);

    if (p->num_page_refs) {
        fputs(" pages=", fp);
        write_page_refs(fp, p);
    }
    fputc('\n', fp);
}

static void write_layout(FILE *fp, const layout_t *L) {
    if (!L || L->count == 0) return;

    fputc('\n', fp);
    for (int i = 0; i < L->count; i++) {
        fprintf(fp, "layout node=%s x=%g y=%g\n", L->nodes[i].key, L->nodes[i].x, L->nodes[i].y);
    }
}

int scenario_save(const sim_t *s, const layout_t *L, const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return SIM_ERR_IO;

    write_settings(fp, s);
    fputc('\n', fp);

    for (int i = 0; i < s->num_processes; i++) {
        if (s->procs[i].state != PROC_UNUSED) write_process(fp, s, &s->procs[i]);
    }
    write_layout(fp, L);

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
        bool parsed = sscanf(line + 6, " node=%31s x=%f y=%f", key, &x, &y) == 3;
        if (!parsed || layout_set(L, key, x, y) != SIM_OK) {
            fprintf(stderr, "%s:%d: bad layout line (expected: layout node=KEY x=N y=N)\n", path, lineno);
        }
    }
}

int scenario_load(sim_t *s, layout_t *L, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return SIM_ERR_IO;

    sim_observer_fn observer = s->observer;
    void *observer_user = s->observer_user;
    sim_init(s, 1, 4, 3, SCHED_RR, REPL_LRU, 42);
    if (!workload_load_file(path, s, NULL)) {
        fclose(fp);
        return SIM_ERR_IO;
    }
    sim_set_policy(s, s->sched_policy, s->repl_policy);
    sim_set_observer(s, observer, observer_user);

    if (L) {
        layout_init(L);
        load_layout(fp, L, path);
    }
    fclose(fp);
    return SIM_OK;
}
