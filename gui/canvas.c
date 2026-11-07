#include "gui_state.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const g_sys_keys[NODE_SYS_COUNT] = { "ready", "sched", "cpu", "memory", "disk" };
const char *const g_sys_titles[NODE_SYS_COUNT] = { "Ready Queue", "Scheduler", "CPU", "Memory", "Disk / I-O" };

#define MEM_COLS 8
#define MEM_CW   34
#define MEM_CH   24

Color proc_color(int pid) {
    return ColorFromHSV((float)((pid * 67) % 360), 0.55f, 0.95f);
}

Color state_color(proc_state_t st) {
    switch (st) {
        case PROC_NEW:            return (Color){130, 135, 145, 255};
        case PROC_READY:          return (Color){ 80, 145, 255, 255};
        case PROC_RUNNING:        return (Color){ 80, 205, 115, 255};
        case PROC_BLOCKED_IO:     return (Color){240, 175,  60, 255};
        case PROC_BLOCKED_FAULT:  return (Color){235,  85,  85, 255};
        case PROC_BLOCKED_RESOURCE:
        case PROC_BLOCKED_IPC:    return (Color){190, 110, 230, 255};
        case PROC_TERMINATED:     return (Color){ 75,  80,  90, 255};
        default:                  return (Color){ 50,  50,  50, 255};
    }
}

static bool proc_live(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

static const char *node_key(const gui_t *g, node_ref_t n) {
    if (n.kind == NODE_PROC) return g->sim->procs[n.pid].name;
    if (n.kind >= 0 && n.kind < NODE_SYS_COUNT) return g_sys_keys[n.kind];
    return NULL;
}

static Vector2 node_size(const gui_t *g, int kind) {
    const sim_t *s = g->sim;
    switch (kind) {
        case NODE_READY:  return (Vector2){250, 96};
        case NODE_SCHED:  return (Vector2){170, 84};
        case NODE_CPU:    return (Vector2){230, 40.0f + s->num_cores * 36.0f};
        case NODE_MEMORY: {
            int rows = (s->total_frames + MEM_COLS - 1) / MEM_COLS;
            return (Vector2){MEM_COLS * MEM_CW + 16.0f, 52.0f + rows * (MEM_CH + 2.0f)};
        }
        case NODE_DISK:   return (Vector2){250, 112};
        case NODE_PROC:   return (Vector2){160, 60};
        default:          return (Vector2){100, 60};
    }
}

Rectangle canvas_node_rect(const gui_t *g, node_ref_t n) {
    float x = 0, y = 0;
    const char *key = node_key(g, n);
    if (key) layout_get(&g->layout, key, &x, &y);
    Vector2 sz = node_size(g, n.kind);
    return (Rectangle){x, y, sz.x, sz.y};
}

Vector2 canvas_node_center(const gui_t *g, node_ref_t n) {
    Rectangle r = canvas_node_rect(g, n);
    return (Vector2){r.x + r.width / 2, r.y + r.height / 2};
}

void canvas_place_process(gui_t *g, int pid) {
    const char *key = g->sim->procs[pid].name;
    if (layout_get(&g->layout, key, NULL, NULL)) return;
    /* First free slot in the process column. */
    for (int slot = 0; slot < 200; slot++) {
        float x = 20, y = 20 + slot * 72.0f;
        if (slot >= 8) { x = -170; y = 20 + (slot - 8) * 72.0f; }
        bool taken = false;
        for (int i = 0; i < g->layout.count; i++) {
            const layout_node_t *ln = &g->layout.nodes[i];
            if (fabsf(ln->x - x) < 40 && fabsf(ln->y - y) < 40) { taken = true; break; }
        }
        if (!taken) { layout_set(&g->layout, key, x, y); return; }
    }
    layout_set(&g->layout, key, 40, 40);
}

void canvas_default_layout(gui_t *g) {
    static const Vector2 pos[NODE_SYS_COUNT] = {
        {290, 150}, {590, 156}, {810, 130}, {770, 360}, {420, 380}
    };
    for (int k = 0; k < NODE_SYS_COUNT; k++) {
        if (!layout_get(&g->layout, g_sys_keys[k], NULL, NULL)) layout_set(&g->layout, g_sys_keys[k], pos[k].x, pos[k].y);
    }
    for (int pid = 0; pid < g->sim->num_processes; pid++) {
        if (proc_live(g->sim, pid)) canvas_place_process(g, pid);
    }
}

/* ------------------------------------------------------------------------
 * Geometry helpers
 * ------------------------------------------------------------------------ */

static Vector2 rect_edge(Rectangle r, Vector2 toward) {
    Vector2 c = {r.x + r.width / 2, r.y + r.height / 2};
    float dx = toward.x - c.x, dy = toward.y - c.y;
    if (dx == 0 && dy == 0) return c;
    float sx = (r.width / 2) / (fabsf(dx) + 1e-6f), sy = (r.height / 2) / (fabsf(dy) + 1e-6f);
    float t = sx < sy ? sx : sy;
    return (Vector2){c.x + dx * t, c.y + dy * t};
}

static void draw_arrow(Vector2 a, Vector2 b, float thick, Color c, bool dashed, bool head_a) {
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    float ux = dx / len, uy = dy / len;
    if (dashed) {
        for (float t = 0; t < len - 10; t += 14) {
            float t2 = (t + 8 < len - 10) ? t + 8 : len - 10;
            DrawLineEx((Vector2){a.x + ux * t, a.y + uy * t}, (Vector2){a.x + ux * t2, a.y + uy * t2}, thick, c);
        }
    } else {
        DrawLineEx(a, (Vector2){b.x - ux * 8, b.y - uy * 8}, thick, c);
    }
    Vector2 p1 = {b.x - ux * 12 - uy * 6, b.y - uy * 12 + ux * 6};
    Vector2 p2 = {b.x - ux * 12 + uy * 6, b.y - uy * 12 - ux * 6};
    DrawTriangle(b, p2, p1, c);
    DrawTriangle(b, p1, p2, c);
    if (head_a) draw_arrow(b, a, thick, c, dashed, false);
}

static void link_nodes(const gui_t *g, node_ref_t a, node_ref_t b, Color c, float thick, bool dashed, bool both) {
    Rectangle ra = canvas_node_rect(g, a), rb = canvas_node_rect(g, b);
    Vector2 ca = canvas_node_center(g, a), cb = canvas_node_center(g, b);
    draw_arrow(rect_edge(ra, cb), rect_edge(rb, ca), thick, c, dashed, both);
}

/* ------------------------------------------------------------------------
 * Node drawing
 * ------------------------------------------------------------------------ */

static void chip(float x, float y, int pid, const sim_t *s, const char *extra) {
    Color c = proc_color(pid);
    Rectangle r = {x, y, 44, 20};
    DrawRectangleRounded(r, 0.4f, 4, ColorAlpha(c, 0.85f));
    char buf[24];
    snprintf(buf, sizeof(buf), "%.4s%s", s->procs[pid].name, extra ? extra : "");
    ui_text_clipped((Rectangle){x + 4, y, 40, 20}, 13, (Color){20, 20, 24, 255}, buf);
}

static void draw_sys_body(const gui_t *g, int kind, Rectangle r) {
    const sim_t *s = g->sim;
    float x = r.x + 10, y = r.y + 30;
    switch (kind) {
        case NODE_READY: {
            int total = 0;
            for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) total += s->ready_count[q];
            ui_text(r.x + r.width - 70, r.y + 6, 13, UI_TEXT_DIM, "%d waiting", total);
            bool levels = (s->sched_policy == SCHED_MLFQ || s->sched_policy == SCHED_ADAPTIVE);
            int nq = levels ? PRISM_MLFQ_QUEUES : 1;
            for (int q = 0; q < nq; q++) {
                float cx = x + (levels ? 24 : 0);
                if (levels) ui_text(x, y + 2, 13, UI_TEXT_DIM, "Q%d", q);
                int shown = 0;
                for (int pid = s->ready_head[q]; pid != -1 && shown < 64; pid = s->procs[pid].next_proc, shown++) {
                    if (cx + 44 > r.x + r.width - 6) { ui_text(cx, y + 2, 13, UI_TEXT_DIM, "+"); break; }
                    chip(cx, y, pid, s, NULL);
                    cx += 47;
                }
                y += levels ? 21 : 24;
            }
            if (total == 0 && !levels) ui_text(x, r.y + 52, 14, UI_TEXT_DIM, "empty");
            break;
        }
        case NODE_SCHED: {
            const sched_ops_t *ops = sched_get_ops(s->sched_policy);
            ui_text(x, y, 20, UI_TEXT, "%s", ops->name);
            ui_text(x, y + 26, 14, UI_TEXT_DIM, "quantum %u  |  %u switches", s->default_quantum, s->total_context_switches);
            break;
        }
        case NODE_CPU: {
            for (int c = 0; c < s->num_cores; c++) {
                Rectangle row = {x, y + c * 36.0f, r.width - 20, 30};
                DrawRectangleRounded(row, 0.2f, 4, (Color){26, 29, 35, 255});
                int pid = s->running_pid[c];
                if (pid < 0) {
                    ui_text(row.x + 8, row.y + 7, 14, UI_TEXT_DIM, "core %d  idle", c);
                } else {
                    const pcb_t *p = &s->procs[pid];
                    DrawRectangleRounded((Rectangle){row.x, row.y, 6, row.height}, 0.5f, 4, proc_color(pid));
                    ui_text(row.x + 12, row.y + 2, 14, UI_TEXT, "core %d  %s", c, p->name);
                    const burst_t *b = &p->bursts[p->curr_burst_idx < p->num_bursts ? p->curr_burst_idx : 0];
                    float done = b->duration ? 1.0f - (float)b->remaining / b->duration : 1;
                    DrawRectangle((int)row.x + 12, (int)row.y + 21, (int)((row.width - 20) * done), 4, UI_GOOD);
                }
            }
            break;
        }
        case NODE_MEMORY: {
            ui_text(r.x + 100, r.y + 6, 13, UI_TEXT_DIM, "%s  %d frames  %u faults", repl_get_ops(s->repl_policy)->name,
                    s->total_frames, s->total_page_faults);
            for (int f = 0; f < s->total_frames; f++) {
                Rectangle cell = {x + (f % MEM_COLS) * (float)MEM_CW, y + (f / MEM_COLS) * (MEM_CH + 2.0f), MEM_CW - 3, MEM_CH};
                const frame_entry_t *fe = &s->frames[f];
                Color bg = fe->occupied ? ColorAlpha(proc_color(fe->owner_pid), 0.75f) : (Color){26, 29, 35, 255};
                DrawRectangleRec(cell, bg);
                double ev = g->anim_now - g->frame_flash[f];
                if (ev >= 0 && ev < 0.8) DrawRectangleLinesEx(cell, 3, ColorAlpha(UI_BAD, (float)(1.0 - ev / 0.8)));
                double ld = g->anim_now - g->frame_load[f];
                if (ld >= 0 && ld < 0.8) DrawRectangleLinesEx(cell, 2, ColorAlpha(UI_GOOD, (float)(1.0 - ld / 0.8)));
                char buf[16];
                if (fe->occupied) snprintf(buf, sizeof(buf), "%d:%u", fe->owner_pid, fe->vpage);
                else snprintf(buf, sizeof(buf), "%d", f);
                float w = ui_text_width(buf, 12);
                ui_text(cell.x + (cell.width - w) / 2, cell.y + 6, 12, fe->occupied ? (Color){15, 15, 20, 255} : UI_TEXT_DIM, "%s", buf);
            }
            break;
        }
        case NODE_DISK: {
            ui_text(r.x + 110, r.y + 6, 13, UI_TEXT_DIM, "latency %d", s->disk_latency);
            ui_text(x, y + 2, 13, UI_TEXT_DIM, "I/O");
            ui_text(x, y + 38, 13, UI_TEXT_DIM, "page-in");
            float cx1 = x + 56, cx2 = x + 56;
            for (int pid = 0; pid < s->num_processes; pid++) {
                char rem[12];
                const pcb_t *p = &s->procs[pid];
                if (p->state == PROC_BLOCKED_IO && cx1 < r.x + r.width - 50) {
                    snprintf(rem, sizeof(rem), " %u", p->io_remaining);
                    chip(cx1, y, pid, s, rem); cx1 += 47;
                } else if (p->state == PROC_BLOCKED_FAULT && cx2 < r.x + r.width - 50) {
                    snprintf(rem, sizeof(rem), " %u", p->fault_remaining);
                    chip(cx2, y + 36, pid, s, rem); cx2 += 47;
                }
            }
            break;
        }
    }
}

static void draw_proc_body(const gui_t *g, int pid, Rectangle r) {
    const pcb_t *p = &g->sim->procs[pid];
    Color sc = state_color(p->state);
    DrawRectangleRounded((Rectangle){r.x, r.y, 7, r.height}, 0.5f, 4, proc_color(pid));
    ui_text(r.x + 14, r.y + 6, 16, p->state == PROC_TERMINATED ? UI_TEXT_DIM : UI_TEXT, "%s", p->name);
    ui_text(r.x + 14, r.y + 25, 13, sc, "pid %d  %s", pid, proc_state_str(p->state));
    uint32_t total = 0, done = 0;
    for (int i = 0; i < p->num_bursts; i++) {
        if (p->bursts[i].kind != BURST_CPU) continue;
        total += p->bursts[i].duration;
        done += (i < p->curr_burst_idx) ? p->bursts[i].duration : (uint32_t)(p->bursts[i].duration - p->bursts[i].remaining);
    }
    if (p->state == PROC_TERMINATED) done = total;
    Rectangle bar = {r.x + 14, r.y + r.height - 13, r.width - 24, 5};
    DrawRectangleRec(bar, (Color){26, 29, 35, 255});
    if (total) DrawRectangle((int)bar.x, (int)bar.y, (int)(bar.width * done / total), (int)bar.height, sc);
    if (p->state == PROC_NEW) ui_text(r.x + r.width - 52, r.y + 7, 12, UI_TEXT_DIM, "t=%" PRIu64, (uint64_t)p->arrival_tick);
}

static void draw_node(const gui_t *g, node_ref_t n) {
    Rectangle r = canvas_node_rect(g, n);
    bool sel = (g->sel.kind == n.kind && (n.kind != NODE_PROC || g->sel.pid == n.pid));
    bool hov = (g->hover.kind == n.kind && (n.kind != NODE_PROC || g->hover.pid == n.pid));
    DrawRectangleRounded((Rectangle){r.x + 4, r.y + 5, r.width, r.height}, 0.12f, 6, (Color){0, 0, 0, 70});
    DrawRectangleRounded(r, 0.12f, 6, UI_PANEL);
    if (n.kind != NODE_PROC) {
        DrawRectangleRounded((Rectangle){r.x, r.y, r.width, 24}, 0.4f, 6, UI_PANEL_HI);
        ui_text(r.x + 10, r.y + 5, 14, UI_TEXT, "%s", g_sys_titles[n.kind]);
        draw_sys_body(g, n.kind, r);
        double fl = g->anim_now - g->node_flash[n.kind];
        if (fl >= 0 && fl < 0.4) DrawRectangleRoundedLinesEx(r, 0.12f, 6, 3, ColorAlpha(UI_ACCENT, (float)(1 - fl / 0.4)));
    } else {
        draw_proc_body(g, n.pid, r);
    }
    Color border = sel ? UI_ACCENT : (hov ? UI_TEXT_DIM : UI_BORDER);
    DrawRectangleRoundedLinesEx(r, 0.12f, 6, sel ? 2.5f : 1.0f, border);
}

/* ------------------------------------------------------------------------ */

bool gui_has_node(const gui_t *g, int kind) {
    return layout_get(&g->layout, g_sys_keys[kind], NULL, NULL);
}

static void draw_grid(const gui_t *g) {
    Vector2 tl = GetScreenToWorld2D((Vector2){g->canvas.x, g->canvas.y}, g->cam);
    Vector2 br = GetScreenToWorld2D((Vector2){g->canvas.x + g->canvas.width, g->canvas.y + g->canvas.height}, g->cam);
    float step = 40;
    Color c = (Color){34, 38, 46, 255};
    for (float x = floorf(tl.x / step) * step; x < br.x; x += step) DrawLineV((Vector2){x, tl.y}, (Vector2){x, br.y}, c);
    for (float y = floorf(tl.y / step) * step; y < br.y; y += step) DrawLineV((Vector2){tl.x, y}, (Vector2){br.x, y}, c);
}

void canvas_draw(gui_t *g) {
    const sim_t *s = g->sim;
    ui_scissor_push(g->canvas);
    DrawRectangleRec(g->canvas, UI_BG);
    BeginMode2D(g->cam);
    draw_grid(g);

    node_ref_t R = {NODE_READY, 0}, S = {NODE_SCHED, 0}, C = {NODE_CPU, 0}, M = {NODE_MEMORY, 0}, D = {NODE_DISK, 0};
    bool hr = gui_has_node(g, NODE_READY), hs = gui_has_node(g, NODE_SCHED), hc = gui_has_node(g, NODE_CPU);
    bool hm = gui_has_node(g, NODE_MEMORY), hd = gui_has_node(g, NODE_DISK);
    Color lc = (Color){95, 105, 125, 255};
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (!proc_live(s, pid) || !hr) continue;
        node_ref_t P = {NODE_PROC, pid};
        Color pc = s->procs[pid].state == PROC_NEW ? (Color){70, 76, 90, 255} : ColorAlpha(proc_color(pid), 0.45f);
        link_nodes(g, P, R, pc, 1.5f, s->procs[pid].state == PROC_NEW, false);
    }
    if (hr && hs) link_nodes(g, R, S, lc, 2.5f, false, false);
    if (hs && hc) link_nodes(g, S, C, lc, 2.5f, false, false);
    if (hc && hd) link_nodes(g, C, D, lc, 2.5f, false, false);
    if (hd && hr) link_nodes(g, D, R, lc, 2.5f, false, false);
    if (hc && hm) link_nodes(g, C, M, lc, 2.5f, false, true);
    if (hc && hr) link_nodes(g, C, R, (Color){70, 78, 95, 255}, 1.5f, true, false);   /* preempt */

    for (int k = 0; k < NODE_SYS_COUNT; k++) {
        if (gui_has_node(g, k)) draw_node(g, (node_ref_t){k, 0});
    }
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (proc_live(s, pid)) draw_node(g, (node_ref_t){NODE_PROC, pid});
    }
    anim_draw(g);
    EndMode2D();

    /* Validation banner */
    char msg[160];
    if (!gui_validate(g, msg, sizeof(msg))) {
        Rectangle b = {g->canvas.x + 12, g->canvas.y + 10, ui_text_width(msg, 15) + 24, 28};
        DrawRectangleRounded(b, 0.3f, 4, ColorAlpha(UI_WARN, 0.18f));
        DrawRectangleRoundedLinesEx(b, 0.3f, 4, 1, UI_WARN);
        ui_text(b.x + 12, b.y + 6, 15, UI_WARN, "%s", msg);
    }
    const char *mode = g->mode == MODE_EDIT ? "EDIT MODE: build the scenario" : (g->playing ? "RUN MODE: playing" : "RUN MODE: paused");
    ui_text(g->canvas.x + g->canvas.width - ui_text_width(mode, 14) - 14, g->canvas.y + g->canvas.height - 22, 14,
            g->mode == MODE_EDIT ? UI_TEXT_DIM : UI_GOOD, "%s", mode);
    if (g->palette_drag >= 0) {
        Vector2 m = GetMousePosition();
        const char *t = g->palette_drag == NODE_PROC ? "Process" : g_sys_titles[g->palette_drag];
        DrawRectangleRounded((Rectangle){m.x - 60, m.y - 16, 120, 32}, 0.3f, 4, ColorAlpha(UI_ACCENT, 0.5f));
        ui_text(m.x - ui_text_width(t, 15) / 2, m.y - 8, 15, UI_TEXT, "%s", t);
    }
    ui_scissor_pop();
}

/* ------------------------------------------------------------------------
 * Interaction
 * ------------------------------------------------------------------------ */

static node_ref_t hit_test(const gui_t *g, Vector2 w) {
    const sim_t *s = g->sim;
    for (int pid = s->num_processes - 1; pid >= 0; pid--) {
        node_ref_t n = {NODE_PROC, pid};
        if (proc_live(s, pid) && CheckCollisionPointRec(w, canvas_node_rect(g, n))) return n;
    }
    for (int k = NODE_SYS_COUNT - 1; k >= 0; k--) {
        node_ref_t n = {k, 0};
        if (gui_has_node(g, k) && CheckCollisionPointRec(w, canvas_node_rect(g, n))) return n;
    }
    return (node_ref_t){NODE_NONE, 0};
}

void canvas_update(gui_t *g) {
    Vector2 mouse = GetMousePosition();
    bool in_canvas = CheckCollisionPointRec(mouse, g->canvas);
    Vector2 world = GetScreenToWorld2D(mouse, g->cam);

    /* Drop from palette */
    if (g->palette_drag >= 0 && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (in_canvas) {
            if (g->palette_drag == NODE_PROC) builder_open_new(g, g->mode == MODE_RUN);
            else gui_add_sys_node(g, g->palette_drag, world);
        }
        g->palette_drag = -1;
    }

    /* Panning keeps working while the mouse is over a node. */
    bool pan_btn = IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
                   (IsKeyDown(KEY_SPACE) && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ui_text_focused());
    if (g->panning && pan_btn) {
        Vector2 d = GetMouseDelta();
        g->cam.target.x -= d.x / g->cam.zoom;
        g->cam.target.y -= d.y / g->cam.zoom;
    } else {
        g->panning = false;
    }

    if (g->drag.kind != NODE_NONE) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float x = world.x - g->drag_off.x, y = world.y - g->drag_off.y;
            if (g->snap) { x = roundf(x / 20) * 20; y = roundf(y / 20) * 20; }
            const char *key = (g->drag.kind == NODE_PROC) ? g->sim->procs[g->drag.pid].name : g_sys_keys[g->drag.kind];
            layout_set(&g->layout, key, x, y);
        } else {
            g->drag.kind = NODE_NONE;
        }
        return;
    }

    g->hover = (node_ref_t){NODE_NONE, 0};
    if (!in_canvas || !ui_input_free()) return;
    g->hover = hit_test(g, world);

    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        float z = g->cam.zoom * (wheel > 0 ? 1.1f : 1 / 1.1f);
        if (z < 0.3f) z = 0.3f;
        if (z > 2.5f) z = 2.5f;
        g->cam.offset = mouse;
        g->cam.target = world;
        g->cam.zoom = z;
    }

    if (pan_btn && (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))) {
        g->panning = true;
        return;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g->sel = g->hover;
        g->ctx_open = false;
        if (g->hover.kind != NODE_NONE) {
            Rectangle r = canvas_node_rect(g, g->hover);
            g->drag = g->hover;
            g->drag_off = (Vector2){world.x - r.x, world.y - r.y};
            static double last_click;
            static node_ref_t last_node = {NODE_NONE, 0};
            double now = GetTime();
            if (g->hover.kind == NODE_PROC && last_node.kind == NODE_PROC && last_node.pid == g->hover.pid &&
                now - last_click < 0.45 && g->mode == MODE_EDIT) {
                builder_open_edit(g, g->hover.pid);
                g->drag.kind = NODE_NONE;
            }
            last_click = now;
            last_node = g->hover;
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && g->hover.kind != NODE_NONE) {
        g->sel = g->hover;
        g->ctx_node = g->hover;
        g->ctx_pos = mouse;
        g->ctx_open = true;
    }
}
