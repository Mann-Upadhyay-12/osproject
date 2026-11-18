#include "gui_state.h"
#include "prism/workload.h"
#include "prism/prng.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define BID(n) (1000 + (n))

#define DIALOG_W        600
#define DIALOG_H        740
#define VISIBLE_BURSTS  7
#define BURST_ROW_H     32.0f
#define IPC_SEND_DATA   100

enum { TEMPLATE_CPU_BOUND, TEMPLATE_IO_BOUND, TEMPLATE_INTERACTIVE, TEMPLATE_MEMORY_HOG, TEMPLATE_RANDOM };

static bool name_taken(const gui_t *g, const char *name, int except_pid) {
    const sim_t *s = g->sim;
    for (int pid = 0; pid < s->num_processes; pid++) {
        bool live = s->procs[pid].state != PROC_UNUSED;
        if (pid != except_pid && live && strcmp(s->procs[pid].name, name) == 0) return true;
    }
    return false;
}

static void unique_name(const gui_t *g, const char *prefix, char *out, size_t size) {
    for (int i = 1; i < 1000; i++) {
        snprintf(out, size, "%s%d", prefix, i);
        if (!name_taken(g, out, -1)) return;
    }
}

static void append_burst(builder_t *b, int kind, int duration) {
    if (b->num_bursts >= PRISM_MAX_BURSTS) return;
    b->bursts[b->num_bursts++] = (builder_burst_t){ .kind = kind, .duration = duration, .target = 0 };
}

static void set_alternating_bursts(builder_t *b, const int *durations, int count) {
    b->num_bursts = 0;
    for (int i = 0; i < count; i++) {
        append_burst(b, i % 2 == 0 ? BURST_CPU : BURST_IO, durations[i]);
    }
}

static void random_template(builder_t *b) {
    uint64_t *rng = &b->rng;
    int count = (int)prng_range(rng, 1, 3) * 2 - 1;

    b->num_bursts = 0;
    for (int i = 0; i < count; i++) {
        bool is_cpu = i % 2 == 0;
        append_burst(b, is_cpu ? BURST_CPU : BURST_IO, (int)prng_range(rng, 2, is_cpu ? 14 : 8));
    }

    int highest_page = (int)prng_range(rng, 2, 9);
    int num_refs = (int)prng_range(rng, 6, 20);
    snprintf(b->pages, sizeof(b->pages), "random:0:%d:%d", highest_page, num_refs);
    b->priority = (int)prng_range(rng, 0, 3);
}

void builder_template(gui_t *g, int which) {
    builder_t *b = &g->builder;

    switch (which) {
        case TEMPLATE_CPU_BOUND: {
            static const int bursts[] = {20, 2, 12};
            set_alternating_bursts(b, bursts, 3);
            snprintf(b->pages, sizeof(b->pages), "loop:0:3:6");
            break;
        }
        case TEMPLATE_IO_BOUND: {
            static const int bursts[] = {2, 8, 2, 8, 2};
            set_alternating_bursts(b, bursts, 5);
            snprintf(b->pages, sizeof(b->pages), "0,1,0,1,0,1");
            break;
        }
        case TEMPLATE_INTERACTIVE: {
            static const int bursts[] = {3, 4, 3, 4, 3};
            set_alternating_bursts(b, bursts, 5);
            snprintf(b->pages, sizeof(b->pages), "seq:0:4");
            break;
        }
        case TEMPLATE_MEMORY_HOG: {
            static const int bursts[] = {24};
            set_alternating_bursts(b, bursts, 1);
            snprintf(b->pages, sizeof(b->pages), "loop:0:11:2");
            break;
        }
        default:
            random_template(b);
            break;
    }
    b->error[0] = '\0';
}

static void reset_builder(builder_t *b, uint64_t rng) {
    memset(b, 0, sizeof(*b));
    b->rng = rng;
    b->open = true;
    b->edit_pid = -1;
    b->parent = PRISM_INIT_PID;
}

void builder_open_new(gui_t *g, bool inject) {
    builder_t *b = &g->builder;
    reset_builder(b, b->rng ? b->rng : 0x9e3779b97f4a7c15ULL);
    b->inject = inject;
    unique_name(g, "P", b->name, sizeof(b->name));
    b->arrival = inject ? (int)g->sim->tick : 0;
    builder_template(g, TEMPLATE_INTERACTIVE);
}

static void format_page_list(char *out, size_t size, const proc_spec_t *spec) {
    size_t used = 0;
    for (int i = 0; i < spec->num_page_refs && used + 6 < size; i++) {
        used += (size_t)snprintf(out + used, size - used, i ? ",%u" : "%u", spec->page_refs[i]);
    }
}

void builder_open_edit(gui_t *g, int pid) {
    proc_spec_t spec;
    if (sim_get_process_spec(g->sim, pid, &spec) != SIM_OK) return;

    builder_t *b = &g->builder;
    reset_builder(b, b->rng);
    b->edit_pid = pid;
    snprintf(b->name, sizeof(b->name), "%s", spec.name);
    b->arrival = (int)spec.arrival;
    b->priority = spec.priority;
    b->period = spec.period;
    b->deadline = spec.deadline != spec.period ? spec.deadline : 0;
    b->jobs = spec.period ? spec.jobs : 1;
    for (int c = 0; c < PRISM_MAX_CORES; c++) b->affinity[c] = (spec.affinity >> c) & 1u;
    b->parent = spec.parent;

    b->num_bursts = spec.num_bursts;
    for (int i = 0; i < spec.num_bursts; i++) {
        b->bursts[i] = (builder_burst_t){
            .kind = spec.bursts[i].kind,
            .duration = spec.bursts[i].duration,
            .target = spec.bursts[i].target_id,
        };
    }
    format_page_list(b->pages, sizeof(b->pages), &spec);
}

static bool fail(builder_t *b, const char *message) {
    snprintf(b->error, sizeof(b->error), "%s", message);
    return false;
}

static void replace_spaces(char *text) {
    for (char *c = text; *c; c++) {
        if (isspace((unsigned char)*c)) *c = '_';
    }
}

static void copy_bursts_to_spec(const builder_t *b, proc_spec_t *spec) {
    for (int i = 0; i < b->num_bursts; i++) {
        const builder_burst_t *burst = &b->bursts[i];
        proc_spec_add_burst(spec, (burst_kind_t)burst->kind, (uint16_t)burst->duration);
        spec->bursts[i].target_id = (int16_t)burst->target;
        if (burst->kind == BURST_IPC_SEND) spec->bursts[i].extra_data = IPC_SEND_DATA;
    }
}

static bool builder_to_spec(gui_t *g, proc_spec_t *spec) {
    builder_t *b = &g->builder;

    replace_spaces(b->name);
    if (!b->name[0]) return fail(b, "Give the process a name.");
    if (name_taken(g, b->name, b->edit_pid)) {
        snprintf(b->error, sizeof(b->error), "Another process is already called '%s'.", b->name);
        return false;
    }

    proc_spec_init(spec, b->name);
    spec->arrival = (tick_t)(b->arrival < 0 ? 0 : b->arrival);
    spec->priority = (uint16_t)b->priority;
    for (int c = 0; c < PRISM_MAX_CORES; c++) {
        if (b->affinity[c]) spec->affinity |= (uint8_t)(1u << c);
    }
    spec->parent = (int16_t)b->parent;
    if (b->period > 0) {
        spec->period = (uint16_t)b->period;
        spec->deadline = (uint16_t)b->deadline;
        spec->jobs = (uint16_t)(b->jobs > 0 ? b->jobs : 1);
    }
    copy_bursts_to_spec(b, spec);

    int pid_guess = b->edit_pid >= 0 ? b->edit_pid : g->sim->num_processes;
    if (b->pages[0] && !workload_parse_pages(spec, b->pages, g->sim->seed, pid_guess)) {
        snprintf(b->error, sizeof(b->error), "Page refs: pages must be 0..%d, at most %d refs.",
                 PRISM_MAX_VIRTUAL_PAGES - 1, PRISM_MAX_PAGE_REFS);
        return false;
    }

    char message[120];
    if (proc_spec_validate(spec, message, sizeof(message)) != SIM_OK) return fail(b, message);
    return true;
}

static bool commit_edit(gui_t *g, const proc_spec_t *spec) {
    builder_t *b = &g->builder;
    char old_name[PRISM_MAX_NAME_LEN];
    snprintf(old_name, sizeof(old_name), "%s", g->sim->procs[b->edit_pid].name);

    int rc = sim_update_process(g->sim, b->edit_pid, spec);
    if (rc != SIM_OK) {
        snprintf(b->error, sizeof(b->error), "Cannot edit: %s.", sim_strerror(rc));
        return false;
    }

    float x, y;
    bool renamed = strcmp(old_name, spec->name) != 0;
    if (renamed && layout_get(&g->layout, old_name, &x, &y)) {
        layout_remove(&g->layout, old_name);
        layout_set(&g->layout, spec->name, x, y);
    }
    gui_status(g, false, "Updated %s", spec->name);
    return true;
}

static bool commit_new(gui_t *g, const proc_spec_t *spec) {
    builder_t *b = &g->builder;
    int pid = sim_spawn_process(g->sim, spec);
    if (pid < 0) {
        snprintf(b->error, sizeof(b->error), "Cannot add: %s.", sim_strerror(pid));
        return false;
    }

    canvas_place_process(g, pid);
    g->selected = (node_ref_t){NODE_PROC, pid};
    if (b->inject) {
        gui_status(g, false, "Injected %s: arrives now (tick %" PRIu64 ")",
                   spec->name, (uint64_t)g->sim->procs[pid].arrival_tick);
    } else {
        gui_status(g, false, "Added %s", spec->name);
    }
    return true;
}

static bool builder_commit(gui_t *g) {
    proc_spec_t spec;
    if (!builder_to_spec(g, &spec)) return false;
    return g->builder.edit_pid >= 0 ? commit_edit(g, &spec) : commit_new(g, &spec);
}

static const char *control_burst_label(const sim_t *s, const builder_burst_t *burst) {
    static char label[48];
    bool known_sem = burst->target >= 0 && burst->target < s->num_semaphores;
    const char *sem = known_sem ? s->semaphores[burst->target].name : "?";
    switch (burst->kind) {
        case BURST_ACQUIRE:    snprintf(label, sizeof(label), "acquire R%d", burst->target); break;
        case BURST_RELEASE:    snprintf(label, sizeof(label), "release R%d", burst->target); break;
        case BURST_IPC_SEND:   snprintf(label, sizeof(label), "send to %d", burst->target); break;
        case BURST_IPC_RECV:   snprintf(label, sizeof(label), "recv from %d", burst->target); break;
        case BURST_CHECKPOINT: snprintf(label, sizeof(label), "checkpoint"); break;
        case BURST_SEM_WAIT:   snprintf(label, sizeof(label), "wait(%s)", sem); break;
        case BURST_SEM_SIGNAL: snprintf(label, sizeof(label), "signal(%s)", sem); break;
        case BURST_FORK:       snprintf(label, sizeof(label), "fork()"); break;
        case BURST_WAIT_CHILD: snprintf(label, sizeof(label), "wait() for a child"); break;
        default:               snprintf(label, sizeof(label), "?"); break;
    }
    return label;
}

static const char *dialog_title(const builder_t *b) {
    if (b->edit_pid >= 0) return "Edit process";
    return b->inject ? "Inject process (arrives now)" : "Process Builder";
}

static float draw_identity_fields(gui_t *g, float x, float y) {
    builder_t *b = &g->builder;

    ui_text(x, y, 14, UI_TEXT_DIM, "Name");
    ui_text(x + 260, y, 14, UI_TEXT_DIM, b->inject ? "Arrival (>= now)" : "Arrival tick");
    ui_text(x + 420, y, 14, UI_TEXT_DIM, "Priority (0 = high)");
    y += 18;

    int earliest_arrival = b->inject ? (int)g->sim->tick : 0;
    ui_text_field(BID(1), (Rectangle){x, y, 240, 28}, b->name, sizeof(b->name));
    ui_int_field(BID(2), (Rectangle){x + 260, y, 140, 28}, &b->arrival, earliest_arrival, 100000);
    ui_int_field(BID(3), (Rectangle){x + 420, y, 140, 28}, &b->priority, 0, 9);
    return y + 40;
}

static float draw_periodic_fields(builder_t *b, float x, float y) {
    bool periodic = b->period > 0;
    ui_text(x, y, 14, UI_TEXT_DIM, "Period (0 = not periodic)");
    if (periodic) {
        ui_text(x + 260, y, 14, UI_TEXT_DIM, "Deadline (0 = period)");
        ui_text(x + 420, y, 14, UI_TEXT_DIM, "Jobs");
    }
    y += 18;

    ui_int_field(BID(5), (Rectangle){x, y, 240, 28}, &b->period, 0, 10000);
    if (periodic) {
        ui_int_field(BID(6), (Rectangle){x + 260, y, 140, 28}, &b->deadline, 0, 10000);
        ui_int_field(BID(7), (Rectangle){x + 420, y, 140, 28}, &b->jobs, 1, 1000);
    } else {
        ui_text(x + 260, y + 6, 13, UI_TEXT_DIM, "a periodic task repeats its bursts every period");
    }
    return y + 40;
}

static float draw_affinity_fields(gui_t *g, float x, float y) {
    builder_t *b = &g->builder;
    ui_text(x, y + 3, 14, UI_TEXT_DIM, "Allowed cores");
    for (int c = 0; c < PRISM_MAX_CORES; c++) {
        bool exists = c < g->sim->num_cores;
        Rectangle box = {x + 120 + c * 78, y, 72, 22};
        if (exists) ui_checkbox(BID(8 + c), box, TextFormat("core %d", c), &b->affinity[c]);
        else ui_text(box.x + 4, y + 3, 13, UI_BORDER, "core %d", c);
    }
    ui_text(x + 440, y + 3, 12, UI_TEXT_DIM, "none ticked = any core");
    return y + 34;
}

static int parent_choices(const gui_t *g, const char **items, int *pids) {
    int own_pid = g->builder.edit_pid >= 0 ? g->builder.edit_pid : g->sim->num_processes;
    int count = 0;
    items[count] = "none (arrives on its own)";
    pids[count++] = PRISM_INIT_PID;
    for (int pid = 0; pid < own_pid && pid < g->sim->num_processes; pid++) {
        if (g->sim->procs[pid].state == PROC_UNUSED) continue;
        items[count] = g->sim->procs[pid].name;
        pids[count++] = pid;
    }
    return count;
}

static float draw_parent_field(gui_t *g, float x, float y) {
    static const char *items[PRISM_MAX_PROCESSES + 1];
    static int pids[PRISM_MAX_PROCESSES + 1];
    builder_t *b = &g->builder;
    int count = parent_choices(g, items, pids);

    int chosen = 0;
    for (int i = 0; i < count; i++) {
        if (pids[i] == b->parent) chosen = i;
    }
    ui_text(x, y + 6, 14, UI_TEXT_DIM, "Parent");
    if (ui_dropdown(BID(40), (Rectangle){x + 120, y, 240, 28}, items, count, &chosen)) b->parent = pids[chosen];
    b->parent = pids[chosen];
    const char *hint = b->parent >= 0 ? "born at the parent's fork()" : "parents use fork / wait";
    ui_text(x + 372, y + 7, 12, UI_TEXT_DIM, "%s", hint);
    return y + 38;
}

static float draw_pages_field(builder_t *b, float x, float y, float width) {
    ui_text(x, y, 14, UI_TEXT_DIM, "Page references (virtual pages 0..%d)", PRISM_MAX_VIRTUAL_PAGES - 1);
    y += 18;
    ui_text_field(BID(4), (Rectangle){x, y, width, 28}, b->pages, sizeof(b->pages));
    y += 31;
    ui_text(x, y, 12, UI_TEXT_DIM, "0,1,2,1  |  loop:min:max:times  |  seq:min:max  |  random:min:max:count");
    return y + 24;
}

static void scroll_burst_list(builder_t *b, Rectangle list) {
    if (CheckCollisionPointRec(GetMousePosition(), list)) {
        float wheel = GetMouseWheelMove();
        if (wheel < 0 && b->scroll + VISIBLE_BURSTS < b->num_bursts) b->scroll++;
        if (wheel > 0 && b->scroll > 0) b->scroll--;
    }
    int max_scroll = b->num_bursts > VISIBLE_BURSTS ? b->num_bursts - VISIBLE_BURSTS : 0;
    if (b->scroll > b->num_bursts - VISIBLE_BURSTS) b->scroll = max_scroll;
}

static void draw_timed_burst(builder_burst_t *burst, int i, float x, float y) {
    bool is_cpu = burst->kind == BURST_CPU;
    if (ui_button_ex(BID(100 + i), (Rectangle){x + 40, y, 46, 26}, "CPU", true, is_cpu)) burst->kind = BURST_CPU;
    if (ui_button_ex(BID(200 + i), (Rectangle){x + 90, y, 46, 26}, "I/O", true, !is_cpu)) burst->kind = BURST_IO;
    ui_int_field(BID(300 + i), (Rectangle){x + 150, y, 150, 26}, &burst->duration, 1, 999);
    ui_text(x + 308, y + 6, 13, UI_TEXT_DIM, "ticks");

    float fill = (float)burst->duration / 40.0f;
    if (fill > 1) fill = 1;
    DrawRectangle((int)(x + 360), (int)y + 9, (int)(120 * fill), 8, is_cpu ? UI_GOOD : UI_WARN);
}

static void move_burst_up(builder_t *b, int i) {
    builder_burst_t above = b->bursts[i - 1];
    b->bursts[i - 1] = b->bursts[i];
    b->bursts[i] = above;
}

static void remove_burst(builder_t *b, int i) {
    memmove(&b->bursts[i], &b->bursts[i + 1], (size_t)(b->num_bursts - i - 1) * sizeof(b->bursts[0]));
    b->num_bursts--;
}

static void scroll_to_last_burst(builder_t *b) {
    if (b->num_bursts > VISIBLE_BURSTS) b->scroll = b->num_bursts - VISIBLE_BURSTS;
}

static float draw_burst_list(const sim_t *sim, builder_t *b, float x, float y, float width) {
    uint32_t cpu_total = 0, io_total = 0;
    for (int i = 0; i < b->num_bursts; i++) {
        if (b->bursts[i].kind == BURST_CPU) cpu_total += (uint32_t)b->bursts[i].duration;
        else if (b->bursts[i].kind == BURST_IO) io_total += (uint32_t)b->bursts[i].duration;
    }
    ui_text(x, y, 14, UI_TEXT_DIM, "Bursts (%d)   total CPU %u, I/O %u ticks", b->num_bursts, cpu_total, io_total);
    y += 20;

    Rectangle list = {x, y, width, VISIBLE_BURSTS * BURST_ROW_H + 6};
    DrawRectangleRec(list, (Color){26, 29, 35, 255});
    DrawRectangleLinesEx(list, 1, UI_BORDER);
    scroll_burst_list(b, list);

    int to_remove = -1, to_raise = -1;
    for (int row = 0; row < VISIBLE_BURSTS && b->scroll + row < b->num_bursts; row++) {
        int i = b->scroll + row;
        builder_burst_t *burst = &b->bursts[i];
        float row_y = list.y + 4 + row * BURST_ROW_H;

        ui_text(x + 10, row_y + 6, 14, UI_TEXT_DIM, "%2d", i + 1);
        if (burst->kind == BURST_CPU || burst->kind == BURST_IO) {
            draw_timed_burst(burst, i, x, row_y);
        } else {
            ui_text(x + 44, row_y + 6, 14, UI_ACCENT, "%s", control_burst_label(sim, burst));
        }
        if (i > 0 && ui_button(BID(400 + i), (Rectangle){x + width - 70, row_y, 28, 26}, "^")) to_raise = i;
        if (ui_button(BID(500 + i), (Rectangle){x + width - 38, row_y, 28, 26}, "x")) to_remove = i;
    }
    if (to_raise > 0) move_burst_up(b, to_raise);
    if (to_remove >= 0) remove_burst(b, to_remove);
    y += list.height + 8;

    bool has_room = b->num_bursts < PRISM_MAX_BURSTS;
    if (ui_button_ex(BID(10), (Rectangle){x, y, 110, 28}, "+ CPU burst", has_room, false)) {
        append_burst(b, BURST_CPU, 5);
        scroll_to_last_burst(b);
    }
    if (ui_button_ex(BID(11), (Rectangle){x + 118, y, 110, 28}, "+ I/O burst", has_room, false)) {
        append_burst(b, BURST_IO, 4);
        scroll_to_last_burst(b);
    }
    if (ui_button_ex(BID(12), (Rectangle){x + 246, y, 80, 28}, "+ fork", has_room, false)) {
        append_burst(b, BURST_FORK, 1);
        scroll_to_last_burst(b);
    }
    if (ui_button_ex(BID(13), (Rectangle){x + 334, y, 80, 28}, "+ wait", has_room, false)) {
        append_burst(b, BURST_WAIT_CHILD, 1);
        scroll_to_last_burst(b);
    }
    return y + 40;
}

static float draw_template_buttons(gui_t *g, float x, float y) {
    static const char *names[] = {"CPU-bound", "I/O-bound", "Interactive", "Memory hog", "Random"};

    ui_text(x, y + 6, 14, UI_TEXT_DIM, "Templates");
    for (int t = 0; t < 5; t++) {
        if (ui_button(BID(20 + t), (Rectangle){x + 80 + t * 98.0f, y, 92, 28}, names[t])) builder_template(g, t);
    }
    return y + 40;
}

static void draw_action_buttons(gui_t *g, float x, float y, float width) {
    builder_t *b = &g->builder;
    bool commit = false, keep_open = false;
    bool escape = IsKeyPressed(KEY_ESCAPE) && !ui_text_focused();
    bool enter = (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !ui_text_focused();

    if (ui_button(BID(30), (Rectangle){x, y, 100, 32}, "Cancel") || escape) b->open = false;

    if (b->edit_pid < 0 && ui_button(BID(31), (Rectangle){x + width - 330, y, 160, 32}, "Add & keep open")) {
        commit = keep_open = true;
    }

    const char *confirm = b->edit_pid >= 0 ? "Save" : (b->inject ? "Inject now" : "Add process");
    if (ui_button_ex(BID(32), (Rectangle){x + width - 160, y, 160, 32}, confirm, true, true)) commit = true;
    if (enter && b->open) commit = true;

    if (!commit || !builder_commit(g)) return;

    if (keep_open) {
        unique_name(g, "P", b->name, sizeof(b->name));
        b->error[0] = '\0';
    } else {
        b->open = false;
    }
}

static Rectangle draw_dialog_frame(const builder_t *b) {
    int screen_w = GetScreenWidth(), screen_h = GetScreenHeight();
    DrawRectangle(0, 0, screen_w, screen_h, (Color){0, 0, 0, 120});

    Rectangle dialog = {(screen_w - DIALOG_W) / 2.0f, (screen_h - DIALOG_H) / 2.0f, DIALOG_W, DIALOG_H};
    if (dialog.y < 10) dialog.y = 10;

    DrawRectangleRec((Rectangle){dialog.x + 6, dialog.y + 8, dialog.width, dialog.height}, (Color){0, 0, 0, 100});
    ui_panel(dialog, NULL);
    ui_text(dialog.x + 18, dialog.y + 14, 20, UI_TEXT, "%s", dialog_title(b));
    return dialog;
}

void builder_draw(gui_t *g) {
    builder_t *b = &g->builder;
    if (!b->open) return;

    ui_set_modal(2);
    ui_push_layer(2);

    Rectangle dialog = draw_dialog_frame(b);
    float x = dialog.x + 18, width = DIALOG_W - 36;
    float y = dialog.y + 52;

    y = draw_identity_fields(g, x, y);
    y = draw_periodic_fields(b, x, y);
    y = draw_affinity_fields(g, x, y);
    y = draw_parent_field(g, x, y);
    y = draw_pages_field(b, x, y, width);
    y = draw_burst_list(g->sim, b, x, y, width);
    y = draw_template_buttons(g, x, y);
    if (b->error[0]) ui_text(x, y, 14, UI_BAD, "%s", b->error);

    draw_action_buttons(g, x, dialog.y + DIALOG_H - 46, width);

    ui_pop_layer();
    if (!b->open) ui_set_modal(0);
}
