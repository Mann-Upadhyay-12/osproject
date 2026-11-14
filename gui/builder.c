#include "gui_state.h"
#include "prism/workload.h"
#include "prism/prng.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Widget ids for this dialog live in 1000..1999. */
#define BID(n) (1000 + (n))

static bool name_taken(const gui_t *g, const char *name, int except_pid) {
    const sim_t *s = g->sim;
    for (int i = 0; i < s->num_processes; i++) {
        if (i != except_pid && s->procs[i].state != PROC_UNUSED && strcmp(s->procs[i].name, name) == 0) return true;
    }
    return false;
}

static void unique_name(const gui_t *g, const char *base, char *out, size_t n) {
    for (int i = 1; i < 1000; i++) {
        snprintf(out, n, "%s%d", base, i);
        if (!name_taken(g, out, -1)) return;
    }
}

static void set_bursts(builder_t *b, const int *spec, int n) {
    b->nb = 0;
    for (int i = 0; i + 1 < n * 2 && b->nb < PRISM_MAX_BURSTS; i += 2) {
        b->kind[b->nb] = spec[i];
        b->dur[b->nb] = spec[i + 1];
        b->target[b->nb] = 0;
        b->nb++;
    }
}

void builder_template(gui_t *g, int which) {
    builder_t *b = &g->b;
    switch (which) {
        case 0: { /* CPU-bound */
            int s[] = {BURST_CPU, 20, BURST_IO, 2, BURST_CPU, 12};
            set_bursts(b, s, 3);
            snprintf(b->pages, sizeof(b->pages), "loop:0:3:6");
            break;
        }
        case 1: { /* I/O-bound */
            int s[] = {BURST_CPU, 2, BURST_IO, 8, BURST_CPU, 2, BURST_IO, 8, BURST_CPU, 2};
            set_bursts(b, s, 5);
            snprintf(b->pages, sizeof(b->pages), "0,1,0,1,0,1");
            break;
        }
        case 2: { /* interactive */
            int s[] = {BURST_CPU, 3, BURST_IO, 4, BURST_CPU, 3, BURST_IO, 4, BURST_CPU, 3};
            set_bursts(b, s, 5);
            snprintf(b->pages, sizeof(b->pages), "seq:0:4");
            break;
        }
        case 3: { /* memory hog */
            int s[] = {BURST_CPU, 24};
            set_bursts(b, s, 1);
            snprintf(b->pages, sizeof(b->pages), "loop:0:11:2");
            break;
        }
        default: { /* random, seeded so "Random" is reproducible per session */
            uint64_t *r = &b->rng;
            int n = (int)prng_range(r, 1, 3) * 2 - 1;
            b->nb = 0;
            for (int i = 0; i < n; i++) {
                b->kind[b->nb] = (i % 2 == 0) ? BURST_CPU : BURST_IO;
                b->dur[b->nb] = (int)prng_range(r, 2, i % 2 ? 8 : 14);
                b->target[b->nb] = 0;
                b->nb++;
            }
            int hi = (int)prng_range(r, 2, 9);
            snprintf(b->pages, sizeof(b->pages), "random:0:%d:%d", hi, (int)prng_range(r, 6, 20));
            b->priority = (int)prng_range(r, 0, 3);
            break;
        }
    }
    b->error[0] = '\0';
}

void builder_open_new(gui_t *g, bool inject) {
    builder_t *b = &g->b;
    uint64_t rng = b->rng ? b->rng : 0x9e3779b97f4a7c15ULL;
    memset(b, 0, sizeof(*b));
    b->rng = rng;
    b->open = true;
    b->inject = inject;
    b->edit_pid = -1;
    unique_name(g, "P", b->name, sizeof(b->name));
    b->arrival = inject ? (int)g->sim->tick : 0;
    builder_template(g, 2);
}

void builder_open_edit(gui_t *g, int pid) {
    proc_spec_t spec;
    if (sim_get_process_spec(g->sim, pid, &spec) != SIM_OK) return;
    builder_t *b = &g->b;
    uint64_t rng = b->rng;
    memset(b, 0, sizeof(*b));
    b->rng = rng;
    b->open = true;
    b->edit_pid = pid;
    snprintf(b->name, sizeof(b->name), "%s", spec.name);
    b->arrival = (int)spec.arrival;
    b->priority = spec.priority;
    b->nb = spec.num_bursts;
    for (int i = 0; i < spec.num_bursts; i++) {
        b->kind[i] = spec.bursts[i].kind;
        b->dur[i] = spec.bursts[i].duration;
        b->target[i] = spec.bursts[i].target_id;
    }
    size_t off = 0;
    for (int i = 0; i < spec.num_page_refs && off + 6 < sizeof(b->pages); i++) {
        off += (size_t)snprintf(b->pages + off, sizeof(b->pages) - off, i ? ",%u" : "%u", spec.page_refs[i]);
    }
}

/* Build a spec from the dialog; returns false with b->error set. */
static bool builder_to_spec(gui_t *g, proc_spec_t *spec) {
    builder_t *b = &g->b;
    for (char *c = b->name; *c; c++) if (isspace((unsigned char)*c)) *c = '_';
    if (!b->name[0]) { snprintf(b->error, sizeof(b->error), "Give the process a name."); return false; }
    if (name_taken(g, b->name, b->edit_pid)) {
        snprintf(b->error, sizeof(b->error), "Another process is already called '%s'.", b->name);
        return false;
    }
    proc_spec_init(spec, b->name);
    spec->arrival = (tick_t)(b->arrival < 0 ? 0 : b->arrival);
    spec->priority = (uint16_t)b->priority;
    for (int i = 0; i < b->nb; i++) {
        proc_spec_add_burst(spec, (burst_kind_t)b->kind[i], (uint16_t)b->dur[i]);
        spec->bursts[i].target_id = (int16_t)b->target[i];
        if (b->kind[i] == BURST_IPC_SEND) spec->bursts[i].extra_data = 100;
    }
    int pid_guess = b->edit_pid >= 0 ? b->edit_pid : g->sim->num_processes;
    if (b->pages[0] && !workload_parse_pages(spec, b->pages, g->sim->seed, pid_guess)) {
        snprintf(b->error, sizeof(b->error), "Page refs: pages must be 0..%d, at most %d refs.",
                 PRISM_MAX_VIRTUAL_PAGES - 1, PRISM_MAX_PAGE_REFS);
        return false;
    }
    char msg[120];
    if (proc_spec_validate(spec, msg, sizeof(msg)) != SIM_OK) {
        snprintf(b->error, sizeof(b->error), "%s", msg);
        return false;
    }
    return true;
}

static bool builder_commit(gui_t *g) {
    builder_t *b = &g->b;
    proc_spec_t spec;
    if (!builder_to_spec(g, &spec)) return false;
    if (b->edit_pid >= 0) {
        char old[PRISM_MAX_NAME_LEN];
        snprintf(old, sizeof(old), "%s", g->sim->procs[b->edit_pid].name);
        int rc = sim_update_process(g->sim, b->edit_pid, &spec);
        if (rc != SIM_OK) { snprintf(b->error, sizeof(b->error), "Cannot edit: %s.", sim_strerror(rc)); return false; }
        float x, y;
        if (strcmp(old, spec.name) != 0 && layout_get(&g->layout, old, &x, &y)) {
            layout_remove(&g->layout, old);
            layout_set(&g->layout, spec.name, x, y);
        }
        gui_status(g, false, "Updated %s", spec.name);
    } else {
        int pid = sim_spawn_process(g->sim, &spec);
        if (pid < 0) { snprintf(b->error, sizeof(b->error), "Cannot add: %s.", sim_strerror(pid)); return false; }
        canvas_place_process(g, pid);
        g->sel = (node_ref_t){NODE_PROC, pid};
        if (b->inject) gui_status(g, false, "Injected %s: arrives now (tick %" PRIu64 ")", spec.name, (uint64_t)g->sim->procs[pid].arrival_tick);
        else gui_status(g, false, "Added %s", spec.name);
    }
    return true;
}

static const char *burst_label(int kind, int target) {
    static char buf[16];
    switch (kind) {
        case BURST_ACQUIRE:    snprintf(buf, sizeof(buf), "acquire R%d", target); break;
        case BURST_RELEASE:    snprintf(buf, sizeof(buf), "release R%d", target); break;
        case BURST_IPC_SEND:   snprintf(buf, sizeof(buf), "send to %d", target); break;
        case BURST_IPC_RECV:   snprintf(buf, sizeof(buf), "recv from %d", target); break;
        case BURST_CHECKPOINT: snprintf(buf, sizeof(buf), "checkpoint"); break;
        default:               snprintf(buf, sizeof(buf), "?"); break;
    }
    return buf;
}

void builder_draw(gui_t *g) {
    builder_t *b = &g->b;
    if (!b->open) return;
    ui_set_modal(2);
    ui_push_layer(2);

    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 120});
    float W = 600, H = 600;
    Rectangle d = {(sw - W) / 2, (sh - H) / 2, W, H};
    if (d.y < 10) d.y = 10;
    DrawRectangleRec((Rectangle){d.x + 6, d.y + 8, d.width, d.height}, (Color){0, 0, 0, 100});
    ui_panel(d, NULL);
    const char *title = b->edit_pid >= 0 ? "Edit process" : (b->inject ? "Inject process (arrives now)" : "Process Builder");
    ui_text(d.x + 18, d.y + 14, 20, UI_TEXT, "%s", title);

    float x = d.x + 18, y = d.y + 52, fw = W - 36;
    ui_text(x, y, 14, UI_TEXT_DIM, "Name");
    ui_text(x + 260, y, 14, UI_TEXT_DIM, b->inject ? "Arrival (>= now)" : "Arrival tick");
    ui_text(x + 420, y, 14, UI_TEXT_DIM, "Priority (0 = high)");
    y += 18;
    ui_text_field(BID(1), (Rectangle){x, y, 240, 28}, b->name, sizeof(b->name));
    ui_int_field(BID(2), (Rectangle){x + 260, y, 140, 28}, &b->arrival, b->inject ? (int)g->sim->tick : 0, 100000);
    ui_int_field(BID(3), (Rectangle){x + 420, y, 140, 28}, &b->priority, 0, 9);
    y += 40;

    ui_text(x, y, 14, UI_TEXT_DIM, "Page references (virtual pages 0..%d)", PRISM_MAX_VIRTUAL_PAGES - 1);
    y += 18;
    ui_text_field(BID(4), (Rectangle){x, y, fw, 28}, b->pages, sizeof(b->pages));
    y += 31;
    ui_text(x, y, 12, UI_TEXT_DIM, "0,1,2,1  |  loop:min:max:times  |  seq:min:max  |  random:min:max:count");
    y += 24;

    /* Burst list */
    uint32_t cpu = 0, io = 0;
    for (int i = 0; i < b->nb; i++) {
        if (b->kind[i] == BURST_CPU) cpu += (uint32_t)b->dur[i];
        else if (b->kind[i] == BURST_IO) io += (uint32_t)b->dur[i];
    }
    ui_text(x, y, 14, UI_TEXT_DIM, "Bursts (%d)   total CPU %u, I/O %u ticks", b->nb, cpu, io);
    y += 20;
    Rectangle list = {x, y, fw, 7 * 32.0f + 6};
    DrawRectangleRec(list, (Color){26, 29, 35, 255});
    DrawRectangleLinesEx(list, 1, UI_BORDER);
    if (CheckCollisionPointRec(GetMousePosition(), list)) {
        float wh = GetMouseWheelMove();
        if (wh < 0 && b->scroll + 7 < b->nb) b->scroll++;
        if (wh > 0 && b->scroll > 0) b->scroll--;
    }
    if (b->scroll > b->nb - 7) b->scroll = b->nb > 7 ? b->nb - 7 : 0;
    int remove = -1, up = -1;
    for (int row = 0; row < 7 && b->scroll + row < b->nb; row++) {
        int i = b->scroll + row;
        float ry = list.y + 4 + row * 32.0f;
        ui_text(x + 10, ry + 6, 14, UI_TEXT_DIM, "%2d", i + 1);
        if (b->kind[i] == BURST_CPU || b->kind[i] == BURST_IO) {
            bool is_cpu = b->kind[i] == BURST_CPU;
            if (ui_button_ex(BID(100 + i), (Rectangle){x + 40, ry, 46, 26}, "CPU", true, is_cpu)) b->kind[i] = BURST_CPU;
            if (ui_button_ex(BID(200 + i), (Rectangle){x + 90, ry, 46, 26}, "I/O", true, !is_cpu)) b->kind[i] = BURST_IO;
            ui_int_field(BID(300 + i), (Rectangle){x + 150, ry, 150, 26}, &b->dur[i], 1, 999);
            ui_text(x + 308, ry + 6, 13, UI_TEXT_DIM, "ticks");
            float bar = (float)b->dur[i] / 40.0f;
            if (bar > 1) bar = 1;
            DrawRectangle((int)(x + 360), (int)ry + 9, (int)(120 * bar), 8, is_cpu ? UI_GOOD : UI_WARN);
        } else {
            ui_text(x + 44, ry + 6, 14, UI_ACCENT, "%s", burst_label(b->kind[i], b->target[i]));
        }
        if (i > 0 && ui_button(BID(400 + i), (Rectangle){x + fw - 70, ry, 28, 26}, "^")) up = i;
        if (ui_button(BID(500 + i), (Rectangle){x + fw - 38, ry, 28, 26}, "x")) remove = i;
    }
    if (up > 0) {
        int t;
        t = b->kind[up]; b->kind[up] = b->kind[up - 1]; b->kind[up - 1] = t;
        t = b->dur[up]; b->dur[up] = b->dur[up - 1]; b->dur[up - 1] = t;
        t = b->target[up]; b->target[up] = b->target[up - 1]; b->target[up - 1] = t;
    }
    if (remove >= 0) {
        for (int i = remove; i + 1 < b->nb; i++) {
            b->kind[i] = b->kind[i + 1]; b->dur[i] = b->dur[i + 1]; b->target[i] = b->target[i + 1];
        }
        b->nb--;
    }
    y += list.height + 8;
    bool room = b->nb < PRISM_MAX_BURSTS;
    if (ui_button_ex(BID(10), (Rectangle){x, y, 110, 28}, "+ CPU burst", room, false)) {
        b->kind[b->nb] = BURST_CPU; b->dur[b->nb] = 5; b->target[b->nb] = 0; b->nb++;
        if (b->nb > 7) b->scroll = b->nb - 7;
    }
    if (ui_button_ex(BID(11), (Rectangle){x + 118, y, 110, 28}, "+ I/O burst", room, false)) {
        b->kind[b->nb] = BURST_IO; b->dur[b->nb] = 4; b->target[b->nb] = 0; b->nb++;
        if (b->nb > 7) b->scroll = b->nb - 7;
    }
    y += 40;

    ui_text(x, y + 6, 14, UI_TEXT_DIM, "Templates");
    static const char *tpl[] = {"CPU-bound", "I/O-bound", "Interactive", "Memory hog", "Random"};
    for (int t = 0; t < 5; t++) {
        if (ui_button(BID(20 + t), (Rectangle){x + 80 + t * 98.0f, y, 92, 28}, tpl[t])) builder_template(g, t);
    }
    y += 40;

    if (b->error[0]) ui_text(x, y, 14, UI_BAD, "%s", b->error);

    float by = d.y + H - 46;
    bool commit = false, keep = false;
    if (ui_button(BID(30), (Rectangle){x, by, 100, 32}, "Cancel") || (IsKeyPressed(KEY_ESCAPE) && !ui_text_focused())) {
        b->open = false;
    }
    if (b->edit_pid < 0 && ui_button(BID(31), (Rectangle){x + fw - 330, by, 160, 32}, "Add & keep open")) {
        commit = keep = true;
    }
    const char *ok = b->edit_pid >= 0 ? "Save" : (b->inject ? "Inject now" : "Add process");
    if (ui_button_ex(BID(32), (Rectangle){x + fw - 160, by, 160, 32}, ok, true, true)) commit = true;
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && !ui_text_focused() && b->open) commit = true;

    if (commit && builder_commit(g)) {
        if (keep) {
            unique_name(g, "P", b->name, sizeof(b->name));
            b->error[0] = '\0';
        } else {
            b->open = false;
        }
    }

    ui_pop_layer();
    if (!b->open) ui_set_modal(0);
}
