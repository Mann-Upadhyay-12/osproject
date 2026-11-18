#include "prism/sim.h"

#define NO_PROCESS (-1)

static bool is_valid_queue(int queue_idx) {
    return queue_idx >= 0 && queue_idx < PRISM_READY_QUEUES;
}

void sim_queue_push(sim_t *s, int queue_idx, sim_pid_t pid) {
    if (!is_valid_queue(queue_idx)) return;
    if (pid < 0 || pid >= s->num_processes) return;

    s->procs[pid].next_proc = NO_PROCESS;

    bool queue_empty = s->ready_head[queue_idx] == NO_PROCESS;
    if (queue_empty) {
        s->ready_head[queue_idx] = (int16_t)pid;
    } else {
        s->procs[s->ready_tail[queue_idx]].next_proc = (int16_t)pid;
    }
    s->ready_tail[queue_idx] = (int16_t)pid;
    s->ready_count[queue_idx]++;
}

sim_pid_t sim_queue_pop(sim_t *s, int queue_idx) {
    if (!is_valid_queue(queue_idx)) return NO_PROCESS;

    sim_pid_t pid = s->ready_head[queue_idx];
    if (pid == NO_PROCESS) return NO_PROCESS;

    pcb_t *p = &s->procs[pid];
    s->ready_head[queue_idx] = p->next_proc;
    if (s->ready_head[queue_idx] == NO_PROCESS) {
        s->ready_tail[queue_idx] = NO_PROCESS;
    }
    p->next_proc = NO_PROCESS;
    s->ready_count[queue_idx]--;
    return pid;
}

bool sim_queue_remove(sim_t *s, int queue_idx, sim_pid_t pid) {
    if (!is_valid_queue(queue_idx)) return false;

    sim_pid_t prev = NO_PROCESS;
    sim_pid_t curr = s->ready_head[queue_idx];

    while (curr != NO_PROCESS && curr != pid) {
        prev = curr;
        curr = s->procs[curr].next_proc;
    }
    if (curr == NO_PROCESS) return false;

    sim_pid_t next = s->procs[curr].next_proc;
    if (prev == NO_PROCESS) {
        s->ready_head[queue_idx] = (int16_t)next;
    } else {
        s->procs[prev].next_proc = (int16_t)next;
    }
    if (s->ready_tail[queue_idx] == curr) {
        s->ready_tail[queue_idx] = (int16_t)prev;
    }

    s->procs[curr].next_proc = NO_PROCESS;
    s->ready_count[queue_idx]--;
    return true;
}
