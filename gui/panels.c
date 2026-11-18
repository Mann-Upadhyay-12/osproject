#include "gui_state.h"
#include "prism/metrics.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TOOLBAR_H         30
#define EVENT_ROW_H       17
#define MAX_EVENT_ROWS    64
#define THRASH_WINDOW     40
#define THRASH_MIN_REFS   20
#define THRASH_PERCENT    40
#define MENU_ITEM_H       28.0f

const char *g_sched_items[SCHED_COUNT];
const char *g_repl_items[REPL_COUNT];

static bool proc_live(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

static const sim_event_t *event_at(const gui_t *g, int i) {
    return &g->events[(g->events_start + (uint32_t)i) % GUI_EV_CAP];
}

void panels_init(void) {
    for (int i = 0; i < SCHED_COUNT; i++) g_sched_items[i] = sched_get_ops((sched_policy_t)i)->name;
    for (int i = 0; i < REPL_COUNT; i++) g_repl_items[i] = repl_get_ops((repl_policy_t)i)->name;
}

static void toggle_playing(gui_t *g) {
    if (g->playing) g->playing = false;
    else if (gui_start_running(g)) g->playing = true;
}

static float draw_view_tabs(gui_t *g, float x, float y) {
    if (ui_button_ex(20, (Rectangle){x, y, 84, TOOLBAR_H}, "Simulate", true, g->view == VIEW_SIM)) g->view = VIEW_SIM;

    if (ui_button_ex(21, (Rectangle){x + 86, y, 84, TOOLBAR_H}, "Compare", true, g->view == VIEW_COMPARE)) {
        if (g->view != VIEW_COMPARE) {
            g->playing = false;
            compare_mark_dirty(g);
        }
        g->view = VIEW_COMPARE;
    }

    if (ui_button_ex(22, (Rectangle){x + 172, y, 70, TOOLBAR_H}, "Labs", true, g->view == VIEW_LABS)) {
        g->playing = false;
        g->view = VIEW_LABS;
    }
    return x + 256;
}

static float draw_playback_controls(gui_t *g, float x, float y) {
    sim_t *s = g->sim;

    if (ui_button_ex(1, (Rectangle){x, y, 70, TOOLBAR_H}, g->playing ? "Pause" : "Run", true, g->playing)) toggle_playing(g);
    x += 74;
    if (ui_button_ex(2, (Rectangle){x, y, 58, TOOLBAR_H}, "< Back", s->tick > 0, false)) gui_step_back(g);
    x += 62;
    if (ui_button(3, (Rectangle){x, y, 64, TOOLBAR_H}, "Step >")) gui_step_event(g);
    x += 68;
    if (ui_button(4, (Rectangle){x, y, 56, TOOLBAR_H}, "Reset")) gui_enter_edit(g);
    x += 64;

    ui_text(x, y - 2, 12, UI_TEXT_DIM, "speed %.0f ticks/s", g->speed);
    ui_slider(5, (Rectangle){x, y + 12, 110, 18}, &g->speed, 1, 60);
    x += 122;

    ui_text(x, y - 2, 12, UI_TEXT_DIM, "tick");
    ui_text(x, y + 11, 18, g->mode == MODE_RUN ? UI_GOOD : UI_TEXT, "%" PRIu64, (uint64_t)s->tick);
    return x + 56;
}

static float draw_policy_pickers(gui_t *g, float x, float y) {
    sim_t *s = g->sim;

    int sched = (int)s->sched_policy;
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "scheduler (live)");
    if (ui_dropdown(6, (Rectangle){x, y + 11, 116, 22}, g_sched_items, SCHED_COUNT, &sched)) {
        sim_set_sched(s, sched_get_ops((sched_policy_t)sched)->key, 0);
        gui_status(g, false, "Scheduler -> %s%s", g_sched_items[sched], g->mode == MODE_RUN ? " (applied live)" : "");
    }
    x += 122;

    int repl = (int)s->repl_policy;
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "replacement (live)");
    if (ui_dropdown(7, (Rectangle){x, y + 11, 100, 22}, g_repl_items, REPL_COUNT, &repl)) {
        sim_set_repl(s, repl_get_ops((repl_policy_t)repl)->key);
        gui_status(g, false, "Replacement -> %s", g_repl_items[repl]);
    }
    return x + 108;
}

static void draw_file_controls(gui_t *g, Rectangle bar, float x, float y) {
    ui_text(x, y - 2, 12, UI_TEXT_DIM, "test case");
    int preset = g->preset_selected;
    if (ui_dropdown(10, (Rectangle){x, y + 11, 130, 22}, g->preset_items, g->num_presets + 1, &preset)) {
        g->preset_selected = preset;
        if (preset == 0) gui_new_scenario(g);
        else gui_load(g, g->presets[preset - 1]);
    }
    x += 138;

    ui_text(x, y - 2, 12, UI_TEXT_DIM, "file");
    float field_w = bar.x + bar.width - x - 132;
    if (field_w < 80) field_w = 80;
    ui_text_field(11, (Rectangle){x, y + 11, field_w, 22}, g->path, sizeof(g->path));
    x += field_w + 6;

    if (ui_button(12, (Rectangle){x, y, 58, TOOLBAR_H}, "Load")) gui_load(g, g->path);
    x += 62;
    if (ui_button(13, (Rectangle){x, y, 58, TOOLBAR_H}, "Save")) gui_save(g, g->path);
}

void panel_toolbar(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    DrawRectangleRec(r, UI_PANEL);
    DrawLine((int)r.x, (int)(r.y + r.height - 1), (int)(r.x + r.width), (int)(r.y + r.height - 1), UI_BORDER);

    float x = r.x + 10, y = r.y + 9;
    x = draw_view_tabs(g, x, y);

    if (g->view == VIEW_COMPARE) {
        ui_text(x, y + 6, 15, UI_TEXT_DIM, "Run the same test cases under several algorithms and compare the results side by side.");
        return;
    }
    if (g->view == VIEW_LABS) {
        ui_text(x, y + 6, 15, UI_TEXT_DIM, "Step through classic OS algorithms on your own inputs, the way they are worked out in class.");
        return;
    }

    x = draw_playback_controls(g, x, y);
    x = draw_policy_pickers(g, x, y);

    if (ui_button(8, (Rectangle){x, y, 92, TOOLBAR_H}, g->mode == MODE_RUN ? "Inject proc" : "+ Process")) {
        builder_open_new(g, g->mode == MODE_RUN);
    }
    x += 100;

    draw_file_controls(g, r, x, y);
}

static float draw_state_legend(float x, float y) {
    static const proc_state_t states[] = {
        PROC_NEW, PROC_READY, PROC_RUNNING, PROC_BLOCKED_IO, PROC_BLOCKED_FAULT, PROC_BLOCKED_SEM, PROC_SLEEPING,
        PROC_TERMINATED
    };
    static const char *labels[] = {
        "new", "ready", "running", "blocked: I/O", "blocked: page fault", "blocked: semaphore", "sleeping: next period", "terminated"
    };
    enum { NUM_STATES = sizeof(labels) / sizeof(labels[0]) };

    ui_text(x, y, 13, UI_TEXT_DIM, "Process states");
    y += 20;
    for (int i = 0; i < NUM_STATES; i++) {
        DrawRectangleRounded((Rectangle){x, y + 2, 12, 12}, 0.3f, 4, state_color(states[i]));
        ui_text(x + 18, y, 13, UI_TEXT, "%s", labels[i]);
        y += 18;
    }
    return y;
}

static void draw_key_help(float x, float y, float bottom) {
    static const char *keys[] = {
        "Space  run / pause", "N  step to next event", "T  one tick forward",
        "B  one tick back", "R  reset to edit", "A  add / inject process",
        "Tab  Simulate / Compare", "Del  delete selected", "F  fit view",
        "Ctrl+S / Ctrl+O  save/load", "wheel zoom, mid-drag pan"
    };

    ui_text(x, y, 13, UI_TEXT_DIM, "Keys");
    y += 18;
    for (int i = 0; i < 11 && y < bottom - 16; i++) {
        ui_text(x, y, 12, UI_TEXT_DIM, "%s", keys[i]);
        y += 16;
    }
}

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
        bool can_add = !gui_has_node(g, k) && g->mode == MODE_EDIT;
        if (ui_button_ex(101 + k, (Rectangle){x, y, w, 30}, g_sys_titles[k], can_add, false)) g->palette_drag = k;
        y += 36;
    }

    y = draw_state_legend(x, y + 10);
    draw_key_help(x, y + 10, r.y + r.height);
}

void panel_kv(cursor_t *c, const char *key, const char *fmt, ...) {
    char value[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);

    ui_text(c->x, c->y, 14, UI_TEXT_DIM, "%s", key);
    ui_text(c->x + 130, c->y, 14, UI_TEXT, "%s", value);
    c->y += 19;
}

void panel_heading(cursor_t *c, const char *title) {
    c->y += 6;
    ui_text(c->x, c->y, 14, UI_ACCENT, "%s", title);
    c->y += 20;
}

Color event_color(event_type_t type) {
    switch (type) {
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
        case EVENT_SEM_BLOCKED:
        case EVENT_STALLED:          return UI_BAD;
        case EVENT_SEM_WAKE:         return UI_GOOD;
        case EVENT_DEADLINE_MISS:    return UI_BAD;
        case EVENT_JOB_COMPLETE:     return UI_GOOD;
        case EVENT_JOB_RELEASE:      return (Color){130, 180, 255, 255};
        default:                     return (Color){200, 150, 240, 255};
    }
}

static const char *sem_name(const sim_t *s, int sem_id) {
    return sem_id >= 0 && sem_id < s->num_semaphores ? s->semaphores[sem_id].name : "?";
}

static void describe_event(const gui_t *g, const sim_event_t *e, char *out, size_t size) {
    switch (e->type) {
        case EVENT_PROC_DISPATCH:    snprintf(out, size, "on core %d", e->core_id); break;
        case EVENT_PROC_BLOCK_IO:    snprintf(out, size, "I/O for %d ticks", e->detail1); break;
        case EVENT_PAGE_ACCESS:      snprintf(out, size, "page %d hit (frame %d)", e->detail1, e->detail2); break;
        case EVENT_PAGE_FAULT:       snprintf(out, size, "page %d not resident", e->detail1); break;
        case EVENT_PROC_BLOCK_FAULT: snprintf(out, size, "waits %d ticks for page %d", g->sim->disk_latency, e->detail1); break;
        case EVENT_PROC_WAKE_FAULT:  snprintf(out, size, "page %d loaded", e->detail1); break;
        case EVENT_PAGE_EVICT:       snprintf(out, size, "page %d evicted from frame %d", e->detail1, e->detail2); break;
        case EVENT_RAM_SHRUNK:       snprintf(out, size, "%d frames", e->detail1); break;
        case EVENT_SEM_WAIT:         snprintf(out, size, "wait(%s) passes, value %d", sem_name(g->sim, e->detail1), e->detail2); break;
        case EVENT_SEM_BLOCKED:      snprintf(out, size, "wait(%s) blocks, %d waiting", sem_name(g->sim, e->detail1), e->detail2); break;
        case EVENT_SEM_SIGNAL:       snprintf(out, size, "signal(%s), value %d", sem_name(g->sim, e->detail1), e->detail2); break;
        case EVENT_SEM_WAKE:         snprintf(out, size, "woken on %s", sem_name(g->sim, e->detail1)); break;
        case EVENT_STALLED:          snprintf(out, size, "%d processes blocked forever", e->detail1); break;
        case EVENT_JOB_RELEASE:      snprintf(out, size, "job %d, due t%d", e->detail1, e->detail2); break;
        case EVENT_JOB_COMPLETE:     snprintf(out, size, e->detail2 > 0 ? "job %d, %d late" : "job %d, %d early", e->detail1, e->detail2 > 0 ? e->detail2 : -e->detail2); break;
        case EVENT_DEADLINE_MISS:    snprintf(out, size, "job %d missed t%d", e->detail1, e->detail2); break;
        case EVENT_MIGRATE:          snprintf(out, size, "core %d -> core %d", e->detail1, e->detail2); break;
        default:                     out[0] = '\0'; break;
    }
}

static bool event_visible(const gui_t *g, const sim_event_t *e) {
    if (g->hide_page_hits && e->type == EVENT_PAGE_ACCESS) return false;
    if (g->events_filter_pid >= 0 && e->pid != g->events_filter_pid) return false;
    return true;
}

static void draw_event_filters(gui_t *g, Rectangle r) {
    bool only_selected = g->events_filter_pid >= 0;
    ui_checkbox(500, (Rectangle){r.x + 8, r.y + 3, 120, 18}, "hide page hits", &g->hide_page_hits);
    if (ui_checkbox(501, (Rectangle){r.x + 140, r.y + 3, 160, 18}, "selected process only", &only_selected)) {
        bool process_selected = g->selected.kind == NODE_PROC;
        g->events_filter_pid = only_selected && process_selected ? g->selected.pid : -1;
    }
}

static int collect_visible_events(gui_t *g, int max_rows, int out[]) {
    int skip = (int)g->events_scroll;
    int count = 0;

    for (int i = (int)g->events_len - 1; i >= 0 && count < max_rows && count < MAX_EVENT_ROWS; i--) {
        if (!event_visible(g, event_at(g, i))) continue;
        if (skip > 0) {
            skip--;
            continue;
        }
        out[count++] = i;
    }

    bool scrolled_past_end = count < max_rows && g->events_scroll > 0 && skip > 0;
    if (scrolled_past_end) g->events_scroll -= (float)skip;
    return count;
}

static void draw_event_row(gui_t *g, const sim_event_t *e, Rectangle body, float y) {
    Rectangle row = {body.x, y, body.width, EVENT_ROW_H};
    bool hovered = CheckCollisionPointRec(GetMousePosition(), row) && ui_content_input();
    bool has_process = e->pid >= 0 && e->pid < g->sim->num_processes;

    if (hovered) {
        DrawRectangleRec(row, UI_PANEL_HI);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && has_process) {
            g->selected = (node_ref_t){NODE_PROC, e->pid};
            g->playing = false;
        }
    }

    char detail[96];
    describe_event(g, e, detail, sizeof(detail));
    ui_text(body.x + 8, y + 1, 13, UI_TEXT_DIM, "%5" PRIu64, (uint64_t)e->tick);
    ui_text(body.x + 56, y + 1, 13, event_color(e->type), "%s", event_type_str(e->type));
    if (e->pid >= 0) DrawRectangle((int)body.x + 170, (int)y + 5, 8, 8, proc_color(e->pid));
    ui_text(body.x + 184, y + 1, 13, UI_TEXT, "%s", has_process ? g->sim->procs[e->pid].name : "-");
    ui_text(body.x + 290, y + 1, 13, UI_TEXT_DIM, "%s", detail);
}

void panel_events(gui_t *g, Rectangle r) {
    draw_event_filters(g, r);

    Rectangle body = {r.x, r.y + 24, r.width, r.height - 24};
    int max_rows = (int)(body.height / EVENT_ROW_H);
    if (CheckCollisionPointRec(GetMousePosition(), body)) {
        g->events_scroll += GetMouseWheelMove() * 3;
        if (g->events_scroll < 0) g->events_scroll = 0;
    }

    int visible[MAX_EVENT_ROWS];
    int count = collect_visible_events(g, max_rows, visible);

    ui_scissor_push(body);
    for (int k = 0; k < count; k++) {
        float y = body.y + body.height - (k + 1) * EVENT_ROW_H;
        draw_event_row(g, event_at(g, visible[k]), body, y);
    }
    if (g->events_len == 0) ui_text(body.x + 10, body.y + 8, 14, UI_TEXT_DIM, "No events yet: press Run or Step.");
    ui_scissor_pop();

    if (g->events_scroll > 0) ui_text(r.x + r.width - 120, r.y + 4, 12, UI_WARN, "scrolled (wheel)");
}

static Rectangle tab_pane(Rectangle r, int first_id, const char *const *names, int count, int *tab) {
    ui_claim_rect(r);
    ui_panel(r, NULL);
    DrawRectangle((int)r.x, (int)r.y, (int)r.width, 26, UI_PANEL_HI);

    float x = r.x + 4;
    for (int i = 0; i < count; i++) {
        float w = ui_text_width(names[i], 14) + 22;
        if (ui_button_ex(first_id + i, (Rectangle){x, r.y + 2, w, 22}, names[i], true, *tab == i)) *tab = i;
        x += w + 4;
    }
    return (Rectangle){r.x + 1, r.y + 27, r.width - 2, r.height - 28};
}

void panel_bottom_left(gui_t *g, Rectangle r) {
    static const char *names[LTAB_COUNT] = {"Explain", "Event list", "Sync", "Real-time"};
    Rectangle body = tab_pane(r, 520, names, LTAB_COUNT, &g->left_tab);

    switch (g->left_tab) {
        case LTAB_EXPLAIN: panel_explain(g, body); break;
        case LTAB_EVENTS:  panel_events(g, body); break;
        case LTAB_SYNC:    panel_sync(g, body); break;
        default:           panel_realtime(g, body); break;
    }
}

void panel_bottom_mid(gui_t *g, Rectangle r) {
    static const char *names[MTAB_COUNT] = {"CPU Gantt chart", "Process states", "Results table"};
    Rectangle body = tab_pane(r, 530, names, MTAB_COUNT, &g->mid_tab);

    switch (g->mid_tab) {
        case MTAB_GANTT:  panel_cpu_gantt(g, body); break;
        case MTAB_STATES: panel_states(g, body); break;
        default:          panel_proc_table(g, body); break;
    }
}

static int recent_fault_percent(const gui_t *g, int *references) {
    int refs = 0, faults = 0;
    for (int i = (int)g->events_len - 1; i >= 0 && refs < THRASH_WINDOW; i--) {
        event_type_t type = event_at(g, i)->type;
        if (type == EVENT_PAGE_ACCESS) {
            refs++;
        } else if (type == EVENT_PAGE_FAULT) {
            refs++;
            faults++;
        }
    }
    *references = refs;
    return refs > 0 ? faults * 100 / refs : 0;
}

void panel_metrics(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, "Metrics (completed processes)");

    const sim_t *s = g->sim;
    cursor_t c = {r.x + 12, r.y + 32, r.width - 24};
    sim_metrics_t m;
    sim_metrics_compute(s, &m);

    bool any_done = m.completed > 0;
    panel_kv(&c, "completed", "%d / %d", m.completed, m.processes);
    panel_kv(&c, "avg waiting", any_done ? "%.2f" : "-", m.avg_wait);
    panel_kv(&c, "avg turnaround", any_done ? "%.2f" : "-", m.avg_turnaround);
    panel_kv(&c, "avg response", any_done ? "%.2f" : "-", m.avg_response);
    panel_kv(&c, "max waiting", "%u", m.max_wait);
    panel_kv(&c, "CPU utilisation", "%.1f%%", m.cpu_util);
    panel_kv(&c, "throughput", "%.2f / 100 ticks", m.throughput);
    panel_kv(&c, "page faults", "%u  (%.1f%%)", m.page_faults, m.fault_rate);
    panel_kv(&c, "context switches", "%u", m.ctx_switches);

    int refs;
    int fault_percent = recent_fault_percent(g, &refs);
    bool thrashing = refs >= THRASH_MIN_REFS && fault_percent >= THRASH_PERCENT;
    if (thrashing) {
        DrawRectangleRounded((Rectangle){c.x, c.y + 2, c.w, 22}, 0.3f, 4, ColorAlpha(UI_BAD, 0.3f));
        ui_text(c.x + 8, c.y + 5, 14, UI_BAD, "THRASHING: %d%% of recent refs fault", fault_percent);
        c.y += 26;
    }
    ui_text(c.x, r.y + r.height - 20, 12, UI_TEXT_DIM, "trace hash %016" PRIx64, s->log_hash);
}

typedef enum { ACTION_NONE = -1, ACTION_EDIT, ACTION_DUPLICATE, ACTION_DELETE } menu_action_t;

typedef struct {
    const char   *label;
    menu_action_t action;
} menu_item_t;

static int build_menu(const gui_t *g, node_ref_t n, menu_item_t items[3]) {
    int count = 0;

    if (n.kind == NODE_PROC) {
        const pcb_t *p = &g->sim->procs[n.pid];
        bool editable = p->state == PROC_NEW && p->start_tick < 0;
        bool deletable = p->state == PROC_NEW || p->state == PROC_TERMINATED;

        if (editable) items[count++] = (menu_item_t){"Edit...", ACTION_EDIT};
        items[count++] = (menu_item_t){"Duplicate", ACTION_DUPLICATE};
        if (deletable) items[count++] = (menu_item_t){"Delete", ACTION_DELETE};
    } else if (g->mode == MODE_EDIT) {
        items[count++] = (menu_item_t){"Delete", ACTION_DELETE};
    } else {
        items[count++] = (menu_item_t){"(locked while running)", ACTION_NONE};
    }
    return count;
}

static void run_menu_action(gui_t *g, node_ref_t n, menu_action_t action) {
    switch (action) {
        case ACTION_EDIT:      builder_open_edit(g, n.pid); break;
        case ACTION_DUPLICATE: gui_duplicate_process(g, n.pid); break;
        case ACTION_DELETE:    gui_delete_node(g, n); break;
        case ACTION_NONE:      break;
    }
}

void panel_context_menu(gui_t *g) {
    if (!g->menu_open) return;

    node_ref_t n = g->menu_node;
    if (n.kind == NODE_PROC && !proc_live(g->sim, n.pid)) {
        g->menu_open = false;
        return;
    }

    menu_item_t items[3];
    int count = build_menu(g, n, items);

    ui_push_layer(1);
    Rectangle menu = {g->menu_pos.x, g->menu_pos.y, 190, MENU_ITEM_H * count + 8};
    ui_claim_rect(menu);
    DrawRectangleRec((Rectangle){menu.x + 3, menu.y + 4, menu.width, menu.height}, (Color){0, 0, 0, 100});
    DrawRectangleRec(menu, UI_PANEL_HI);
    DrawRectangleLinesEx(menu, 1, UI_BORDER);

    for (int i = 0; i < count; i++) {
        Rectangle item = {menu.x + 4, menu.y + 4 + i * MENU_ITEM_H, menu.width - 8, 26};
        bool enabled = items[i].action != ACTION_NONE;
        if (ui_button_ex(600 + i, item, items[i].label, enabled, false)) {
            g->menu_open = false;
            run_menu_action(g, n, items[i].action);
        }
    }
    ui_pop_layer();

    bool clicked = IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    if (clicked && !CheckCollisionPointRec(GetMousePosition(), menu)) g->menu_open = false;
}
