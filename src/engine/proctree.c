#include "prism/sim.h"

#define NO_CORE (-1)

static bool is_child_of(const pcb_t *child, sim_pid_t parent) {
    return child->state != PROC_UNUSED && child->parent_pid == parent && parent != PRISM_INIT_PID;
}

static bool exited(const pcb_t *p) {
    return p->state == PROC_TERMINATED || p->state == PROC_UNUSED;
}

bool sim_tree_unborn(const sim_t *s, const pcb_t *p) {
    if (!p->awaiting_fork) return false;
    bool parent_valid = p->parent_pid >= 0 && p->parent_pid < s->num_processes;
    return !parent_valid || exited(&s->procs[p->parent_pid]);
}

int sim_tree_live_children(const sim_t *s, sim_pid_t parent) {
    int live = 0;
    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *c = &s->procs[i];
        live += is_child_of(c, parent) && !c->awaiting_fork && !exited(c);
    }
    return live;
}

static pcb_t *next_child_to_fork(sim_t *s, sim_pid_t parent) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *c = &s->procs[i];
        if (is_child_of(c, parent) && c->awaiting_fork) return c;
    }
    return NULL;
}

bool sim_tree_fork(sim_t *s, pcb_t *parent) {
    pcb_t *child = next_child_to_fork(s, parent->pid);
    if (!child) return true;

    child->awaiting_fork = false;
    child->arrival_tick = s->tick;
    child->state = PROC_READY;
    sim_log_event(s, EVENT_FORK, parent->pid, parent->assigned_core, child->pid, 0);
    sim_log_event(s, EVENT_PROC_ARRIVE, child->pid, NO_CORE, parent->pid, 0);
    sched_get_ops(s->sched_policy)->on_ready(s, child->pid);
    return true;
}

static pcb_t *zombie_child(sim_t *s, sim_pid_t parent) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *c = &s->procs[i];
        if (is_child_of(c, parent) && c->zombie) return c;
    }
    return NULL;
}

static void reap(sim_t *s, pcb_t *child, sim_pid_t reaper) {
    child->zombie = false;
    sim_log_event(s, EVENT_REAP, child->pid, NO_CORE, reaper, 0);
}

bool sim_tree_wait(sim_t *s, pcb_t *parent) {
    pcb_t *zombie = zombie_child(s, parent->pid);
    if (zombie) {
        reap(s, zombie, parent->pid);
        return true;
    }

    int live = sim_tree_live_children(s, parent->pid);
    if (live == 0) return true;

    parent->state = PROC_BLOCKED_WAIT;
    sim_log_event(s, EVENT_WAIT_BLOCKED, parent->pid, parent->assigned_core, live, 0);
    return false;
}

static void wake_waiting_parent(sim_t *s, pcb_t *parent, pcb_t *child) {
    reap(s, child, parent->pid);
    parent->curr_burst_idx++;
    parent->state = PROC_READY;
    sched_get_ops(s->sched_policy)->on_ready(s, parent->pid);
}

static void notify_parent(sim_t *s, pcb_t *child) {
    if (child->orphaned || child->parent_pid == PRISM_INIT_PID) {
        if (child->orphaned) reap(s, child, PRISM_INIT_PID);
        return;
    }

    pcb_t *parent = &s->procs[child->parent_pid];
    if (parent->state == PROC_BLOCKED_WAIT) {
        wake_waiting_parent(s, parent, child);
        return;
    }
    child->zombie = true;
    sim_log_event(s, EVENT_ZOMBIE, child->pid, NO_CORE, parent->pid, 0);
}

static void hand_children_to_init(sim_t *s, sim_pid_t parent) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *c = &s->procs[i];
        if (!is_child_of(c, parent) || c->awaiting_fork) continue;

        if (c->zombie) {
            reap(s, c, PRISM_INIT_PID);
        } else if (!exited(c)) {
            c->orphaned = true;
            sim_log_event(s, EVENT_ORPHAN, c->pid, NO_CORE, parent, 0);
        }
    }
}

void sim_tree_on_exit(sim_t *s, pcb_t *p) {
    bool has_parent = p->parent_pid >= 0 && p->parent_pid < s->num_processes;
    if (has_parent || p->orphaned) notify_parent(s, p);
    hand_children_to_init(s, p->pid);
}
