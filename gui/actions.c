#include "gui_state.h"
#include "prism/workload.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RANDOM_TEMPLATE      4
#define RANDOM_ARRIVAL_RANGE 12

void gui_status(gui_t *g, bool bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->status, sizeof(g->status), fmt, ap);
    va_end(ap);
    g->status_time = GetTime();
    g->status_bad = bad;
}

void gui_reset_view_state(gui_t *g) {
    g->events_start = g->events_len = 0;
    g->events_scroll = 0;
    g->history_len = 0;
    g->tick_accumulator = 0;
    explain_clear(g);
    anim_clear(g);
}

void gui_enter_edit(gui_t *g) {
    sim_reset(g->sim);
    g->mode = MODE_EDIT;
    g->playing = false;
    gui_reset_view_state(g);
    gui_status(g, false, "Reset to tick 0 (edit mode)");
}

static void record_history(gui_t *g) {
    const sim_t *s = g->sim;
    uint32_t slot = g->history_len % GUI_HIST_TICKS;

    uint8_t *states = g->state_history[slot];
    for (int pid = 0; pid < PRISM_MAX_PROCESSES; pid++) {
        states[pid] = pid < s->num_processes ? (uint8_t)s->procs[pid].state : PROC_UNUSED;
    }

    int8_t *cores = g->core_history[slot];
    uint32_t latest = (s->gantt_count + PRISM_GANTT_HISTORY - 1) % PRISM_GANTT_HISTORY;
    for (int c = 0; c < PRISM_MAX_CORES; c++) {
        cores[c] = c < s->num_cores ? (int8_t)s->gantt_history[latest][c] : -1;
    }
    g->history_len++;
}

void gui_step_tick(gui_t *g) {
    if (sim_is_done(g->sim)) return;
    sim_step(g->sim);
    record_history(g);
}

bool gui_start_running(gui_t *g) {
    char problem[160];
    if (!gui_validate(g, problem, sizeof(problem))) {
        gui_status(g, true, "%s", problem);
        return false;
    }
    if (sim_is_done(g->sim)) {
        gui_status(g, false, "Run finished. Back (B) / click the Gantt chart to rewind, Reset (R) to edit.");
        return false;
    }
    g->mode = MODE_RUN;
    return true;
}

void gui_seek(gui_t *g, tick_t target) {
    g->playing = false;
    if (target == 0) {
        gui_enter_edit(g);
        return;
    }
    if (target == g->sim->tick) return;
    if (!gui_start_running(g) && target > g->sim->tick) return;

    bool rewinding = target < g->sim->tick;
    if (rewinding) {
        sim_reset(g->sim);
        gui_reset_view_state(g);
    }

    g->mode = MODE_RUN;
    g->replaying = true;
    while (g->sim->tick < target && !sim_is_done(g->sim)) gui_step_tick(g);
    g->replaying = false;
    anim_clear(g);

    if (rewinding) {
        gui_status(g, false, "Rewound to tick %" PRIu64 " (replayed from 0 with the current policies)", (uint64_t)g->sim->tick);
    }
}

void gui_step_back(gui_t *g) {
    if (g->sim->tick == 0) {
        gui_status(g, false, "Already at tick 0");
        return;
    }
    gui_seek(g, g->sim->tick - 1);
}

void gui_step_event(gui_t *g) {
    if (!gui_start_running(g)) return;
    g->playing = false;

    uint64_t events_before = g->sim->notable_events;
    for (int i = 0; i < PRISM_STEP_EVENT_MAX_TICKS; i++) {
        if (sim_is_done(g->sim) || g->sim->notable_events != events_before) break;
        gui_step_tick(g);
    }
}

static bool has_live_process(const sim_t *s) {
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (s->procs[pid].state != PROC_UNUSED) return true;
    }
    return false;
}

bool gui_validate(const gui_t *g, char *msg, size_t size) {
    static const char *missing_node_hint[NODE_SYS_COUNT] = {
        "Add a Ready Queue node", "Add a Scheduler node", "Add a CPU node", "Add a Memory node", "Add a Disk / I-O node"
    };
    for (int kind = 0; kind < NODE_SYS_COUNT; kind++) {
        if (!gui_has_node(g, kind)) {
            snprintf(msg, size, "%s (drag it from the palette)", missing_node_hint[kind]);
            return false;
        }
    }
    if (has_live_process(g->sim)) return true;

    snprintf(msg, size, "Add a process: drag Process from the palette or press A");
    return false;
}

static bool visible_node(const gui_t *g, int index, node_ref_t *ref) {
    if (index < NODE_SYS_COUNT) {
        *ref = (node_ref_t){index, 0};
        return gui_has_node(g, index);
    }
    int pid = index - NODE_SYS_COUNT;
    *ref = (node_ref_t){NODE_PROC, pid};
    return pid < g->sim->num_processes && g->sim->procs[pid].state != PROC_UNUSED;
}

static bool nodes_bounding_box(const gui_t *g, Vector2 *min, Vector2 *max) {
    *min = (Vector2){1e9f, 1e9f};
    *max = (Vector2){-1e9f, -1e9f};
    bool any = false;

    for (int index = 0; index <= NODE_SYS_COUNT + PRISM_MAX_PROCESSES; index++) {
        node_ref_t ref;
        if (!visible_node(g, index, &ref)) continue;

        Rectangle r = canvas_node_rect(g, ref);
        if (r.x < min->x) min->x = r.x;
        if (r.y < min->y) min->y = r.y;
        if (r.x + r.width > max->x) max->x = r.x + r.width;
        if (r.y + r.height > max->y) max->y = r.y + r.height;
        any = true;
    }
    return any;
}

void gui_fit_view(gui_t *g) {
    Vector2 min, max;
    if (!nodes_bounding_box(g, &min, &max)) return;
    if (min.x > 0) min.x = 0;
    if (min.y > 0) min.y = 0;

    float zoom_x = g->canvas.width / (max.x - min.x + 80);
    float zoom_y = g->canvas.height / (max.y - min.y + 80);
    float zoom = zoom_x < zoom_y ? zoom_x : zoom_y;
    if (zoom > 1.25f) zoom = 1.25f;
    if (zoom < 0.4f) zoom = 0.4f;

    g->camera.zoom = zoom;
    g->camera.offset = (Vector2){g->canvas.x + g->canvas.width / 2, g->canvas.y + g->canvas.height / 2};
    g->camera.target = (Vector2){(min.x + max.x) / 2, (min.y + max.y) / 2};
}

static void enter_fresh_edit_mode(gui_t *g) {
    g->mode = MODE_EDIT;
    g->playing = false;
    g->selected = (node_ref_t){NODE_NONE, 0};
    gui_reset_view_state(g);
    gui_fit_view(g);
}

void gui_new_scenario(gui_t *g) {
    sim_init(g->sim, 1, 8, 3, SCHED_RR, REPL_LRU, 42);
    g->sim->default_quantum = 4;
    sim_set_observer(g->sim, anim_observer, g);
    layout_init(&g->layout);
    canvas_default_layout(g);
    enter_fresh_edit_mode(g);
    gui_status(g, false, "New scenario: add processes from the palette");
}

static int preset_index_of(const gui_t *g, const char *path) {
    int found = 0;
    for (int i = 0; i < g->num_presets; i++) {
        if (strcmp(GetFileName(g->presets[i]), GetFileName(path)) == 0) found = i + 1;
    }
    return found;
}

void gui_load(gui_t *g, const char *path) {
    int rc = scenario_load(g->sim, &g->layout, path);
    if (rc != SIM_OK) {
        gui_status(g, true, "Cannot load %s: %s", path, sim_strerror(rc));
        return;
    }
    sim_set_observer(g->sim, anim_observer, g);
    canvas_default_layout(g);

    g->preset_selected = preset_index_of(g, path);
    bool is_preset = g->preset_selected > 0 && strstr(path, "workloads") != NULL;
    if (is_preset) {
        snprintf(g->path, sizeof(g->path), "my_%s", GetFileName(path));
    } else {
        snprintf(g->path, sizeof(g->path), "%s", path);
    }

    compare_mark_dirty(g);
    enter_fresh_edit_mode(g);

    if (is_preset) {
        gui_status(g, false, "Loaded test case %s (Save writes %s, the preset stays untouched)", GetFileNameWithoutExt(path), g->path);
    } else {
        gui_status(g, false, "Loaded %s", path);
    }
}

void gui_save(gui_t *g, const char *path) {
    int rc = scenario_save(g->sim, &g->layout, path);
    if (rc != SIM_OK) gui_status(g, true, "Cannot save %s: %s", path, sim_strerror(rc));
    else gui_status(g, false, "Saved %s", path);
}

static bool layout_locked(gui_t *g) {
    if (g->mode == MODE_EDIT) return false;
    gui_status(g, true, "Reset (R) to change the system layout");
    return true;
}

void gui_add_sys_node(gui_t *g, int kind, Vector2 world) {
    if (layout_locked(g)) return;
    if (gui_has_node(g, kind)) {
        gui_status(g, true, "There is already a %s node", g_sys_titles[kind]);
        return;
    }
    layout_set(&g->layout, g_sys_keys[kind], world.x - 100, world.y - 40);
    g->selected = (node_ref_t){kind, 0};
}

static bool delete_process(gui_t *g, int pid) {
    char name[PRISM_MAX_NAME_LEN];
    snprintf(name, sizeof(name), "%s", g->sim->procs[pid].name);

    int rc = sim_remove_process(g->sim, pid);
    if (rc != SIM_OK) {
        gui_status(g, true, "Cannot delete %s: %s", name, sim_strerror(rc));
        return false;
    }
    layout_remove(&g->layout, name);
    gui_status(g, false, "Deleted %s", name);
    return true;
}

void gui_delete_node(gui_t *g, node_ref_t node) {
    if (node.kind == NODE_PROC) {
        if (!delete_process(g, node.pid)) return;
    } else if (node.kind >= 0 && node.kind < NODE_SYS_COUNT) {
        if (layout_locked(g)) return;
        layout_remove(&g->layout, g_sys_keys[node.kind]);
    }
    g->selected = (node_ref_t){NODE_NONE, 0};
}

static bool name_taken(const sim_t *s, const char *name) {
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (s->procs[pid].state != PROC_UNUSED && strcmp(s->procs[pid].name, name) == 0) return true;
    }
    return false;
}

static void pick_copy_name(const sim_t *s, proc_spec_t *spec) {
    char base[25];
    memcpy(base, spec->name, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    for (int suffix = 2; suffix < 1000; suffix++) {
        snprintf(spec->name, sizeof(spec->name), "%s_%d", base, suffix);
        if (!name_taken(s, spec->name)) return;
    }
}

void gui_duplicate_process(gui_t *g, int pid) {
    proc_spec_t spec;
    if (sim_get_process_spec(g->sim, pid, &spec) != SIM_OK) return;
    pick_copy_name(g->sim, &spec);

    int copy = sim_spawn_process(g->sim, &spec);
    if (copy < 0) {
        gui_status(g, true, "Cannot duplicate: %s", sim_strerror(copy));
        return;
    }
    canvas_place_process(g, copy);
    g->selected = (node_ref_t){NODE_PROC, copy};
    gui_status(g, false, g->mode == MODE_RUN ? "Duplicated as %s (arrives now)" : "Duplicated as %s", spec.name);
}

static void spec_from_builder(const gui_t *g, proc_spec_t *spec) {
    const builder_t *b = &g->builder;
    proc_spec_init(spec, b->name);
    spec->arrival = (tick_t)b->arrival;
    spec->priority = (uint16_t)b->priority;
    for (int k = 0; k < b->num_bursts; k++) {
        proc_spec_add_burst(spec, (burst_kind_t)b->bursts[k].kind, (uint16_t)b->bursts[k].duration);
    }
    workload_parse_pages(spec, b->pages, g->sim->seed, g->sim->num_processes);
}

void gui_random_processes(gui_t *g, int count) {
    int added = 0;
    for (int i = 0; i < count; i++) {
        builder_open_new(g, g->mode == MODE_RUN);
        builder_template(g, RANDOM_TEMPLATE);
        if (g->mode == MODE_EDIT) g->builder.arrival = (int)(g->builder.rng % RANDOM_ARRIVAL_RANGE);

        proc_spec_t spec;
        spec_from_builder(g, &spec);
        g->builder.open = false;

        int pid = sim_spawn_process(g->sim, &spec);
        if (pid < 0) break;
        canvas_place_process(g, pid);
        added++;
    }
    bool table_full = added < count;
    gui_status(g, table_full, table_full ? "Added %d processes (table full)" : "Added %d random processes", added);
}
