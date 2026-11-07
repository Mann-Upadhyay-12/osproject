#include "gui_state.h"
#include "prism/workload.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * prism-gui [scenario-file] [--shot out.png] [--ticks N] [--select NAME|cpu|memory|...] [--builder]
 *
 * --shot renders one frame after loading (and running N ticks) and writes a
 * PNG, then exits: used for README screenshots and to smoke-test the GUI in CI
 * under Xvfb.
 */

static gui_t *G;   /* single window; the observer gets it through user data */

void gui_status(gui_t *g, bool bad, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->status, sizeof(g->status), fmt, ap);
    va_end(ap);
    g->status_t = GetTime();
    g->status_bad = bad;
}

void gui_reset_view_state(gui_t *g) {
    g->ev_start = g->ev_len = 0;
    g->ev_scroll = 0;
    g->hist_len = 0;
    g->tick_acc = 0;
    anim_clear(g);
}

/* Reset the run to tick 0 and go back to editing the scenario. */
void gui_enter_edit(gui_t *g) {
    sim_reset(g->sim);
    g->mode = MODE_EDIT;
    g->playing = false;
    gui_reset_view_state(g);
    gui_status(g, false, "Reset to tick 0 (edit mode)");
}

static void record_history(gui_t *g) {
    const sim_t *s = g->sim;
    uint8_t *row = g->hist[g->hist_len % GUI_HIST_TICKS];
    for (int pid = 0; pid < PRISM_MAX_PROCESSES; pid++) {
        row[pid] = pid < s->num_processes ? (uint8_t)s->procs[pid].state : PROC_UNUSED;
    }
    g->hist_len++;
}

void gui_step_tick(gui_t *g) {
    if (sim_is_done(g->sim)) return;
    sim_step(g->sim);
    record_history(g);
}

/* Same rule as sim_step_event, but one tick at a time so the timeline gets
 * every tick. */
void gui_step_event(gui_t *g) {
    char msg[160];
    if (!gui_validate(g, msg, sizeof(msg))) { gui_status(g, true, "%s", msg); return; }
    if (sim_is_done(g->sim)) { gui_status(g, false, "Run finished. Reset (R) to replay."); return; }
    g->mode = MODE_RUN;
    g->playing = false;
    uint64_t before = g->sim->notable_events;
    for (int i = 0; i < PRISM_STEP_EVENT_MAX_TICKS && !sim_is_done(g->sim) && g->sim->notable_events == before; i++) {
        gui_step_tick(g);
    }
}

bool gui_validate(const gui_t *g, char *msg, size_t n) {
    static const char *need[NODE_SYS_COUNT] = {
        "Add a Ready Queue node", "Add a Scheduler node", "Add a CPU node", "Add a Memory node", "Add a Disk / I-O node"
    };
    for (int k = 0; k < NODE_SYS_COUNT; k++) {
        if (!gui_has_node(g, k)) { snprintf(msg, n, "%s (drag it from the palette)", need[k]); return false; }
    }
    for (int pid = 0; pid < g->sim->num_processes; pid++) {
        if (g->sim->procs[pid].state != PROC_UNUSED) return true;
    }
    snprintf(msg, n, "Add a process: drag Process from the palette or press A");
    return false;
}

static void fit_view(gui_t *g) {
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    int n = 0;
    for (int k = 0; k <= NODE_SYS_COUNT + PRISM_MAX_PROCESSES; k++) {
        node_ref_t ref;
        if (k < NODE_SYS_COUNT) {
            if (!gui_has_node(g, k)) continue;
            ref = (node_ref_t){k, 0};
        } else {
            int pid = k - NODE_SYS_COUNT;
            if (pid >= g->sim->num_processes || g->sim->procs[pid].state == PROC_UNUSED) continue;
            ref = (node_ref_t){NODE_PROC, pid};
        }
        Rectangle r = canvas_node_rect(g, ref);
        if (r.x < minx) minx = r.x;
        if (r.y < miny) miny = r.y;
        if (r.x + r.width > maxx) maxx = r.x + r.width;
        if (r.y + r.height > maxy) maxy = r.y + r.height;
        n++;
    }
    if (n == 0) return;
    if (minx > 0) minx = 0;   /* keep the process column in view */
    if (miny > 0) miny = 0;
    float zx = g->canvas.width / (maxx - minx + 80), zy = g->canvas.height / (maxy - miny + 80);
    float z = zx < zy ? zx : zy;
    if (z > 1.25f) z = 1.25f;
    if (z < 0.4f) z = 0.4f;
    g->cam.zoom = z;
    g->cam.offset = (Vector2){g->canvas.x + g->canvas.width / 2, g->canvas.y + g->canvas.height / 2};
    g->cam.target = (Vector2){(minx + maxx) / 2, (miny + maxy) / 2};
}

void gui_new_scenario(gui_t *g) {
    sim_init(g->sim, 1, 8, 3, SCHED_RR, REPL_LRU, 42);
    g->sim->default_quantum = 4;
    sim_set_observer(g->sim, anim_observer, g);
    layout_init(&g->layout);
    canvas_default_layout(g);
    g->mode = MODE_EDIT;
    g->playing = false;
    g->sel = (node_ref_t){NODE_NONE, 0};
    gui_reset_view_state(g);
    fit_view(g);
    gui_status(g, false, "New scenario: add processes from the palette");
}

void gui_load(gui_t *g, const char *path) {
    int rc = scenario_load(g->sim, &g->layout, path);
    if (rc != SIM_OK) { gui_status(g, true, "Cannot load %s: %s", path, sim_strerror(rc)); return; }
    sim_set_observer(g->sim, anim_observer, g);
    canvas_default_layout(g);
    snprintf(g->path, sizeof(g->path), "%s", path);
    g->preset_sel = 0;
    for (int i = 0; i < g->npresets; i++) {
        if (strcmp(GetFileName(g->presets[i]), GetFileName(path)) == 0) g->preset_sel = i + 1;
    }
    g->mode = MODE_EDIT;
    g->playing = false;
    g->sel = (node_ref_t){NODE_NONE, 0};
    gui_reset_view_state(g);
    fit_view(g);
    gui_status(g, false, "Loaded %s", path);
}

void gui_save(gui_t *g, const char *path) {
    int rc = scenario_save(g->sim, &g->layout, path);
    if (rc != SIM_OK) gui_status(g, true, "Cannot save %s: %s", path, sim_strerror(rc));
    else gui_status(g, false, "Saved %s", path);
}

void gui_add_sys_node(gui_t *g, int kind, Vector2 world) {
    if (g->mode != MODE_EDIT) { gui_status(g, true, "Reset (R) to change the system layout"); return; }
    if (gui_has_node(g, kind)) { gui_status(g, true, "There is already a %s node", g_sys_titles[kind]); return; }
    layout_set(&g->layout, g_sys_keys[kind], world.x - 100, world.y - 40);
    g->sel = (node_ref_t){kind, 0};
}

void gui_delete_node(gui_t *g, node_ref_t n) {
    if (n.kind == NODE_PROC) {
        char name[PRISM_MAX_NAME_LEN];
        snprintf(name, sizeof(name), "%s", g->sim->procs[n.pid].name);
        int rc = sim_remove_process(g->sim, n.pid);
        if (rc != SIM_OK) { gui_status(g, true, "Cannot delete %s: %s", name, sim_strerror(rc)); return; }
        layout_remove(&g->layout, name);
        gui_status(g, false, "Deleted %s", name);
    } else if (n.kind >= 0 && n.kind < NODE_SYS_COUNT) {
        if (g->mode != MODE_EDIT) { gui_status(g, true, "Reset (R) to change the system layout"); return; }
        layout_remove(&g->layout, g_sys_keys[n.kind]);
    }
    g->sel = (node_ref_t){NODE_NONE, 0};
}

void gui_duplicate_process(gui_t *g, int pid) {
    proc_spec_t spec;
    if (sim_get_process_spec(g->sim, pid, &spec) != SIM_OK) return;
    char base[25];
    memcpy(base, spec.name, sizeof(base) - 1);   /* leave room for "_NNN" */
    base[sizeof(base) - 1] = '\0';
    for (int i = 2; i < 1000; i++) {
        snprintf(spec.name, sizeof(spec.name), "%s_%d", base, i % 1000);
        bool taken = false;
        for (int k = 0; k < g->sim->num_processes; k++) {
            if (g->sim->procs[k].state != PROC_UNUSED && strcmp(g->sim->procs[k].name, spec.name) == 0) taken = true;
        }
        if (!taken) break;
    }
    int np = sim_spawn_process(g->sim, &spec);
    if (np < 0) { gui_status(g, true, "Cannot duplicate: %s", sim_strerror(np)); return; }
    canvas_place_process(g, np);
    g->sel = (node_ref_t){NODE_PROC, np};
    gui_status(g, false, g->mode == MODE_RUN ? "Duplicated as %s (arrives now)" : "Duplicated as %s", spec.name);
}

void gui_random_processes(gui_t *g, int n) {
    int added = 0;
    for (int i = 0; i < n; i++) {
        builder_open_new(g, g->mode == MODE_RUN);
        builder_template(g, 4);
        if (g->mode == MODE_EDIT) g->b.arrival = (int)(g->b.rng % 12);
        /* Reuse the builder's validation path without showing the dialog. */
        proc_spec_t spec;
        proc_spec_init(&spec, g->b.name);
        spec.arrival = (tick_t)g->b.arrival;
        spec.priority = (uint16_t)g->b.priority;
        for (int k = 0; k < g->b.nb; k++) proc_spec_add_burst(&spec, (burst_kind_t)g->b.kind[k], (uint16_t)g->b.dur[k]);
        workload_parse_pages(&spec, g->b.pages, g->sim->seed, g->sim->num_processes);
        g->b.open = false;
        int pid = sim_spawn_process(g->sim, &spec);
        if (pid < 0) break;
        canvas_place_process(g, pid);
        added++;
    }
    gui_status(g, added < n, added < n ? "Added %d processes (table full)" : "Added %d random processes", added);
}

static void load_presets(gui_t *g) {
    g->npresets = 0;
    g->preset_items[0] = "New (empty)";
    if (!DirectoryExists("workloads")) return;
    FilePathList files = LoadDirectoryFilesEx("workloads", ".txt;.wl", false);
    for (unsigned i = 0; i < files.count && g->npresets < 16; i++) {
        snprintf(g->presets[g->npresets], sizeof(g->presets[0]), "%s", files.paths[i]);
        g->preset_items[g->npresets + 1] = GetFileNameWithoutExt(g->presets[g->npresets]);
        /* GetFileNameWithoutExt returns a static buffer: copy it. */
        static char names[16][64];
        snprintf(names[g->npresets], sizeof(names[0]), "%s", g->preset_items[g->npresets + 1]);
        g->preset_items[g->npresets + 1] = names[g->npresets];
        g->npresets++;
    }
    UnloadDirectoryFiles(files);
}

static void handle_keys(gui_t *g, bool *space_used) {
    if (ui_text_focused() || g->b.open) return;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER);
    if (ctrl && IsKeyPressed(KEY_S)) { gui_save(g, g->path); return; }
    if (ctrl && IsKeyPressed(KEY_O)) { gui_load(g, g->path); return; }
    if (ctrl) return;
    /* Space toggles on release, unless it was held to pan. */
    if (IsKeyDown(KEY_SPACE) && (g->panning || IsMouseButtonDown(MOUSE_BUTTON_LEFT))) *space_used = true;
    if (IsKeyReleased(KEY_SPACE)) {
        if (!*space_used) {
            if (g->playing) g->playing = false;
            else {
                char msg[160];
                if (!gui_validate(g, msg, sizeof(msg))) gui_status(g, true, "%s", msg);
                else if (sim_is_done(g->sim)) gui_status(g, false, "Run finished. Reset (R) to replay.");
                else { g->mode = MODE_RUN; g->playing = true; }
            }
        }
        *space_used = false;
    }
    if (IsKeyPressed(KEY_N)) gui_step_event(g);
    if (IsKeyPressed(KEY_T)) {
        char msg[160];
        if (gui_validate(g, msg, sizeof(msg))) { g->mode = MODE_RUN; gui_step_tick(g); } else gui_status(g, true, "%s", msg);
    }
    if (IsKeyPressed(KEY_R)) gui_enter_edit(g);
    if (IsKeyPressed(KEY_A)) builder_open_new(g, g->mode == MODE_RUN);
    if (IsKeyPressed(KEY_F)) fit_view(g);
    if (IsKeyPressed(KEY_G)) g->snap = !g->snap;
    if ((IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) && g->sel.kind != NODE_NONE) gui_delete_node(g, g->sel);
    if (IsKeyPressed(KEY_ESCAPE)) { g->sel = (node_ref_t){NODE_NONE, 0}; g->ctx_open = false; }
}

static void advance(gui_t *g, float dt) {
    g->anim_dur = 0.9f / g->speed;
    if (g->anim_dur > 0.45f) g->anim_dur = 0.45f;
    if (g->speed > 30) g->anim_dur = 0;   /* too fast to follow: just update the views */
    if (!g->playing) return;
    g->tick_acc += dt * g->speed;
    int steps = 0;
    while (g->tick_acc >= 1.0 && steps < 200) {
        gui_step_tick(g);
        g->tick_acc -= 1.0;
        steps++;
        if (sim_is_done(g->sim)) {
            g->playing = false;
            g->tick_acc = 0;
            gui_status(g, false, "Finished at tick %" PRIu64 ". Reset (R) to replay, or inject a process and continue.", (uint64_t)g->sim->tick);
            break;
        }
    }
}

static void draw_status(gui_t *g, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL_HI);
    double age = GetTime() - g->status_t;
    if (g->status[0] && age < 8) {
        Color c = g->status_bad ? UI_BAD : UI_TEXT;
        ui_text(r.x + 10, r.y + 4, 14, ColorAlpha(c, age > 6 ? (float)(8 - age) / 2 : 1), "%s", g->status);
    }
    const char *right = TextFormat("%s  |  zoom %.0f%%  |  snap %s (G)", g->mode == MODE_EDIT ? "edit" : "run",
                                   g->cam.zoom * 100, g->snap ? "on" : "off");
    ui_text(r.x + r.width - ui_text_width(right, 13) - 10, r.y + 5, 13, UI_TEXT_DIM, "%s", right);
}

static void frame(gui_t *g, float dt, bool *space_used) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    const float top = 48, left = 170, right = 340, bottom = 230, status = 22;
    Rectangle toolbar = {0, 0, (float)sw, top};
    Rectangle palette = {0, top, left, sh - top - bottom - status};
    Rectangle inspector = {sw - right, top, right, sh - top - bottom - status};
    g->canvas = (Rectangle){left, top, sw - left - right, sh - top - bottom - status};
    float by = sh - bottom - status;
    float evw = (sw - 330) * 0.52f;
    Rectangle events = {0, by, evw, bottom};
    Rectangle gantt = {evw, by, sw - 330 - evw, bottom};
    Rectangle metrics = {sw - 330.0f, by, 330, bottom};
    Rectangle statusbar = {0, (float)sh - status, (float)sw, status};

    advance(g, dt);
    anim_update(g);

    BeginDrawing();
    ClearBackground(UI_BG);
    ui_begin();
    canvas_draw(g);
    panel_palette(g, palette);
    panel_inspector(g, inspector);
    panel_events(g, events);
    panel_gantt(g, gantt);
    panel_metrics(g, metrics);
    panel_toolbar(g, toolbar);
    draw_status(g, statusbar);
    panel_context_menu(g);
    builder_draw(g);
    canvas_update(g);
    handle_keys(g, space_used);
    ui_end();
    EndDrawing();
}

int main(int argc, char **argv) {
    const char *file = NULL, *shot = NULL, *select = NULL;
    int ticks = 0;
    bool open_builder = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc) select = argv[++i];
        else if (strcmp(argv[i], "--builder") == 0) open_builder = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: prism-gui [scenario] [--shot out.png] [--ticks N] [--select NODE] [--builder]\n");
            return 0;
        }
        else file = argv[i];
    }

    G = calloc(1, sizeof(gui_t));
    if (!G) { fprintf(stderr, "prism-gui: out of memory\n"); return 1; }
    G->sim = malloc(sizeof(sim_t));
    if (!G->sim) { fprintf(stderr, "prism-gui: out of memory\n"); free(G); return 1; }
    gui_t *g = G;
    g->speed = 4;
    g->snap = true;
    g->hide_hits = true;
    g->ev_filter_pid = -1;
    g->palette_drag = -1;
    g->drag.kind = g->sel.kind = g->hover.kind = NODE_NONE;
    g->cam.zoom = 1;
    snprintf(g->path, sizeof(g->path), "scenario.txt");

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | (shot ? 0 : FLAG_VSYNC_HINT));
    InitWindow(1440, 880, "PRISM - process & memory simulator");
    SetWindowMinSize(1200, 720);
    SetExitKey(KEY_NULL);   /* Esc closes dialogs, not the app */
    SetTargetFPS(60);
    ui_init();
    panels_init();
    load_presets(g);

    /* Lay out once so fit_view knows the canvas size. */
    g->canvas = (Rectangle){170, 48, GetScreenWidth() - 510.0f, GetScreenHeight() - 300.0f};
    if (file) gui_load(g, file);
    else if (FileExists("workloads/mixed_demo.txt")) gui_load(g, "workloads/mixed_demo.txt");
    else gui_new_scenario(g);
    if (!file) snprintf(g->path, sizeof(g->path), "scenario.txt");

    if (ticks > 0) {
        g->mode = MODE_RUN;
        for (int i = 0; i < ticks - 1; i++) gui_step_tick(g);
    }
    if (select) {
        for (int k = 0; k < NODE_SYS_COUNT; k++) if (strcmp(select, g_sys_keys[k]) == 0) g->sel = (node_ref_t){k, 0};
        for (int pid = 0; pid < g->sim->num_processes; pid++) {
            if (strcmp(select, g->sim->procs[pid].name) == 0) g->sel = (node_ref_t){NODE_PROC, pid};
        }
    }
    if (open_builder) builder_open_new(g, g->mode == MODE_RUN);

    bool space_used = false;
    if (shot) {
        frame(g, 0, &space_used);
        if (ticks > 0) {
            /* Step the last tick after the clock is running so its tokens are mid-flight. */
            anim_clear(g);
            g->anim_dur = 0.45f;
            g->anim_now = GetTime();
            gui_step_tick(g);
            WaitTime(0.2);
        }
        for (int i = 0; i < 2; i++) frame(g, 0, &space_used);
        Image img = LoadImageFromScreen();
        bool ok = ExportImage(img, shot);
        UnloadImage(img);
        if (!ok) fprintf(stderr, "prism-gui: cannot write %s\n", shot);
        ui_shutdown();
        CloseWindow();
        free(g->sim);
        free(g);
        return ok ? 0 : 1;
    }

    while (!WindowShouldClose()) {
        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0) gui_load(g, dropped.paths[0]);
            UnloadDroppedFiles(dropped);
        }
        frame(g, GetFrameTime(), &space_used);
    }

    ui_shutdown();
    CloseWindow();
    free(g->sim);
    free(g);
    return 0;
}
