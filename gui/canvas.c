#include "gui_state.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const g_sys_keys[NODE_SYS_COUNT] = { "ready", "sched", "cpu", "memory", "disk" };
const char *const g_sys_titles[NODE_SYS_COUNT] = { "Ready Queue", "Scheduler", "CPU", "Memory", "Disk / I-O" };

#define GRID_STEP       40
#define SNAP_STEP       20
#define PLACE_SLOT_H    72.0f
#define PLACE_COLUMN    8
#define DOUBLE_CLICK_S  0.45
#define MIN_ZOOM        0.3f
#define MAX_ZOOM        2.5f

static const Color LINK_COLOR = {95, 105, 125, 255};

static const node_ref_t NO_NODE = {NODE_NONE, 0};

static bool proc_live(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

static Vector2 placement_slot(int slot) {
    if (slot < PLACE_COLUMN) return (Vector2){20, 20 + slot * PLACE_SLOT_H};
    return (Vector2){-170, 20 + (slot - PLACE_COLUMN) * PLACE_SLOT_H};
}

static bool slot_occupied(const layout_t *layout, Vector2 slot) {
    for (int i = 0; i < layout->count; i++) {
        const layout_node_t *node = &layout->nodes[i];
        if (fabsf(node->x - slot.x) < 40 && fabsf(node->y - slot.y) < 40) return true;
    }
    return false;
}

void canvas_place_process(gui_t *g, int pid) {
    const char *key = g->sim->procs[pid].name;
    if (layout_get(&g->layout, key, NULL, NULL)) return;

    for (int slot = 0; slot < 200; slot++) {
        Vector2 pos = placement_slot(slot);
        if (!slot_occupied(&g->layout, pos)) {
            layout_set(&g->layout, key, pos.x, pos.y);
            return;
        }
    }
    layout_set(&g->layout, key, 40, 40);
}

void canvas_default_layout(gui_t *g) {
    static const Vector2 default_pos[NODE_SYS_COUNT] = {
        {290, 150}, {590, 156}, {810, 130}, {770, 360}, {420, 380}
    };

    for (int k = 0; k < NODE_SYS_COUNT; k++) {
        if (!gui_has_node(g, k)) layout_set(&g->layout, g_sys_keys[k], default_pos[k].x, default_pos[k].y);
    }
    for (int pid = 0; pid < g->sim->num_processes; pid++) {
        if (proc_live(g->sim, pid)) canvas_place_process(g, pid);
    }
}

static Vector2 rect_edge_toward(Rectangle r, Vector2 toward) {
    Vector2 center = {r.x + r.width / 2, r.y + r.height / 2};
    float dx = toward.x - center.x, dy = toward.y - center.y;
    if (dx == 0 && dy == 0) return center;

    float scale_x = (r.width / 2) / (fabsf(dx) + 1e-6f);
    float scale_y = (r.height / 2) / (fabsf(dy) + 1e-6f);
    float t = scale_x < scale_y ? scale_x : scale_y;
    return (Vector2){center.x + dx * t, center.y + dy * t};
}

static void draw_dashed_line(Vector2 a, float ux, float uy, float length, float thick, Color c) {
    float end = length - 10;
    for (float t = 0; t < end; t += 14) {
        float t2 = t + 8 < end ? t + 8 : end;
        DrawLineEx((Vector2){a.x + ux * t, a.y + uy * t}, (Vector2){a.x + ux * t2, a.y + uy * t2}, thick, c);
    }
}

static void draw_arrow_head(Vector2 tip, float ux, float uy, Color c) {
    Vector2 left  = {tip.x - ux * 12 - uy * 6, tip.y - uy * 12 + ux * 6};
    Vector2 right = {tip.x - ux * 12 + uy * 6, tip.y - uy * 12 - ux * 6};
    DrawTriangle(tip, right, left, c);
    DrawTriangle(tip, left, right, c);
}

static void draw_arrow(Vector2 a, Vector2 b, float thick, Color c, bool dashed, bool both_ends) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 1) return;

    float ux = dx / length, uy = dy / length;
    if (dashed) {
        draw_dashed_line(a, ux, uy, length, thick, c);
    } else {
        DrawLineEx(a, (Vector2){b.x - ux * 8, b.y - uy * 8}, thick, c);
    }
    draw_arrow_head(b, ux, uy, c);

    if (both_ends) draw_arrow(b, a, thick, c, dashed, false);
}

static void link_nodes(const gui_t *g, node_ref_t a, node_ref_t b, Color c, float thick, bool dashed, bool both_ends) {
    Vector2 from = rect_edge_toward(canvas_node_rect(g, a), canvas_node_center(g, b));
    Vector2 to = rect_edge_toward(canvas_node_rect(g, b), canvas_node_center(g, a));
    draw_arrow(from, to, thick, c, dashed, both_ends);
}

bool gui_has_node(const gui_t *g, int kind) {
    return layout_get(&g->layout, g_sys_keys[kind], NULL, NULL);
}

static void draw_grid(const gui_t *g) {
    Vector2 top_left = GetScreenToWorld2D((Vector2){g->canvas.x, g->canvas.y}, g->camera);
    Vector2 bottom_right = GetScreenToWorld2D(
        (Vector2){g->canvas.x + g->canvas.width, g->canvas.y + g->canvas.height}, g->camera);
    Color c = (Color){34, 38, 46, 255};

    for (float x = floorf(top_left.x / GRID_STEP) * GRID_STEP; x < bottom_right.x; x += GRID_STEP) {
        DrawLineV((Vector2){x, top_left.y}, (Vector2){x, bottom_right.y}, c);
    }
    for (float y = floorf(top_left.y / GRID_STEP) * GRID_STEP; y < bottom_right.y; y += GRID_STEP) {
        DrawLineV((Vector2){top_left.x, y}, (Vector2){bottom_right.x, y}, c);
    }
}

static node_ref_t sys_node(int kind) {
    return (node_ref_t){kind, 0};
}

static void link_if_present(const gui_t *g, int a, int b, Color c, float thick, bool dashed, bool both_ends) {
    if (gui_has_node(g, a) && gui_has_node(g, b)) link_nodes(g, sys_node(a), sys_node(b), c, thick, dashed, both_ends);
}

static void draw_links(const gui_t *g) {
    const sim_t *s = g->sim;

    if (gui_has_node(g, NODE_READY)) {
        for (int pid = 0; pid < s->num_processes; pid++) {
            if (!proc_live(s, pid)) continue;
            bool not_arrived = s->procs[pid].state == PROC_NEW;
            Color c = not_arrived ? (Color){70, 76, 90, 255} : ColorAlpha(proc_color(pid), 0.45f);
            link_nodes(g, (node_ref_t){NODE_PROC, pid}, sys_node(NODE_READY), c, 1.5f, not_arrived, false);
        }
    }

    link_if_present(g, NODE_READY, NODE_SCHED, LINK_COLOR, 2.5f, false, false);
    link_if_present(g, NODE_SCHED, NODE_CPU, LINK_COLOR, 2.5f, false, false);
    link_if_present(g, NODE_CPU, NODE_DISK, LINK_COLOR, 2.5f, false, false);
    link_if_present(g, NODE_DISK, NODE_READY, LINK_COLOR, 2.5f, false, false);
    link_if_present(g, NODE_CPU, NODE_MEMORY, LINK_COLOR, 2.5f, false, true);
    link_if_present(g, NODE_CPU, NODE_READY, (Color){70, 78, 95, 255}, 1.5f, true, false);
}

static void draw_validation_banner(const gui_t *g) {
    char message[160];
    if (gui_validate(g, message, sizeof(message))) return;

    Rectangle banner = {g->canvas.x + 12, g->canvas.y + 10, ui_text_width(message, 15) + 24, 28};
    DrawRectangleRounded(banner, 0.3f, 4, ColorAlpha(UI_WARN, 0.18f));
    DrawRectangleRoundedLinesEx(banner, 0.3f, 4, 1, UI_WARN);
    ui_text(banner.x + 12, banner.y + 6, 15, UI_WARN, "%s", message);
}

static void draw_mode_label(const gui_t *g) {
    const char *label;
    if (g->mode == MODE_EDIT) label = "EDIT MODE: build the scenario";
    else label = g->playing ? "RUN MODE: playing" : "RUN MODE: paused";

    float x = g->canvas.x + g->canvas.width - ui_text_width(label, 14) - 14;
    float y = g->canvas.y + g->canvas.height - 22;
    ui_text(x, y, 14, g->mode == MODE_EDIT ? UI_TEXT_DIM : UI_GOOD, "%s", label);
}

static void draw_palette_ghost(const gui_t *g) {
    if (g->palette_drag < 0) return;

    Vector2 mouse = GetMousePosition();
    const char *title = g->palette_drag == NODE_PROC ? "Process" : g_sys_titles[g->palette_drag];
    DrawRectangleRounded((Rectangle){mouse.x - 60, mouse.y - 16, 120, 32}, 0.3f, 4, ColorAlpha(UI_ACCENT, 0.5f));
    ui_text(mouse.x - ui_text_width(title, 15) / 2, mouse.y - 8, 15, UI_TEXT, "%s", title);
}

void canvas_draw(gui_t *g) {
    const sim_t *s = g->sim;
    ui_scissor_push(g->canvas);
    DrawRectangleRec(g->canvas, UI_BG);

    BeginMode2D(g->camera);
    draw_grid(g);
    draw_links(g);
    for (int k = 0; k < NODE_SYS_COUNT; k++) {
        if (gui_has_node(g, k)) canvas_draw_node(g, sys_node(k));
    }
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (proc_live(s, pid)) canvas_draw_node(g, (node_ref_t){NODE_PROC, pid});
    }
    anim_draw(g);
    EndMode2D();

    draw_validation_banner(g);
    draw_mode_label(g);
    draw_palette_ghost(g);
    ui_scissor_pop();
}

static node_ref_t hit_test(const gui_t *g, Vector2 world) {
    const sim_t *s = g->sim;
    for (int pid = s->num_processes - 1; pid >= 0; pid--) {
        node_ref_t n = {NODE_PROC, pid};
        if (proc_live(s, pid) && CheckCollisionPointRec(world, canvas_node_rect(g, n))) return n;
    }
    for (int k = NODE_SYS_COUNT - 1; k >= 0; k--) {
        if (gui_has_node(g, k) && CheckCollisionPointRec(world, canvas_node_rect(g, sys_node(k)))) return sys_node(k);
    }
    return NO_NODE;
}

static void drop_palette_item(gui_t *g, bool in_canvas, Vector2 world) {
    if (g->palette_drag < 0 || !IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) return;

    if (in_canvas) {
        if (g->palette_drag == NODE_PROC) builder_open_new(g, g->mode == MODE_RUN);
        else gui_add_sys_node(g, g->palette_drag, world);
    }
    g->palette_drag = -1;
}

static void update_panning(gui_t *g, bool pan_held) {
    if (!g->panning || !pan_held) {
        g->panning = false;
        return;
    }
    Vector2 delta = GetMouseDelta();
    g->camera.target.x -= delta.x / g->camera.zoom;
    g->camera.target.y -= delta.y / g->camera.zoom;
}

static void move_dragged_node(gui_t *g, Vector2 world) {
    float x = world.x - g->drag_offset.x, y = world.y - g->drag_offset.y;
    if (g->snap) {
        x = roundf(x / SNAP_STEP) * SNAP_STEP;
        y = roundf(y / SNAP_STEP) * SNAP_STEP;
    }
    layout_set(&g->layout, canvas_node_key(g, g->dragged), x, y);
}

static void zoom_at_mouse(gui_t *g, Vector2 mouse, Vector2 world) {
    float wheel = GetMouseWheelMove();
    if (wheel == 0) return;

    float zoom = g->camera.zoom * (wheel > 0 ? 1.1f : 1 / 1.1f);
    if (zoom < MIN_ZOOM) zoom = MIN_ZOOM;
    if (zoom > MAX_ZOOM) zoom = MAX_ZOOM;
    g->camera.offset = mouse;
    g->camera.target = world;
    g->camera.zoom = zoom;
}

static bool is_double_click_on_process(const gui_t *g, node_ref_t clicked) {
    static double last_click_time;
    static node_ref_t last_clicked = {NODE_NONE, 0};

    double now = GetTime();
    bool double_click = clicked.kind == NODE_PROC && last_clicked.kind == NODE_PROC &&
                        last_clicked.pid == clicked.pid && now - last_click_time < DOUBLE_CLICK_S &&
                        g->mode == MODE_EDIT;
    last_click_time = now;
    last_clicked = clicked;
    return double_click;
}

static void handle_left_click(gui_t *g, Vector2 world) {
    g->selected = g->hovered;
    g->menu_open = false;
    if (g->hovered.kind == NODE_NONE) return;

    Rectangle r = canvas_node_rect(g, g->hovered);
    g->dragged = g->hovered;
    g->drag_offset = (Vector2){world.x - r.x, world.y - r.y};

    if (is_double_click_on_process(g, g->hovered)) {
        builder_open_edit(g, g->hovered.pid);
        g->dragged.kind = NODE_NONE;
    }
}

static void open_context_menu(gui_t *g, Vector2 mouse) {
    g->selected = g->hovered;
    g->menu_node = g->hovered;
    g->menu_pos = mouse;
    g->menu_open = true;
}

void canvas_update(gui_t *g) {
    Vector2 mouse = GetMousePosition();
    Vector2 world = GetScreenToWorld2D(mouse, g->camera);
    bool in_canvas = CheckCollisionPointRec(mouse, g->canvas);

    drop_palette_item(g, in_canvas, world);

    bool pan_held = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
                    (IsKeyDown(KEY_SPACE) && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui_text_focused());
    update_panning(g, pan_held);

    if (g->dragged.kind != NODE_NONE) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            move_dragged_node(g, world);
        } else {
            g->dragged.kind = NODE_NONE;
        }
        return;
    }

    g->hovered = NO_NODE;
    if (!in_canvas || !ui_input_free()) return;
    g->hovered = hit_test(g, world);

    zoom_at_mouse(g, mouse, world);

    bool pan_started = IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (pan_held && pan_started) {
        g->panning = true;
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) handle_left_click(g, world);
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && g->hovered.kind != NODE_NONE) open_context_menu(g, mouse);
}
