#include "gui_state.h"
#include "prism/metrics.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Widget ids: toolbar 1..99, palette 100..199, inspector 200..499 (inspector.c), panels 500..699. */

static bool proc_live(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

/* ------------------------------------------------------------------------
 * Toolbar
 * ------------------------------------------------------------------------ */

const char *g_sched_items[SCHED_COUNT];
const char *g_repl_items[REPL_COUNT];

void panels_init(void) {
    for (int i = 0; i < SCHED_COUNT; i++) g_sched_items[i] = sched_get_ops((sched_policy_t)i)->name;
    for (int i = 0; i < REPL_COUNT; i++) g_repl_items[i] = repl_get_ops((repl_policy_t)i)->name;
}

/* Simulate | Compare switch, shared by both views' toolbars. */
static float view_tabs(gui_t *g, float x, float y, float h) {
    if (ui_button_ex(20, (Rectangle){x, y, 84, h}, "Simulate", true, g->view == VIEW_SIM)) g->view = VIEW_SIM;
    if (ui_button_ex(21, (Rectangle){x + 86, y, 84, h}, "Compare", true, g->view == VIEW_COMPARE)) {
        if (g->view != VIEW_COMPARE) { g->playing = false; compare_mark_dirty(g); }
        g->view = VIEW_COMPARE;
    }
    return x + 186;
}

void panel_toolbar(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    DrawRectangleRec(r, UI_PANEL);
    DrawLine((int)r.x, (int)(r.y + r.height - 1), (int)(r.x + r.width), (int)(r.y + r.height - 1), UI_BORDER);
    float x = r.x + 10, y = r.y + 9, h = 30;
    sim_t *s = g->sim;
    x = view_tabs(g, x, y, h);
    if (g->view == VIEW_COMPARE) {
        ui_text(x, y + 6, 15, UI_TEXT_DIM, "Run the same test cases under several algorithms and compare the results side by side.");
        return;
    }

    if (ui_button_ex(1, (Rectangle){x, y, 70, h}, g->playing ? "Pause" : "Run", true, g->playing)) {
        if (g->playing) g->playing = false;
        else if (gui_start_running(g)) g->playing = true;
    }
    x += 74;
    if (ui_button_ex(2, (Rectangle){x, y, 58, h}, "< Back", s->tick > 0, false)) gui_step_back(g);
    x += 62;
    if (ui_button(3, (Rectangle){x, y, 64, h}, "Step >")) gui_step_event(g);
    x += 68;
    if (ui_button(4, (Rectangle){x, y, 56, h}, "Reset")) gui_enter_edit(g);
    x += 64;

    ui_text(x, y - 2, 12, UI_TEXT_DIM, "speed %.0f ticks/s", g->speed);
    ui_slider(5, (Rectangle){x, y + 12, 110, 18}, &g->speed, 1, 60);
    x += 122;
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "tick");
    ui_text(x, y + 11, 18, g->mode == MODE_RUN ? UI_GOOD : UI_TEXT, "%" PRIu64, (uint64_t)s->tick);
    x += 56;

    int sp = (int)s->sched_policy;
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "scheduler (live)");
    if (ui_dropdown(6, (Rectangle){x, y + 11, 116, 22}, g_sched_items, SCHED_COUNT, &sp)) {
        sim_set_sched(s, sched_get_ops((sched_policy_t)sp)->key, 0);
        gui_status(g, false, "Scheduler -> %s%s", g_sched_items[sp], g->mode == MODE_RUN ? " (applied live)" : "");
    }
    x += 122;
    int rp = (int)s->repl_policy;
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "replacement (live)");
    if (ui_dropdown(7, (Rectangle){x, y + 11, 100, 22}, g_repl_items, REPL_COUNT, &rp)) {
        sim_set_repl(s, repl_get_ops((repl_policy_t)rp)->key);
        gui_status(g, false, "Replacement -> %s", g_repl_items[rp]);
    }
    x += 108;

    if (ui_button(8, (Rectangle){x, y, 92, h}, g->mode == MODE_RUN ? "Inject proc" : "+ Process")) {
        builder_open_new(g, g->mode == MODE_RUN);
    }
    x += 100;

    ui_text(x, y - 2, 12, UI_TEXT_DIM, "test case");
    int ps = g->preset_sel;
    if (ui_dropdown(10, (Rectangle){x, y + 11, 130, 22}, g->preset_items, g->npresets + 1, &ps)) {
        g->preset_sel = ps;
        if (ps == 0) gui_new_scenario(g);
        else gui_load(g, g->presets[ps - 1]);
    }
    x += 138;
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "file");
    float fw = r.x + r.width - x - 132;
    if (fw < 80) fw = 80;
    ui_text_field(11, (Rectangle){x, y + 11, fw, 22}, g->path, sizeof(g->path));
    x += fw + 6;
    if (ui_button(12, (Rectangle){x, y, 58, h}, "Load")) gui_load(g, g->path);
    x += 62;
    if (ui_button(13, (Rectangle){x, y, 58, h}, "Save")) gui_save(g, g->path);
}

/* ------------------------------------------------------------------------
 * Palette
 * ------------------------------------------------------------------------ */

void panel_palette(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, "Devices");
    float x = r.x + 10, y = r.y + 34, w = r.width - 20;
    ui_text(x, y, 12, UI_TEXT_DIM, "drag onto the canvas");
    y += 20;
    if (ui_button(100, (Rectangle){x, y, w, 34}, "Process")) g->palette_drag = NODE_PROC;
    y += 38;
    if (ui_button(99, (Rectangle){x, y, w, 26}, "+ 5 random")) gui_random_processes(g, 5);
    y += 34;
    for (int k = 0; k < NODE_SYS_COUNT; k++) {
        bool present = gui_has_node(g, k);
        if (ui_button_ex(101 + k, (Rectangle){x, y, w, 30}, g_sys_titles[k], !present && g->mode == MODE_EDIT, false)) {
            g->palette_drag = k;
        }
        y += 36;
    }

    y += 10;
    ui_text(x, y, 13, UI_TEXT_DIM, "Process states");
    y += 20;
    static const proc_state_t st[] = {PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED_IO, PROC_BLOCKED_FAULT, PROC_TERMINATED};
    static const char *nm[] = {"new", "ready", "running", "blocked: I/O", "blocked: page fault", "terminated"};
    for (int i = 0; i < 6; i++) {
        DrawRectangleRounded((Rectangle){x, y + 2, 12, 12}, 0.3f, 4, state_color(st[i]));
        ui_text(x + 18, y, 13, UI_TEXT, "%s", nm[i]);
        y += 18;
    }
    y += 10;
    ui_text(x, y, 13, UI_TEXT_DIM, "Keys");
    y += 18;
    static const char *keys[] = {"Space  run / pause", "N  step to next event", "T  one tick forward",
                                 "B  one tick back", "R  reset to edit", "A  add / inject process",
                                 "Tab  Simulate / Compare", "Del  delete selected", "F  fit view",
                                 "Ctrl+S / Ctrl+O  save/load", "wheel zoom, mid-drag pan"};
    for (int i = 0; i < 11 && y < r.y + r.height - 16; i++) {
        ui_text(x, y, 12, UI_TEXT_DIM, "%s", keys[i]);
        y += 16;
    }
}

void panel_kv(cursor_t *c, const char *k, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_text(c->x, c->y, 14, UI_TEXT_DIM, "%s", k);
    ui_text(c->x + 130, c->y, 14, UI_TEXT, "%s", buf);
    c->y += 19;
}

void panel_heading(cursor_t *c, const char *t) {
    c->y += 6;
    ui_text(c->x, c->y, 14, UI_ACCENT, "%s", t);
    c->y += 20;
}

/* ------------------------------------------------------------------------
 * Event list
 * ------------------------------------------------------------------------ */

Color event_color(event_type_t t) {
    switch (t) {
        case EVENT_PROC_ARRIVE:      return (Color){130, 180, 255, 255};
        case EVENT_PROC_DISPATCH:    return UI_GOOD;
        case EVENT_PROC_PREEMPT:     return (Color){160, 160, 255, 255};
        case EVENT_PROC_BLOCK_IO:
        case EVENT_PROC_WAKE_IO:     return UI_WARN;
        case EVENT_PAGE_FAULT:
        case EVENT_PROC_BLOCK_FAULT:
        case EVENT_PAGE_EVICT:       return UI_BAD;
        case EVENT_PROC_WAKE_FAULT:  return (Color){240, 140, 140, 255};
        case EVENT_PROC_TERMINATE:   return UI_TEXT;
        case EVENT_PAGE_ACCESS:      return UI_TEXT_DIM;
        default:                     return (Color){200, 150, 240, 255};
    }
}

static void event_detail(const gui_t *g, const sim_event_t *e, char *buf, size_t n) {
    switch (e->type) {
        case EVENT_PROC_DISPATCH:    snprintf(buf, n, "on core %d", e->core_id); break;
        case EVENT_PROC_BLOCK_IO:    snprintf(buf, n, "I/O for %d ticks", e->detail1); break;
        case EVENT_PAGE_ACCESS:      snprintf(buf, n, "page %d hit (frame %d)", e->detail1, e->detail2); break;
        case EVENT_PAGE_FAULT:       snprintf(buf, n, "page %d not resident", e->detail1); break;
        case EVENT_PROC_BLOCK_FAULT: snprintf(buf, n, "waits %d ticks for page %d", g->sim->disk_latency, e->detail1); break;
        case EVENT_PROC_WAKE_FAULT:  snprintf(buf, n, "page %d loaded", e->detail1); break;
        case EVENT_PAGE_EVICT:       snprintf(buf, n, "page %d evicted from frame %d", e->detail1, e->detail2); break;
        case EVENT_RAM_SHRUNK:       snprintf(buf, n, "%d frames", e->detail1); break;
        default:                     buf[0] = '\0'; break;
    }
}

static bool event_shown(const gui_t *g, const sim_event_t *e) {
    if (g->hide_hits && e->type == EVENT_PAGE_ACCESS) return false;
    if (g->ev_filter_pid >= 0 && e->pid != g->ev_filter_pid) return false;
    return true;
}

/* r is the pane body (below the tab strip). */
void panel_events(gui_t *g, Rectangle r) {
    bool only_sel = g->ev_filter_pid >= 0;
    ui_checkbox(500, (Rectangle){r.x + 8, r.y + 3, 120, 18}, "hide page hits", &g->hide_hits);
    if (ui_checkbox(501, (Rectangle){r.x + 140, r.y + 3, 160, 18}, "selected process only", &only_sel)) {
        g->ev_filter_pid = (only_sel && g->sel.kind == NODE_PROC) ? g->sel.pid : -1;
    }

    Rectangle body = {r.x, r.y + 24, r.width, r.height - 24};
    const float rh = 17;
    int rows = (int)(body.height / rh);
    if (CheckCollisionPointRec(GetMousePosition(), body)) {
        float wh = GetMouseWheelMove();
        g->ev_scroll += wh * 3;
        if (g->ev_scroll < 0) g->ev_scroll = 0;
    }

    /* Collect the visible window walking back from the newest event. */
    int skip = (int)g->ev_scroll;
    int idx[64];
    int n = 0;
    for (int i = (int)g->ev_len - 1; i >= 0 && n < rows && n < 64; i--) {
        const sim_event_t *e = &g->ev[(g->ev_start + (uint32_t)i) % GUI_EV_CAP];
        if (!event_shown(g, e)) continue;
        if (skip > 0) { skip--; continue; }
        idx[n++] = i;
    }
    if (n < rows && g->ev_scroll > 0 && skip > 0) g->ev_scroll -= (float)skip;

    ui_scissor_push(body);
    for (int k = 0; k < n; k++) {
        const sim_event_t *e = &g->ev[(g->ev_start + (uint32_t)idx[k]) % GUI_EV_CAP];
        float y = body.y + body.height - (k + 1) * rh;
        Rectangle row = {body.x, y, body.width, rh};
        bool hov = CheckCollisionPointRec(GetMousePosition(), row) && ui_content_input();
        if (hov) {
            DrawRectangleRec(row, UI_PANEL_HI);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && e->pid >= 0 && e->pid < g->sim->num_processes) {
                g->sel = (node_ref_t){NODE_PROC, e->pid};
                g->playing = false;
            }
        }
        char det[96];
        event_detail(g, e, det, sizeof(det));
        ui_text(body.x + 8, y + 1, 13, UI_TEXT_DIM, "%5" PRIu64, (uint64_t)e->tick);
        ui_text(body.x + 56, y + 1, 13, event_color(e->type), "%s", event_type_str(e->type));
        const char *name = (e->pid >= 0 && e->pid < g->sim->num_processes) ? g->sim->procs[e->pid].name : "-";
        if (e->pid >= 0) DrawRectangle((int)body.x + 170, (int)y + 5, 8, 8, proc_color(e->pid));
        ui_text(body.x + 184, y + 1, 13, UI_TEXT, "%s", name);
        ui_text(body.x + 290, y + 1, 13, UI_TEXT_DIM, "%s", det);
    }
    if (g->ev_len == 0) ui_text(body.x + 10, body.y + 8, 14, UI_TEXT_DIM, "No events yet: press Run or Step.");
    ui_scissor_pop();
    if (g->ev_scroll > 0) ui_text(r.x + r.width - 120, r.y + 4, 12, UI_WARN, "scrolled (wheel)");
}

/* Tabbed pane: header strip of tabs, returns the body rect. */
static Rectangle tab_pane(Rectangle r, int id0, const char *const *names, int n, int *tab) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    DrawRectangle((int)r.x, (int)r.y, (int)r.width, 26, UI_PANEL_HI);
    float x = r.x + 4;
    for (int i = 0; i < n; i++) {
        float w = ui_text_width(names[i], 14) + 22;
        if (ui_button_ex(id0 + i, (Rectangle){x, r.y + 2, w, 22}, names[i], true, *tab == i)) *tab = i;
        x += w + 4;
    }
    return (Rectangle){r.x + 1, r.y + 27, r.width - 2, r.height - 28};
}

void panel_bottom_left(gui_t *g, Rectangle r) {
    static const char *names[LTAB_COUNT] = {"Explain", "Event list"};
    Rectangle body = tab_pane(r, 520, names, LTAB_COUNT, &g->ltab);
    if (g->ltab == LTAB_EXPLAIN) panel_explain(g, body);
    else panel_events(g, body);
}

void panel_bottom_mid(gui_t *g, Rectangle r) {
    static const char *names[MTAB_COUNT] = {"CPU Gantt chart", "Process states", "Results table"};
    Rectangle body = tab_pane(r, 530, names, MTAB_COUNT, &g->mtab);
    if (g->mtab == MTAB_GANTT) panel_cpu_gantt(g, body);
    else if (g->mtab == MTAB_STATES) panel_states(g, body);
    else panel_proc_table(g, body);
}

/* ------------------------------------------------------------------------
 * Metrics
 * ------------------------------------------------------------------------ */

void panel_metrics(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, "Metrics (completed processes)");
    const sim_t *s = g->sim;
    cursor_t c = {r.x + 12, r.y + 32, r.width - 24};
    sim_metrics_t m;
    sim_metrics_compute(s, &m);

    /* Thrashing: fault share of the most recent memory references. */
    int acc = 0, flt = 0;
    for (int i = (int)g->ev_len - 1; i >= 0 && acc < 40; i--) {
        const sim_event_t *e = &g->ev[(g->ev_start + (uint32_t)i) % GUI_EV_CAP];
        if (e->type == EVENT_PAGE_ACCESS) acc++;
        else if (e->type == EVENT_PAGE_FAULT) { acc++; flt++; }
    }
    bool thrash = acc >= 20 && flt * 100 / acc >= 40;

    bool d = m.completed > 0;
    panel_kv(&c, "completed", "%d / %d", m.completed, m.processes);
    panel_kv(&c, "avg waiting", d ? "%.2f" : "-", m.avg_wait);
    panel_kv(&c, "avg turnaround", d ? "%.2f" : "-", m.avg_turnaround);
    panel_kv(&c, "avg response", d ? "%.2f" : "-", m.avg_response);
    panel_kv(&c, "max waiting", "%u", m.max_wait);
    panel_kv(&c, "CPU utilisation", "%.1f%%", m.cpu_util);
    panel_kv(&c, "throughput", "%.2f / 100 ticks", m.throughput);
    panel_kv(&c, "page faults", "%u  (%.1f%%)", m.page_faults, m.fault_rate);
    panel_kv(&c, "context switches", "%u", m.ctx_switches);
    if (thrash) {
        DrawRectangleRounded((Rectangle){c.x, c.y + 2, c.w, 22}, 0.3f, 4, ColorAlpha(UI_BAD, 0.3f));
        ui_text(c.x + 8, c.y + 5, 14, UI_BAD, "THRASHING: %d%% of recent refs fault", flt * 100 / acc);
        c.y += 26;
    }
    ui_text(c.x, r.y + r.height - 20, 12, UI_TEXT_DIM, "trace hash %016" PRIx64, s->log_hash);
}

/* ------------------------------------------------------------------------
 * Right-click menu
 * ------------------------------------------------------------------------ */

void panel_context_menu(gui_t *g) {
    if (!g->ctx_open) return;
    node_ref_t n = g->ctx_node;
    if (n.kind == NODE_PROC && !proc_live(g->sim, n.pid)) { g->ctx_open = false; return; }
    const char *items[3];
    int act[3], cnt = 0;
    if (n.kind == NODE_PROC) {
        const pcb_t *p = &g->sim->procs[n.pid];
        if (p->state == PROC_NEW && p->start_tick < 0) { items[cnt] = "Edit..."; act[cnt++] = 0; }
        items[cnt] = "Duplicate"; act[cnt++] = 1;
        if (p->state == PROC_NEW || p->state == PROC_TERMINATED) { items[cnt] = "Delete"; act[cnt++] = 2; }
    } else if (g->mode == MODE_EDIT) {
        items[cnt] = "Delete"; act[cnt++] = 2;
    } else {
        items[cnt] = "(locked while running)"; act[cnt++] = -1;
    }
    ui_push_layer(1);
    Rectangle m = {g->ctx_pos.x, g->ctx_pos.y, 190, 28.0f * cnt + 8};
    ui_claim_rect(m);
    DrawRectangleRec((Rectangle){m.x + 3, m.y + 4, m.width, m.height}, (Color){0, 0, 0, 100});
    DrawRectangleRec(m, UI_PANEL_HI);
    DrawRectangleLinesEx(m, 1, UI_BORDER);
    for (int i = 0; i < cnt; i++) {
        if (ui_button_ex(600 + i, (Rectangle){m.x + 4, m.y + 4 + i * 28.0f, m.width - 8, 26}, items[i], act[i] >= 0, false)) {
            g->ctx_open = false;
            if (act[i] == 0) builder_open_edit(g, n.pid);
            else if (act[i] == 1) gui_duplicate_process(g, n.pid);
            else if (act[i] == 2) gui_delete_node(g, n);
        }
    }
    ui_pop_layer();
    if ((IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) &&
        !CheckCollisionPointRec(GetMousePosition(), m)) {
        g->ctx_open = false;
    }
}
