#include "compare.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

static double avg_wait(const sim_metrics_t *m)       { return m->avg_wait; }
static double avg_turnaround(const sim_metrics_t *m) { return m->avg_turnaround; }
static double avg_response(const sim_metrics_t *m)   { return m->avg_response; }
static double max_wait(const sim_metrics_t *m)       { return m->max_wait; }
static double ctx_switches(const sim_metrics_t *m)   { return m->ctx_switches; }
static double cpu_util(const sim_metrics_t *m)       { return m->cpu_util; }
static double throughput(const sim_metrics_t *m)     { return m->throughput; }
static double makespan(const sim_metrics_t *m)       { return (double)m->makespan; }
static double page_faults(const sim_metrics_t *m)    { return m->page_faults; }
static double fault_rate(const sim_metrics_t *m)     { return m->fault_rate; }
static double deadline_misses(const sim_metrics_t *m) { return m->deadline_misses; }
static double migrations(const sim_metrics_t *m)      { return m->migrations; }

static void *alloc_or_die(size_t size) {
    void *p = calloc(1, size);
    if (!p) {
        fprintf(stderr, "prism-gui: out of memory\n");
        exit(1);
    }
    return p;
}

static const cmp_metric_t g_sched_metrics[] = {
    {"Avg waiting", "Average ticks a process spent READY (in the queue, not running).", true, "%.2f", avg_wait},
    {"Avg turnaround", "Average finish - arrival: how long a job takes end to end.", true, "%.2f", avg_turnaround},
    {"Avg response", "Average first run - arrival: how fast a job gets its first CPU time.", true, "%.2f", avg_response},
    {"Max waiting", "Longest READY time of any one process. High = someone is starving.", true, "%.0f", max_wait},
    {"Context switches", "Dispatches. Each one costs real time in a real OS.", true, "%.0f", ctx_switches},
    {"CPU utilisation", "Percent of core-ticks spent running a process.", false, "%.1f%%", cpu_util},
    {"Throughput", "Processes completed per 100 ticks.", false, "%.2f", throughput},
    {"Makespan", "Tick at which the last process finished.", true, "%.0f", makespan},
    {"Deadline misses", "Jobs of periodic tasks that were not done by their deadline.", true, "%.0f", deadline_misses},
    {"Core changes", "Times a process ran on a different core than last time (its cache goes cold).", true, "%.0f", migrations},
};

static const cmp_metric_t g_vm_metrics[] = {
    {"Page faults", "References that found their page missing from RAM.", true, "%.0f", page_faults},
    {"Fault rate", "Percent of page references that faulted.", true, "%.1f%%", fault_rate},
    {"Makespan", "Tick at which the last process finished (faults cost disk_latency ticks each).", true, "%.0f", makespan},
    {"CPU utilisation", "Percent of core-ticks spent running. Drops when processes wait for the disk.", false, "%.1f%%", cpu_util},
    {"Avg turnaround", "Average finish - arrival.", true, "%.2f", avg_turnaround},
};

const cmp_metric_t *cmp_metrics(int mode, int *count) {
    if (mode == CMP_VM) {
        *count = ARRAY_LEN(g_vm_metrics);
        return g_vm_metrics;
    }
    *count = ARRAY_LEN(g_sched_metrics);
    return g_sched_metrics;
}

int cmp_num_algs(const compare_t *c) {
    return c->mode == CMP_VM ? REPL_COUNT : SCHED_COUNT;
}

bool cmp_alg_on(const compare_t *c, int a) {
    if (c->mode == CMP_VM) return a < REPL_COUNT && c->repl_on[a];
    return a < SCHED_COUNT && c->sched_on[a];
}

const char *cmp_alg_name(const compare_t *c, int a) {
    return c->mode == CMP_VM ? repl_get_ops((repl_policy_t)a)->name : sched_get_ops((sched_policy_t)a)->name;
}

const algo_info_t *cmp_alg_info(const compare_t *c, int a) {
    return c->mode == CMP_VM ? explain_repl_info((repl_policy_t)a) : explain_sched_info((sched_policy_t)a);
}

Color cmp_alg_color(int a) {
    static const Color palette[CMP_MAX_ALGS] = {
        {110, 170, 255, 255}, {255, 160,  70, 255}, {110, 210, 130, 255}, {235,  95, 110, 255},
        {180, 130, 240, 255}, {230, 210,  90, 255}, { 90, 210, 220, 255}, {240, 130, 200, 255},
        {170, 200,  90, 255}, {200, 160, 120, 255}, {120, 140, 230, 255},
    };
    return palette[a % CMP_MAX_ALGS];
}

const char *cmp_case_name(int pid, void *user) {
    const cmp_case_t *k = (const cmp_case_t *)user;
    return pid >= 0 && pid < PRISM_MAX_PROCESSES ? k->names[pid] : "?";
}

bool cmp_run_ok(const compare_t *c, int case_idx, int alg) {
    const cmp_run_t *r = &c->runs[case_idx][alg];
    return cmp_alg_on(c, alg) && r->valid && r->status == SIM_OK;
}

bool cmp_case_usable(const compare_t *c, int i) {
    const cmp_case_t *k = &c->cases[i];
    return k->loaded && k->num_procs > 0 && (c->mode != CMP_VM || k->has_pages);
}

static void case_meta_from_sim(cmp_case_t *k, const sim_t *s) {
    k->num_procs = 0;
    k->has_pages = false;
    memset(k->names, 0, sizeof(k->names));
    for (int pid = 0; pid < s->num_processes; pid++) {
        snprintf(k->names[pid], sizeof(k->names[pid]), "%s", s->procs[pid].name);
        if (s->procs[pid].state == PROC_UNUSED) continue;
        k->num_procs++;
        if (s->procs[pid].num_page_refs > 0) k->has_pages = true;
    }
    k->cores = s->num_cores;
    k->frames = s->configured_frames;
    k->quantum = s->default_quantum;
    k->loaded = true;
}

static void read_description(cmp_case_t *k) {
    k->description[0] = '\0';
    FILE *f = fopen(k->path, "r");
    if (!f) return;

    char line[256];
    size_t used = 0, capacity = sizeof(k->description);
    while (fgets(line, sizeof(line), f) && line[0] == '#') {
        char *text = line + 1;
        while (*text == ' ') text++;
        text[strcspn(text, "\r\n")] = '\0';
        if (strncmp(text, "PRISM scenario", 14) == 0) continue;

        int written = snprintf(k->description + used, capacity - used, "%s%s", used ? " " : "", text);
        if (written < 0 || (size_t)written >= capacity - used) break;
        used += (size_t)written;
    }
    fclose(f);
}

static void add_current_scenario_case(compare_t *c) {
    cmp_case_t *k = &c->cases[c->num_cases++];
    snprintf(k->label, sizeof(k->label), "current scenario");
    snprintf(k->description, sizeof(k->description),
             "Whatever is loaded in the Simulate view right now, including your edits.");
    k->current = true;
    k->enabled = true;
}

static void add_preset_cases(compare_t *c, const gui_t *g) {
    for (int i = 0; i < g->num_presets && c->num_cases < CMP_MAX_CASES; i++) {
        cmp_case_t *k = &c->cases[c->num_cases];
        snprintf(k->path, sizeof(k->path), "%s", g->presets[i]);
        snprintf(k->label, sizeof(k->label), "%s", g->preset_items[i + 1]);
        if (scenario_load(c->base, NULL, k->path) != SIM_OK) continue;

        case_meta_from_sim(k, c->base);
        read_description(k);
        k->enabled = true;
        c->num_cases++;
    }
}

void compare_init(gui_t *g) {
    compare_t *c = alloc_or_die(sizeof(compare_t));
    c->work = alloc_or_die(sizeof(sim_t));
    c->base = alloc_or_die(sizeof(sim_t));
    for (int i = 0; i < CMP_MAX_CASES; i++) {
        for (int a = 0; a < CMP_MAX_ALGS; a++) {
            c->runs[i][a].lanes = alloc_or_die((size_t)CMP_LANE_CAP * PRISM_MAX_CORES);
        }
    }

    for (int a = 0; a < SCHED_COUNT; a++) c->sched_on[a] = true;
    for (int a = 0; a < REPL_COUNT; a++) c->repl_on[a] = true;
    c->hover_alg = -1;
    c->table_alg = REPL_FIFO;

    add_current_scenario_case(c);
    add_preset_cases(c, g);
    c->dirty = true;
    g->compare = c;
}

void compare_free(gui_t *g) {
    compare_t *c = g->compare;
    if (!c) return;
    for (int i = 0; i < CMP_MAX_CASES; i++) {
        for (int a = 0; a < CMP_MAX_ALGS; a++) free(c->runs[i][a].lanes);
    }
    free(c->work);
    free(c->base);
    free(c);
    g->compare = NULL;
}

void compare_mark_dirty(gui_t *g) {
    if (g->compare) g->compare->dirty = true;
}

static int first_usable_preset(const compare_t *c) {
    for (int i = 0; i < c->num_cases; i++) {
        if (cmp_case_usable(c, i) && !c->cases[i].current) return i;
    }
    return c->focused_case;
}

static int case_matching_label(const compare_t *c, const char *label, int fallback) {
    int found = fallback;
    for (int i = 0; i < c->num_cases; i++) {
        if (strcmp(c->cases[i].label, label) == 0 && cmp_case_usable(c, i)) found = i;
    }
    return found;
}

void compare_setup_args(gui_t *g, const char *mode) {
    compare_t *c = g->compare;
    bool memory_mode = strcmp(mode, "vm") == 0 || strcmp(mode, "memory") == 0;
    c->mode = memory_mode ? CMP_VM : CMP_SCHED;
    g->view = VIEW_COMPARE;

    c->focused_case = first_usable_preset(c);
    if (g->preset_selected > 0) {
        c->focused_case = case_matching_label(c, g->preset_items[g->preset_selected], c->focused_case);
    }
    c->dirty = true;
}

static const sim_t *case_base(gui_t *g, int i) {
    compare_t *c = g->compare;
    cmp_case_t *k = &c->cases[i];
    if (k->current) {
        case_meta_from_sim(k, g->sim);
        return g->sim;
    }
    if (scenario_load(c->base, NULL, k->path) != SIM_OK) {
        k->loaded = false;
        return NULL;
    }
    sim_set_observer(c->base, NULL, NULL);
    return c->base;
}

static int prepare(compare_t *c, const sim_t *base, int alg, int frames_override) {
    int sched = c->mode == CMP_SCHED ? alg : SIM_VARIANT_KEEP;
    int repl = c->mode == CMP_VM ? alg : SIM_VARIANT_KEEP;
    int quantum = c->mode == CMP_SCHED && c->quantum > 0 ? c->quantum : SIM_VARIANT_KEEP;
    int frames = frames_override > 0 ? frames_override : SIM_VARIANT_KEEP;
    return sim_prepare_variant(c->work, base, sched, repl, quantum, frames);
}

static void append_latest_gantt_tick(cmp_run_t *r, const sim_t *w) {
    uint32_t latest = (w->gantt_count + PRISM_GANTT_HISTORY - 1) % PRISM_GANTT_HISTORY;
    for (int core = 0; core < w->num_cores; core++) {
        r->lanes[r->len * PRISM_MAX_CORES + core] = (int8_t)w->gantt_history[latest][core];
    }
    r->len++;
}

static void run_one(compare_t *c, const sim_t *base, cmp_run_t *r, int alg) {
    r->valid = true;
    r->len = 0;
    r->status = prepare(c, base, alg, c->mode == CMP_VM ? c->frames : 0);
    if (r->status != SIM_OK) return;

    sim_t *w = c->work;
    r->ncores = w->num_cores;
    while (w->tick < CMP_MAX_TICKS && sim_step(w)) {
        if (r->len < CMP_LANE_CAP) append_latest_gantt_tick(r, w);
    }

    bool final_tick_missing = sim_is_done(w) && r->len < w->tick;
    if (final_tick_missing && r->len < CMP_LANE_CAP) append_latest_gantt_tick(r, w);

    sim_metrics_compute(w, &r->metrics);
}

static int count_distinct_pages(const sim_t *s) {
    int distinct = 0;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state == PROC_UNUSED) continue;

        bool seen[PRISM_MAX_VIRTUAL_PAGES] = {false};
        for (int i = 0; i < p->num_page_refs; i++) {
            if (seen[p->page_refs[i]]) continue;
            seen[p->page_refs[i]] = true;
            distinct++;
        }
    }
    return distinct;
}

static void run_sweep(compare_t *c, const sim_t *base, const cmp_case_t *k) {
    c->sweep_current = c->frames > 0 ? c->frames : k->frames;
    c->sweep_min = 1;
    c->sweep_max = count_distinct_pages(base) + 1;
    if (c->sweep_max < c->sweep_current + 2) c->sweep_max = c->sweep_current + 2;
    if (c->sweep_max > CMP_SWEEP_MAX) c->sweep_max = CMP_SWEEP_MAX;

    for (int alg = 0; alg < REPL_COUNT; alg++) {
        for (int frames = c->sweep_min; frames <= c->sweep_max; frames++) {
            c->sweep[alg][frames] = 0;
            if (prepare(c, base, alg, frames) != SIM_OK) continue;
            sim_run_bounded(c->work, CMP_MAX_TICKS);
            c->sweep[alg][frames] = c->work->total_page_faults;
        }
    }
    c->sweep_ready = true;
}

typedef struct {
    compare_t *c;
    const sim_t *s;
    bool pending_fault[PRISM_MAX_PROCESSES];
    int  pending_frame[PRISM_MAX_PROCESSES];
} recorder_t;

static void record_access_column(recorder_t *rec, const sim_event_t *ev) {
    compare_t *c = rec->c;
    const sim_t *s = rec->s;
    int pid = ev->pid;
    if (c->num_columns >= CMP_TBL_COLS) {
        c->table_truncated = true;
        return;
    }

    cmp_col_t *col = &c->columns[c->num_columns++];
    col->page = (int16_t)ev->detail1;
    col->pid = (int8_t)pid;
    col->fault = rec->pending_fault[pid];
    col->loaded = (int8_t)(rec->pending_fault[pid] ? rec->pending_frame[pid] : -1);
    col->hit_frame = (int8_t)ev->detail2;
    for (int f = 0; f < CMP_TBL_FRAMES; f++) {
        bool occupied = f < s->total_frames && s->frames[f].occupied;
        col->frame_page[f] = occupied ? (int16_t)s->frames[f].vpage : -1;
        col->frame_pid[f] = occupied ? (int8_t)s->frames[f].owner_pid : -1;
    }

    rec->pending_fault[pid] = false;
    rec->pending_frame[pid] = -1;
}

static void record_ref(const sim_event_t *ev, void *user) {
    recorder_t *rec = (recorder_t *)user;
    int pid = ev->pid;
    if (pid < 0 || pid >= PRISM_MAX_PROCESSES) return;

    bool valid_page = ev->detail1 >= 0 && ev->detail1 < PRISM_MAX_VIRTUAL_PAGES;
    switch (ev->type) {
        case EVENT_PAGE_FAULT:
            rec->pending_fault[pid] = true;
            break;
        case EVENT_PROC_WAKE_FAULT:
            if (valid_page) rec->pending_frame[pid] = rec->s->procs[pid].pt[ev->detail1].frame_idx;
            break;
        case EVENT_PAGE_ACCESS:
            record_access_column(rec, ev);
            break;
        default:
            break;
    }
}

static void run_table(compare_t *c, const sim_t *base) {
    c->num_columns = 0;
    c->table_truncated = false;
    c->table_ready = false;
    if (prepare(c, base, c->table_alg, c->frames) != SIM_OK) return;

    c->table_frames = c->work->total_frames;
    if (c->table_frames > CMP_TBL_FRAMES) return;

    recorder_t *rec = alloc_or_die(sizeof(recorder_t));
    rec->c = c;
    rec->s = c->work;
    for (int i = 0; i < PRISM_MAX_PROCESSES; i++) rec->pending_frame[i] = -1;

    sim_set_observer(c->work, record_ref, rec);
    sim_run_bounded(c->work, CMP_MAX_TICKS);
    sim_set_observer(c->work, NULL, NULL);
    free(rec);
    c->table_ready = true;
}

static void invalidate_results(compare_t *c) {
    for (int i = 0; i < c->num_cases; i++) {
        for (int a = 0; a < CMP_MAX_ALGS; a++) c->runs[i][a].valid = false;
    }
    c->sweep_ready = false;
    c->table_ready = false;
}

void cmp_run_all(gui_t *g) {
    compare_t *c = g->compare;
    double started = GetTime();
    invalidate_results(c);
    case_meta_from_sim(&c->cases[0], g->sim);

    for (int i = 0; i < c->num_cases; i++) {
        bool focused = i == c->focused_case;
        if (!c->cases[i].enabled && !focused) continue;

        const sim_t *base = case_base(g, i);
        if (!base || !cmp_case_usable(c, i)) continue;

        for (int alg = 0; alg < cmp_num_algs(c); alg++) {
            if (cmp_alg_on(c, alg)) run_one(c, base, &c->runs[i][alg], alg);
        }
        if (focused && c->mode == CMP_VM) {
            run_sweep(c, base, &c->cases[i]);
            run_table(c, base);
        }
    }
    c->run_ms = (GetTime() - started) * 1000.0;
    c->dirty = false;
}

void cmp_open_in_sim(gui_t *g, int case_idx, int alg) {
    compare_t *c = g->compare;
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
    g->selected = (node_ref_t){c->mode == CMP_SCHED ? NODE_SCHED : NODE_MEMORY, 0};
    g->left_tab = LTAB_EXPLAIN;
    gui_status(g, false, "Opened %s with %s: press Step (N) to follow each decision", k->label, cmp_alg_name(c, alg));
}
