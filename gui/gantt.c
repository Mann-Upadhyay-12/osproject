#include "gui_state.h"
#include "prism/metrics.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Timelines. gantt_draw() is the textbook CPU Gantt chart (one lane per
 * core, consecutive ticks of the same process merged into a labelled block)
 * and is shared by the Simulate view and the Compare view. Clicking a
 * timeline in the Simulate view seeks there (gui_seek replays the run).
 */

static int8_t lane_at(const lanes_t *L, uint32_t t, int c) {
    return L->cells[(size_t)(t % L->ring) * (size_t)L->stride + (size_t)c];
}

/* Pick a tick-label step that keeps labels at least ~36px apart. */
static uint32_t axis_step(float px_per_tick) {
    static const uint32_t steps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (steps[i] * px_per_tick >= 36) return steps[i];
    }
    return 10000;
}

static void draw_axis(float x0, float y, float cw, uint32_t first, uint32_t span) {
    uint32_t step = axis_step(cw);
    for (uint32_t t = (first + step - 1) / step * step; t <= first + span; t += step) {
        float x = x0 + (t - first) * cw;
        DrawLine((int)x, (int)y, (int)x, (int)y + 4, UI_TEXT_DIM);
        char b[16];
        snprintf(b, sizeof(b), "%u", t);
        ui_text(x - ui_text_width(b, 11) / 2, y + 5, 11, UI_TEXT_DIM, "%s", b);
    }
}

int gantt_draw(Rectangle r, const lanes_t *L, uint32_t first, uint32_t span, pid_name_fn name, void *user,
               bool axis, int64_t cursor) {
    if (span == 0) span = 1;
    const float label_w = L->ncores > 1 ? 34 : 0;
    float axis_h = axis ? 18 : 0;
    float x0 = r.x + label_w, pw = r.width - label_w;
    float cw = pw / (float)span;
    float lane_h = (r.height - axis_h) / (float)(L->ncores > 0 ? L->ncores : 1);
    if (lane_h > 34) lane_h = 34;
    Vector2 m = GetMousePosition();
    int hovered = -1;

    for (int c = 0; c < L->ncores; c++) {
        float y = r.y + c * lane_h;
        if (L->ncores > 1) ui_text(r.x, y + lane_h / 2 - 7, 12, UI_TEXT_DIM, "cpu%d", c);
        DrawRectangle((int)x0, (int)y + 1, (int)pw, (int)lane_h - 2, (Color){26, 29, 35, 255});
        uint32_t end = first + span < L->len ? first + span : L->len;
        uint32_t t = first;
        while (t < end) {
            int pid = lane_at(L, t, c);
            uint32_t u = t + 1;
            while (u < end && lane_at(L, u, c) == pid) u++;
            Rectangle b = {x0 + (t - first) * cw, y + 1, (u - t) * cw, lane_h - 2};
            if (pid >= 0) {
                DrawRectangleRec(b, ColorAlpha(proc_color(pid), 0.85f));
                const char *nm = name ? name(pid, user) : "";
                if (b.width > ui_text_width(nm, 12) + 6) {
                    ui_text(b.x + (b.width - ui_text_width(nm, 12)) / 2, b.y + b.height / 2 - 7, 12, (Color){15, 15, 20, 255}, "%s", nm);
                }
                DrawLine((int)b.x, (int)b.y, (int)b.x, (int)(b.y + b.height), (Color){15, 15, 20, 255});
            }
            if (CheckCollisionPointRec(m, b)) {
                hovered = (int)(first + (uint32_t)((m.x - x0) / cw));
                if (pid >= 0) ui_tooltip("%s on cpu%d\nticks %u..%u (%u ticks)", name ? name(pid, user) : "", c, t, u, u - t);
                else ui_tooltip("cpu%d idle\nticks %u..%u", c, t, u);
            }
            t = u;
        }
    }
    float lanes_bottom = r.y + L->ncores * lane_h;
    if (axis) draw_axis(x0, lanes_bottom, cw, first, span);
    if (cursor >= (int64_t)first && cursor <= (int64_t)(first + span)) {
        float cx = x0 + (float)(cursor - (int64_t)first) * cw;
        DrawLineEx((Vector2){cx, r.y}, (Vector2){cx, lanes_bottom}, 2, UI_TEXT);
    }
    return hovered;
}

/* ------------------------------------------------------------------------
 * Simulate view panes
 * ------------------------------------------------------------------------ */

static const char *sim_name(int pid, void *user) {
    const sim_t *s = (const sim_t *)user;
    return (pid >= 0 && pid < s->num_processes) ? s->procs[pid].name : "?";
}

/* Window of history to show: everything if it fits at >= min_cw px/tick,
 * otherwise the newest ticks. */
static void hist_window(const gui_t *g, float width, float min_cw, uint32_t *first, uint32_t *span) {
    uint32_t avail = g->hist_len < GUI_HIST_TICKS ? g->hist_len : GUI_HIST_TICKS;
    uint32_t oldest = g->hist_len - avail;
    uint32_t fit = (uint32_t)(width / min_cw);
    uint32_t want = g->hist_len - oldest;
    if (want < 20) want = 20;   /* don't stretch the first ticks across the whole panel */
    if (want > fit) want = fit;
    *span = want;
    *first = g->hist_len > want ? g->hist_len - want : 0;
    if (*first < oldest) *first = oldest;
}

static void click_to_seek(gui_t *g, int hovered) {
    if (hovered < 0) return;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && ui_content_input()) {
        g->playing = false;
        gui_seek(g, (tick_t)hovered + 1);   /* show the state right after that tick ran */
    }
}

void panel_cpu_gantt(gui_t *g, Rectangle body) {
    if (g->hist_len == 0) {
        ui_text_wrapped(body.x + 10, body.y + 8, body.width - 20, 14, UI_TEXT_DIM,
                        "The CPU Gantt chart fills in as the simulation runs: which process held each core at every tick. "
                        "Click a tick to jump back to it.");
        return;
    }
    Rectangle r = {body.x + 8, body.y + 8, body.width - 16, body.height - 16};
    lanes_t L = {&g->hist_core[0][0], GUI_HIST_TICKS, PRISM_MAX_CORES, g->sim->num_cores, g->hist_len};
    uint32_t first, span;
    hist_window(g, r.width, 6, &first, &span);
    float lanes_h = r.height - 18;
    float lane = lanes_h / g->sim->num_cores;
    if (lane > 34) lane = 34;
    r.height = lane * g->sim->num_cores + 18;
    int h = gantt_draw(r, &L, first, span, sim_name, g->sim, true, (int64_t)g->hist_len);
    click_to_seek(g, h);
    float y = r.y + r.height + 6;
    if (y + 16 < body.y + body.height) {
        sim_metrics_t m;
        sim_metrics_compute(g->sim, &m);
        ui_text(body.x + 8, y, 12, UI_TEXT_DIM, "CPU busy %.0f%%  |  %u dispatches  |  idle = dark gaps  |  click to rewind",
                m.cpu_util, m.ctx_switches);
    }
}

void panel_states(gui_t *g, Rectangle body) {
    const sim_t *s = g->sim;
    Rectangle in = {body.x + 8, body.y + 6, body.width - 16, body.height - 12};
    float label_w = 70, plot_w = in.width - label_w;
    int live[PRISM_MAX_PROCESSES], nl = 0;
    for (int pid = 0; pid < s->num_processes; pid++) if (s->procs[pid].state != PROC_UNUSED) live[nl++] = pid;
    if (nl == 0) { ui_text(in.x, in.y, 14, UI_TEXT_DIM, "No processes."); return; }
    float rows_h = in.height - 18;
    float rh = rows_h / nl;
    if (rh > 18) rh = 18;
    if (rh < 6) rh = 6;
    int visible = (int)(rows_h / rh);
    uint32_t first, span;
    hist_window(g, plot_w, 4, &first, &span);
    float cw = plot_w / (float)span;
    Vector2 m = GetMousePosition();
    int hovered = -1;

    ui_scissor_push(in);
    for (int k = 0; k < nl && k < visible; k++) {
        int pid = live[k];
        float y = in.y + k * rh;
        ui_text_clipped((Rectangle){in.x, y, label_w - 6, rh}, rh > 14 ? 13 : 10, proc_color(pid), s->procs[pid].name);
        for (uint32_t t = first; t < g->hist_len; t++) {
            uint8_t st = g->hist[t % GUI_HIST_TICKS][pid];
            if (st == PROC_NEW || st == PROC_UNUSED || st == PROC_TERMINATED) continue;
            Rectangle c = {in.x + label_w + (t - first) * cw, y + 1, cw + 0.5f, rh - 2};
            DrawRectangleRec(c, state_color((proc_state_t)st));
            if (CheckCollisionPointRec(m, c)) {
                hovered = (int)t;
                ui_tooltip("%s at tick %u: %s", s->procs[pid].name, t, proc_state_str((proc_state_t)st));
            }
        }
    }
    float ay = in.y + (nl < visible ? nl : visible) * rh;
    draw_axis(in.x + label_w, ay, cw, first, span);
    float cx = in.x + label_w + (g->hist_len - first) * cw;
    DrawLine((int)cx, (int)in.y, (int)cx, (int)ay, UI_TEXT);
    ui_scissor_pop();
    click_to_seek(g, hovered);
}

/* ------------------------------------------------------------------------
 * Results table (the one students fill in by hand)
 * ------------------------------------------------------------------------ */

void panel_proc_table(gui_t *g, Rectangle body) {
    const sim_t *s = g->sim;
    static const char *hdr[] = {"Process", "Arrive", "CPU", "I/O", "Start", "Finish", "Turnaround", "Waiting", "Response", "Faults"};
    static const float colw[] = {84, 52, 44, 40, 48, 52, 82, 62, 70, 50};
    const int ncol = 10;
    float x0 = body.x + 8, y = body.y + 6;
    float total_w = 0;
    for (int i = 0; i < ncol; i++) total_w += colw[i];
    float scale = (body.width - 16) / total_w;
    if (scale > 1.4f) scale = 1.4f;

    float x = x0;
    for (int i = 0; i < ncol; i++) {
        ui_text(x, y, 12, UI_TEXT_DIM, "%s", hdr[i]);
        x += colw[i] * scale;
    }
    y += 17;
    DrawLine((int)x0, (int)y - 2, (int)(x0 + total_w * scale), (int)y - 2, UI_BORDER);

    int done = 0;
    double sum_tat = 0, sum_wait = 0, sum_resp = 0;
    float bottom = body.y + body.height - 40;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state == PROC_UNUSED) continue;
        uint32_t cpu = 0, io = 0;
        for (int i = 0; i < p->num_bursts; i++) {
            if (p->bursts[i].kind == BURST_CPU) cpu += p->bursts[i].duration;
            else if (p->bursts[i].kind == BURST_IO) io += p->bursts[i].duration;
        }
        bool fin = p->state == PROC_TERMINATED && p->finish_tick >= 0;
        char v[10][24];
        snprintf(v[0], 24, "%s", p->name);
        snprintf(v[1], 24, "%" PRIu64, (uint64_t)p->arrival_tick);
        snprintf(v[2], 24, "%u", cpu);
        snprintf(v[3], 24, "%u", io);
        snprintf(v[4], 24, p->start_tick >= 0 ? "%" PRId64 : "-", p->start_tick);
        snprintf(v[5], 24, fin ? "%" PRId64 : "-", p->finish_tick);
        snprintf(v[6], 24, fin ? "%" PRId64 : "-", p->finish_tick - (int64_t)p->arrival_tick);
        snprintf(v[7], 24, "%u", p->total_wait_time);
        snprintf(v[8], 24, p->start_tick >= 0 ? "%" PRId64 : "-", p->start_tick - (int64_t)p->arrival_tick);
        snprintf(v[9], 24, "%u", p->num_page_faults);
        if (fin) {
            done++;
            sum_tat += (double)(p->finish_tick - (int64_t)p->arrival_tick);
            sum_wait += p->total_wait_time;
            sum_resp += (double)(p->start_tick - (int64_t)p->arrival_tick);
        }
        if (y < bottom) {
            x = x0;
            for (int i = 0; i < ncol; i++) {
                Color c = i == 0 ? proc_color(pid) : (fin ? UI_TEXT : UI_TEXT_DIM);
                ui_text_clipped((Rectangle){x, y, colw[i] * scale - 4, 16}, 13, c, v[i]);
                x += colw[i] * scale;
            }
            y += 17;
        }
    }
    DrawLine((int)x0, (int)y + 1, (int)(x0 + total_w * scale), (int)y + 1, UI_BORDER);
    y += 4;
    if (done) {
        float xa = x0;
        for (int i = 0; i < 6; i++) xa += colw[i] * scale;
        ui_text(x0, y, 13, UI_ACCENT, "Average (%d done)", done);
        ui_text(xa, y, 13, UI_ACCENT, "%.2f", sum_tat / done);
        ui_text(xa + colw[6] * scale, y, 13, UI_ACCENT, "%.2f", sum_wait / done);
        ui_text(xa + (colw[6] + colw[7]) * scale, y, 13, UI_ACCENT, "%.2f", sum_resp / done);
    }
    ui_text(x0, body.y + body.height - 18, 12, UI_TEXT_DIM,
            "Turnaround = Finish - Arrive.  Waiting = ticks in the ready queue.  Response = Start - Arrive.");
}
