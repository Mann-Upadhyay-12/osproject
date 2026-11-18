#include "gui_state.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CHIP_H       18.0f
#define VALUE_BOX_W  44.0f

static const char *sem_name(const sim_t *s, int sem_id) {
    return sem_id >= 0 && sem_id < s->num_semaphores ? s->semaphores[sem_id].name : "?";
}

static void explain_signal(gui_t *g, const sim_event_t *ev, const char *who, const char *sem_label) {
    const sim_semaphore_t *sem = &g->sim->semaphores[ev->detail1];
    if (sem->num_waiters > 0) {
        const char *first = explain_process_name(g->sim, sem->waiters[0]);
        explain_narrate(g, ev, "%s calls signal(%s). %s is first in the semaphore's queue, so %s is woken and takes the "
                        "permit directly: the value stays %d.", who, sem_label, first, first, ev->detail2);
    } else {
        explain_narrate(g, ev, "%s calls signal(%s). Nobody is waiting, so the value goes up to %d.", who, sem_label,
                        ev->detail2);
    }
}

void explain_sync_event(gui_t *g, const sim_event_t *ev) {
    const sim_t *s = g->sim;
    const char *who = explain_process_name(s, ev->pid);
    const char *sem = sem_name(s, ev->detail1);

    switch (ev->type) {
        case EVENT_SEM_WAIT:
            explain_narrate(g, ev, "%s calls wait(%s). The value was %d > 0, so it drops to %d and %s carries on.", who, sem,
                            ev->detail2 + 1, ev->detail2, who);
            break;
        case EVENT_SEM_BLOCKED:
            explain_narrate(g, ev, "%s calls wait(%s) but the value is 0: %s blocks and joins the semaphore's queue "
                            "(%d waiting). The CPU goes to someone else.", who, sem, who, ev->detail2);
            break;
        case EVENT_SEM_SIGNAL:
            explain_signal(g, ev, who, sem);
            break;
        case EVENT_SEM_WAKE:
            explain_narrate(g, ev, "%s was signalled on %s by %s: it now holds the permit and moves to the ready queue.", who,
                            sem, explain_process_name(s, ev->detail2));
            break;
        case EVENT_STALLED: {
            char detail[400];
            sync_describe_stall(s, detail, sizeof(detail));
            explain_narrate(g, ev, "DEADLOCK: nobody can run again. %s", detail);
            break;
        }
        default:
            break;
    }
}

static const char *blocked_reason(const sim_t *s, const pcb_t *p) {
    switch (p->state) {
        case PROC_BLOCKED_SEM:      return sem_name(s, p->waiting_semaphore);
        case PROC_BLOCKED_RESOURCE: return TextFormat("resource %s", p->waiting_resource >= 0 ? s->resources[p->waiting_resource].name : "?");
        case PROC_BLOCKED_IPC:      return TextFormat("a message from %s", explain_process_name(s, p->waiting_ipc_pid));
        default:                    return NULL;
    }
}

void sync_describe_stall(const sim_t *s, char *out, size_t size) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < s->num_processes && used + 1 < size; i++) {
        const char *reason = blocked_reason(s, &s->procs[i]);
        if (!reason) continue;
        used += (size_t)snprintf(out + used, size - used, "%s%s waits for %s", used ? ", " : "", s->procs[i].name, reason);
    }
    if (used + 1 < size) snprintf(out + used, size - used, ".");
}

static float draw_chip(float x, float y, const char *text, Color color) {
    float w = ui_text_width(text, 12) + 12;
    DrawRectangleRounded((Rectangle){x, y, w, CHIP_H}, 0.4f, 4, ColorAlpha(color, 0.85f));
    ui_text(x + 6, y + 3, 12, (Color){20, 20, 25, 255}, "%s", text);
    return x + w + 4;
}

static float draw_waiter_chips(const sim_t *s, const sim_semaphore_t *sem, float x, float y, float right) {
    if (sem->num_waiters == 0) {
        ui_text(x, y + 3, 12, UI_TEXT_DIM, "no one waiting");
        return x;
    }
    for (int i = 0; i < sem->num_waiters; i++) {
        if (x > right - 50) {
            ui_text(x, y + 3, 12, UI_TEXT_DIM, "+%d", sem->num_waiters - i);
            break;
        }
        sim_pid_t pid = sem->waiters[i];
        x = draw_chip(x, y + 2, s->procs[pid].name, proc_color(pid));
    }
    return x;
}

static float draw_semaphore(const sim_t *s, int id, float x, float y, float w) {
    const sim_semaphore_t *sem = &s->semaphores[id];
    Color value_color = sem->value > 0 ? UI_GOOD : sem->num_waiters > 0 ? UI_BAD : UI_TEXT_DIM;

    Rectangle name_area = {x, y + 4, 84, 18};
    ui_text_clipped(name_area, 14, UI_TEXT, sem->name);
    Rectangle box = {x + 88, y, VALUE_BOX_W, 22};
    DrawRectangleRounded(box, 0.3f, 4, ColorAlpha(value_color, 0.22f));
    DrawRectangleRoundedLines(box, 0.3f, 4, value_color);
    const char *value = TextFormat("%d", sem->value);
    ui_text(box.x + (box.width - ui_text_width(value, 15)) / 2, box.y + 3, 15, value_color, "%s", value);
    ui_text(box.x + box.width + 6, y + 5, 12, UI_TEXT_DIM, "of %d", sem->initial_value);

    const char *stats = TextFormat("waits %u, blocked %u", sem->total_waits, sem->total_blocks);
    float stats_x = x + w - ui_text_width(stats, 12);
    ui_text(stats_x, y + 5, 12, UI_TEXT_DIM, "%s", stats);
    draw_waiter_chips(s, sem, box.x + box.width + 44, y, stats_x - 8);
    return y + 26;
}

static float draw_resource(const sim_t *s, int id, float x, float y) {
    const sim_resource_t *res = &s->resources[id];
    ui_text(x, y, 14, UI_TEXT, "%s", res->name);
    char holders[160] = "";
    size_t used = 0;
    for (int pid = 0; pid < s->num_processes && used + 1 < sizeof(holders); pid++) {
        if (res->allocated_mask & (1ULL << pid)) {
            used += (size_t)snprintf(holders + used, sizeof(holders) - used, "%s%s", used ? ", " : "", s->procs[pid].name);
        }
    }
    ui_text(x + 110, y + 1, 12, UI_TEXT_DIM, "%d of %d free%s%s", res->available_instances, res->total_instances,
            used ? ", held by " : "", holders);
    return y + 20;
}

static float draw_blocked_list(const sim_t *s, float x, float y) {
    bool any = false;
    for (int i = 0; i < s->num_processes; i++) {
        const char *reason = blocked_reason(s, &s->procs[i]);
        if (!reason) continue;
        if (!any) {
            ui_text(x, y, 13, UI_ACCENT, "BLOCKED");
            y += 20;
            any = true;
        }
        draw_chip(x, y, s->procs[i].name, proc_color(i));
        ui_text(x + ui_text_width(s->procs[i].name, 12) + 22, y + 2, 12, UI_TEXT, "waits for %s", reason);
        y += CHIP_H + 4;
    }
    return y;
}

static float draw_stall_banner(const sim_t *s, float x, float y, float w) {
    if (!sim_is_stalled(s)) return y;
    char detail[440];
    sync_describe_stall(s, detail, sizeof(detail));
    snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail), " Each one waits for something only another blocked process can give.");
    float text_h = ui_text_wrapped_height(w - 16, 12, detail);
    Rectangle banner = {x, y, w, text_h + 30};
    DrawRectangleRec(banner, ColorAlpha(UI_BAD, 0.18f));
    DrawRectangleLinesEx(banner, 1, UI_BAD);
    ui_text(x + 8, y + 5, 14, UI_BAD, "DEADLOCK at tick %" PRIu64 ": every live process is blocked", (uint64_t)s->tick);
    ui_text_wrapped(x + 8, y + 24, w - 16, 12, UI_TEXT, detail);
    return y + banner.height + 10;
}

void panel_sync(gui_t *g, Rectangle body) {
    const sim_t *s = g->sim;
    ui_scissor_push(body);
    float x = body.x + 10, y = body.y + 8, w = body.width - 20;

    if (s->num_semaphores == 0 && s->num_resources == 0) {
        ui_text_wrapped(x, y, w, 13, UI_TEXT_DIM,
                        "This scenario has no semaphores. Open producer_consumer, readers_writers, dining_philosophers "
                        "or dining_philosophers_fixed to watch wait() and signal() at work. In a scenario file: "
                        "\"semaphore NAME VALUE\", and bursts like wait:NAME / signal:NAME.");
        ui_scissor_pop();
        return;
    }

    y = draw_stall_banner(s, x, y, w);
    if (s->num_semaphores > 0) {
        ui_text(x, y, 13, UI_ACCENT, "SEMAPHORES  value = permits left, chips = waiting queue");
        y += 22;
        for (int i = 0; i < s->num_semaphores; i++) y = draw_semaphore(s, i, x, y, w);
        y += 4;
    }
    if (s->num_resources > 0) {
        ui_text(x, y, 13, UI_ACCENT, "RESOURCES");
        y += 20;
        for (int i = 0; i < s->num_resources; i++) y = draw_resource(s, i, x, y);
        y += 6;
    }
    draw_blocked_list(s, x, y);
    ui_scissor_pop();
}
