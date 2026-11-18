#include "gui_state.h"
#include <inttypes.h>
#include <stdio.h>

#define MEM_COLS        8
#define MEM_CELL_W      34
#define MEM_CELL_H      24
#define CHIP_W          44
#define CHIP_STEP       47
#define CORE_ROW_H      36.0f
#define FLASH_SECONDS   0.8
#define NODE_FLASH_S    0.4

static const Color DARK_FILL = {26, 29, 35, 255};
static const Color DARK_TEXT = {15, 15, 20, 255};

Color proc_color(int pid) {
    return ColorFromHSV((float)((pid * 67) % 360), 0.55f, 0.95f);
}

Color state_color(proc_state_t state) {
    switch (state) {
        case PROC_NEW:              return (Color){130, 135, 145, 255};
        case PROC_READY:            return (Color){ 80, 145, 255, 255};
        case PROC_RUNNING:          return (Color){ 80, 205, 115, 255};
        case PROC_BLOCKED_IO:       return (Color){240, 175,  60, 255};
        case PROC_BLOCKED_FAULT:    return (Color){235,  85,  85, 255};
        case PROC_BLOCKED_RESOURCE:
        case PROC_BLOCKED_IPC:      return (Color){190, 110, 230, 255};
        case PROC_BLOCKED_SEM:      return (Color){230,  95, 170, 255};
        case PROC_SLEEPING:         return (Color){ 70, 120, 140, 255};
        case PROC_TERMINATED:       return (Color){ 75,  80,  90, 255};
        default:                    return (Color){ 50,  50,  50, 255};
    }
}

static bool same_node(node_ref_t a, node_ref_t b) {
    return a.kind == b.kind && (a.kind != NODE_PROC || a.pid == b.pid);
}

const char *canvas_node_key(const gui_t *g, node_ref_t n) {
    if (n.kind == NODE_PROC) return g->sim->procs[n.pid].name;
    if (n.kind >= 0 && n.kind < NODE_SYS_COUNT) return g_sys_keys[n.kind];
    return NULL;
}

static Vector2 node_size(const gui_t *g, int kind) {
    const sim_t *s = g->sim;
    switch (kind) {
        case NODE_READY:  return (Vector2){250, 96};
        case NODE_SCHED:  return (Vector2){170, 84};
        case NODE_CPU:    return (Vector2){230, 40.0f + s->num_cores * CORE_ROW_H};
        case NODE_MEMORY: {
            int rows = (s->total_frames + MEM_COLS - 1) / MEM_COLS;
            return (Vector2){MEM_COLS * MEM_CELL_W + 16.0f, 52.0f + rows * (MEM_CELL_H + 2.0f)};
        }
        case NODE_DISK:   return (Vector2){250, 112};
        case NODE_PROC:   return (Vector2){160, 60};
        default:          return (Vector2){100, 60};
    }
}

Rectangle canvas_node_rect(const gui_t *g, node_ref_t n) {
    float x = 0, y = 0;
    const char *key = canvas_node_key(g, n);
    if (key) layout_get(&g->layout, key, &x, &y);

    Vector2 size = node_size(g, n.kind);
    return (Rectangle){x, y, size.x, size.y};
}

Vector2 canvas_node_center(const gui_t *g, node_ref_t n) {
    Rectangle r = canvas_node_rect(g, n);
    return (Vector2){r.x + r.width / 2, r.y + r.height / 2};
}

static void draw_chip(float x, float y, int pid, const sim_t *s, const char *suffix) {
    DrawRectangleRounded((Rectangle){x, y, CHIP_W, 20}, 0.4f, 4, ColorAlpha(proc_color(pid), 0.85f));

    char label[24];
    snprintf(label, sizeof(label), "%.4s%s", s->procs[pid].name, suffix ? suffix : "");
    ui_text_clipped((Rectangle){x + 4, y, 40, 20}, 13, (Color){20, 20, 24, 255}, label);
}

int gui_ready_queue_rows(const sim_t *s) {
    switch (s->sched_policy) {
        case SCHED_MLFQ:
        case SCHED_ADAPTIVE: return PRISM_MLFQ_QUEUES;
        case SCHED_SMP:      return s->num_cores;
        default:             return 1;
    }
}

const char *gui_ready_queue_label(const sim_t *s, int row) {
    return s->sched_policy == SCHED_SMP ? TextFormat("c%d", row) : TextFormat("Q%d", row);
}

static void draw_ready_body(const sim_t *s, Rectangle r, float x, float y) {
    int waiting = 0;
    for (int q = 0; q < PRISM_READY_QUEUES; q++) waiting += s->ready_count[q];
    ui_text(r.x + r.width - 70, r.y + 6, 13, UI_TEXT_DIM, "%d waiting", waiting);

    int rows = gui_ready_queue_rows(s);
    bool levels = rows > 1;
    float row_step = rows > PRISM_MLFQ_QUEUES ? 17 : levels ? 21 : 24;

    for (int q = 0; q < rows; q++) {
        float chip_x = x + (levels ? 24 : 0);
        if (levels) ui_text(x, y + 2, 13, UI_TEXT_DIM, "%s", gui_ready_queue_label(s, q));

        int shown = 0;
        for (int pid = s->ready_head[q]; pid != -1 && shown < 64; pid = s->procs[pid].next_proc, shown++) {
            bool no_room = chip_x + CHIP_W > r.x + r.width - 6;
            if (no_room) {
                ui_text(chip_x, y + 2, 13, UI_TEXT_DIM, "+");
                break;
            }
            draw_chip(chip_x, y, pid, s, NULL);
            chip_x += CHIP_STEP;
        }
        y += row_step;
    }

    if (waiting == 0 && !levels) ui_text(x, r.y + 52, 14, UI_TEXT_DIM, "empty");
}

static void draw_sched_body(const sim_t *s, float x, float y) {
    ui_text(x, y, 20, UI_TEXT, "%s", sched_get_ops(s->sched_policy)->name);
    ui_text(x, y + 26, 14, UI_TEXT_DIM, "quantum %u  |  %u switches", s->default_quantum, s->total_context_switches);
}

static float burst_progress(const pcb_t *p) {
    int idx = p->curr_burst_idx < p->num_bursts ? p->curr_burst_idx : 0;
    const burst_t *burst = &p->bursts[idx];
    return burst->duration ? 1.0f - (float)burst->remaining / burst->duration : 1;
}

static void draw_cpu_body(const sim_t *s, Rectangle r, float x, float y) {
    for (int c = 0; c < s->num_cores; c++) {
        Rectangle row = {x, y + c * CORE_ROW_H, r.width - 20, 30};
        DrawRectangleRounded(row, 0.2f, 4, DARK_FILL);

        int pid = s->running_pid[c];
        if (pid < 0) {
            ui_text(row.x + 8, row.y + 7, 14, UI_TEXT_DIM, "core %d  idle", c);
            continue;
        }

        const pcb_t *p = &s->procs[pid];
        DrawRectangleRounded((Rectangle){row.x, row.y, 6, row.height}, 0.5f, 4, proc_color(pid));
        ui_text(row.x + 12, row.y + 2, 14, UI_TEXT, "core %d  %s", c, p->name);
        DrawRectangle((int)row.x + 12, (int)row.y + 21, (int)((row.width - 20) * burst_progress(p)), 4, UI_GOOD);
    }
}

static void draw_fading_outline(Rectangle cell, double since, float thick, Color c) {
    if (since >= 0 && since < FLASH_SECONDS) {
        DrawRectangleLinesEx(cell, thick, ColorAlpha(c, (float)(1.0 - since / FLASH_SECONDS)));
    }
}

static void draw_frame_cell(const gui_t *g, int f, float x, float y) {
    const frame_entry_t *frame = &g->sim->frames[f];
    Rectangle cell = {
        x + (f % MEM_COLS) * (float)MEM_CELL_W,
        y + (f / MEM_COLS) * (MEM_CELL_H + 2.0f),
        MEM_CELL_W - 3, MEM_CELL_H
    };

    Color fill = frame->occupied ? ColorAlpha(proc_color(frame->owner_pid), 0.75f) : DARK_FILL;
    DrawRectangleRec(cell, fill);
    draw_fading_outline(cell, g->anim_now - g->frame_flash[f], 3, UI_BAD);
    draw_fading_outline(cell, g->anim_now - g->frame_load[f], 2, UI_GOOD);

    char label[16];
    if (frame->occupied) {
        snprintf(label, sizeof(label), "%d:%u", frame->owner_pid, frame->vpage);
    } else {
        snprintf(label, sizeof(label), "%d", f);
    }
    float w = ui_text_width(label, 12);
    ui_text(cell.x + (cell.width - w) / 2, cell.y + 6, 12, frame->occupied ? DARK_TEXT : UI_TEXT_DIM, "%s", label);
}

static void draw_memory_body(const gui_t *g, Rectangle r, float x, float y) {
    const sim_t *s = g->sim;
    ui_text(r.x + 100, r.y + 6, 13, UI_TEXT_DIM, "%s  %d frames  %u faults",
            repl_get_ops(s->repl_policy)->name, s->total_frames, s->total_page_faults);
    for (int f = 0; f < s->total_frames; f++) draw_frame_cell(g, f, x, y);
}

static void draw_disk_body(const sim_t *s, Rectangle r, float x, float y) {
    ui_text(r.x + 110, r.y + 6, 13, UI_TEXT_DIM, "latency %d", s->disk_latency);
    ui_text(x, y + 2, 13, UI_TEXT_DIM, "I/O");
    ui_text(x, y + 38, 13, UI_TEXT_DIM, "page-in");

    float io_x = x + 56, fault_x = x + 56;
    float right_limit = r.x + r.width - 50;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        char remaining[12];

        if (p->state == PROC_BLOCKED_IO && io_x < right_limit) {
            snprintf(remaining, sizeof(remaining), " %u", p->io_remaining);
            draw_chip(io_x, y, pid, s, remaining);
            io_x += CHIP_STEP;
        } else if (p->state == PROC_BLOCKED_FAULT && fault_x < right_limit) {
            snprintf(remaining, sizeof(remaining), " %u", p->fault_remaining);
            draw_chip(fault_x, y + 36, pid, s, remaining);
            fault_x += CHIP_STEP;
        }
    }
}

static void draw_sys_body(const gui_t *g, int kind, Rectangle r) {
    const sim_t *s = g->sim;
    float x = r.x + 10, y = r.y + 30;

    switch (kind) {
        case NODE_READY:  draw_ready_body(s, r, x, y); break;
        case NODE_SCHED:  draw_sched_body(s, x, y); break;
        case NODE_CPU:    draw_cpu_body(s, r, x, y); break;
        case NODE_MEMORY: draw_memory_body(g, r, x, y); break;
        case NODE_DISK:   draw_disk_body(s, r, x, y); break;
    }
}

static void cpu_progress(const pcb_t *p, uint32_t *done, uint32_t *total) {
    *done = *total = 0;
    for (int i = 0; i < p->num_bursts; i++) {
        const burst_t *burst = &p->bursts[i];
        if (burst->kind != BURST_CPU) continue;

        *total += burst->duration;
        bool finished = i < p->curr_burst_idx;
        *done += finished ? burst->duration : (uint32_t)(burst->duration - burst->remaining);
    }
    if (p->state == PROC_TERMINATED) *done = *total;
}

static void draw_proc_body(const gui_t *g, int pid, Rectangle r) {
    const pcb_t *p = &g->sim->procs[pid];
    Color color = state_color(p->state);

    DrawRectangleRounded((Rectangle){r.x, r.y, 7, r.height}, 0.5f, 4, proc_color(pid));
    ui_text(r.x + 14, r.y + 6, 16, p->state == PROC_TERMINATED ? UI_TEXT_DIM : UI_TEXT, "%s", p->name);
    ui_text(r.x + 14, r.y + 25, 13, color, "pid %d  %s", pid, proc_state_str(p->state));

    uint32_t done, total;
    cpu_progress(p, &done, &total);
    Rectangle bar = {r.x + 14, r.y + r.height - 13, r.width - 24, 5};
    DrawRectangleRec(bar, DARK_FILL);
    if (total) DrawRectangle((int)bar.x, (int)bar.y, (int)(bar.width * done / total), (int)bar.height, color);

    if (p->state == PROC_NEW) {
        ui_text(r.x + r.width - 52, r.y + 7, 12, UI_TEXT_DIM, "t=%" PRIu64, (uint64_t)p->arrival_tick);
    }
}

void canvas_draw_node(const gui_t *g, node_ref_t n) {
    Rectangle r = canvas_node_rect(g, n);
    bool selected = same_node(g->selected, n);
    bool hovered = same_node(g->hovered, n);

    DrawRectangleRounded((Rectangle){r.x + 4, r.y + 5, r.width, r.height}, 0.12f, 6, (Color){0, 0, 0, 70});
    DrawRectangleRounded(r, 0.12f, 6, UI_PANEL);

    if (n.kind == NODE_PROC) {
        draw_proc_body(g, n.pid, r);
    } else {
        DrawRectangleRounded((Rectangle){r.x, r.y, r.width, 24}, 0.4f, 6, UI_PANEL_HI);
        ui_text(r.x + 10, r.y + 5, 14, UI_TEXT, "%s", g_sys_titles[n.kind]);
        draw_sys_body(g, n.kind, r);

        double since_flash = g->anim_now - g->node_flash[n.kind];
        if (since_flash >= 0 && since_flash < NODE_FLASH_S) {
            DrawRectangleRoundedLinesEx(r, 0.12f, 6, 3, ColorAlpha(UI_ACCENT, (float)(1 - since_flash / NODE_FLASH_S)));
        }
    }

    Color border = selected ? UI_ACCENT : (hovered ? UI_TEXT_DIM : UI_BORDER);
    DrawRectangleRoundedLinesEx(r, 0.12f, 6, selected ? 2.5f : 1.0f, border);
}
