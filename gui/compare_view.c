#include "compare.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CID(n) CMP_WIDGET_ID(n)
#define SETUP_W 292
#define MAX_METRICS 16

static Color dimmed(Color c) {
    return ColorAlpha(c, 0.35f);
}

static int selected_metric(const compare_t *c) {
    int count;
    cmp_metrics(c->mode, &count);
    int m = c->chart_metric[c->mode];
    return m >= 0 && m < count ? m : 0;
}

static bool is_better(const cmp_metric_t *metric, double a, double b) {
    return metric->lower_better ? a < b : a > b;
}

static bool best_and_worst(const compare_t *c, int case_idx, const cmp_metric_t *metric, double *best, double *worst) {
    bool any = false;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        const cmp_run_t *run = &c->runs[case_idx][alg];
        if (!cmp_run_ok(c, case_idx, alg) || !run->metrics.finished) continue;

        double v = metric->value(&run->metrics);
        if (!any) {
            *best = *worst = v;
            any = true;
            continue;
        }
        if (is_better(metric, v, *best)) *best = v;
        if (is_better(metric, *worst, v)) *worst = v;
    }
    return any;
}

static void focus_case(compare_t *c, int case_idx) {
    if (c->focused_case == case_idx) return;
    c->focused_case = case_idx;
    c->dirty = true;
}

static void set_mode(compare_t *c, int mode) {
    if (c->mode == mode) return;
    c->mode = mode;
    c->hover_alg = -1;
    c->dirty = true;
}

static float draw_mode_buttons(compare_t *c, float x, float y, float w) {
    ui_text(x, y, 13, UI_ACCENT, "1. WHAT TO COMPARE");
    y += 20;

    float half = (w - 6) / 2;
    if (ui_button_ex(CID(1), (Rectangle){x, y, half, 28}, "CPU scheduling", true, c->mode == CMP_SCHED)) {
        set_mode(c, CMP_SCHED);
    }
    if (ui_button_ex(CID(2), (Rectangle){x + half + 6, y, half, 28}, "Page replacement", true, c->mode == CMP_VM)) {
        set_mode(c, CMP_VM);
    }
    return y + 40;
}

static void set_all(bool *enabled, int count, bool value) {
    for (int a = 0; a < count; a++) enabled[a] = value;
}

static float draw_algorithm_toggles(compare_t *c, float x, float y, float w) {
    bool *enabled = c->mode == CMP_VM ? c->repl_on : c->sched_on;
    int count = cmp_num_algs(c);

    ui_text(x, y + 2, 13, UI_ACCENT, "2. ALGORITHMS");
    if (ui_button(CID(3), (Rectangle){x + w - 96, y, 44, 20}, "all")) {
        set_all(enabled, count, true);
        c->dirty = true;
    }
    if (ui_button(CID(4), (Rectangle){x + w - 48, y, 48, 20}, "none")) {
        set_all(enabled, count, false);
        c->dirty = true;
    }
    y += 24;

    for (int alg = 0; alg < count; alg++) {
        Rectangle row = {x, y, w, 22};
        if (CheckCollisionPointRec(GetMousePosition(), row)) c->hover_alg = alg;

        Color swatch = enabled[alg] ? cmp_alg_color(alg) : dimmed(cmp_alg_color(alg));
        DrawRectangleRounded((Rectangle){x, y + 5, 12, 12}, 0.3f, 4, swatch);
        if (ui_checkbox(CID(10 + alg), (Rectangle){x + 20, y, w - 20, 22}, cmp_alg_name(c, alg), &enabled[alg])) {
            c->dirty = true;
        }
        y += 23;
    }
    return y + 8;
}

static float draw_parameter_field(compare_t *c, float x, float y) {
    if (c->mode == CMP_SCHED) {
        ui_text(x, y + 5, 13, UI_TEXT_DIM, "quantum (RR)");
        if (ui_int_field(CID(30), (Rectangle){x + 100, y, 120, 26}, &c->quantum, 0, 50)) c->dirty = true;
    } else {
        ui_text(x, y + 5, 13, UI_TEXT_DIM, "RAM frames");
        if (ui_int_field(CID(31), (Rectangle){x + 100, y, 120, 26}, &c->frames, 0, PRISM_MAX_FRAMES)) c->dirty = true;
    }
    y += 28;
    ui_text(x, y, 11, UI_TEXT_DIM, "0 = use each test case's own setting");
    return y + 22;
}

static float draw_case_list(compare_t *c, float x, float y, float w, float bottom) {
    ui_text(x, y, 13, UI_ACCENT, "3. TEST CASES");
    ui_text(x + 110, y + 1, 11, UI_TEXT_DIM, "tick = include, click = details");
    y += 20;

    for (int i = 0; i < c->num_cases && y < bottom - 30; i++) {
        cmp_case_t *k = &c->cases[i];
        bool usable = cmp_case_usable(c, i);
        if (ui_checkbox(CID(40 + i), (Rectangle){x, y + 1, 20, 22}, "", &k->enabled)) c->dirty = true;

        char label[80];
        if (usable) {
            snprintf(label, sizeof(label), "%s", k->label);
        } else {
            snprintf(label, sizeof(label), "%s  (%s)", k->label, k->num_procs == 0 ? "empty" : "no page refs");
        }
        if (ui_button_ex(CID(60 + i), (Rectangle){x + 24, y, w - 24, 23}, label, usable, c->focused_case == i)) {
            focus_case(c, i);
        }
        y += 25;
    }
    return y + 10;
}

static void draw_algorithm_card(const compare_t *c, Rectangle panel, float x, float y, float w) {
    Rectangle card = {panel.x + 1, y, panel.width - 2, panel.y + panel.height - y - 1};
    if (card.height < 60) return;

    DrawLine((int)x, (int)y - 4, (int)(x + w), (int)y - 4, UI_BORDER);
    ui_scissor_push(card);
    if (c->hover_alg >= 0 && c->hover_alg < cmp_num_algs(c)) {
        explain_draw_card(cmp_alg_info(c, c->hover_alg), x, y + 4, w);
    } else {
        ui_text_wrapped(x, y + 4, w, 13, UI_TEXT_DIM,
                        "Hover an algorithm (here or in the table) to see how it works. "
                        "Every run uses a fresh copy of the test case with the same seed: only the algorithm differs, "
                        "so any difference in the numbers is caused by the algorithm.");
    }
    ui_scissor_pop();
}

static void setup_panel(gui_t *g, Rectangle r) {
    compare_t *c = g->compare;
    ui_claim_rect(r);
    ui_panel(r, NULL);

    float x = r.x + 12, y = r.y + 10, w = r.width - 24;
    y = draw_mode_buttons(c, x, y, w);
    y = draw_algorithm_toggles(c, x, y, w);
    y = draw_parameter_field(c, x, y);
    y = draw_case_list(c, x, y, w, r.y + r.height);
    draw_algorithm_card(c, r, x, y, w);
}

static void draw_metric_tabs(compare_t *c, Rectangle r, const cmp_metric_t *metrics, int count, int selected) {
    float x = r.x + 10;
    for (int k = 0; k < count; k++) {
        float tab_w = ui_text_width(metrics[k].name, 14) + 20;
        if (ui_button_ex(CID(100 + k), (Rectangle){x, r.y + 6, tab_w, 24}, metrics[k].name, true, k == selected)) {
            c->chart_metric[c->mode] = k;
        }
        x += tab_w + 4;
    }
    const cmp_metric_t *metric = &metrics[selected];
    ui_text(r.x + 12, r.y + 36, 13, UI_TEXT_DIM, "%s  %s", metric->tooltip,
            metric->lower_better ? "Lower is better." : "Higher is better.");
}

static int cases_with_results(const compare_t *c, int out[CMP_MAX_CASES]) {
    int count = 0;
    for (int i = 0; i < c->num_cases; i++) {
        if (!c->cases[i].enabled || !cmp_case_usable(c, i)) continue;

        bool any = false;
        for (int alg = 0; alg < cmp_num_algs(c); alg++) any |= cmp_run_ok(c, i, alg);
        if (any) out[count++] = i;
    }
    return count;
}

static int enabled_algorithms(const compare_t *c) {
    int count = 0;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) count += cmp_alg_on(c, alg);
    return count;
}

static double largest_value(const compare_t *c, int case_idx, const cmp_metric_t *metric) {
    double largest = 0;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        if (!cmp_run_ok(c, case_idx, alg)) continue;
        double v = metric->value(&c->runs[case_idx][alg].metrics);
        if (v > largest) largest = v;
    }
    return largest;
}

typedef struct {
    Rectangle plot;
    float     group_w;
    float     bar_w;
    int       num_bars;
} bar_layout_t;

static void draw_case_group(compare_t *c, const bar_layout_t *L, int slot_idx, int case_idx,
                            const cmp_metric_t *metric, int wins[CMP_MAX_ALGS]) {
    Rectangle plot = L->plot;
    float group_x = plot.x + slot_idx * L->group_w;
    double best = 0, worst = 0;
    bool has_best = best_and_worst(c, case_idx, metric, &best, &worst);
    double largest = largest_value(c, case_idx, metric);
    float bars_x = group_x + (L->group_w - L->bar_w * L->num_bars) / 2;
    Vector2 mouse = GetMousePosition();

    if (case_idx == c->focused_case) {
        DrawRectangleRec((Rectangle){group_x + 2, plot.y - 4, L->group_w - 4, plot.height + 4}, ColorAlpha(UI_ACCENT, 0.07f));
    }

    int bar = 0;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        if (!cmp_alg_on(c, alg)) continue;
        float bar_x = bars_x + bar++ * L->bar_w;
        if (!cmp_run_ok(c, case_idx, alg)) continue;

        const sim_metrics_t *m = &c->runs[case_idx][alg].metrics;
        double v = metric->value(m);
        float h = largest > 0 ? (float)(v / largest) * (plot.height - 16) : 0;
        if (h < 1 && v > 0) h = 1;

        Rectangle rect = {bar_x + 1, plot.y + plot.height - h, L->bar_w - 2, h};
        bool is_best = has_best && m->finished && v == best && best != worst;
        if (is_best) wins[alg]++;

        DrawRectangleRec(rect, m->finished ? cmp_alg_color(alg) : dimmed(cmp_alg_color(alg)));
        if (is_best) DrawRectangleLinesEx((Rectangle){rect.x - 1, rect.y - 1, rect.width + 2, rect.height + 2}, 2, UI_TEXT);

        char value[32];
        snprintf(value, sizeof(value), metric->format, v);
        if (L->bar_w >= 26) {
            float text_w = ui_text_width(value, 10);
            ui_text(bar_x + (L->bar_w - text_w) / 2, rect.y - 13, 10, is_best ? UI_TEXT : UI_TEXT_DIM, "%s", value);
        }

        Rectangle hit = {bar_x, plot.y, L->bar_w, plot.height};
        if (CheckCollisionPointRec(mouse, hit)) {
            c->hover_alg = alg;
            ui_tooltip("%s on %s\n%s = %s%s%s", cmp_alg_name(c, alg), c->cases[case_idx].label, metric->name, value,
                       m->finished ? "" : "\n(did not finish)", is_best ? "\nbest on this test case" : "");
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focus_case(c, case_idx);
        }
    }

    if (largest <= 0) {
        ui_text(group_x + 6, plot.y + plot.height - 18, 12, UI_TEXT_DIM, "all 0");
    } else if (has_best && best == worst) {
        ui_text(group_x + 6, plot.y - 2, 11, UI_TEXT_DIM, "all equal");
    }

    Rectangle label = {group_x + 2, plot.y + plot.height + 4, L->group_w - 4, 18};
    ui_text_clipped(label, 13, case_idx == c->focused_case ? UI_ACCENT : UI_TEXT_DIM, c->cases[case_idx].label);
    if (CheckCollisionPointRec(mouse, label) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) focus_case(c, case_idx);
}

static void draw_overall_winner(const compare_t *c, Rectangle r, const int wins[CMP_MAX_ALGS], int num_groups) {
    int top = -1;
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        if (cmp_alg_on(c, alg) && (top < 0 || wins[alg] > wins[top])) top = alg;
    }
    if (top < 0 || wins[top] == 0) return;

    const char *text = TextFormat("Best on most test cases: %s (%d of %d)", cmp_alg_name(c, top), wins[top], num_groups);
    ui_text(r.x + r.width - ui_text_width(text, 13) - 12, r.y + 36, 13, cmp_alg_color(top), "%s", text);
}

static void bar_chart(gui_t *g, Rectangle r) {
    compare_t *c = g->compare;
    ui_claim_rect(r);
    ui_panel(r, NULL);

    int num_metrics;
    const cmp_metric_t *metrics = cmp_metrics(c->mode, &num_metrics);
    int selected = selected_metric(c);
    draw_metric_tabs(c, r, metrics, num_metrics, selected);

    int groups[CMP_MAX_CASES];
    int num_groups = cases_with_results(c, groups);
    int num_bars = enabled_algorithms(c);
    if (num_groups == 0 || num_bars == 0) {
        ui_text(r.x + 12, r.y + 70, 15, UI_TEXT_DIM,
                num_bars == 0 ? "Tick at least one algorithm." : "Tick at least one test case that fits this mode.");
        return;
    }

    bar_layout_t layout;
    layout.plot = (Rectangle){r.x + 12, r.y + 62, r.width - 24, r.height - 62 - 34};
    layout.group_w = layout.plot.width / num_groups;
    layout.bar_w = (layout.group_w - 18) / num_bars;
    if (layout.bar_w > 30) layout.bar_w = 30;
    layout.num_bars = num_bars;

    Rectangle plot = layout.plot;
    DrawLine((int)plot.x, (int)(plot.y + plot.height), (int)(plot.x + plot.width), (int)(plot.y + plot.height), UI_BORDER);

    int wins[CMP_MAX_ALGS] = {0};
    for (int slot = 0; slot < num_groups; slot++) {
        draw_case_group(c, &layout, slot, groups[slot], &metrics[selected], wins);
    }

    draw_overall_winner(c, r, wins, num_groups);
    ui_text(r.x + 12, r.y + r.height - 14, 11, UI_TEXT_DIM,
            "bars scaled per test case  |  white outline = best  |  faded = did not finish  |  click a group for details");
}

static Color cell_color(double v, bool has_range, double best, double worst, bool finished) {
    if (!has_range || best == worst || !finished) return UI_TEXT;
    if (v == best) return UI_GOOD;
    if (v == worst) return UI_BAD;
    return UI_TEXT;
}

static void draw_result_values(const cmp_run_t *run, const cmp_metric_t *metrics, int count, float x, float y, float col_w,
                               const bool has[], const double best[], const double worst[]) {
    if (!run->valid || run->status != SIM_OK) {
        ui_text(x, y + 1, 13, UI_BAD, "%s", run->valid ? sim_strerror(run->status) : "not run");
        return;
    }
    for (int k = 0; k < count; k++) {
        double v = metrics[k].value(&run->metrics);
        char value[32];
        snprintf(value, sizeof(value), metrics[k].format, v);
        Color color = cell_color(v, has[k], best[k], worst[k], run->metrics.finished);
        ui_text(x + k * col_w, y + 1, 14, color, "%s%s", value, run->metrics.finished ? "" : "*");
    }
}

static float results_table(gui_t *g, Rectangle r, int case_idx) {
    compare_t *c = g->compare;
    int count;
    const cmp_metric_t *metrics = cmp_metrics(c->mode, &count);
    const float name_w = 150, open_w = 64;
    float col_w = (r.width - name_w - open_w - 16) / count;
    float x0 = r.x + 8, y = r.y;

    for (int k = 0; k < count; k++) {
        Rectangle header = {x0 + name_w + k * col_w, y, col_w - 6, 18};
        ui_text_clipped(header, 12, k == selected_metric(c) ? UI_ACCENT : UI_TEXT_DIM, metrics[k].name);
        if (CheckCollisionPointRec(GetMousePosition(), header)) {
            ui_tooltip("%s\n%s", metrics[k].tooltip, metrics[k].lower_better ? "lower is better" : "higher is better");
        }
    }
    y += 20;

    double best[MAX_METRICS], worst[MAX_METRICS];
    bool has[MAX_METRICS];
    for (int k = 0; k < count && k < MAX_METRICS; k++) has[k] = best_and_worst(c, case_idx, &metrics[k], &best[k], &worst[k]);

    Vector2 mouse = GetMousePosition();
    for (int alg = 0; alg < cmp_num_algs(c); alg++) {
        if (!cmp_alg_on(c, alg)) continue;

        Rectangle row = {r.x + 2, y - 1, r.width - 4, 21};
        bool hovered = CheckCollisionPointRec(mouse, row);
        bool shown_in_table = c->mode == CMP_VM && c->table_alg == alg;
        if (hovered) {
            DrawRectangleRec(row, UI_PANEL_HI);
            c->hover_alg = alg;
        }
        if (shown_in_table) DrawRectangleLinesEx(row, 1, UI_ACCENT);

        DrawRectangleRounded((Rectangle){x0, y + 4, 10, 10}, 0.3f, 4, cmp_alg_color(alg));
        ui_text(x0 + 16, y + 1, 14, UI_TEXT, "%s", cmp_alg_name(c, alg));
        draw_result_values(&c->runs[case_idx][alg], metrics, count, x0 + name_w, y, col_w, has, best, worst);

        Rectangle open = {r.x + r.width - open_w - 6, y, open_w, 19};
        if (ui_button(CID(200 + alg), open, "Open")) cmp_open_in_sim(g, case_idx, alg);
        if (CheckCollisionPointRec(mouse, open)) {
            ui_tooltip("Open this test case in Simulate with %s\nto step through every decision", cmp_alg_name(c, alg));
        }

        bool clicked_row = hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mouse.x < r.x + r.width - open_w - 10;
        if (clicked_row && c->mode == CMP_VM && c->table_alg != alg) {
            c->table_alg = alg;
            c->dirty = true;
        }
        y += 22;
    }
    return y - r.y;
}

static void ensure_focused_case_usable(compare_t *c) {
    if (cmp_case_usable(c, c->focused_case)) return;
    for (int i = 0; i < c->num_cases; i++) {
        if (cmp_case_usable(c, i)) {
            c->focused_case = i;
            c->dirty = true;
            return;
        }
    }
}

static float draw_case_header(const compare_t *c, Rectangle d) {
    const cmp_case_t *k = &c->cases[c->focused_case];
    int frames = c->mode == CMP_VM && c->frames > 0 ? c->frames : k->frames;
    int quantum = c->mode == CMP_SCHED && c->quantum > 0 ? c->quantum : k->quantum;

    float y = d.y + 8;
    ui_text(d.x + 10, y, 17, UI_TEXT, "%s", k->label);
    ui_text(d.x + 20 + ui_text_width(k->label, 17), y + 3, 12, UI_TEXT_DIM, "%d processes, %d core%s, %d frames, quantum %d",
            k->num_procs, k->cores, k->cores == 1 ? "" : "s", frames, quantum);
    y += 24;
    if (k->description[0]) y += ui_text_wrapped(d.x + 10, y, d.width - 20, 13, UI_TEXT_DIM, k->description) + 4;
    return y + 4;
}

static void details_panel(gui_t *g, Rectangle d) {
    compare_t *c = g->compare;
    ui_claim_rect(d);
    ui_panel(d, NULL);

    float y = draw_case_header(c, d);
    y += results_table(g, (Rectangle){d.x, y, d.width, 0}, c->focused_case) + 8;
    DrawLine((int)d.x + 8, (int)y - 4, (int)(d.x + d.width - 8), (int)y - 4, UI_BORDER);

    Rectangle rest = {d.x, y + 4, d.width, d.y + d.height - y - 26};
    if (c->mode == CMP_SCHED) {
        cmp_draw_gantt_stack(g, rest, c->focused_case);
    } else {
        float sweep_w = rest.width * 0.40f;
        cmp_draw_sweep_chart(g, (Rectangle){rest.x, rest.y, sweep_w, rest.height});
        cmp_draw_frame_table(g, (Rectangle){rest.x + sweep_w + 8, rest.y, rest.width - sweep_w - 8, rest.height});
    }
    ui_text(d.x + 10, d.y + d.height - 18, 11, UI_TEXT_DIM,
            "Green = best, red = worst in a column; * = did not finish within %d ticks. Recomputed in %.0f ms whenever a setting changes. "
            "\"Open\" loads the case in Simulate to step through it.", CMP_MAX_TICKS, c->run_ms);
}

void compare_view(gui_t *g, Rectangle r) {
    compare_t *c = g->compare;
    ensure_focused_case_usable(c);
    if (c->dirty) cmp_run_all(g);

    setup_panel(g, (Rectangle){r.x, r.y, SETUP_W, r.height});

    Rectangle right = {r.x + SETUP_W, r.y, r.width - SETUP_W, r.height};
    float chart_h = right.height * 0.34f;
    if (chart_h < 220) chart_h = 220;
    bar_chart(g, (Rectangle){right.x, right.y, right.width, chart_h});
    details_panel(g, (Rectangle){right.x, right.y + chart_h, right.width, right.height - chart_h});
}
