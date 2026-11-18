#include "gui_state.h"
#include <math.h>
#include <string.h>

#define TOKEN_RADIUS   13
#define LONG_AGO       (-10)
#define MAX_QUEUE_LAG  3

static double g_busy_until[PRISM_MAX_PROCESSES];

void anim_clear(gui_t *g) {
    g->num_tokens = 0;
    memset(g_busy_until, 0, sizeof(g_busy_until));
    for (int f = 0; f < PRISM_MAX_FRAMES; f++) {
        g->frame_flash[f] = LONG_AGO;
        g->frame_load[f] = LONG_AGO;
    }
    for (int k = 0; k < NODE_SYS_COUNT; k++) g->node_flash[k] = LONG_AGO;
}

static void record_event(gui_t *g, const sim_event_t *ev) {
    if (g->events_len < GUI_EV_CAP) {
        g->events[(g->events_start + g->events_len) % GUI_EV_CAP] = *ev;
        g->events_len++;
    } else {
        g->events[g->events_start] = *ev;
        g->events_start = (g->events_start + 1) % GUI_EV_CAP;
    }
}

static void build_path(const gui_t *g, token_t *t, int pid, const int *nodes, int count) {
    for (int i = 0; i < count && t->num_points < GUI_MAX_PATH; i++) {
        int kind = nodes[i];
        if (kind == NODE_PROC) {
            if (pid >= 0) t->points[t->num_points++] = canvas_node_center(g, (node_ref_t){NODE_PROC, pid});
        } else if (gui_has_node(g, kind)) {
            t->points[t->num_points++] = canvas_node_center(g, (node_ref_t){kind, 0});
        }
    }
}

static double queue_after_previous_token(gui_t *g, int pid, double duration) {
    double start = g->anim_now;
    if (pid < 0 || pid >= PRISM_MAX_PROCESSES) return start;

    double busy = g_busy_until[pid];
    bool still_moving = busy > start && busy - start < MAX_QUEUE_LAG * duration;
    if (still_moving) start = busy;
    g_busy_until[pid] = start + duration;
    return start;
}

static void add_token(gui_t *g, int pid, const int *nodes, int count, bool fault) {
    token_t t;
    memset(&t, 0, sizeof(t));
    build_path(g, &t, pid, nodes, count);
    if (t.num_points < 2) return;

    t.pid = pid;
    t.color = proc_color(pid);
    t.fault = fault;
    t.duration = g->anim_duration;
    t.start_time = queue_after_previous_token(g, pid, t.duration);

    if (g->num_tokens == GUI_MAX_TOKENS) {
        memmove(&g->tokens[0], &g->tokens[1], (GUI_MAX_TOKENS - 1) * sizeof(token_t));
        g->num_tokens--;
    }
    g->tokens[g->num_tokens++] = t;
}

#define MOVE(g, pid, fault, ...) do {                                   \
        const int path_[] = {__VA_ARGS__};                              \
        add_token(g, pid, path_, (int)(sizeof(path_) / sizeof(path_[0])), fault); \
    } while (0)

static void mark_loaded_frame(gui_t *g, const sim_event_t *ev) {
    int pid = ev->pid;
    int vpage = ev->detail1;
    if (pid < 0 || vpage < 0 || vpage >= PRISM_MAX_VIRTUAL_PAGES) return;

    int frame = g->sim->procs[pid].pt[vpage].frame_idx;
    if (frame >= 0 && frame < PRISM_MAX_FRAMES) g->frame_load[frame] = g->anim_now;
}

void anim_observer(const sim_event_t *ev, void *user) {
    gui_t *g = (gui_t *)user;
    record_event(g, ev);
    explain_event(g, ev);

    bool animations_off = g->anim_duration <= 0 || g->replaying;
    if (animations_off) return;

    int pid = ev->pid;
    switch (ev->type) {
        case EVENT_PROC_ARRIVE:
            MOVE(g, pid, false, NODE_PROC, NODE_READY);
            break;
        case EVENT_PROC_DISPATCH:
            MOVE(g, pid, false, NODE_READY, NODE_SCHED, NODE_CPU);
            g->node_flash[NODE_SCHED] = g->anim_now;
            break;
        case EVENT_PROC_PREEMPT:
            MOVE(g, pid, false, NODE_CPU, NODE_READY);
            break;
        case EVENT_PROC_BLOCK_IO:
            MOVE(g, pid, false, NODE_CPU, NODE_DISK);
            break;
        case EVENT_PROC_WAKE_IO:
            MOVE(g, pid, false, NODE_DISK, NODE_READY);
            break;
        case EVENT_PROC_BLOCK_FAULT:
            MOVE(g, pid, true, NODE_CPU, NODE_MEMORY, NODE_DISK);
            break;
        case EVENT_PROC_WAKE_FAULT:
            MOVE(g, pid, false, NODE_DISK, NODE_MEMORY, NODE_READY);
            mark_loaded_frame(g, ev);
            break;
        case EVENT_PROC_TERMINATE:
        case EVENT_PROC_KILLED: {
            int from = ev->core_id >= 0 ? NODE_CPU : NODE_DISK;
            MOVE(g, pid, ev->type == EVENT_PROC_KILLED, from, NODE_PROC);
            break;
        }
        case EVENT_SEM_BLOCKED:
            MOVE(g, pid, true, NODE_CPU, NODE_PROC);
            break;
        case EVENT_SEM_WAKE:
            MOVE(g, pid, false, NODE_PROC, NODE_READY);
            break;
        case EVENT_PAGE_FAULT:
            g->node_flash[NODE_MEMORY] = g->anim_now;
            break;
        case EVENT_PAGE_EVICT:
            if (ev->detail2 >= 0 && ev->detail2 < PRISM_MAX_FRAMES) g->frame_flash[ev->detail2] = g->anim_now;
            break;
        default:
            break;
    }
}

void anim_update(gui_t *g) {
    g->anim_now = GetTime();

    int kept = 0;
    for (int i = 0; i < g->num_tokens; i++) {
        bool still_moving = g->anim_now < g->tokens[i].start_time + g->tokens[i].duration;
        if (still_moving) g->tokens[kept++] = g->tokens[i];
    }
    g->num_tokens = kept;
}

static Vector2 lerp(Vector2 a, Vector2 b, float k) {
    return (Vector2){a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k};
}

static Vector2 point_along_path(const token_t *t, float progress) {
    float segment[GUI_MAX_PATH], total = 0;
    for (int i = 0; i + 1 < t->num_points; i++) {
        float dx = t->points[i + 1].x - t->points[i].x;
        float dy = t->points[i + 1].y - t->points[i].y;
        segment[i] = sqrtf(dx * dx + dy * dy);
        total += segment[i];
    }

    float distance = progress * total;
    for (int i = 0; i + 1 < t->num_points; i++) {
        bool last_segment = i + 2 == t->num_points;
        if (distance <= segment[i] || last_segment) {
            float k = segment[i] > 0 ? distance / segment[i] : 1;
            if (k > 1) k = 1;
            return lerp(t->points[i], t->points[i + 1], k);
        }
        distance -= segment[i];
    }
    return t->points[t->num_points - 1];
}

static float ease_in_out(double u) {
    return (float)(u < 0.5 ? 2 * u * u : 1 - pow(-2 * u + 2, 2) / 2);
}

static void draw_token(const gui_t *g, const token_t *t, Vector2 p) {
    DrawCircleV((Vector2){p.x + 2, p.y + 3}, TOKEN_RADIUS, (Color){0, 0, 0, 90});
    DrawCircleV(p, TOKEN_RADIUS, t->color);
    if (t->fault) {
        DrawRing(p, TOKEN_RADIUS, TOKEN_RADIUS + 4, 0, 360, 24, UI_BAD);
    } else {
        DrawCircleLinesV(p, TOKEN_RADIUS, (Color){20, 20, 25, 255});
    }

    char initials[4] = {0};
    if (t->pid >= 0) memcpy(initials, g->sim->procs[t->pid].name, 2);
    ui_text(p.x - ui_text_width(initials, 13) / 2, p.y - 7, 13, (Color){15, 15, 20, 255}, "%s", initials);
}

void anim_draw(const gui_t *g) {
    for (int i = 0; i < g->num_tokens; i++) {
        const token_t *t = &g->tokens[i];
        double progress = (g->anim_now - t->start_time) / t->duration;
        if (progress < 0) continue;
        if (progress > 1) progress = 1;

        draw_token(g, t, point_along_path(t, ease_in_out(progress)));
    }
}
