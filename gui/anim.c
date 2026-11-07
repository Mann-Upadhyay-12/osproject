#include "gui_state.h"
#include <math.h>
#include <string.h>

/*
 * Trace events -> animations. Each transition becomes a token travelling
 * along the matching links (e.g. DISPATCH: Ready -> Scheduler -> CPU).
 * Tokens of one process are queued back to back so a wake followed by a
 * dispatch in the same tick reads as one continuous move.
 */

static double g_busy_until[PRISM_MAX_PROCESSES];

void anim_clear(gui_t *g) {
    g->ntok = 0;
    memset(g_busy_until, 0, sizeof(g_busy_until));
    for (int f = 0; f < PRISM_MAX_FRAMES; f++) { g->frame_flash[f] = -10; g->frame_load[f] = -10; }
    for (int k = 0; k < NODE_SYS_COUNT; k++) g->node_flash[k] = -10;
}

static void push_event(gui_t *g, const sim_event_t *ev) {
    if (g->ev_len < GUI_EV_CAP) {
        g->ev[(g->ev_start + g->ev_len) % GUI_EV_CAP] = *ev;
        g->ev_len++;
    } else {
        g->ev[g->ev_start] = *ev;
        g->ev_start = (g->ev_start + 1) % GUI_EV_CAP;
    }
}

static void add_token(gui_t *g, int pid, const int *nodes, int n, bool fault) {
    token_t t;
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < n && t.npts < GUI_MAX_PATH; i++) {
        int k = nodes[i];
        if (k == NODE_PROC) {
            if (pid < 0) continue;
            t.pts[t.npts++] = canvas_node_center(g, (node_ref_t){NODE_PROC, pid});
        } else if (gui_has_node(g, k)) {
            t.pts[t.npts++] = canvas_node_center(g, (node_ref_t){k, 0});
        }
    }
    if (t.npts < 2) return;
    t.pid = pid;
    t.color = proc_color(pid);
    t.fault = fault;
    t.dur = g->anim_dur;
    double start = g->anim_now;
    if (pid >= 0 && pid < PRISM_MAX_PROCESSES) {
        double busy = g_busy_until[pid];
        if (busy > start && busy - start < 3 * t.dur) start = busy;
        g_busy_until[pid] = start + t.dur;
    }
    t.t0 = start;
    if (g->ntok == GUI_MAX_TOKENS) {
        memmove(&g->tok[0], &g->tok[1], (GUI_MAX_TOKENS - 1) * sizeof(token_t));
        g->ntok--;
    }
    g->tok[g->ntok++] = t;
}

void anim_observer(const sim_event_t *ev, void *user) {
    gui_t *g = (gui_t *)user;
    push_event(g, ev);
    if (g->anim_dur <= 0) return;   /* animations off (very fast playback) */

    double now = g->anim_now;
    int pid = ev->pid;
    switch (ev->type) {
        case EVENT_PROC_ARRIVE: {
            int p[] = {NODE_PROC, NODE_READY};
            add_token(g, pid, p, 2, false);
            break;
        }
        case EVENT_PROC_DISPATCH: {
            int p[] = {NODE_READY, NODE_SCHED, NODE_CPU};
            add_token(g, pid, p, 3, false);
            g->node_flash[NODE_SCHED] = now;
            break;
        }
        case EVENT_PROC_PREEMPT: {
            int p[] = {NODE_CPU, NODE_READY};
            add_token(g, pid, p, 2, false);
            break;
        }
        case EVENT_PROC_BLOCK_IO: {
            int p[] = {NODE_CPU, NODE_DISK};
            add_token(g, pid, p, 2, false);
            break;
        }
        case EVENT_PROC_WAKE_IO: {
            int p[] = {NODE_DISK, NODE_READY};
            add_token(g, pid, p, 2, false);
            break;
        }
        case EVENT_PROC_BLOCK_FAULT: {
            int p[] = {NODE_CPU, NODE_MEMORY, NODE_DISK};
            add_token(g, pid, p, 3, true);
            break;
        }
        case EVENT_PROC_WAKE_FAULT: {
            int p[] = {NODE_DISK, NODE_MEMORY, NODE_READY};
            add_token(g, pid, p, 3, false);
            if (pid >= 0 && ev->detail1 >= 0 && ev->detail1 < PRISM_MAX_VIRTUAL_PAGES) {
                int f = g->sim->procs[pid].pt[ev->detail1].frame_idx;
                if (f >= 0 && f < PRISM_MAX_FRAMES) g->frame_load[f] = now;
            }
            break;
        }
        case EVENT_PROC_TERMINATE:
        case EVENT_PROC_KILLED: {
            int from = (ev->core_id >= 0) ? NODE_CPU : NODE_DISK;
            int p[] = {from, NODE_PROC};
            add_token(g, pid, p, 2, ev->type == EVENT_PROC_KILLED);
            break;
        }
        case EVENT_PAGE_FAULT:
            g->node_flash[NODE_MEMORY] = now;
            break;
        case EVENT_PAGE_EVICT:
            if (ev->detail2 >= 0 && ev->detail2 < PRISM_MAX_FRAMES) g->frame_flash[ev->detail2] = now;
            break;
        default:
            break;
    }
}

void anim_update(gui_t *g) {
    g->anim_now = GetTime();
    int w = 0;
    for (int i = 0; i < g->ntok; i++) {
        if (g->anim_now < g->tok[i].t0 + g->tok[i].dur) g->tok[w++] = g->tok[i];
    }
    g->ntok = w;
}

static Vector2 path_point(const token_t *t, float u) {
    float len[GUI_MAX_PATH], total = 0;
    for (int i = 0; i + 1 < t->npts; i++) {
        float dx = t->pts[i + 1].x - t->pts[i].x, dy = t->pts[i + 1].y - t->pts[i].y;
        len[i] = sqrtf(dx * dx + dy * dy);
        total += len[i];
    }
    float d = u * total;
    for (int i = 0; i + 1 < t->npts; i++) {
        if (d <= len[i] || i + 2 == t->npts) {
            float k = len[i] > 0 ? d / len[i] : 1;
            if (k > 1) k = 1;
            return (Vector2){t->pts[i].x + (t->pts[i + 1].x - t->pts[i].x) * k,
                             t->pts[i].y + (t->pts[i + 1].y - t->pts[i].y) * k};
        }
        d -= len[i];
    }
    return t->pts[t->npts - 1];
}

void anim_draw(const gui_t *g) {
    for (int i = 0; i < g->ntok; i++) {
        const token_t *t = &g->tok[i];
        double u = (g->anim_now - t->t0) / t->dur;
        if (u < 0) continue;
        if (u > 1) u = 1;
        float e = (float)(u < 0.5 ? 2 * u * u : 1 - pow(-2 * u + 2, 2) / 2);   /* ease in-out */
        Vector2 p = path_point(t, e);
        DrawCircleV((Vector2){p.x + 2, p.y + 3}, 13, (Color){0, 0, 0, 90});
        DrawCircleV(p, 13, t->color);
        if (t->fault) DrawRing(p, 13, 17, 0, 360, 24, UI_BAD);
        else DrawCircleLinesV(p, 13, (Color){20, 20, 25, 255});
        char s[4] = {0};
        if (t->pid >= 0) memcpy(s, g->sim->procs[t->pid].name, 2);
        ui_text(p.x - ui_text_width(s, 13) / 2, p.y - 7, 13, (Color){15, 15, 20, 255}, "%s", s);
    }
}
