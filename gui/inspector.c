#include "gui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Right-hand panel: details and live controls for the selected node.
 * Widget ids 200..499. */

static bool proc_live(const sim_t *s, int pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

static const char *sched_blurb(sched_policy_t p) {
    switch (p) {
        case SCHED_FCFS:     return "First come, first served. Non-preemptive.";
        case SCHED_RR:       return "Round robin: preempt after each quantum.";
        case SCHED_SJF:      return "Shortest job first (current burst). Non-preemptive.";
        case SCHED_SRTF:     return "Shortest remaining time; preempts on arrival.";
        case SCHED_MLFQ:     return "MLFQ: 3 levels (q=2/4/8), demotes, boost/50 ticks.";
        case SCHED_ADAPTIVE: return "Aging + demotes processes that thrash.";
        case SCHED_CFS:      return "Linux CFS: lowest virtual runtime runs.";
        case SCHED_BFS:      return "Earliest virtual deadline first.";
        default:             return "";
    }
}

static void inspect_process(gui_t *g, int pid, cursor_t *c, Rectangle r) {
    sim_t *s = g->sim;
    const pcb_t *p = &s->procs[pid];
    DrawRectangleRounded((Rectangle){c->x, c->y + 2, 8, 22}, 0.4f, 4, proc_color(pid));
    ui_text(c->x + 16, c->y, 20, UI_TEXT, "%s", p->name);
    c->y += 26;
    ui_text(c->x, c->y, 14, state_color(p->state), "pid %d  -  %s", pid, proc_state_str(p->state));
    c->y += 24;

    bool editable = (p->state == PROC_NEW && p->start_tick < 0);
    float bw = (c->w - 12) / 3;
    if (ui_button_ex(200, (Rectangle){c->x, c->y, bw, 26}, "Edit", editable, false)) builder_open_edit(g, pid);
    if (ui_button(201, (Rectangle){c->x + bw + 6, c->y, bw, 26}, "Duplicate")) gui_duplicate_process(g, pid);
    bool deletable = (p->state == PROC_NEW || p->state == PROC_TERMINATED);
    if (ui_button_ex(202, (Rectangle){c->x + 2 * (bw + 6), c->y, bw, 26}, "Delete", deletable, false)) {
        gui_delete_node(g, (node_ref_t){NODE_PROC, pid});
        return;
    }
    c->y += 34;

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

    panel_heading(c, "Bursts");
    for (int i = 0; i < p->num_bursts && c->y < r.y + r.height - 120; i++) {
        const burst_t *b = &p->bursts[i];
        bool cur = (i == p->curr_burst_idx && p->state != PROC_TERMINATED);
        bool done = i < p->curr_burst_idx || p->state == PROC_TERMINATED;
        const char *kind = b->kind == BURST_CPU ? "CPU" : (b->kind == BURST_IO ? "I/O" : "ctl");
        Color col = done ? UI_TEXT_DIM : (cur ? UI_TEXT : UI_TEXT_DIM);
        ui_text(c->x, c->y, 13, cur ? UI_ACCENT : col, "%s %2d  %s %u", cur ? ">" : " ", i + 1, kind, b->duration);
        if (b->kind == BURST_CPU || b->kind == BURST_IO) {
            float frac = done ? 1.0f : (cur && b->duration ? 1.0f - (float)b->remaining / b->duration : 0);
            if (cur && b->kind == BURST_IO && b->duration) frac = 1.0f - (float)p->io_remaining / b->duration;
            DrawRectangle((int)(c->x + 120), (int)c->y + 4, (int)((c->w - 130) * frac), 8, b->kind == BURST_CPU ? UI_GOOD : UI_WARN);
            DrawRectangleLines((int)(c->x + 120), (int)c->y + 4, (int)(c->w - 130), 8, UI_BORDER);
        }
        c->y += 17;
    }

    panel_heading(c, "Memory");
    panel_kv(c, "page refs", "%u / %u issued", p->curr_page_ref_idx, p->num_page_refs);
    char buf[256] = "";
    size_t off = 0;
    int resident = 0;
    for (int pg = 0; pg < PRISM_MAX_VIRTUAL_PAGES; pg++) {
        if (p->pt[pg].valid) {
            resident++;
            if (off + 12 < sizeof(buf)) off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%d->f%d  ", pg, p->pt[pg].frame_idx);
        }
    }
    panel_kv(c, "resident pages", "%d", resident);
    if (resident) {
        ui_text_clipped((Rectangle){c->x, c->y, c->w, 18}, 13, UI_TEXT, buf);
        c->y += 20;
    }
}

void panel_inspector(gui_t *g, Rectangle r) {
    ui_claim_rect(r);
    ui_panel(r, "Inspector");
    sim_t *s = g->sim;
    cursor_t c = {r.x + 12, r.y + 34, r.width - 24};
    ui_scissor_push((Rectangle){r.x, r.y + 24, r.width, r.height - 24});

    node_ref_t sel = g->sel;
    if (sel.kind == NODE_PROC && !proc_live(s, sel.pid)) sel.kind = NODE_NONE;

    switch (sel.kind) {
        case NODE_PROC:
            inspect_process(g, sel.pid, &c, r);
            break;
        case NODE_READY: {
            ui_text(c.x, c.y, 20, UI_TEXT, "Ready Queue");
            c.y += 30;
            for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
                if (q > 0 && s->ready_count[q] == 0) continue;
                panel_heading(&c, q == 0 ? "Queue 0 (head first)" : (q == 1 ? "Queue 1" : "Queue 2"));
                int n = 0;
                for (int pid = s->ready_head[q]; pid != -1 && n < 64; pid = s->procs[pid].next_proc, n++) {
                    DrawRectangle((int)c.x, (int)c.y + 3, 10, 10, proc_color(pid));
                    ui_text(c.x + 16, c.y, 14, UI_TEXT, "%d. %s  (waited %u)", n + 1, s->procs[pid].name, s->procs[pid].total_wait_time);
                    c.y += 18;
                }
                if (n == 0) { ui_text(c.x, c.y, 14, UI_TEXT_DIM, "empty"); c.y += 18; }
            }
            break;
        }
        case NODE_SCHED: {
            ui_text(c.x, c.y, 20, UI_TEXT, "Scheduler");
            c.y += 32;
            int sp = (int)s->sched_policy;
            ui_text(c.x, c.y + 4, 14, UI_TEXT_DIM, "policy");
            if (ui_dropdown(210, (Rectangle){c.x + 90, c.y, c.w - 90, 26}, g_sched_items, SCHED_COUNT, &sp)) {
                sim_set_sched(s, sched_get_ops((sched_policy_t)sp)->key, 0);
            }
            c.y += 34;
            int q = s->default_quantum;
            ui_text(c.x, c.y + 4, 14, UI_TEXT_DIM, "quantum");
            if (ui_int_field(211, (Rectangle){c.x + 90, c.y, 150, 26}, &q, 1, 100)) {
                sim_set_sched(s, sched_get_ops(s->sched_policy)->key, q);
            }
            c.y += 36;
            ui_text(c.x, c.y, 13, UI_TEXT_DIM, "%s", sched_blurb(s->sched_policy));
            c.y += 24;
            ui_text(c.x, c.y, 13, UI_TEXT_DIM, "Changes apply immediately, even mid-run;");
            c.y += 16;
            ui_text(c.x, c.y, 13, UI_TEXT_DIM, "waiting processes move to the new policy.");
            c.y += 24;
            panel_kv(&c, "context switches", "%u", s->total_context_switches);
            break;
        }
        case NODE_CPU: {
            ui_text(c.x, c.y, 20, UI_TEXT, "CPU");
            c.y += 32;
            int cores = s->num_cores;
            ui_text(c.x, c.y + 4, 14, UI_TEXT_DIM, "cores");
            bool edit = g->mode == MODE_EDIT;
            if (edit) {
                if (ui_int_field(220, (Rectangle){c.x + 90, c.y, 150, 26}, &cores, 1, PRISM_MAX_CORES)) sim_set_cores(s, cores);
            } else {
                ui_text(c.x + 90, c.y + 4, 14, UI_TEXT, "%d  (reset to change)", cores);
            }
            c.y += 36;
            for (int k = 0; k < s->num_cores; k++) {
                int pid = s->running_pid[k];
                float util = s->tick ? 100.0f * s->core_busy_ticks[k] / (float)s->tick : 0;
                panel_kv(&c, TextFormat("core %d", k), "%s  (%.0f%% busy)", pid < 0 ? "idle" : s->procs[pid].name, util);
            }
            break;
        }
        case NODE_MEMORY: {
            ui_text(c.x, c.y, 20, UI_TEXT, "Memory");
            c.y += 32;
            int frames = s->configured_frames;
            ui_text(c.x, c.y + 4, 14, UI_TEXT_DIM, "frames");
            if (g->mode == MODE_EDIT) {
                if (ui_int_field(230, (Rectangle){c.x + 110, c.y, 150, 26}, &frames, 1, PRISM_MAX_FRAMES)) sim_set_frames(s, frames);
            } else {
                ui_text(c.x + 110, c.y + 4, 14, UI_TEXT, "%d  (reset to change)", s->total_frames);
            }
            c.y += 34;
            int rp = (int)s->repl_policy;
            ui_text(c.x, c.y + 4, 14, UI_TEXT_DIM, "replacement");
            if (ui_dropdown(231, (Rectangle){c.x + 110, c.y, c.w - 110, 26}, g_repl_items, REPL_COUNT, &rp)) {
                sim_set_repl(s, repl_get_ops((repl_policy_t)rp)->key);
            }
            c.y += 36;
            panel_kv(&c, "accesses", "%u", s->total_page_accesses);
            panel_kv(&c, "faults", "%u", s->total_page_faults);
            panel_kv(&c, "fault rate", "%.1f%%", s->total_page_accesses ? 100.0 * s->total_page_faults / s->total_page_accesses : 0.0);
            panel_heading(&c, "Frame table");
            ui_text(c.x, c.y, 12, UI_TEXT_DIM, "frame  owner          page  loaded  used");
            c.y += 16;
            for (int f = 0; f < s->total_frames && c.y < r.y + r.height - 18; f++) {
                const frame_entry_t *fe = &s->frames[f];
                if (!fe->occupied) {
                    ui_text(c.x, c.y, 12, UI_TEXT_DIM, "%3d    free", f);
                } else {
                    DrawRectangle((int)c.x + 30, (int)c.y + 3, 8, 8, proc_color(fe->owner_pid));
                    ui_text(c.x, c.y, 12, UI_TEXT, "%3d        %-12.12s  %3u  %5" PRIu64 "  %5" PRIu64, f,
                            s->procs[fe->owner_pid].name, fe->vpage, (uint64_t)fe->load_tick, (uint64_t)fe->last_access_tick);
                }
                c.y += 15;
            }
            break;
        }
        case NODE_DISK: {
            ui_text(c.x, c.y, 20, UI_TEXT, "Disk / I-O");
            c.y += 32;
            int lat = s->disk_latency;
            ui_text(c.x, c.y + 4, 14, UI_TEXT_DIM, "page-in ticks");
            if (ui_int_field(240, (Rectangle){c.x + 110, c.y, 150, 26}, &lat, 1, 100)) sim_set_disk_latency(s, lat);
            c.y += 32;
            ui_text(c.x, c.y, 12, UI_TEXT_DIM, "applies live to new page faults");
            c.y += 16;
            panel_heading(&c, "Waiting on I/O");
            int n = 0;
            for (int pid = 0; pid < s->num_processes; pid++) {
                if (s->procs[pid].state == PROC_BLOCKED_IO) { panel_kv(&c, s->procs[pid].name, "%u ticks left", s->procs[pid].io_remaining); n++; }
            }
            if (!n) { ui_text(c.x, c.y, 14, UI_TEXT_DIM, "none"); c.y += 18; }
            panel_heading(&c, "Page-in (fault service)");
            n = 0;
            for (int pid = 0; pid < s->num_processes; pid++) {
                const pcb_t *p = &s->procs[pid];
                if (p->state == PROC_BLOCKED_FAULT) { panel_kv(&c, p->name, "page %u, %u left", p->fault_vpage, p->fault_remaining); n++; }
            }
            if (!n) { ui_text(c.x, c.y, 14, UI_TEXT_DIM, "none"); c.y += 18; }
            break;
        }
        default: {
            ui_text(c.x, c.y, 20, UI_TEXT, "Scenario");
            c.y += 30;
            int live = 0;
            for (int pid = 0; pid < s->num_processes; pid++) live += proc_live(s, pid);
            panel_kv(&c, "processes", "%d", live);
            panel_kv(&c, "cores", "%d", s->num_cores);
            panel_kv(&c, "frames", "%d", s->total_frames);
            panel_kv(&c, "scheduler", "%s (q=%u)", sched_get_ops(s->sched_policy)->name, s->default_quantum);
            panel_kv(&c, "replacement", "%s", repl_get_ops(s->repl_policy)->name);
            panel_kv(&c, "seed", "%" PRIu64, s->seed);
            c.y += 12;
            static const char *help[] = {
                "1. Drag a Process from the palette",
                "   (or press A) and script its bursts.",
                "2. Click a node to inspect / configure it.",
                "3. Run (Space) or Step (N) to watch",
                "   processes move through the system.",
                "4. While running, Inject a process",
                "   or switch policies live.",
                "5. Reset (R) returns to edit mode.",
            };
            for (int i = 0; i < 8; i++) { ui_text(c.x, c.y, 13, UI_TEXT_DIM, "%s", help[i]); c.y += 17; }
            break;
        }
    }
    ui_scissor_pop();
}

