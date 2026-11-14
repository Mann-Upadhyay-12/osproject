#include "compare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Compare runner: executes every (test case x algorithm) pair headlessly on
 * scratch sim_t copies and keeps the metrics and a per-core Gantt trace of
 * each run. Results are recomputed only when something changes (dirty flag).
 */

/* ------------------------------------------------------------------------
 * Metric table
 * ------------------------------------------------------------------------ */

static double m_wait(const sim_metrics_t *m) { return m->avg_wait; }
static double m_tat(const sim_metrics_t *m) { return m->avg_turnaround; }
static double m_resp(const sim_metrics_t *m) { return m->avg_response; }
static double m_maxw(const sim_metrics_t *m) { return m->max_wait; }
static double m_ctx(const sim_metrics_t *m) { return m->ctx_switches; }
static double m_util(const sim_metrics_t *m) { return m->cpu_util; }
static double m_thru(const sim_metrics_t *m) { return m->throughput; }
static double m_span(const sim_metrics_t *m) { return (double)m->makespan; }
static double m_faults(const sim_metrics_t *m) { return m->page_faults; }
static double m_frate(const sim_metrics_t *m) { return m->fault_rate; }

static const cmp_metric_t g_sched_metrics[] = {
    {"Avg waiting", "Average ticks a process spent READY (in the queue, not running).", true, "%.2f", m_wait},
    {"Avg turnaround", "Average finish - arrival: how long a job takes end to end.", true, "%.2f", m_tat},
    {"Avg response", "Average first run - arrival: how fast a job gets its first CPU time.", true, "%.2f", m_resp},
    {"Max waiting", "Longest READY time of any one process. High = someone is starving.", true, "%.0f", m_maxw},
    {"Context switches", "Dispatches. Each one costs real time in a real OS.", true, "%.0f", m_ctx},
    {"CPU utilisation", "Percent of core-ticks spent running a process.", false, "%.1f%%", m_util},
    {"Throughput", "Processes completed per 100 ticks.", false, "%.2f", m_thru},
    {"Makespan", "Tick at which the last process finished.", true, "%.0f", m_span},
};

static const cmp_metric_t g_vm_metrics[] = {
    {"Page faults", "References that found their page missing from RAM.", true, "%.0f", m_faults},
    {"Fault rate", "Percent of page references that faulted.", true, "%.1f%%", m_frate},
    {"Makespan", "Tick at which the last process finished (faults cost disk_latency ticks each).", true, "%.0f", m_span},
    {"CPU utilisation", "Percent of core-ticks spent running. Drops when processes wait for the disk.", false, "%.1f%%", m_util},
    {"Avg turnaround", "Average finish - arrival.", true, "%.2f", m_tat},
};

const cmp_metric_t *cmp_metrics(int mode, int *n) {
    if (mode == CMP_VM) { *n = (int)(sizeof(g_vm_metrics) / sizeof(g_vm_metrics[0])); return g_vm_metrics; }
    *n = (int)(sizeof(g_sched_metrics) / sizeof(g_sched_metrics[0]));
    return g_sched_metrics;
}

/* ------------------------------------------------------------------------
 * Algorithms
 * ------------------------------------------------------------------------ */

int cmp_nalgs(const compare_t *c) { return c->mode == CMP_VM ? REPL_COUNT : SCHED_COUNT; }

bool cmp_alg_on(const compare_t *c, int a) {
    return c->mode == CMP_VM ? (a < REPL_COUNT && c->repl_on[a]) : (a < SCHED_COUNT && c->sched_on[a]);
}

const char *cmp_alg_name(const compare_t *c, int a) {
    return c->mode == CMP_VM ? repl_get_ops((repl_policy_t)a)->name : sched_get_ops((sched_policy_t)a)->name;
}

const algo_info_t *cmp_alg_info(const compare_t *c, int a) {
    return c->mode == CMP_VM ? explain_repl_info((repl_policy_t)a) : explain_sched_info((sched_policy_t)a);
}

Color cmp_alg_color(int a) {
    static const Color pal[CMP_MAX_ALGS] = {
        {110, 170, 255, 255}, {255, 160,  70, 255}, {110, 210, 130, 255}, {235,  95, 110, 255},
        {180, 130, 240, 255}, {230, 210,  90, 255}, { 90, 210, 220, 255}, {240, 130, 200, 255},
    };
    return pal[a % CMP_MAX_ALGS];
}

const char *cmp_case_name(int pid, void *user) {
    const cmp_case_t *k = (const cmp_case_t *)user;
    return (pid >= 0 && pid < PRISM_MAX_PROCESSES) ? k->names[pid] : "?";
}

bool cmp_case_usable(const compare_t *c, int i) {
    const cmp_case_t *k = &c->cases[i];
    return k->loaded && k->nproc > 0 && (c->mode != CMP_VM || k->has_pages);
}

/* ------------------------------------------------------------------------
 * Test cases
 * ------------------------------------------------------------------------ */

static void case_meta_from_sim(cmp_case_t *k, const sim_t *s) {
    k->nproc = 0;
    k->has_pages = false;
    memset(k->names, 0, sizeof(k->names));
    for (int pid = 0; pid < s->num_processes; pid++) {
        snprintf(k->names[pid], sizeof(k->names[pid]), "%s", s->procs[pid].name);
        if (s->procs[pid].state == PROC_UNUSED) continue;
        k->nproc++;
        if (s->procs[pid].num_page_refs > 0) k->has_pages = true;
    }
    k->cores = s->num_cores;
    k->frames = s->configured_frames;
    k->quantum = s->default_quantum;
    k->loaded = true;
}

/* The leading comment block of a workload file is its "what to look for". */
static void read_description(cmp_case_t *k) {
    k->desc[0] = '\0';
    FILE *f = fopen(k->path, "r");
    if (!f) return;
    char line[256];
    size_t off = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != '#') break;
        char *t = line + 1;
        while (*t == ' ') t++;
        t[strcspn(t, "\r\n")] = '\0';
        if (strncmp(t, "PRISM scenario", 14) == 0) continue;   /* header written by scenario_save */
        int w = snprintf(k->desc + off, sizeof(k->desc) - off, "%s%s", off ? " " : "", t);
        if (w < 0 || (size_t)w >= sizeof(k->desc) - off) break;
        off += (size_t)w;
    }
    fclose(f);
}

void compare_init(gui_t *g) {
    compare_t *c = calloc(1, sizeof(compare_t));
    if (!c) { fprintf(stderr, "prism-gui: out of memory\n"); exit(1); }
    c->work = malloc(sizeof(sim_t));
    c->base = malloc(sizeof(sim_t));
    if (!c->work || !c->base) { fprintf(stderr, "prism-gui: out of memory\n"); exit(1); }
    for (int i = 0; i < CMP_MAX_CASES; i++) {
        for (int a = 0; a < CMP_MAX_ALGS; a++) {
            c->runs[i][a].lanes = calloc((size_t)CMP_LANE_CAP * PRISM_MAX_CORES, 1);
            if (!c->runs[i][a].lanes) { fprintf(stderr, "prism-gui: out of memory\n"); exit(1); }
        }
    }
    for (int a = 0; a < SCHED_COUNT; a++) c->sched_on[a] = true;
    for (int a = 0; a < REPL_COUNT; a++) c->repl_on[a] = true;
    c->hover_alg = -1;
    c->tbl_alg = REPL_FIFO;

    cmp_case_t *cur = &c->cases[c->ncases++];
    snprintf(cur->label, sizeof(cur->label), "current scenario");
    cur->current = true;
    cur->on = true;
    snprintf(cur->desc, sizeof(cur->desc), "Whatever is loaded in the Simulate view right now, including your edits.");
    for (int i = 0; i < g->npresets && c->ncases < CMP_MAX_CASES; i++) {
        cmp_case_t *k = &c->cases[c->ncases];
        snprintf(k->path, sizeof(k->path), "%s", g->presets[i]);
        snprintf(k->label, sizeof(k->label), "%s", g->preset_items[i + 1]);
        if (scenario_load(c->base, NULL, k->path) != SIM_OK) continue;
        case_meta_from_sim(k, c->base);
        read_description(k);
        k->on = true;
        c->ncases++;
    }
    c->dirty = true;
    g->cmp = c;
}

void compare_free(gui_t *g) {
    compare_t *c = g->cmp;
    if (!c) return;
    for (int i = 0; i < CMP_MAX_CASES; i++) {
        for (int a = 0; a < CMP_MAX_ALGS; a++) free(c->runs[i][a].lanes);
    }
    free(c->work);
    free(c->base);
    free(c);
    g->cmp = NULL;
}

void compare_mark_dirty(gui_t *g) {
    if (g->cmp) g->cmp->dirty = true;
}

void compare_setup_args(gui_t *g, const char *mode) {
    compare_t *c = g->cmp;
    c->mode = (strcmp(mode, "vm") == 0 || strcmp(mode, "memory") == 0) ? CMP_VM : CMP_SCHED;
    g->view = VIEW_COMPARE;
    /* Focus the first case that has something to show in this mode. */
    for (int i = 0; i < c->ncases; i++) {
        if (cmp_case_usable(c, i) && !c->cases[i].current) { c->focus = i; break; }
    }
    if (g->preset_sel > 0) {
        for (int i = 0; i < c->ncases; i++) {
            if (strcmp(c->cases[i].label, g->preset_items[g->preset_sel]) == 0 && cmp_case_usable(c, i)) c->focus = i;
        }
    }
    c->dirty = true;
}

/* ------------------------------------------------------------------------
 * Running
 * ------------------------------------------------------------------------ */

/* Load test case i into c->base (or point at the live scenario). */
static const sim_t *case_base(gui_t *g, int i) {
    compare_t *c = g->cmp;
    cmp_case_t *k = &c->cases[i];
    if (k->current) {
        case_meta_from_sim(k, g->sim);
        return g->sim;
    }
    if (scenario_load(c->base, NULL, k->path) != SIM_OK) { k->loaded = false; return NULL; }
    sim_set_observer(c->base, NULL, NULL);
    return c->base;
}

static int prepare(compare_t *c, const sim_t *base, int alg, int frames_override) {
    int sched = c->mode == CMP_SCHED ? alg : SIM_VARIANT_KEEP;
    int repl = c->mode == CMP_VM ? alg : SIM_VARIANT_KEEP;
    int q = c->mode == CMP_SCHED && c->quantum > 0 ? c->quantum : SIM_VARIANT_KEEP;
    int f = frames_override > 0 ? frames_override : SIM_VARIANT_KEEP;
    return sim_prepare_variant(c->work, base, sched, repl, q, f);
}

static void run_one(compare_t *c, const sim_t *base, cmp_run_t *r, int alg) {
    r->valid = true;
    r->len = 0;
    r->rc = prepare(c, base, alg, c->mode == CMP_VM ? c->frames : 0);
    if (r->rc != SIM_OK) return;
    sim_t *w = c->work;
    r->ncores = w->num_cores;
    while (w->tick < CMP_MAX_TICKS && sim_step(w)) {
        if (r->len < CMP_LANE_CAP) {
            uint32_t gi = (w->gantt_count + PRISM_GANTT_HISTORY - 1) % PRISM_GANTT_HISTORY;
            for (int k = 0; k < w->num_cores; k++) r->lanes[r->len * PRISM_MAX_CORES + k] = (int8_t)w->gantt_history[gi][k];
            r->len++;
        }
    }
    /* sim_step returns false on the step that finishes the run: record it too. */
    if (sim_is_done(w) && r->len < CMP_LANE_CAP && r->len < w->tick) {
        uint32_t gi = (w->gantt_count + PRISM_GANTT_HISTORY - 1) % PRISM_GANTT_HISTORY;
        for (int k = 0; k < w->num_cores; k++) r->lanes[r->len * PRISM_MAX_CORES + k] = (int8_t)w->gantt_history[gi][k];
        r->len++;
    }
    sim_metrics_compute(w, &r->m);
}

/* Faults for each replacement policy over a range of frame counts. */
static void run_sweep(compare_t *c, const sim_t *base, const cmp_case_t *k) {
    int distinct = 0;
    for (int pid = 0; pid < base->num_processes; pid++) {
        bool seen[PRISM_MAX_VIRTUAL_PAGES] = {false};
        const pcb_t *p = &base->procs[pid];
        if (p->state == PROC_UNUSED) continue;
        for (int i = 0; i < p->num_page_refs; i++) {
            if (!seen[p->page_refs[i]]) { seen[p->page_refs[i]] = true; distinct++; }
        }
    }
    c->sweep_cur = c->frames > 0 ? c->frames : k->frames;
    c->sweep_lo = 1;
    c->sweep_hi = distinct + 1;
    if (c->sweep_hi < c->sweep_cur + 2) c->sweep_hi = c->sweep_cur + 2;
    if (c->sweep_hi > CMP_SWEEP_MAX) c->sweep_hi = CMP_SWEEP_MAX;
    for (int a = 0; a < REPL_COUNT; a++) {
        for (int f = c->sweep_lo; f <= c->sweep_hi; f++) {
            c->sweep[a][f] = 0;
            if (prepare(c, base, a, f) != SIM_OK) continue;
            sim_run_bounded(c->work, CMP_MAX_TICKS);
            c->sweep[a][f] = c->work->total_page_faults;
        }
    }
    c->sweep_ok = true;
}

typedef struct {
    compare_t *c;
    const sim_t *s;
    bool pend_fault[PRISM_MAX_PROCESSES];
    int  pend_loaded[PRISM_MAX_PROCESSES];
} recorder_t;

static void record_ref(const sim_event_t *ev, void *user) {
    recorder_t *R = (recorder_t *)user;
    compare_t *c = R->c;
    const sim_t *s = R->s;
    int pid = ev->pid;
    if (pid < 0 || pid >= PRISM_MAX_PROCESSES) return;
    if (ev->type == EVENT_PAGE_FAULT) {
        R->pend_fault[pid] = true;
    } else if (ev->type == EVENT_PROC_WAKE_FAULT && ev->detail1 >= 0 && ev->detail1 < PRISM_MAX_VIRTUAL_PAGES) {
        R->pend_loaded[pid] = s->procs[pid].pt[ev->detail1].frame_idx;
    } else if (ev->type == EVENT_PAGE_ACCESS) {
        if (c->ncol >= CMP_TBL_COLS) { c->tbl_trunc = true; return; }
        cmp_col_t *col = &c->col[c->ncol++];
        col->page = (int16_t)ev->detail1;
        col->pid = (int8_t)pid;
        col->fault = R->pend_fault[pid];
        col->loaded = (int8_t)(R->pend_fault[pid] ? R->pend_loaded[pid] : -1);
        col->hit_frame = (int8_t)ev->detail2;
        for (int f = 0; f < CMP_TBL_FRAMES; f++) {
            bool occ = f < s->total_frames && s->frames[f].occupied;
            col->fpage[f] = occ ? (int16_t)s->frames[f].vpage : -1;
            col->fpid[f] = occ ? (int8_t)s->frames[f].owner_pid : -1;
        }
        R->pend_fault[pid] = false;
        R->pend_loaded[pid] = -1;
    }
}

/* Textbook frame table: frame contents after every reference. */
static void run_table(compare_t *c, const sim_t *base) {
    c->ncol = 0;
    c->tbl_trunc = false;
    c->tbl_ok = false;
    if (prepare(c, base, c->tbl_alg, c->frames) != SIM_OK) return;
    c->tbl_frames = c->work->total_frames;
    if (c->tbl_frames > CMP_TBL_FRAMES) return;
    recorder_t *R = calloc(1, sizeof(recorder_t));
    if (!R) { fprintf(stderr, "prism-gui: out of memory\n"); exit(1); }
    R->c = c;
    R->s = c->work;
    for (int i = 0; i < PRISM_MAX_PROCESSES; i++) R->pend_loaded[i] = -1;
    sim_set_observer(c->work, record_ref, R);
    sim_run_bounded(c->work, CMP_MAX_TICKS);
    sim_set_observer(c->work, NULL, NULL);
    free(R);
    c->tbl_ok = true;
}

void cmp_run_all(gui_t *g) {
    compare_t *c = g->cmp;
    double t0 = GetTime();
    for (int i = 0; i < c->ncases; i++) {
        for (int a = 0; a < CMP_MAX_ALGS; a++) c->runs[i][a].valid = false;
    }
    c->sweep_ok = false;
    c->tbl_ok = false;
    case_meta_from_sim(&c->cases[0], g->sim);   /* the live scenario may have been edited */
    for (int i = 0; i < c->ncases; i++) {
        /* The focused case is always run: the detail panes show it even if unticked. */
        if (!c->cases[i].on && i != c->focus) continue;
        const sim_t *base = case_base(g, i);
        if (!base || !cmp_case_usable(c, i)) continue;
        for (int a = 0; a < cmp_nalgs(c); a++) {
            if (cmp_alg_on(c, a)) run_one(c, base, &c->runs[i][a], a);
        }
        if (i == c->focus && c->mode == CMP_VM) {
            run_sweep(c, base, &c->cases[i]);
            run_table(c, base);
        }
    }
    c->run_ms = (GetTime() - t0) * 1000.0;
    c->dirty = false;
}

/* Load a test case into the Simulate view with one algorithm selected. */
void cmp_open_in_sim(gui_t *g, int case_idx, int alg) {
    compare_t *c = g->cmp;
    cmp_case_t *k = &c->cases[case_idx];
    if (!k->current) gui_load(g, k->path);
    sim_t *s = g->sim;
    if (c->mode == CMP_SCHED) {
        sim_set_sched(s, sched_get_ops((sched_policy_t)alg)->key, c->quantum > 0 ? c->quantum : 0);
    } else {
        sim_set_repl(s, repl_get_ops((repl_policy_t)alg)->key);
        if (c->frames > 0) sim_set_frames(s, c->frames);
    }
    gui_enter_edit(g);
    g->view = VIEW_SIM;
    g->sel = (node_ref_t){c->mode == CMP_SCHED ? NODE_SCHED : NODE_MEMORY, 0};
    g->ltab = LTAB_EXPLAIN;
    gui_status(g, false, "Opened %s with %s: press Step (N) to follow each decision", k->label, cmp_alg_name(c, alg));
}
