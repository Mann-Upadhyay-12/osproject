#include "gui_state.h"
#include "prism/metrics.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define AXIS_H           18
#define MAX_LANE_H       34
#define MIN_TICK_PX_CPU  6
#define MIN_TICK_PX_ROW  4
#define MIN_VISIBLE_SPAN 20
#define TABLE_COLUMNS    10

static const Color LANE_BG   = {26, 29, 35, 255};
static const Color DARK_TEXT = {15, 15, 20, 255};

static int8_t lane_at(const lanes_t *L, uint32_t tick, int core) {
    return L->cells[(size_t)(tick % L->ring) * (size_t)L->stride + (size_t)core];
}

static uint32_t axis_step(float px_per_tick) {
    static const uint32_t steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (steps[i] * px_per_tick >= 36) return steps[i];
    }
    return 10000;
}

static void draw_axis(float x0, float y, float px_per_tick, uint32_t first, uint32_t span) {
    uint32_t step = axis_step(px_per_tick);
    uint32_t first_label = (first + step - 1) / step * step;

    for (uint32_t t = first_label; t <= first + span; t += step) {
        float x = x0 + (t - first) * px_per_tick;
        DrawLine((int)x, (int)y, (int)x, (int)y + 4, UI_TEXT_DIM);

        char label[16];
        snprintf(label, sizeof(label), "%u", t);
        ui_text(x - ui_text_width(label, 11) / 2, y + 5, 11, UI_TEXT_DIM, "%s", label);
    }
}

static void draw_run_block(Rectangle block, int pid, const char *name) {
    DrawRectangleRec(block, ColorAlpha(proc_color(pid), 0.85f));

    float text_w = ui_text_width(name, 12);
    if (block.width > text_w + 6) {
        ui_text(block.x + (block.width - text_w) / 2, block.y + block.height / 2 - 7, 12, DARK_TEXT, "%s", name);
    }
    DrawLine((int)block.x, (int)block.y, (int)block.x, (int)(block.y + block.height), DARK_TEXT);
}

int gantt_draw(Rectangle r, const lanes_t *L, uint32_t first, uint32_t span, pid_name_fn name, void *user,
               bool axis, int64_t cursor) {
    if (span == 0) span = 1;

    float label_w = L->ncores > 1 ? 34 : 0;
    float axis_h = axis ? AXIS_H : 0;
    float x0 = r.x + label_w;
    float plot_w = r.width - label_w;
    float px_per_tick = plot_w / (float)span;
    float lane_h = (r.height - axis_h) / (float)(L->ncores > 0 ? L->ncores : 1);
    if (lane_h > MAX_LANE_H) lane_h = MAX_LANE_H;

    Vector2 mouse = GetMousePosition();
    int hovered_tick = -1;
    uint32_t end = first + span < L->len ? first + span : L->len;

    for (int core = 0; core < L->ncores; core++) {
        float y = r.y + core * lane_h;
        if (L->ncores > 1) ui_text(r.x, y + lane_h / 2 - 7, 12, UI_TEXT_DIM, "cpu%d", core);
        DrawRectangle((int)x0, (int)y + 1, (int)plot_w, (int)lane_h - 2, LANE_BG);

        uint32_t start = first;
        while (start < end) {
            int pid = lane_at(L, start, core);
            uint32_t stop = start + 1;
            while (stop < end && lane_at(L, stop, core) == pid) stop++;

            Rectangle block = {x0 + (start - first) * px_per_tick, y + 1, (stop - start) * px_per_tick, lane_h - 2};
            const char *pid_name = pid >= 0 && name ? name(pid, user) : "";
            if (pid >= 0) draw_run_block(block, pid, pid_name);

            if (CheckCollisionPointRec(mouse, block)) {
                hovered_tick = (int)(first + (uint32_t)((mouse.x - x0) / px_per_tick));
                if (pid >= 0) {
                    ui_tooltip("%s on cpu%d\nticks %u..%u (%u ticks)", pid_name, core, start, stop, stop - start);
                } else {
                    ui_tooltip("cpu%d idle\nticks %u..%u", core, start, stop);
                }
            }
            start = stop;
        }
    }

    float lanes_bottom = r.y + L->ncores * lane_h;
    if (axis) draw_axis(x0, lanes_bottom, px_per_tick, first, span);

    bool cursor_visible = cursor >= (int64_t)first && cursor <= (int64_t)(first + span);
    if (cursor_visible) {
        float cx = x0 + (float)(cursor - (int64_t)first) * px_per_tick;
        DrawLineEx((Vector2){cx, r.y}, (Vector2){cx, lanes_bottom}, 2, UI_TEXT);
    }
    return hovered_tick;
}

static const char *sim_process_name(int pid, void *user) {
    const sim_t *s = (const sim_t *)user;
    return pid >= 0 && pid < s->num_processes ? s->procs[pid].name : "?";
}

static void visible_window(const gui_t *g, float width, float min_px_per_tick, uint32_t *first, uint32_t *span) {
    uint32_t recorded = g->history_len < GUI_HIST_TICKS ? g->history_len : GUI_HIST_TICKS;
    uint32_t oldest = g->history_len - recorded;
    uint32_t fits = (uint32_t)(width / min_px_per_tick);

    uint32_t wanted = g->history_len - oldest;
    if (wanted < MIN_VISIBLE_SPAN) wanted = MIN_VISIBLE_SPAN;
    if (wanted > fits) wanted = fits;

    *span = wanted;
    *first = g->history_len > wanted ? g->history_len - wanted : 0;
    if (*first < oldest) *first = oldest;
}

static void seek_on_click(gui_t *g, int hovered_tick) {
    if (hovered_tick < 0) return;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui_content_input()) {
        g->playing = false;
        gui_seek(g, (tick_t)hovered_tick + 1);
    }
}

void panel_cpu_gantt(gui_t *g, Rectangle body) {
    if (g->history_len == 0) {
        ui_text_wrapped(body.x + 10, body.y + 8, body.width - 20, 14, UI_TEXT_DIM,
                        "The CPU Gantt chart fills in as the simulation runs: which process held each core at every tick. "
                        "Click a tick to jump back to it.");
        return;
    }

    int cores = g->sim->num_cores;
    Rectangle chart = {body.x + 8, body.y + 8, body.width - 16, body.height - 16};
    lanes_t lanes = {&g->core_history[0][0], GUI_HIST_TICKS, PRISM_MAX_CORES, cores, g->history_len};

    uint32_t first, span;
    visible_window(g, chart.width, MIN_TICK_PX_CPU, &first, &span);

    float lane_h = (chart.height - AXIS_H) / cores;
    if (lane_h > MAX_LANE_H) lane_h = MAX_LANE_H;
    chart.height = lane_h * cores + AXIS_H;

    int hovered = gantt_draw(chart, &lanes, first, span, sim_process_name, g->sim, true, (int64_t)g->history_len);
    seek_on_click(g, hovered);

    float y = chart.y + chart.height + 6;
    if (y + 16 < body.y + body.height) {
        sim_metrics_t m;
        sim_metrics_compute(g->sim, &m);
        ui_text(body.x + 8, y, 12, UI_TEXT_DIM, "CPU busy %.0f%%  |  %u dispatches  |  idle = dark gaps  |  click to rewind",
                m.cpu_util, m.ctx_switches);
    }
}

static bool state_is_drawn(uint8_t state) {
    return state != PROC_NEW && state != PROC_UNUSED && state != PROC_TERMINATED;
}

static int live_processes(const sim_t *s, int out[PRISM_MAX_PROCESSES]) {
    int count = 0;
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (s->procs[pid].state != PROC_UNUSED) out[count++] = pid;
    }
    return count;
}

void panel_states(gui_t *g, Rectangle body) {
    const sim_t *s = g->sim;
    Rectangle area = {body.x + 8, body.y + 6, body.width - 16, body.height - 12};
    float label_w = 70, plot_w = area.width - label_w;

    int live[PRISM_MAX_PROCESSES];
    int num_live = live_processes(s, live);
    if (num_live == 0) {
        ui_text(area.x, area.y, 14, UI_TEXT_DIM, "No processes.");
        return;
    }

    float rows_h = area.height - AXIS_H;
    float row_h = rows_h / num_live;
    if (row_h > 18) row_h = 18;
    if (row_h < 6) row_h = 6;
    int visible_rows = (int)(rows_h / row_h);

    uint32_t first, span;
    visible_window(g, plot_w, MIN_TICK_PX_ROW, &first, &span);
    float px_per_tick = plot_w / (float)span;
    Vector2 mouse = GetMousePosition();
    int hovered_tick = -1;

    ui_scissor_push(area);
    for (int row = 0; row < num_live && row < visible_rows; row++) {
        int pid = live[row];
        float y = area.y + row * row_h;
        ui_text_clipped((Rectangle){area.x, y, label_w - 6, row_h}, row_h > 14 ? 13 : 10, proc_color(pid), s->procs[pid].name);

        for (uint32_t t = first; t < g->history_len; t++) {
            uint8_t state = g->state_history[t % GUI_HIST_TICKS][pid];
            if (!state_is_drawn(state)) continue;

            Rectangle cell = {area.x + label_w + (t - first) * px_per_tick, y + 1, px_per_tick + 0.5f, row_h - 2};
            DrawRectangleRec(cell, state_color((proc_state_t)state));
            if (CheckCollisionPointRec(mouse, cell)) {
                hovered_tick = (int)t;
                ui_tooltip("%s at tick %u: %s", s->procs[pid].name, t, proc_state_str((proc_state_t)state));
            }
        }
    }

    int rows_drawn = num_live < visible_rows ? num_live : visible_rows;
    float axis_y = area.y + rows_drawn * row_h;
    draw_axis(area.x + label_w, axis_y, px_per_tick, first, span);

    float cursor_x = area.x + label_w + (g->history_len - first) * px_per_tick;
    DrawLine((int)cursor_x, (int)area.y, (int)cursor_x, (int)axis_y, UI_TEXT);
    ui_scissor_pop();

    seek_on_click(g, hovered_tick);
}

typedef struct {
    int    completed;
    double turnaround;
    double waiting;
    double response;
} table_totals_t;

static void burst_totals(const pcb_t *p, uint32_t *cpu, uint32_t *io) {
    *cpu = *io = 0;
    for (int i = 0; i < p->num_bursts; i++) {
        if (p->bursts[i].kind == BURST_CPU) *cpu += p->bursts[i].duration;
        else if (p->bursts[i].kind == BURST_IO) *io += p->bursts[i].duration;
    }
}

static void format_process_row(const pcb_t *p, bool finished, char cells[TABLE_COLUMNS][24]) {
    uint32_t cpu, io;
    burst_totals(p, &cpu, &io);
    bool started = p->start_tick >= 0;

    snprintf(cells[0], 24, "%s", p->name);
    snprintf(cells[1], 24, "%" PRIu64, (uint64_t)p->arrival_tick);
    snprintf(cells[2], 24, "%u", cpu);
    snprintf(cells[3], 24, "%u", io);
    snprintf(cells[4], 24, started ? "%" PRId64 : "-", p->start_tick);
    snprintf(cells[5], 24, finished ? "%" PRId64 : "-", p->finish_tick);
    snprintf(cells[6], 24, finished ? "%" PRId64 : "-", p->finish_tick - (int64_t)p->arrival_tick);
    snprintf(cells[7], 24, "%u", p->total_wait_time);
    snprintf(cells[8], 24, started ? "%" PRId64 : "-", p->start_tick - (int64_t)p->arrival_tick);
    snprintf(cells[9], 24, "%u", p->num_page_faults);
}

static void add_to_totals(table_totals_t *totals, const pcb_t *p) {
    totals->completed++;
    totals->turnaround += (double)(p->finish_tick - (int64_t)p->arrival_tick);
    totals->waiting += p->total_wait_time;
    totals->response += (double)(p->start_tick - (int64_t)p->arrival_tick);
}

void panel_proc_table(gui_t *g, Rectangle body) {
    static const char *headers[TABLE_COLUMNS] = {
        "Process", "Arrive", "CPU", "I/O", "Start", "Finish", "Turnaround", "Waiting", "Response", "Faults"
    };
    static const float widths[TABLE_COLUMNS] = {84, 52, 44, 40, 48, 52, 82, 62, 70, 50};
    const sim_t *s = g->sim;

    float total_w = 0;
    for (int i = 0; i < TABLE_COLUMNS; i++) total_w += widths[i];
    float scale = (body.width - 16) / total_w;
    if (scale > 1.4f) scale = 1.4f;

    float x0 = body.x + 8, y = body.y + 6, x = x0;
    for (int i = 0; i < TABLE_COLUMNS; i++) {
        ui_text(x, y, 12, UI_TEXT_DIM, "%s", headers[i]);
        x += widths[i] * scale;
    }
    y += 17;
    DrawLine((int)x0, (int)y - 2, (int)(x0 + total_w * scale), (int)y - 2, UI_BORDER);

    table_totals_t totals = {0};
    float bottom = body.y + body.height - 40;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state == PROC_UNUSED) continue;

        bool finished = p->state == PROC_TERMINATED && p->finish_tick >= 0;
        if (finished) add_to_totals(&totals, p);
        if (y >= bottom) continue;

        char cells[TABLE_COLUMNS][24];
        format_process_row(p, finished, cells);
        x = x0;
        for (int i = 0; i < TABLE_COLUMNS; i++) {
            Color color = i == 0 ? proc_color(pid) : (finished ? UI_TEXT : UI_TEXT_DIM);
            ui_text_clipped((Rectangle){x, y, widths[i] * scale - 4, 16}, 13, color, cells[i]);
            x += widths[i] * scale;
        }
        y += 17;
    }

    DrawLine((int)x0, (int)y + 1, (int)(x0 + total_w * scale), (int)y + 1, UI_BORDER);
    y += 4;

    if (totals.completed) {
        int n = totals.completed;
        float turnaround_x = x0;
        for (int i = 0; i < 6; i++) turnaround_x += widths[i] * scale;

        ui_text(x0, y, 13, UI_ACCENT, "Average (%d done)", n);
        ui_text(turnaround_x, y, 13, UI_ACCENT, "%.2f", totals.turnaround / n);
        ui_text(turnaround_x + widths[6] * scale, y, 13, UI_ACCENT, "%.2f", totals.waiting / n);
        ui_text(turnaround_x + (widths[6] + widths[7]) * scale, y, 13, UI_ACCENT, "%.2f", totals.response / n);
    }
    ui_text(x0, body.y + body.height - 18, 12, UI_TEXT_DIM,
            "Turnaround = Finish - Arrive.  Waiting = ticks in the ready queue.  Response = Start - Arrive.");
}
