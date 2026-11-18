#include "gui_state.h"
#include <inttypes.h>
#include <stdio.h>

#define ROW_H       22.0f
#define INDENT      26.0f
#define CHIP_H      18.0f
#define MAX_DEPTH   PRISM_MAX_PROCESSES

static const char *parent_name(const sim_t *s, sim_pid_t pid) {
    return pid < 0 ? "init" : explain_process_name(s, pid);
}

static void explain_reap(gui_t *g, const sim_event_t *ev, const char *child) {
    const sim_t *s = g->sim;
    if (ev->detail1 < 0) {
        explain_narrate(g, ev, "init reaps the orphan %s: its exit status is collected and its process entry disappears.", child);
        return;
    }
    const char *parent = parent_name(s, ev->detail1);
    if (s->procs[ev->detail1].state == PROC_BLOCKED_WAIT) {
        explain_narrate(g, ev, "%s exits while %s is blocked in wait(): %s collects its exit status and wakes up.", child,
                        parent, parent);
    } else {
        explain_narrate(g, ev, "%s calls wait() and finds the zombie %s: it collects the exit status straight away (no "
                        "blocking) and the zombie is gone.", parent, child);
    }
}

void explain_tree_event(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const char *n = explain_process_name(s, ev->pid);

    switch (ev->type) {
        case EVENT_FORK:
            explain_narrate(g, ev, "%s calls fork(): a new child process %s (pid %d) is created and joins the ready queue. "
                            "%s carries on with its next burst.", n, explain_process_name(s, ev->detail1), ev->detail1, n);
            break;
        case EVENT_WAIT_BLOCKED:
            explain_narrate(g, ev, "%s calls wait(), but none of its %d running child%s has exited yet, so %s blocks until "
                            "one does.", n, ev->detail1, ev->detail1 == 1 ? "" : "ren", n);
            break;
        case EVENT_ZOMBIE:
            explain_narrate(g, ev, "%s exits, but its parent %s has not called wait(): %s becomes a ZOMBIE. Its memory is "
                            "freed; only the process entry and exit status remain until the parent reaps it.", n,
                            parent_name(s, ev->detail1), n);
            break;
        case EVENT_REAP:
            explain_reap(g, ev, n);
            break;
        case EVENT_ORPHAN:
            explain_narrate(g, ev, "%s exited while its child %s was still running: %s is an ORPHAN and init adopts it, "
                            "so someone will still reap it when it exits.", parent_name(s, ev->detail1), n, n);
            break;
        default:
            break;
    }
}

static bool shown_under(const pcb_t *p, sim_pid_t parent) {
    if (p->state == PROC_UNUSED) return false;
    if (parent == PRISM_INIT_PID) return p->parent_pid == PRISM_INIT_PID || p->orphaned;
    return p->parent_pid == parent && !p->orphaned;
}

static const char *process_tag(const pcb_t *p, Color *color) {
    *color = UI_TEXT_DIM;
    if (p->zombie) {
        *color = (Color){240, 175, 60, 255};
        return "zombie: exited, waiting to be reaped";
    }
    if (p->awaiting_fork) return "its parent has not reached that fork() yet";
    if (p->state == PROC_TERMINATED) return p->orphaned ? "exited, reaped by init" : "exited and reaped";
    if (p->orphaned) {
        *color = (Color){240, 175, 60, 255};
        return "orphan, adopted by init";
    }
    if (p->state == PROC_BLOCKED_WAIT) {
        *color = state_color(p->state);
        return "in wait() for a child";
    }
    return NULL;
}

static void draw_connector(float x, float y, float parent_y) {
    float elbow_x = x - INDENT / 2;
    DrawLineEx((Vector2){elbow_x, parent_y}, (Vector2){elbow_x, y + ROW_H / 2}, 1, UI_BORDER);
    DrawLineEx((Vector2){elbow_x, y + ROW_H / 2}, (Vector2){x - 3, y + ROW_H / 2}, 1, UI_BORDER);
}

static float draw_process_row(const sim_t *s, sim_pid_t pid, float x, float y) {
    const pcb_t *p = &s->procs[pid];
    bool faded = p->awaiting_fork || (p->state == PROC_TERMINATED && !p->zombie);
    const char *label = TextFormat("%s  %d", p->name, pid);
    float w = ui_text_width(label, 12) + 14;

    Color chip = faded ? ColorAlpha(proc_color(pid), 0.35f) : proc_color(pid);
    DrawRectangleRounded((Rectangle){x, y + 2, w, CHIP_H}, 0.4f, 4, chip);
    if (p->zombie) DrawRectangleRoundedLinesEx((Rectangle){x, y + 2, w, CHIP_H}, 0.4f, 4, 2, (Color){240, 175, 60, 255});
    ui_text(x + 7, y + 5, 12, (Color){20, 20, 25, 255}, "%s", label);

    float info_x = x + w + 8;
    Color state = state_color(p->state);
    ui_text(info_x, y + 5, 12, faded ? UI_TEXT_DIM : state, "%s", gui_proc_state_label(p));

    Color tag_color;
    const char *tag = process_tag(p, &tag_color);
    if (tag) ui_text(info_x + 84, y + 5, 12, tag_color, "%s", tag);
    return y + ROW_H;
}

static float draw_children(const sim_t *s, sim_pid_t parent, float x, float y, float parent_y, int depth) {
    if (depth >= MAX_DEPTH) return y;
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (!shown_under(&s->procs[pid], parent)) continue;
        draw_connector(x, y, parent_y);
        float row_y = y;
        y = draw_process_row(s, pid, x, y);
        y = draw_children(s, pid, x + INDENT, y, row_y + ROW_H - 2, depth + 1);
    }
    return y;
}

static bool has_tree(const sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        if (s->procs[i].state != PROC_UNUSED && s->procs[i].parent_pid >= 0) return true;
    }
    return false;
}

static void draw_counts(const sim_t *s, float x, float y) {
    int zombies = 0, orphans = 0, unborn = 0;
    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        zombies += p->zombie;
        orphans += p->orphaned;
        unborn += p->awaiting_fork;
    }
    ui_text(x, y, 13, UI_ACCENT, "PROCESS TREE");
    ui_text(x + 120, y + 1, 12, UI_TEXT_DIM, "tick %" PRIu64 "   zombies %d   orphans adopted %d   not forked %d",
            (uint64_t)s->tick, zombies, orphans, unborn);
}

void panel_tree(gui_t *g, Rectangle body) {
    const sim_t *s = g->sim;
    ui_scissor_push(body);
    float x = body.x + 10, y = body.y + 8, w = body.width - 20;

    draw_counts(s, x, y);
    y += 24;
    if (!has_tree(s)) {
        ui_text_wrapped(x, y, w, 13, UI_TEXT_DIM,
                        "Every process here arrives on its own, so they are all children of init. Open process_tree "
                        "to see fork(), wait(), zombies and orphans. In a scenario file: \"parent=NAME\" on the child, "
                        "and fork / wait bursts on the parent (or use the builder's Parent field).");
        y += 54;
    }

    DrawRectangleRounded((Rectangle){x, y + 2, 44, CHIP_H}, 0.4f, 4, UI_PANEL_HI);
    ui_text(x + 8, y + 5, 12, UI_TEXT, "init");
    ui_text(x + 54, y + 5, 12, UI_TEXT_DIM, "adopts orphans and reaps them");
    draw_children(s, PRISM_INIT_PID, x + INDENT, y + ROW_H, y + ROW_H - 2, 0);
    ui_scissor_pop();
}
