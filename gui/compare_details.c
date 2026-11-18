#include "compare.h"
#include <stdio.h>

void cmp_draw_gantt_stack(gui_t *g, Rectangle r, int case_idx) {
    compare_t *c = g->compare;
    cmp_case_t *k = &c->cases[case_idx];

    int num_rows = 0;
    uint32_t span = 1;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        if (!cmp_run_ok(c, case_idx, alg)) continue;
        num_rows++;
        if (c->runs[case_idx][alg].len > span) span = c->runs[case_idx][alg].len;
    }

    ui_text(r.x + 8, r.y, 13, UI_ACCENT, "CPU GANTT CHART PER ALGORITHM");
    ui_text(r.x + 240, r.y + 1, 12, UI_TEXT_DIM, "same time axis: a shorter bar finished sooner. Hover a block for details.");
    if (num_rows == 0) return;

    const float label_w = 110;
    float top = r.y + 20;
    float row_h = (r.height - 20 - 20) / num_rows;
    if (row_h > 56) row_h = 56;
    float plot_x = r.x + 8 + label_w, plot_w = r.width - 16 - label_w;
    int hovered_tick = -1;

    int row = 0;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        if (!cmp_run_ok(c, case_idx, alg)) continue;

        const cmp_run_t *run = &c->runs[case_idx][alg];
        float y = top + row * row_h;
        row++;
        DrawRectangleRounded((Rectangle){r.x + 8, y + row_h / 2 - 6, 10, 10}, 0.3f, 4, cmp_alg_color(alg));
        ui_text(r.x + 24, y + row_h / 2 - 9, 14, UI_TEXT, "%s", cmp_alg_name(c, alg));

        bool last_row = row == num_rows;
        lanes_t lanes = {run->lanes, CMP_LANE_CAP, PRISM_MAX_CORES, run->ncores, run->len};
        Rectangle chart = {plot_x, y + 2, plot_w, row_h - 6 + (last_row ? 18 : 0)};
        int hovered = gantt_draw(chart, &lanes, 0, span, cmp_case_name, k, last_row, -1);
        if (hovered >= 0) {
            hovered_tick = hovered;
            c->hover_alg = alg;
        }
    }

    if (hovered_tick >= 0) {
        float core_label_w = c->runs[case_idx][0].ncores > 1 ? 34 : 0;
        float cx = plot_x + core_label_w + (hovered_tick + 0.5f) * (plot_w - core_label_w) / span;
        DrawLineEx((Vector2){cx, top}, (Vector2){cx, top + row * row_h}, 1, ColorAlpha(UI_TEXT, 0.6f));
    }
    if (span >= CMP_LANE_CAP) ui_text(r.x + 8, r.y + r.height - 14, 11, UI_WARN, "showing the first %d ticks", CMP_LANE_CAP);
}

static uint32_t sweep_peak(const compare_t *c) {
    uint32_t peak = 1;
    for (int alg = 0; alg < REPL_COUNT; alg++) {
        if (!c->repl_on[alg]) continue;
        for (int f = c->sweep_min; f <= c->sweep_max; f++) {
            if (c->sweep[alg][f] > peak) peak = c->sweep[alg][f];
        }
    }
    return peak;
}

static void draw_sweep_axes(const compare_t *c, Rectangle p, float step_x, uint32_t peak) {
    DrawLine((int)p.x, (int)(p.y + p.height), (int)(p.x + p.width), (int)(p.y + p.height), UI_BORDER);
    DrawLine((int)p.x, (int)p.y, (int)p.x, (int)(p.y + p.height), UI_BORDER);

    for (int k = 0; k <= 2; k++) {
        float y = p.y + p.height - p.height * k / 2.0f;
        ui_text(p.x - 36, y - 7, 11, UI_TEXT_DIM, "%5u", peak * (uint32_t)k / 2);
        if (k) DrawLine((int)p.x, (int)y, (int)(p.x + p.width), (int)y, (Color){40, 44, 54, 255});
    }

    int label_every = step_x >= 18 ? 1 : (step_x >= 8 ? 2 : 4);
    for (int f = c->sweep_min; f <= c->sweep_max; f += label_every) {
        float x = p.x + (f - c->sweep_min) * step_x;
        ui_text(x - 4, p.y + p.height + 4, 11, f == c->sweep_current ? UI_ACCENT : UI_TEXT_DIM, "%d", f);
    }
    ui_text(p.x + p.width - 40, p.y + p.height + 16, 11, UI_TEXT_DIM, "frames");

    if (c->sweep_current >= c->sweep_min && c->sweep_current <= c->sweep_max) {
        float x = p.x + (c->sweep_current - c->sweep_min) * step_x;
        for (float y = p.y; y < p.y + p.height; y += 8) DrawLine((int)x, (int)y, (int)x, (int)(y + 4), UI_ACCENT);
    }
}

static void draw_sweep_line(const compare_t *c, int alg, Rectangle p, float step_x, uint32_t peak,
                            char *belady, size_t belady_size) {
    Color color = cmp_alg_color(alg);
    float line_w = 2.0f + (REPL_COUNT - 1 - alg) * 1.8f;
    Vector2 mouse = GetMousePosition();
    Vector2 prev = {0};

    for (int f = c->sweep_min; f <= c->sweep_max; f++) {
        uint32_t faults = c->sweep[alg][f];
        Vector2 point = {p.x + (f - c->sweep_min) * step_x, p.y + p.height - p.height * (float)faults / peak};
        bool has_prev = f > c->sweep_min;
        if (has_prev) DrawLineEx(prev, point, line_w, color);
        DrawCircleV(point, line_w / 2 + 1.5f, color);

        bool more_frames_more_faults = has_prev && faults > c->sweep[alg][f - 1];
        if (more_frames_more_faults) {
            DrawRing(point, 6, 9, 0, 360, 20, UI_BAD);
            if (!belady[0]) {
                snprintf(belady, belady_size, "Belady's anomaly! %s: %d frames -> %u faults, but %d frames -> %u faults.",
                         cmp_alg_name(c, alg), f - 1, c->sweep[alg][f - 1], f, faults);
            }
        }
        if (CheckCollisionPointCircle(mouse, point, 6)) ui_tooltip("%s with %d frames: %u faults", cmp_alg_name(c, alg), f, faults);
        prev = point;
    }
}

void cmp_draw_sweep_chart(gui_t *g, Rectangle r) {
    compare_t *c = g->compare;
    ui_text(r.x + 8, r.y, 13, UI_ACCENT, "FAULTS vs NUMBER OF FRAMES");
    if (!c->sweep_ready) return;

    Rectangle plot = {r.x + 44, r.y + 26, r.width - 60, r.height - 26 - 50};
    uint32_t peak = sweep_peak(c);
    int steps = c->sweep_max - c->sweep_min;
    if (steps < 1) steps = 1;
    float step_x = plot.width / steps;
    draw_sweep_axes(c, plot, step_x, peak);

    char belady[160] = "";
    for (int alg = 0; alg < REPL_COUNT; alg++) {
        if (c->repl_on[alg]) draw_sweep_line(c, alg, plot, step_x, peak, belady, sizeof(belady));
    }

    float text_y = r.y + r.height - 18;
    if (belady[0]) {
        ui_text(r.x + 8, text_y, 13, UI_BAD, "%s", belady);
    } else {
        ui_text(r.x + 8, text_y, 12, UI_TEXT_DIM, "More frames never added faults here (no Belady's anomaly). Dashed line = frames used above.");
    }
}

static void draw_table_alg_buttons(compare_t *c, Rectangle r) {
    float x = r.x + 110;
    for (int alg = 0; alg < REPL_COUNT; alg++) {
        const char *name = repl_get_ops((repl_policy_t)alg)->name;
        float w = ui_text_width(name, 14) + 16;
        bool active = c->table_alg == alg;
        if (ui_button_ex(CMP_WIDGET_ID(300 + alg), (Rectangle){x, r.y - 3, w, 20}, name, true, active) && !active) {
            c->table_alg = alg;
            c->dirty = true;
        }
        x += w + 4;
    }
}

static bool references_from_several_processes(const compare_t *c) {
    for (int i = 1; i < c->num_columns; i++) {
        if (c->columns[i].pid != c->columns[0].pid) return true;
    }
    return false;
}

static int count_table_faults(const compare_t *c) {
    int faults = 0;
    for (int i = 0; i < c->num_columns; i++) faults += c->columns[i].fault;
    return faults;
}

static void clamp_table_scroll(compare_t *c, int visible) {
    if (c->table_scroll > c->num_columns - visible) c->table_scroll = c->num_columns - visible;
    if (c->table_scroll < 0) c->table_scroll = 0;
}

typedef struct {
    float top;
    float row_h;
    float col_w;
    int   num_frames;
    bool  multi_process;
} table_layout_t;

static void draw_reference_column(const compare_t *c, const table_layout_t *L, int ref_idx, float x) {
    const cmp_col_t *col = &c->columns[ref_idx];
    int nf = L->num_frames;
    float rh = L->row_h;

    ui_text(x + 6, L->top + 3, 13, L->multi_process ? proc_color(col->pid) : UI_TEXT, "%d", col->page);

    for (int f = 0; f < nf; f++) {
        Rectangle cell = {x + 1, L->top + (f + 1) * rh + 1, L->col_w - 2, rh - 2};
        bool loaded_here = col->fault && col->loaded == f;
        bool hit_here = !col->fault && col->hit_frame == f;

        DrawRectangleRec(cell, loaded_here ? ColorAlpha(UI_BAD, 0.55f) : (Color){30, 33, 40, 255});
        if (hit_here) DrawRectangleLinesEx(cell, 2, UI_GOOD);
        if (col->frame_page[f] >= 0) {
            Color text = L->multi_process ? proc_color(col->frame_pid[f]) : UI_TEXT;
            if (loaded_here) text = UI_TEXT;
            ui_text(cell.x + 5, cell.y + (rh - 2 - 13) / 2, 13, text, "%d", col->frame_page[f]);
        }
    }

    float fault_y = L->top + (nf + 1) * rh + 3;
    if (col->fault) ui_text(x + 8, fault_y, 13, UI_BAD, "F");
    else ui_text(x + 10, fault_y, 13, UI_TEXT_DIM, ".");

    Rectangle whole = {x, L->top, L->col_w, rh * (nf + 2)};
    if (CheckCollisionPointRec(GetMousePosition(), whole)) {
        const char *outcome = col->fault ? TextFormat("FAULT: loaded into frame %d", col->loaded)
                                         : TextFormat("hit in frame %d", col->hit_frame);
        ui_tooltip("reference %d: %s page %d\n%s", ref_idx + 1, c->cases[c->focused_case].names[col->pid], col->page, outcome);
    }
}

void cmp_draw_frame_table(gui_t *g, Rectangle r) {
    compare_t *c = g->compare;
    ui_text(r.x + 8, r.y, 13, UI_ACCENT, "FRAME TABLE");
    draw_table_alg_buttons(c, r);

    if (!c->table_ready) {
        ui_text_wrapped(r.x + 8, r.y + 30, r.width - 16, 13, UI_TEXT_DIM,
                        c->table_frames > CMP_TBL_FRAMES ? "The frame table is shown for up to 16 frames: lower the RAM frames setting."
                                                         : "No page references in this test case.");
        return;
    }

    const float label_w = 54;
    table_layout_t L;
    L.num_frames = c->table_frames;
    L.top = r.y + 24;
    L.row_h = (r.height - 24 - 40) / (L.num_frames + 2);
    if (L.row_h > 22) L.row_h = 22;
    L.col_w = 26;
    L.multi_process = references_from_several_processes(c);
    float table_h = L.row_h * (L.num_frames + 2);

    int visible = (int)((r.width - 16 - label_w) / L.col_w);
    if (visible < 1) visible = 1;
    if (CheckCollisionPointRec(GetMousePosition(), (Rectangle){r.x, L.top, r.width, table_h})) {
        c->table_scroll -= (int)GetMouseWheelMove() * 4;
    }
    clamp_table_scroll(c, visible);

    float x0 = r.x + 8;
    ui_text(x0, L.top + 3, 12, UI_TEXT_DIM, "ref");
    for (int f = 0; f < L.num_frames; f++) ui_text(x0, L.top + (f + 1) * L.row_h + 3, 12, UI_TEXT_DIM, "frame %d", f);
    ui_text(x0, L.top + (L.num_frames + 1) * L.row_h + 3, 12, UI_TEXT_DIM, "fault?");
    int faults = count_table_faults(c);

    ui_scissor_push((Rectangle){x0 + label_w, L.top, r.width - label_w - 16, table_h});
    for (int v = 0; v < visible && c->table_scroll + v < c->num_columns; v++) {
        draw_reference_column(c, &L, c->table_scroll + v, x0 + label_w + v * L.col_w);
    }
    ui_scissor_pop();

    float y = L.top + table_h + 6;
    double fault_percent = c->num_columns ? 100.0 * faults / c->num_columns : 0.0;
    ui_text(x0, y, 13, UI_TEXT, "%s: %d references, %d faults (%.0f%%)%s", repl_get_ops((repl_policy_t)c->table_alg)->name,
            c->num_columns, faults, fault_percent, c->table_truncated ? " (first 256 shown)" : "");
    ui_text(x0, y + 18, 11, UI_TEXT_DIM, "red = page loaded on a fault (replacing what was there)  |  green box = hit  |  wheel to scroll%s",
            L.multi_process ? "  |  colours = process" : "");
}
