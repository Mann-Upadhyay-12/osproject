#include "gui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool proc_live(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

static void draw_title(cursor_t *c, const char *title, float gap) {
    ui_text(c->x, c->y, 20, UI_TEXT, "%s", title);
    c->y += gap;
}

static void draw_none(cursor_t *c) {
    ui_text(c->x, c->y, 14, UI_TEXT_DIM, "none");
    c->y += 18;
}

static bool draw_process_actions(gui_t *g, int pid, cursor_t *c) {
    const pcb_t *p = &g->sim->procs[pid];
    bool editable = p->state == PROC_NEW && p->start_tick < 0;
    bool deletable = p->state == PROC_NEW || p->state == PROC_TERMINATED;
    float button_w = (c->w - 12) / 3;

    if (ui_button_ex(200, (Rectangle){c->x, c->y, button_w, 26}, "Edit", editable, false)) builder_open_edit(g, pid);
    if (ui_button(201, (Rectangle){c->x + button_w + 6, c->y, button_w, 26}, "Duplicate")) gui_duplicate_process(g, pid);
    if (ui_button_ex(202, (Rectangle){c->x + 2 * (button_w + 6), c->y, button_w, 26}, "Delete", deletable, false)) {
        gui_delete_node(g, (node_ref_t){NODE_PROC, pid});
        return false;
    }
    c->y += 34;
    return true;
}

static void draw_pcb(const pcb_t *p, cursor_t *c) {
    panel_heading(c, "PCB");
    panel_kv(c, "arrival", "%" PRIu64, (uint64_t)p->arrival_tick);
    panel_kv(c, "priority", "%u (base %u)", p->priority, p->base_priority);
    panel_kv(c, "first run", p->start_tick < 0 ? "-" : "%" PRId64, p->start_tick);
    panel_kv(c, "finished", p->finish_tick < 0 ? "-" : "%" PRId64, p->finish_tick);
    panel_kv(c, "cpu / io / fault", "%u / %u / %u", p->total_cpu_time, p->total_io_time, p->total_fault_time);
    panel_kv(c, "waiting", "%u ticks", p->total_wait_time);
    panel_kv(c, "page faults", "%u", p->num_page_faults);
    panel_kv(c, "dispatches", "%u", p->num_context_switches);

    if (p->state == PROC_TERMINATED && p->finish_tick >= 0) {
        panel_kv(c, "turnaround", "%" PRId64, p->finish_tick - (int64_t)p->arrival_tick);
        panel_kv(c, "response", "%" PRId64, p->start_tick - (int64_t)p->arrival_tick);
    }
}

static float burst_fraction_done(const pcb_t *p, const burst_t *b, bool current, bool done) {
    if (done) return 1.0f;
    if (!current || !b->duration) return 0;
    if (b->kind == BURST_IO) return 1.0f - (float)p->io_remaining / b->duration;
    return 1.0f - (float)b->remaining / b->duration;
}

static const char *burst_kind_label(const burst_t *b) {
    if (b->kind == BURST_CPU) return "CPU";
    if (b->kind == BURST_IO) return "I/O";
    return "ctl";
}

static void draw_bursts(const pcb_t *p, cursor_t *c, float bottom) {
    panel_heading(c, "Bursts");

    for (int i = 0; i < p->num_bursts && c->y < bottom - 120; i++) {
        const burst_t *b = &p->bursts[i];
        bool terminated = p->state == PROC_TERMINATED;
        bool current = i == p->curr_burst_idx && !terminated;
        bool done = i < p->curr_burst_idx || terminated;

        ui_text(c->x, c->y, 13, current ? UI_ACCENT : UI_TEXT_DIM, "%s %2d  %s %u",
                current ? ">" : " ", i + 1, burst_kind_label(b), b->duration);

        if (b->kind == BURST_CPU || b->kind == BURST_IO) {
            float fraction = burst_fraction_done(p, b, current, done);
            float bar_w = c->w - 130;
            DrawRectangle((int)(c->x + 120), (int)c->y + 4, (int)(bar_w * fraction), 8, b->kind == BURST_CPU ? UI_GOOD : UI_WARN);
            DrawRectangleLines((int)(c->x + 120), (int)c->y + 4, (int)bar_w, 8, UI_BORDER);
        }
        c->y += 17;
    }
}

static void draw_resident_pages(const pcb_t *p, cursor_t *c) {
    panel_heading(c, "Memory");
    panel_kv(c, "page refs", "%u / %u issued", p->curr_page_ref_idx, p->num_page_refs);

    char mapping[256] = "";
    size_t used = 0;
    int resident = 0;
    for (int page = 0; page < PRISM_MAX_VIRTUAL_PAGES; page++) {
        if (!p->pt[page].valid) continue;
        resident++;
        if (used + 12 < sizeof(mapping)) {
            used += (size_t)snprintf(mapping + used, sizeof(mapping) - used, "%d->f%d  ", page, p->pt[page].frame_idx);
        }
    }

    panel_kv(c, "resident pages", "%d", resident);
    if (resident) {
        ui_text_clipped((Rectangle){c->x, c->y, c->w, 18}, 13, UI_TEXT, mapping);
        c->y += 20;
    }
}

static void inspect_process(gui_t *g, int pid, cursor_t *c, Rectangle r) {
    const pcb_t *p = &g->sim->procs[pid];

    DrawRectangleRounded((Rectangle){c->x, c->y + 2, 8, 22}, 0.4f, 4, proc_color(pid));
    ui_text(c->x + 16, c->y, 20, UI_TEXT, "%s", p->name);
    c->y += 26;
    ui_text(c->x, c->y, 14, state_color(p->state), "pid %d  -  %s", pid, proc_state_str(p->state));
    c->y += 24;

    if (!draw_process_actions(g, pid, c)) return;
    draw_pcb(p, c);
    draw_bursts(p, c, r.y + r.height);
    draw_resident_pages(p, c);
}

static const char *queue_title(const sim_t *s, int row) {
    if (s->sched_policy == SCHED_SMP) return TextFormat("Core %d run queue", row);
    return row == 0 ? "Queue 0 (head first)" : TextFormat("Queue %d", row);
}

static void inspect_ready_queue(const sim_t *s, cursor_t *c) {
    draw_title(c, "Ready Queue", 30);

    int rows = gui_ready_queue_rows(s);
    for (int q = 0; q < rows; q++) {
        if (q > 0 && s->ready_count[q] == 0 && s->sched_policy != SCHED_SMP) continue;
        panel_heading(c, queue_title(s, q));

        int position = 0;
        for (int pid = s->ready_head[q]; pid != -1 && position < 64; pid = s->procs[pid].next_proc, position++) {
            DrawRectangle((int)c->x, (int)c->y + 3, 10, 10, proc_color(pid));
            ui_text(c->x + 16, c->y, 14, UI_TEXT, "%d. %s  (waited %u)",
                    position + 1, s->procs[pid].name, s->procs[pid].total_wait_time);
            c->y += 18;
        }
        if (position == 0) {
            ui_text(c->x, c->y, 14, UI_TEXT_DIM, "empty");
            c->y += 18;
        }
    }
}

static void inspect_scheduler(sim_t *s, cursor_t *c) {
    draw_title(c, "Scheduler", 32);

    int policy = (int)s->sched_policy;
    ui_text(c->x, c->y + 4, 14, UI_TEXT_DIM, "policy");
    if (ui_dropdown(210, (Rectangle){c->x + 90, c->y, c->w - 90, 26}, g_sched_items, SCHED_COUNT, &policy)) {
        sim_set_sched(s, sched_get_ops((sched_policy_t)policy)->key, 0);
    }
    c->y += 34;

    int quantum = s->default_quantum;
    ui_text(c->x, c->y + 4, 14, UI_TEXT_DIM, "quantum");
    if (ui_int_field(211, (Rectangle){c->x + 90, c->y, 150, 26}, &quantum, 1, 100)) {
        sim_set_sched(s, sched_get_ops(s->sched_policy)->key, quantum);
    }
    c->y += 36;

    panel_kv(c, "context switches", "%u", s->total_context_switches);
    c->y += 6;
    c->y += explain_draw_card(explain_sched_info(s->sched_policy), c->x, c->y, c->w) + 10;
    c->y += ui_text_wrapped(c->x, c->y, c->w, 12, UI_TEXT_DIM,
                            "Changes apply immediately, even mid-run: waiting processes move to the new policy. "
                            "To see every policy on the same input, use Compare (Tab).");
}

static void inspect_cpu(gui_t *g, cursor_t *c) {
    sim_t *s = g->sim;
    draw_title(c, "CPU", 32);

    int cores = s->num_cores;
    ui_text(c->x, c->y + 4, 14, UI_TEXT_DIM, "cores");
    if (g->mode == MODE_EDIT) {
        if (ui_int_field(220, (Rectangle){c->x + 90, c->y, 150, 26}, &cores, 1, PRISM_MAX_CORES)) sim_set_cores(s, cores);
    } else {
        ui_text(c->x + 90, c->y + 4, 14, UI_TEXT, "%d  (reset to change)", cores);
    }
    c->y += 36;

    for (int core = 0; core < s->num_cores; core++) {
        int pid = s->running_pid[core];
        float busy = s->tick ? 100.0f * s->core_busy_ticks[core] / (float)s->tick : 0;
        panel_kv(c, TextFormat("core %d", core), "%s  (%.0f%% busy)", pid < 0 ? "idle" : s->procs[pid].name, busy);
    }
}

static void draw_frame_table(const sim_t *s, cursor_t *c, float bottom) {
    panel_heading(c, "Frame table");
    ui_text(c->x, c->y, 12, UI_TEXT_DIM, "frame  owner          page  loaded  used");
    c->y += 16;

    for (int f = 0; f < s->total_frames && c->y < bottom - 18; f++) {
        const frame_entry_t *frame = &s->frames[f];
        if (!frame->occupied) {
            ui_text(c->x, c->y, 12, UI_TEXT_DIM, "%3d    free", f);
        } else {
            DrawRectangle((int)c->x + 30, (int)c->y + 3, 8, 8, proc_color(frame->owner_pid));
            ui_text(c->x, c->y, 12, UI_TEXT, "%3d        %-12.12s  %3u  %5" PRIu64 "  %5" PRIu64, f,
                    s->procs[frame->owner_pid].name, frame->vpage,
                    (uint64_t)frame->load_tick, (uint64_t)frame->last_access_tick);
        }
        c->y += 15;
    }
}

static void inspect_memory(gui_t *g, cursor_t *c, Rectangle r) {
    sim_t *s = g->sim;
    draw_title(c, "Memory", 32);

    int frames = s->configured_frames;
    ui_text(c->x, c->y + 4, 14, UI_TEXT_DIM, "frames");
    if (g->mode == MODE_EDIT) {
        if (ui_int_field(230, (Rectangle){c->x + 110, c->y, 150, 26}, &frames, 1, PRISM_MAX_FRAMES)) sim_set_frames(s, frames);
    } else {
        ui_text(c->x + 110, c->y + 4, 14, UI_TEXT, "%d  (reset to change)", s->total_frames);
    }
    c->y += 34;

    int policy = (int)s->repl_policy;
    ui_text(c->x, c->y + 4, 14, UI_TEXT_DIM, "replacement");
    if (ui_dropdown(231, (Rectangle){c->x + 110, c->y, c->w - 110, 26}, g_repl_items, REPL_COUNT, &policy)) {
        sim_set_repl(s, repl_get_ops((repl_policy_t)policy)->key);
    }
    c->y += 36;

    double fault_rate = s->total_page_accesses ? 100.0 * s->total_page_faults / s->total_page_accesses : 0.0;
    panel_kv(c, "accesses", "%u", s->total_page_accesses);
    panel_kv(c, "faults", "%u", s->total_page_faults);
    panel_kv(c, "fault rate", "%.1f%%", fault_rate);
    c->y += 4;
    c->y += explain_draw_card(explain_repl_info(s->repl_policy), c->x, c->y, c->w) + 4;

    draw_frame_table(s, c, r.y + r.height);
}

static void inspect_disk(sim_t *s, cursor_t *c) {
    draw_title(c, "Disk / I-O", 32);

    int latency = s->disk_latency;
    ui_text(c->x, c->y + 4, 14, UI_TEXT_DIM, "page-in ticks");
    if (ui_int_field(240, (Rectangle){c->x + 110, c->y, 150, 26}, &latency, 1, 100)) sim_set_disk_latency(s, latency);
    c->y += 32;
    ui_text(c->x, c->y, 12, UI_TEXT_DIM, "applies live to new page faults");
    c->y += 16;

    panel_heading(c, "Waiting on I/O");
    int count = 0;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state != PROC_BLOCKED_IO) continue;
        panel_kv(c, p->name, "%u ticks left", p->io_remaining);
        count++;
    }
    if (count == 0) draw_none(c);

    panel_heading(c, "Page-in (fault service)");
    count = 0;
    for (int pid = 0; pid < s->num_processes; pid++) {
        const pcb_t *p = &s->procs[pid];
        if (p->state != PROC_BLOCKED_FAULT) continue;
        panel_kv(c, p->name, "page %u, %u left", p->fault_vpage, p->fault_remaining);
        count++;
    }
    if (count == 0) draw_none(c);
}

static void inspect_scenario(const sim_t *s, cursor_t *c) {
    static const char *help[] = {
        "1. Pick a test case (toolbar) or drag a Process from the palette (A) and script its bursts.",
        "2. Click a node to inspect it: the Scheduler and Memory nodes explain their algorithm.",
        "3. Step (N) one decision at a time and read \"What just happened\". Back (B) undoes a tick; "
        "click the Gantt chart to jump to any tick.",
        "4. While running, inject a process or switch policies live.",
        "5. Compare (Tab) runs every algorithm on the same test cases side by side.",
    };

    draw_title(c, "Scenario", 30);

    int live = 0;
    for (int pid = 0; pid < s->num_processes; pid++) live += proc_live(s, pid);
    panel_kv(c, "processes", "%d", live);
    panel_kv(c, "cores", "%d", s->num_cores);
    panel_kv(c, "frames", "%d", s->total_frames);
    panel_kv(c, "scheduler", "%s (q=%u)", sched_get_ops(s->sched_policy)->name, s->default_quantum);
    panel_kv(c, "replacement", "%s", repl_get_ops(s->repl_policy)->name);
    panel_kv(c, "seed", "%" PRIu64, s->seed);
    c->y += 12;

    for (int i = 0; i < 5; i++) c->y += ui_text_wrapped(c->x, c->y, c->w, 13, UI_TEXT_DIM, help[i]) + 6;
}

void panel_inspector(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, "Inspector");

    sim_t *s = g->sim;
    cursor_t c = {r.x + 12, r.y + 34, r.width - 24};
    ui_scissor_push((Rectangle){r.x, r.y + 24, r.width, r.height - 24});

    node_ref_t selected = g->selected;
    if (selected.kind == NODE_PROC && !proc_live(s, selected.pid)) selected.kind = NODE_NONE;

    switch (selected.kind) {
        case NODE_PROC:   inspect_process(g, selected.pid, &c, r); break;
        case NODE_READY:  inspect_ready_queue(s, &c); break;
        case NODE_SCHED:  inspect_scheduler(s, &c); break;
        case NODE_CPU:    inspect_cpu(g, &c); break;
        case NODE_MEMORY: inspect_memory(g, &c, r); break;
        case NODE_DISK:   inspect_disk(s, &c); break;
        default:          inspect_scenario(s, &c); break;
    }
    ui_scissor_pop();
}
