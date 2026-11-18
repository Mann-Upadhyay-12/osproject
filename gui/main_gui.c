#include "gui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PRESETS        16
#define MAX_STEPS_PER_FRAME 200
#define STATUS_VISIBLE_S   8
#define STATUS_FADE_S      6

#define TOOLBAR_H   48.0f
#define PALETTE_W   170.0f
#define INSPECTOR_W 340.0f
#define BOTTOM_H    250.0f
#define STATUS_H    22.0f
#define METRICS_W   330.0f

typedef struct {
    const char *scenario;
    const char *shot;
    const char *select;
    const char *tab;
    const char *compare_mode;
    const char *lab;
    int         ticks;
    int         back;
    bool        open_builder;
} gui_args_t;

static void load_presets(gui_t *g) {
    static char names[MAX_PRESETS][64];

    g->num_presets = 0;
    g->preset_items[0] = "New (empty)";
    if (!DirectoryExists("workloads")) return;

    FilePathList files = LoadDirectoryFilesEx("workloads", ".txt;.wl", false);
    for (unsigned i = 0; i < files.count && g->num_presets < MAX_PRESETS; i++) {
        int n = g->num_presets;
        snprintf(g->presets[n], sizeof(g->presets[0]), "%s", files.paths[i]);
        snprintf(names[n], sizeof(names[0]), "%s", GetFileNameWithoutExt(g->presets[n]));
        g->preset_items[n + 1] = names[n];
        g->num_presets++;
    }
    UnloadDirectoryFiles(files);
}

static bool ctrl_down(void) {
    return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER);
}

static void toggle_view(gui_t *g) {
    g->view = (g->view + 1) % VIEW_COUNT;
    g->playing = false;
    if (g->view == VIEW_COMPARE) compare_mark_dirty(g);
}

static void handle_space(gui_t *g, bool *space_used) {
    bool space_for_panning = IsKeyDown(KEY_SPACE) && (g->panning || IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    if (space_for_panning) *space_used = true;
    if (!IsKeyReleased(KEY_SPACE)) return;

    if (!*space_used) {
        if (g->playing) g->playing = false;
        else if (gui_start_running(g)) g->playing = true;
    }
    *space_used = false;
}

static void step_one_tick(gui_t *g) {
    if (!gui_start_running(g)) return;
    g->playing = false;
    gui_step_tick(g);
}

static void handle_simulation_keys(gui_t *g, bool *space_used) {
    handle_space(g, space_used);

    if (IsKeyPressed(KEY_N)) gui_step_event(g);
    if (IsKeyPressed(KEY_T) || IsKeyPressed(KEY_RIGHT)) step_one_tick(g);
    if (IsKeyPressed(KEY_B) || IsKeyPressed(KEY_LEFT)) gui_step_back(g);
    if (IsKeyPressed(KEY_R)) gui_enter_edit(g);
    if (IsKeyPressed(KEY_A)) builder_open_new(g, g->mode == MODE_RUN);
    if (IsKeyPressed(KEY_F)) gui_fit_view(g);
    if (IsKeyPressed(KEY_G)) g->snap = !g->snap;

    bool delete_pressed = IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE);
    if (delete_pressed && g->selected.kind != NODE_NONE) gui_delete_node(g, g->selected);

    if (IsKeyPressed(KEY_ESCAPE)) {
        g->selected = (node_ref_t){NODE_NONE, 0};
        g->menu_open = false;
    }
}

static void handle_keys(gui_t *g, bool *space_used) {
    if (ui_text_focused() || g->builder.open) return;

    if (ctrl_down()) {
        if (IsKeyPressed(KEY_S)) gui_save(g, g->path);
        else if (IsKeyPressed(KEY_O)) gui_load(g, g->path);
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        toggle_view(g);
        return;
    }
    if (g->view == VIEW_SIM) handle_simulation_keys(g, space_used);
}

static void update_anim_duration(gui_t *g) {
    g->anim_duration = 0.9f / g->speed;
    if (g->anim_duration > 0.45f) g->anim_duration = 0.45f;
    if (g->speed > 30) g->anim_duration = 0;
}

static void advance(gui_t *g, float dt) {
    update_anim_duration(g);
    if (!g->playing) return;

    g->tick_accumulator += dt * g->speed;
    for (int steps = 0; g->tick_accumulator >= 1.0 && steps < MAX_STEPS_PER_FRAME; steps++) {
        gui_step_tick(g);
        g->tick_accumulator -= 1.0;
        if (sim_is_done(g->sim)) {
            g->playing = false;
            g->tick_accumulator = 0;
            return;
        }
    }
}

static void draw_status(gui_t *g, Rectangle r) {
    DrawRectangleRec(r, UI_PANEL_HI);

    double age = GetTime() - g->status_time;
    if (g->status[0] && age < STATUS_VISIBLE_S) {
        float alpha = age > STATUS_FADE_S ? (float)(STATUS_VISIBLE_S - age) / 2 : 1;
        ui_text(r.x + 10, r.y + 4, 14, ColorAlpha(g->status_bad ? UI_BAD : UI_TEXT, alpha), "%s", g->status);
    }

    const char *summary = g->view == VIEW_COMPARE ? "compare view  |  Tab: Labs"
                        : g->view == VIEW_LABS ? "labs  |  Tab: back to Simulate"
        : TextFormat("%s  |  zoom %.0f%%  |  snap %s (G)", g->mode == MODE_EDIT ? "edit" : "run",
                     g->camera.zoom * 100, g->snap ? "on" : "off");
    ui_text(r.x + r.width - ui_text_width(summary, 13) - 10, r.y + 5, 13, UI_TEXT_DIM, "%s", summary);
}

typedef struct {
    Rectangle toolbar, status, palette, inspector;
    Rectangle bottom_left, bottom_mid, metrics;
    Rectangle full_body;
} screen_layout_t;

static screen_layout_t compute_layout(gui_t *g) {
    float w = (float)GetScreenWidth(), h = (float)GetScreenHeight();
    float middle_h = h - TOOLBAR_H - BOTTOM_H - STATUS_H;
    float bottom_y = h - BOTTOM_H - STATUS_H;
    float bottom_left_w = (w - METRICS_W) * 0.46f;

    screen_layout_t L;
    L.toolbar = (Rectangle){0, 0, w, TOOLBAR_H};
    L.status = (Rectangle){0, h - STATUS_H, w, STATUS_H};
    L.palette = (Rectangle){0, TOOLBAR_H, PALETTE_W, middle_h};
    L.inspector = (Rectangle){w - INSPECTOR_W, TOOLBAR_H, INSPECTOR_W, middle_h};
    L.bottom_left = (Rectangle){0, bottom_y, bottom_left_w, BOTTOM_H};
    L.bottom_mid = (Rectangle){bottom_left_w, bottom_y, w - METRICS_W - bottom_left_w, BOTTOM_H};
    L.metrics = (Rectangle){w - METRICS_W, bottom_y, METRICS_W, BOTTOM_H};
    L.full_body = (Rectangle){0, TOOLBAR_H, w, h - TOOLBAR_H - STATUS_H};
    g->canvas = (Rectangle){PALETTE_W, TOOLBAR_H, w - PALETTE_W - INSPECTOR_W, middle_h};
    return L;
}

static void draw_simulate_view(gui_t *g, const screen_layout_t *L) {
    canvas_draw(g);
    explain_draw_latest(g, g->canvas);
    panel_palette(g, L->palette);
    panel_inspector(g, L->inspector);
    panel_bottom_left(g, L->bottom_left);
    panel_bottom_mid(g, L->bottom_mid);
    panel_metrics(g, L->metrics);
}

static void frame(gui_t *g, float dt, bool *space_used) {
    screen_layout_t L = compute_layout(g);
    bool simulating = g->view == VIEW_SIM;

    if (simulating) advance(g, dt);
    anim_update(g);

    BeginDrawing();
    ClearBackground(UI_BG);
    ui_begin();

    if (simulating) draw_simulate_view(g, &L);
    else if (g->view == VIEW_COMPARE) compare_view(g, L.full_body);
    else labs_view(g, L.full_body);
    panel_toolbar(g, L.toolbar);
    draw_status(g, L.status);

    if (simulating) {
        panel_context_menu(g);
        builder_draw(g);
        canvas_update(g);
    }
    handle_keys(g, space_used);

    ui_end();
    EndDrawing();
}

static void print_usage(void) {
    printf("usage: prism-gui [scenario] [--shot out.png] [--ticks N] [--select NODE] [--builder]\n"
           "                 [--tab explain|events|gantt|states|table] [--compare sched|vm] [--back N]\n"
           "                 [--lab refstring|disk|memory|paging]\n");
}

static bool parse_args(int argc, char **argv, gui_args_t *a) {
    memset(a, 0, sizeof(*a));
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        bool has_value = i + 1 < argc;

        if (strcmp(arg, "--shot") == 0 && has_value) a->shot = argv[++i];
        else if (strcmp(arg, "--ticks") == 0 && has_value) a->ticks = atoi(argv[++i]);
        else if (strcmp(arg, "--select") == 0 && has_value) a->select = argv[++i];
        else if (strcmp(arg, "--builder") == 0) a->open_builder = true;
        else if (strcmp(arg, "--tab") == 0 && has_value) a->tab = argv[++i];
        else if (strcmp(arg, "--back") == 0 && has_value) a->back = atoi(argv[++i]);
        else if (strcmp(arg, "--compare") == 0 && has_value) a->compare_mode = argv[++i];
        else if (strcmp(arg, "--lab") == 0 && has_value) a->lab = argv[++i];
        else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage();
            return false;
        } else {
            a->scenario = arg;
        }
    }
    return true;
}

static gui_t *create_gui(void) {
    gui_t *g = calloc(1, sizeof(gui_t));
    if (!g) return NULL;
    g->sim = malloc(sizeof(sim_t));
    if (!g->sim) {
        free(g);
        return NULL;
    }

    g->speed = 4;
    g->snap = true;
    g->hide_page_hits = true;
    g->events_filter_pid = -1;
    g->palette_drag = -1;
    g->dragged.kind = g->selected.kind = g->hovered.kind = NODE_NONE;
    g->camera.zoom = 1;
    snprintf(g->path, sizeof(g->path), "scenario.txt");
    return g;
}

static void destroy_gui(gui_t *g) {
    labs_free(g);
    compare_free(g);
    ui_shutdown();
    CloseWindow();
    free(g->sim);
    free(g);
}

static void open_window(bool screenshot_only) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | (screenshot_only ? 0 : FLAG_VSYNC_HINT));
    InitWindow(1440, 880, "PRISM - process & memory simulator");
    SetWindowMinSize(1200, 720);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
}

static void load_initial_scenario(gui_t *g, const char *scenario) {
    g->canvas = (Rectangle){PALETTE_W, TOOLBAR_H, GetScreenWidth() - 510.0f, GetScreenHeight() - 300.0f};
    gui_new_scenario(g);

    if (scenario) {
        gui_load(g, scenario);
        return;
    }
    if (FileExists("workloads/mixed_demo.txt")) gui_load(g, "workloads/mixed_demo.txt");
    snprintf(g->path, sizeof(g->path), "scenario.txt");
}

static void select_node_by_name(gui_t *g, const char *name) {
    for (int kind = 0; kind < NODE_SYS_COUNT; kind++) {
        if (strcmp(name, g_sys_keys[kind]) == 0) g->selected = (node_ref_t){kind, 0};
    }
    for (int pid = 0; pid < g->sim->num_processes; pid++) {
        if (strcmp(name, g->sim->procs[pid].name) == 0) g->selected = (node_ref_t){NODE_PROC, pid};
    }
}

static void select_tab_by_name(gui_t *g, const char *name) {
    static const char *left_tabs[LTAB_COUNT] = {"explain", "events", "sync"};
    static const char *mid_tabs[] = {"gantt", "states", "table"};

    for (int i = 0; i < LTAB_COUNT; i++) {
        if (strcmp(name, left_tabs[i]) == 0) g->left_tab = i;
    }
    for (int i = 0; i < 3; i++) {
        if (strcmp(name, mid_tabs[i]) == 0) g->mid_tab = i;
    }
}

static void apply_startup_args(gui_t *g, const gui_args_t *a) {
    if (a->ticks > 0) {
        g->mode = MODE_RUN;
        bool leave_last_tick_for_animation = a->shot && a->back == 0;
        int ticks = leave_last_tick_for_animation ? a->ticks - 1 : a->ticks;
        for (int i = 0; i < ticks; i++) gui_step_tick(g);
        for (int i = 0; i < a->back; i++) gui_step_back(g);
    }
    if (a->select) select_node_by_name(g, a->select);
    if (a->open_builder) builder_open_new(g, g->mode == MODE_RUN);
    if (a->tab) select_tab_by_name(g, a->tab);
    if (a->compare_mode) compare_setup_args(g, a->compare_mode);
    if (a->lab) labs_select(g, a->lab);
}

static bool take_screenshot(gui_t *g, const gui_args_t *a) {
    bool space_used = false;
    frame(g, 0, &space_used);

    if (a->ticks > 0 && a->back == 0) {
        anim_clear(g);
        g->anim_duration = 0.45f;
        g->anim_now = GetTime();
        gui_step_tick(g);
        WaitTime(0.2);
    }
    for (int i = 0; i < 2; i++) frame(g, 0, &space_used);

    Image image = LoadImageFromScreen();
    bool saved = ExportImage(image, a->shot);
    UnloadImage(image);
    if (!saved) fprintf(stderr, "prism-gui: cannot write %s\n", a->shot);

    printf("tick %" PRIu64 " hash %016" PRIx64 " events %u narration %u\n", (uint64_t)g->sim->tick, g->sim->log_hash,
           g->events_len, g->narration_len);
    return saved;
}

static void load_dropped_file(gui_t *g) {
    if (!IsFileDropped()) return;
    FilePathList dropped = LoadDroppedFiles();
    if (dropped.count > 0) {
        if (g->view == VIEW_LABS) labs_drop_file(g, dropped.paths[0]);
        else gui_load(g, dropped.paths[0]);
    }
    UnloadDroppedFiles(dropped);
}

static void run_interactive(gui_t *g) {
    bool space_used = false;
    while (!WindowShouldClose()) {
        load_dropped_file(g);
        frame(g, GetFrameTime(), &space_used);
    }
}

int main(int argc, char **argv) {
    gui_args_t args;
    if (!parse_args(argc, argv, &args)) return 0;

    gui_t *g = create_gui();
    if (!g) {
        fprintf(stderr, "prism-gui: out of memory\n");
        return 1;
    }

    open_window(args.shot != NULL);
    ui_init();
    panels_init();
    load_presets(g);
    compare_init(g);
    labs_init(g);
    load_initial_scenario(g, args.scenario);
    apply_startup_args(g, &args);

    int exit_code = 0;
    if (args.shot) exit_code = take_screenshot(g, &args) ? 0 : 1;
    else run_interactive(g);

    destroy_gui(g);
    return exit_code;
}
