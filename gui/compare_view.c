#include "compare.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Compare view: setup column (mode, algorithms, overrides, test cases), a
 * grouped bar chart of one metric over all test cases, the full results
 * table for the focused test case, and a detail pane: stacked Gantt charts
 * (scheduling) or faults-vs-frames + the textbook frame table (memory).
 * Widget ids 2000..2999.
 */

#define CID(n) (2000 + (n))
#define SETUP_W 292

static Color dimmed(Color c) { return ColorAlpha(c, 0.35f); }

static int metric_idx(const compare_t *c) {
    int n;
    cmp_metrics(c->mode, &n);
    int m = c->metric[c->mode];
    return (m >= 0 && m < n) ? m : 0;
}

static bool run_ok(const compare_t *c, int i, int a) {
    const cmp_run_t *r = &c->runs[i][a];
    return cmp_alg_on(c, a) && r->valid && r->rc == SIM_OK;
}

/* Best (and worst) value of metric mi over the enabled algorithms of case i. */
static bool best_worst(const compare_t *c, int i, const cmp_metric_t *mt, double *best, double *worst) {
    bool any = false;
    for (int a = 0; a < cmp_nalgs(c); a++) {
        if (!run_ok(c, i, a) || !c->runs[i][a].m.finished) continue;
        double v = mt->get(&c->runs[i][a].m);
        if (!any) { *best = *worst = v; any = true; continue; }
        if (mt->lower_better ? v < *best : v > *best) *best = v;
        if (mt->lower_better ? v > *worst : v < *worst) *worst = v;
    }
    return any;
}

/* ------------------------------------------------------------------------
 * Setup column
 * ------------------------------------------------------------------------ */

static void setup_panel(gui_t *g, Rectangle r) {
    compare_t *c = g->cmp;
    ui_claim_rect(r);
    ui_panel(r, NULL);
    float x = r.x + 12, y = r.y + 10, w = r.width - 24;

    ui_text(x, y, 13, UI_ACCENT, "1. WHAT TO COMPARE");
    y += 20;
    float hw = (w - 6) / 2;
    if (ui_button_ex(CID(1), (Rectangle){x, y, hw, 28}, "CPU scheduling", true, c->mode == CMP_SCHED) && c->mode != CMP_SCHED) {
        c->mode = CMP_SCHED; c->hover_alg = -1; c->dirty = true;
    }
    if (ui_button_ex(CID(2), (Rectangle){x + hw + 6, y, hw, 28}, "Page replacement", true, c->mode == CMP_VM) && c->mode != CMP_VM) {
        c->mode = CMP_VM; c->hover_alg = -1; c->dirty = true;
    }
    y += 40;

    ui_text(x, y + 2, 13, UI_ACCENT, "2. ALGORITHMS");
    bool *on = c->mode == CMP_VM ? c->repl_on : c->sched_on;
    int na = cmp_nalgs(c);
    if (ui_button(CID(3), (Rectangle){x + w - 96, y, 44, 20}, "all")) { for (int a = 0; a < na; a++) on[a] = true; c->dirty = true; }
    if (ui_button(CID(4), (Rectangle){x + w - 48, y, 48, 20}, "none")) { for (int a = 0; a < na; a++) on[a] = false; c->dirty = true; }
    y += 24;
    for (int a = 0; a < na; a++) {
        Rectangle row = {x, y, w, 22};
        if (CheckCollisionPointRec(GetMousePosition(), row)) c->hover_alg = a;
        DrawRectangleRounded((Rectangle){x, y + 5, 12, 12}, 0.3f, 4, on[a] ? cmp_alg_color(a) : dimmed(cmp_alg_color(a)));
        if (ui_checkbox(CID(10 + a), (Rectangle){x + 20, y, w - 20, 22}, cmp_alg_name(c, a), &on[a])) c->dirty = true;
        y += 23;
    }
    y += 8;

    if (c->mode == CMP_SCHED) {
        ui_text(x, y + 5, 13, UI_TEXT_DIM, "quantum (RR)");
        if (ui_int_field(CID(30), (Rectangle){x + 100, y, 120, 26}, &c->quantum, 0, 50)) c->dirty = true;
    } else {
        ui_text(x, y + 5, 13, UI_TEXT_DIM, "RAM frames");
        if (ui_int_field(CID(31), (Rectangle){x + 100, y, 120, 26}, &c->frames, 0, PRISM_MAX_FRAMES)) c->dirty = true;
    }
    y += 28;
    ui_text(x, y, 11, UI_TEXT_DIM, "0 = use each test case's own setting");
    y += 22;

    ui_text(x, y, 13, UI_ACCENT, "3. TEST CASES");
    ui_text(x + 110, y + 1, 11, UI_TEXT_DIM, "tick = include, click = details");
    y += 20;
    for (int i = 0; i < c->ncases && y < r.y + r.height - 30; i++) {
        cmp_case_t *k = &c->cases[i];
        bool usable = cmp_case_usable(c, i);
        if (ui_checkbox(CID(40 + i), (Rectangle){x, y + 1, 20, 22}, "", &k->on)) c->dirty = true;
        const char *label = k->label;
        char buf[80];
        if (!usable) {
            snprintf(buf, sizeof(buf), "%s  (%s)", k->label, k->nproc == 0 ? "empty" : "no page refs");
            label = buf;
        }
        if (ui_button_ex(CID(60 + i), (Rectangle){x + 24, y, w - 24, 23}, label, usable, c->focus == i) && c->focus != i) {
            c->focus = i;
            c->dirty = true;
        }
        y += 25;
    }
    y += 10;

    /* Card of the last hovered algorithm: what to expect before looking at numbers. */
    Rectangle card = {r.x + 1, y, r.width - 2, r.y + r.height - y - 1};
    if (card.height < 60) return;
    DrawLine((int)x, (int)y - 4, (int)(x + w), (int)y - 4, UI_BORDER);
    ui_scissor_push(card);
    if (c->hover_alg >= 0 && c->hover_alg < na) {
        explain_draw_card(cmp_alg_info(c, c->hover_alg), x, y + 4, w);
    } else {
        ui_text_wrapped(x, y + 4, w, 13, UI_TEXT_DIM,
                        "Hover an algorithm (here or in the table) to see how it works. "
                        "Every run uses a fresh copy of the test case with the same seed: only the algorithm differs, "
                        "so any difference in the numbers is caused by the algorithm.");
    }
    ui_scissor_pop();
}

/* ------------------------------------------------------------------------
 * Grouped bar chart: one group per test case, one bar per algorithm
 * ------------------------------------------------------------------------ */

static void bar_chart(gui_t *g, Rectangle r) {
    compare_t *c = g->cmp;
    ui_claim_rect(r);
    ui_panel(r, NULL);
    int nm;
    const cmp_metric_t *mts = cmp_metrics(c->mode, &nm);
    int mi = metric_idx(c);
    const cmp_metric_t *mt = &mts[mi];

    float x = r.x + 10;
    for (int k = 0; k < nm; k++) {
        float bw = ui_text_width(mts[k].name, 14) + 20;
        if (ui_button_ex(CID(100 + k), (Rectangle){x, r.y + 6, bw, 24}, mts[k].name, true, k == mi)) c->metric[c->mode] = k;
        x += bw + 4;
    }
    ui_text(r.x + 12, r.y + 36, 13, UI_TEXT_DIM, "%s  %s", mt->tip, mt->lower_better ? "Lower is better." : "Higher is better.");

    int groups[CMP_MAX_CASES], ng = 0, wins[CMP_MAX_ALGS] = {0}, nb = 0;
    for (int i = 0; i < c->ncases; i++) {
        if (!c->cases[i].on || !cmp_case_usable(c, i)) continue;
        bool any = false;
        for (int a = 0; a < cmp_nalgs(c); a++) any |= run_ok(c, i, a);
        if (any) groups[ng++] = i;
    }
    for (int a = 0; a < cmp_nalgs(c); a++) nb += cmp_alg_on(c, a);
    if (ng == 0 || nb == 0) {
        ui_text(r.x + 12, r.y + 70, 15, UI_TEXT_DIM, nb == 0 ? "Tick at least one algorithm." : "Tick at least one test case that fits this mode.");
        return;
    }

    Rectangle plot = {r.x + 12, r.y + 62, r.width - 24, r.height - 62 - 34};
    float gw = plot.width / ng;
    float bw = (gw - 18) / nb;
    if (bw > 30) bw = 30;
    Vector2 mouse = GetMousePosition();
    DrawLine((int)plot.x, (int)(plot.y + plot.height), (int)(plot.x + plot.width), (int)(plot.y + plot.height), UI_BORDER);

    for (int gi = 0; gi < ng; gi++) {
        int i = groups[gi];
        double mx = 0, best = 0, worst = 0;
        bool has_best = best_worst(c, i, mt, &best, &worst);
        for (int a = 0; a < cmp_nalgs(c); a++) if (run_ok(c, i, a) && mt->get(&c->runs[i][a].m) > mx) mx = mt->get(&c->runs[i][a].m);
        float gx = plot.x + gi * gw + (gw - bw * nb) / 2;
        if (i == c->focus) DrawRectangleRec((Rectangle){plot.x + gi * gw + 2, plot.y - 4, gw - 4, plot.height + 4}, ColorAlpha(UI_ACCENT, 0.07f));
        int slot = 0;
        for (int a = 0; a < cmp_nalgs(c); a++) {
            if (!cmp_alg_on(c, a)) continue;
            float bx = gx + slot * bw;
            slot++;
            if (!run_ok(c, i, a)) continue;
            const sim_metrics_t *m = &c->runs[i][a].m;
            double v = mt->get(m);
            float h = mx > 0 ? (float)(v / mx) * (plot.height - 16) : 0;
            if (h < 1 && v > 0) h = 1;
            Rectangle b = {bx + 1, plot.y + plot.height - h, bw - 2, h};
            /* A tie across every algorithm is not a win for anyone. */
            bool is_best = has_best && m->finished && v == best && best != worst;
            if (is_best) wins[a]++;
            DrawRectangleRec(b, m->finished ? cmp_alg_color(a) : dimmed(cmp_alg_color(a)));
            if (is_best) DrawRectangleLinesEx((Rectangle){b.x - 1, b.y - 1, b.width + 2, b.height + 2}, 2, UI_TEXT);
            char val[32];
            snprintf(val, sizeof(val), mt->fmt, v);
            if (bw >= 26) {
                float tw = ui_text_width(val, 10);
                ui_text(bx + (bw - tw) / 2, b.y - 13, 10, is_best ? UI_TEXT : UI_TEXT_DIM, "%s", val);
            }
            Rectangle hit = {bx, plot.y, bw, plot.height};
            if (CheckCollisionPointRec(mouse, hit)) {
                c->hover_alg = a;
                ui_tooltip("%s on %s\n%s = %s%s%s", cmp_alg_name(c, a), c->cases[i].label, mt->name, val,
                           m->finished ? "" : "\n(did not finish)", is_best ? "\nbest on this test case" : "");
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && c->focus != i) { c->focus = i; c->dirty = true; }
            }
        }
        if (mx <= 0) ui_text(plot.x + gi * gw + 6, plot.y + plot.height - 18, 12, UI_TEXT_DIM, "all 0");
        else if (has_best && best == worst) ui_text(plot.x + gi * gw + 6, plot.y - 2, 11, UI_TEXT_DIM, "all equal");
        Rectangle lab = {plot.x + gi * gw + 2, plot.y + plot.height + 4, gw - 4, 18};
        ui_text_clipped(lab, 13, i == c->focus ? UI_ACCENT : UI_TEXT_DIM, c->cases[i].label);
        if (CheckCollisionPointRec(mouse, lab) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && c->focus != i) { c->focus = i; c->dirty = true; }
    }

    /* Which algorithm wins most often on this metric. */
    int top = -1;
    for (int a = 0; a < cmp_nalgs(c); a++) if (cmp_alg_on(c, a) && (top < 0 || wins[a] > wins[top])) top = a;
    if (top >= 0 && wins[top] > 0) {
        const char *t = TextFormat("Best on most test cases: %s (%d of %d)", cmp_alg_name(c, top), wins[top], ng);
        ui_text(r.x + r.width - ui_text_width(t, 13) - 12, r.y + 36, 13, cmp_alg_color(top), "%s", t);
    }
    ui_text(r.x + 12, r.y + r.height - 14, 11, UI_TEXT_DIM, "bars scaled per test case  |  white outline = best  |  faded = did not finish  |  click a group for details");
}

/* ------------------------------------------------------------------------
 * Results table for the focused test case
 * ------------------------------------------------------------------------ */

static float results_table(gui_t *g, Rectangle r, int ci) {
    compare_t *c = g->cmp;
    int nm;
    const cmp_metric_t *mts = cmp_metrics(c->mode, &nm);
    const float name_w = 150, open_w = 64;
    float cw = (r.width - name_w - open_w - 16) / nm;
    float x0 = r.x + 8, y = r.y;
    for (int k = 0; k < nm; k++) {
        Rectangle h = {x0 + name_w + k * cw, y, cw - 6, 18};
        ui_text_clipped(h, 12, k == metric_idx(c) ? UI_ACCENT : UI_TEXT_DIM, mts[k].name);
        if (CheckCollisionPointRec(GetMousePosition(), h)) ui_tooltip("%s\n%s", mts[k].tip, mts[k].lower_better ? "lower is better" : "higher is better");
    }
    y += 20;
    double best[16], worst[16];
    bool has[16];
    for (int k = 0; k < nm && k < 16; k++) has[k] = best_worst(c, ci, &mts[k], &best[k], &worst[k]);

    Vector2 mouse = GetMousePosition();
    for (int a = 0; a < cmp_nalgs(c); a++) {
        if (!cmp_alg_on(c, a)) continue;
        Rectangle row = {r.x + 2, y - 1, r.width - 4, 21};
        bool hov = CheckCollisionPointRec(mouse, row);
        bool picked = c->mode == CMP_VM && c->tbl_alg == a;
        if (hov) { DrawRectangleRec(row, UI_PANEL_HI); c->hover_alg = a; }
        if (picked) DrawRectangleLinesEx(row, 1, UI_ACCENT);
        DrawRectangleRounded((Rectangle){x0, y + 4, 10, 10}, 0.3f, 4, cmp_alg_color(a));
        ui_text(x0 + 16, y + 1, 14, UI_TEXT, "%s", cmp_alg_name(c, a));
        const cmp_run_t *run = &c->runs[ci][a];
        if (!run->valid || run->rc != SIM_OK) {
            ui_text(x0 + name_w, y + 1, 13, UI_BAD, "%s", run->valid ? sim_strerror(run->rc) : "not run");
        } else {
            for (int k = 0; k < nm; k++) {
                double v = mts[k].get(&run->m);
                Color col = UI_TEXT;
                if (has[k] && best[k] != worst[k] && run->m.finished) {
                    if (v == best[k]) col = UI_GOOD;
                    else if (v == worst[k]) col = UI_BAD;
                }
                char val[32];
                snprintf(val, sizeof(val), mts[k].fmt, v);
                ui_text(x0 + name_w + k * cw, y + 1, 14, col, "%s%s", val, run->m.finished ? "" : "*");
            }
        }
        Rectangle open = {r.x + r.width - open_w - 6, y, open_w, 19};
        if (ui_button(CID(200 + a), open, "Open")) cmp_open_in_sim(g, ci, a);
        if (CheckCollisionPointRec(mouse, open)) {
            ui_tooltip("Open this test case in Simulate with %s\nto step through every decision", cmp_alg_name(c, a));
        }
        if (hov && c->mode == CMP_VM && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && c->tbl_alg != a &&
            mouse.x < r.x + r.width - open_w - 10) {
            c->tbl_alg = a;
            c->dirty = true;
        }
        y += 22;
    }
    return y - r.y;
}

/* ------------------------------------------------------------------------
 * Scheduling detail: one Gantt chart per algorithm, same time axis
 * ------------------------------------------------------------------------ */

static void gantt_stack(gui_t *g, Rectangle r, int ci) {
    compare_t *c = g->cmp;
    cmp_case_t *k = &c->cases[ci];
    int n = 0;
    uint32_t span = 1;
    for (int a = 0; a < cmp_nalgs(c); a++) {
        if (!run_ok(c, ci, a)) continue;
        n++;
        if (c->runs[ci][a].len > span) span = c->runs[ci][a].len;
    }
    ui_text(r.x + 8, r.y, 13, UI_ACCENT, "CPU GANTT CHART PER ALGORITHM");
    ui_text(r.x + 240, r.y + 1, 12, UI_TEXT_DIM, "same time axis: a shorter bar finished sooner. Hover a block for details.");
    if (n == 0) return;
    const float label_w = 110;
    float top = r.y + 20, avail = r.height - 20 - 20;
    float rh = avail / n;
    if (rh > 56) rh = 56;
    int hovered = -1;
    float plot_x = r.x + 8 + label_w, plot_w = r.width - 16 - label_w;
    int row = 0;
    for (int a = 0; a < cmp_nalgs(c); a++) {
        if (!run_ok(c, ci, a)) continue;
        const cmp_run_t *run = &c->runs[ci][a];
        float y = top + row * rh;
        row++;
        DrawRectangleRounded((Rectangle){r.x + 8, y + rh / 2 - 6, 10, 10}, 0.3f, 4, cmp_alg_color(a));
        ui_text(r.x + 24, y + rh / 2 - 9, 14, UI_TEXT, "%s", cmp_alg_name(c, a));
        lanes_t L = {run->lanes, CMP_LANE_CAP, PRISM_MAX_CORES, run->ncores, run->len};
        bool last = (row == n);
        Rectangle gr = {plot_x, y + 2, plot_w, rh - 6 + (last ? 18 : 0)};
        int h = gantt_draw(gr, &L, 0, span, cmp_case_name, k, last, -1);
        if (h >= 0) { hovered = h; c->hover_alg = a; }
    }
    if (hovered >= 0) {
        float lab = c->runs[ci][0].ncores > 1 ? 34 : 0;
        float cx = plot_x + lab + (hovered + 0.5f) * (plot_w - lab) / span;
        DrawLineEx((Vector2){cx, top}, (Vector2){cx, top + row * rh}, 1, ColorAlpha(UI_TEXT, 0.6f));
    }
    if (span >= CMP_LANE_CAP) ui_text(r.x + 8, r.y + r.height - 14, 11, UI_WARN, "showing the first %d ticks", CMP_LANE_CAP);
}

/* ------------------------------------------------------------------------
 * Memory detail: faults vs frames, and the textbook frame table
 * ------------------------------------------------------------------------ */

static void sweep_chart(gui_t *g, Rectangle r) {
    compare_t *c = g->cmp;
    ui_text(r.x + 8, r.y, 13, UI_ACCENT, "FAULTS vs NUMBER OF FRAMES");
    if (!c->sweep_ok) return;
    Rectangle p = {r.x + 44, r.y + 26, r.width - 60, r.height - 26 - 50};
    uint32_t ymax = 1;
    for (int a = 0; a < REPL_COUNT; a++) {
        if (!c->repl_on[a]) continue;
        for (int f = c->sweep_lo; f <= c->sweep_hi; f++) if (c->sweep[a][f] > ymax) ymax = c->sweep[a][f];
    }
    int nx = c->sweep_hi - c->sweep_lo;
    if (nx < 1) nx = 1;
    float dx = p.width / nx;
    DrawLine((int)p.x, (int)(p.y + p.height), (int)(p.x + p.width), (int)(p.y + p.height), UI_BORDER);
    DrawLine((int)p.x, (int)p.y, (int)p.x, (int)(p.y + p.height), UI_BORDER);
    for (int k = 0; k <= 2; k++) {
        float y = p.y + p.height - p.height * k / 2.0f;
        ui_text(p.x - 36, y - 7, 11, UI_TEXT_DIM, "%5u", ymax * (uint32_t)k / 2);
        if (k) DrawLine((int)p.x, (int)y, (int)(p.x + p.width), (int)y, (Color){40, 44, 54, 255});
    }
    int lstep = dx >= 18 ? 1 : (dx >= 8 ? 2 : 4);
    for (int f = c->sweep_lo; f <= c->sweep_hi; f += lstep) {
        float x = p.x + (f - c->sweep_lo) * dx;
        ui_text(x - 4, p.y + p.height + 4, 11, f == c->sweep_cur ? UI_ACCENT : UI_TEXT_DIM, "%d", f);
    }
    ui_text(p.x + p.width - 40, p.y + p.height + 16, 11, UI_TEXT_DIM, "frames");
    if (c->sweep_cur >= c->sweep_lo && c->sweep_cur <= c->sweep_hi) {
        float x = p.x + (c->sweep_cur - c->sweep_lo) * dx;
        for (float y = p.y; y < p.y + p.height; y += 8) DrawLine((int)x, (int)y, (int)x, (int)(y + 4), UI_ACCENT);
    }

    Vector2 mouse = GetMousePosition();
    char belady[160] = "";
    for (int a = 0; a < REPL_COUNT; a++) {
        if (!c->repl_on[a]) continue;
        Color col = cmp_alg_color(a);
        /* Earlier policies draw wider so identical curves stay visible underneath. */
        float lw = 2.0f + (REPL_COUNT - 1 - a) * 1.8f;
        Vector2 prev = {0};
        for (int f = c->sweep_lo; f <= c->sweep_hi; f++) {
            Vector2 pt = {p.x + (f - c->sweep_lo) * dx, p.y + p.height - p.height * (float)c->sweep[a][f] / ymax};
            if (f > c->sweep_lo) DrawLineEx(prev, pt, lw, col);
            DrawCircleV(pt, lw / 2 + 1.5f, col);
            if (f > c->sweep_lo && c->sweep[a][f] > c->sweep[a][f - 1]) {
                DrawRing(pt, 6, 9, 0, 360, 20, UI_BAD);
                if (!belady[0]) {
                    snprintf(belady, sizeof(belady), "Belady's anomaly! %s: %d frames -> %u faults, but %d frames -> %u faults.",
                             cmp_alg_name(c, a), f - 1, c->sweep[a][f - 1], f, c->sweep[a][f]);
                }
            }
            if (CheckCollisionPointCircle(mouse, pt, 6)) ui_tooltip("%s with %d frames: %u faults", cmp_alg_name(c, a), f, c->sweep[a][f]);
            prev = pt;
        }
    }
    float ty = r.y + r.height - 18;
    if (belady[0]) ui_text(r.x + 8, ty, 13, UI_BAD, "%s", belady);
    else ui_text(r.x + 8, ty, 12, UI_TEXT_DIM, "More frames never added faults here (no Belady's anomaly). Dashed line = frames used above.");
}

static void frame_table(gui_t *g, Rectangle r) {
    compare_t *c = g->cmp;
    ui_text(r.x + 8, r.y, 13, UI_ACCENT, "FRAME TABLE");
    float bx = r.x + 110;
    for (int a = 0; a < REPL_COUNT; a++) {
        const char *nm = repl_get_ops((repl_policy_t)a)->name;
        float w = ui_text_width(nm, 14) + 16;
        if (ui_button_ex(CID(300 + a), (Rectangle){bx, r.y - 3, w, 20}, nm, true, c->tbl_alg == a) && c->tbl_alg != a) {
            c->tbl_alg = a;
            c->dirty = true;
        }
        bx += w + 4;
    }
    if (!c->tbl_ok) {
        ui_text_wrapped(r.x + 8, r.y + 30, r.width - 16, 13, UI_TEXT_DIM,
                        c->tbl_frames > CMP_TBL_FRAMES ? "The frame table is shown for up to 16 frames: lower the RAM frames setting."
                                                       : "No page references in this test case.");
        return;
    }
    int nf = c->tbl_frames;
    float top = r.y + 24, avail = r.height - 24 - 40;
    float rh = avail / (nf + 2);
    if (rh > 22) rh = 22;
    const float lab = 54, cw = 26;
    int vis = (int)((r.width - 16 - lab) / cw);
    if (vis < 1) vis = 1;
    Rectangle area = {r.x, top, r.width, rh * (nf + 2)};
    if (CheckCollisionPointRec(GetMousePosition(), area)) c->tbl_scroll -= (int)GetMouseWheelMove() * 4;
    if (c->tbl_scroll > c->ncol - vis) c->tbl_scroll = c->ncol - vis;
    if (c->tbl_scroll < 0) c->tbl_scroll = 0;

    bool multi = false;
    for (int i = 1; i < c->ncol; i++) if (c->col[i].pid != c->col[0].pid) multi = true;
    float x0 = r.x + 8;
    ui_text(x0, top + 3, 12, UI_TEXT_DIM, "ref");
    for (int f = 0; f < nf; f++) ui_text(x0, top + (f + 1) * rh + 3, 12, UI_TEXT_DIM, "frame %d", f);
    ui_text(x0, top + (nf + 1) * rh + 3, 12, UI_TEXT_DIM, "fault?");
    int faults = 0;
    for (int i = 0; i < c->ncol; i++) faults += c->col[i].fault;

    ui_scissor_push((Rectangle){x0 + lab, top, r.width - lab - 16, rh * (nf + 2)});
    for (int v = 0; v < vis && c->tbl_scroll + v < c->ncol; v++) {
        const cmp_col_t *col = &c->col[c->tbl_scroll + v];
        float x = x0 + lab + v * cw;
        Color hc = multi ? proc_color(col->pid) : UI_TEXT;
        ui_text(x + 6, top + 3, 13, hc, "%d", col->page);
        for (int f = 0; f < nf; f++) {
            Rectangle cell = {x + 1, top + (f + 1) * rh + 1, cw - 2, rh - 2};
            bool loaded = col->fault && col->loaded == f;
            bool hit = !col->fault && col->hit_frame == f;
            Color bg = loaded ? ColorAlpha(UI_BAD, 0.55f) : (Color){30, 33, 40, 255};
            DrawRectangleRec(cell, bg);
            if (hit) DrawRectangleLinesEx(cell, 2, UI_GOOD);
            if (col->fpage[f] >= 0) {
                Color tc = multi ? proc_color(col->fpid[f]) : UI_TEXT;
                if (loaded) tc = UI_TEXT;
                ui_text(cell.x + 5, cell.y + (rh - 2 - 13) / 2, 13, tc, "%d", col->fpage[f]);
            }
        }
        float fy = top + (nf + 1) * rh + 3;
        if (col->fault) ui_text(x + 8, fy, 13, UI_BAD, "F");
        else ui_text(x + 10, fy, 13, UI_TEXT_DIM, ".");
        if (CheckCollisionPointRec(GetMousePosition(), (Rectangle){x, top, cw, rh * (nf + 2)})) {
            ui_tooltip("reference %d: %s page %d\n%s", c->tbl_scroll + v + 1, c->cases[c->focus].names[col->pid], col->page,
                       col->fault ? TextFormat("FAULT: loaded into frame %d", col->loaded) : TextFormat("hit in frame %d", col->hit_frame));
        }
    }
    ui_scissor_pop();
    float y = top + rh * (nf + 2) + 6;
    ui_text(x0, y, 13, UI_TEXT, "%s: %d references, %d faults (%.0f%%)%s", repl_get_ops((repl_policy_t)c->tbl_alg)->name, c->ncol,
            faults, c->ncol ? 100.0 * faults / c->ncol : 0.0, c->tbl_trunc ? " (first 256 shown)" : "");
    ui_text(x0, y + 18, 11, UI_TEXT_DIM, "red = page loaded on a fault (replacing what was there)  |  green box = hit  |  wheel to scroll%s",
            multi ? "  |  colours = process" : "");
}

/* ------------------------------------------------------------------------ */

void compare_view(gui_t *g, Rectangle r) {
    compare_t *c = g->cmp;
    if (!cmp_case_usable(c, c->focus)) {
        for (int i = 0; i < c->ncases; i++) if (cmp_case_usable(c, i)) { c->focus = i; c->dirty = true; break; }
    }
    if (c->dirty) cmp_run_all(g);

    setup_panel(g, (Rectangle){r.x, r.y, SETUP_W, r.height});
    Rectangle R = {r.x + SETUP_W, r.y, r.width - SETUP_W, r.height};
    float h1 = R.height * 0.34f;
    if (h1 < 220) h1 = 220;
    bar_chart(g, (Rectangle){R.x, R.y, R.width, h1});

    Rectangle d = {R.x, R.y + h1, R.width, R.height - h1};
    ui_claim_rect(d);
    ui_panel(d, NULL);
    cmp_case_t *k = &c->cases[c->focus];
    float y = d.y + 8;
    ui_text(d.x + 10, y, 17, UI_TEXT, "%s", k->label);
    ui_text(d.x + 20 + ui_text_width(k->label, 17), y + 3, 12, UI_TEXT_DIM, "%d processes, %d core%s, %d frames, quantum %d",
            k->nproc, k->cores, k->cores == 1 ? "" : "s", c->mode == CMP_VM && c->frames > 0 ? c->frames : k->frames,
            c->mode == CMP_SCHED && c->quantum > 0 ? c->quantum : k->quantum);
    y += 24;
    if (k->desc[0]) y += ui_text_wrapped(d.x + 10, y, d.width - 20, 13, UI_TEXT_DIM, k->desc) + 4;
    y += 4;
    y += results_table(g, (Rectangle){d.x, y, d.width, 0}, c->focus) + 8;
    DrawLine((int)d.x + 8, (int)y - 4, (int)(d.x + d.width - 8), (int)y - 4, UI_BORDER);

    Rectangle rest = {d.x, y + 4, d.width, d.y + d.height - y - 26};
    if (c->mode == CMP_SCHED) {
        gantt_stack(g, rest, c->focus);
    } else {
        float sw = rest.width * 0.40f;
        sweep_chart(g, (Rectangle){rest.x, rest.y, sw, rest.height});
        frame_table(g, (Rectangle){rest.x + sw + 8, rest.y, rest.width - sw - 8, rest.height});
    }
    ui_text(d.x + 10, d.y + d.height - 18, 11, UI_TEXT_DIM,
            "Green = best, red = worst in a column; * = did not finish within %d ticks. Recomputed in %.0f ms whenever a setting changes. "
            "\"Open\" loads the case in Simulate to step through it.", CMP_MAX_TICKS, c->run_ms);
}
