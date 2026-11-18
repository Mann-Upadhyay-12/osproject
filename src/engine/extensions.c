#include "prism/sim.h"
#include <stdio.h>

#define NO_PROCESS (-1)
#define NO_CORE    (-1)

static bool is_valid_pid(const sim_t *s, sim_pid_t pid) {
    return pid >= 0 && pid < s->num_processes;
}

static bool is_valid_resource(const sim_t *s, int32_t resource_id) {
    return resource_id >= 0 && resource_id < s->num_resources;
}

static bool holds_resource(const pcb_t *p, int32_t resource_id) {
    return (p->held_resources_mask & (1 << resource_id)) != 0;
}

static void notify_ready(sim_t *s, sim_pid_t pid) {
    sched_get_ops(s->sched_policy)->on_ready(s, pid);
}

bool sim_inject_kill(sim_t *s, sim_pid_t pid) {
    if (!is_valid_pid(s, pid)) return false;

    pcb_t *p = &s->procs[pid];
    if (p->state == PROC_TERMINATED || p->state == PROC_UNUSED) return false;

    if (p->assigned_core >= 0) {
        s->running_pid[p->assigned_core] = NO_PROCESS;
        p->assigned_core = NO_CORE;
    }
    for (int q = 0; q < PRISM_READY_QUEUES; q++) {
        sim_queue_remove(s, q, pid);
    }
    sim_sem_forget(s, pid);
    sim_release_all_resources(s, pid);

    p->state = PROC_TERMINATED;
    p->finish_tick = (int64_t)s->tick;
    s->completed_processes++;
    sim_release_frames(s, pid);

    sim_log_event(s, EVENT_PROC_KILLED, pid, NO_CORE, 0, 0);
    sim_tree_on_exit(s, p);
    return true;
}

bool sim_inject_shrink_ram(sim_t *s, int new_frames) {
    if (new_frames < 1 || new_frames >= s->total_frames) return false;

    for (int f = new_frames; f < s->total_frames; f++) {
        frame_entry_t *frame = &s->frames[f];
        if (!frame->occupied || frame->owner_pid < 0) continue;

        page_table_entry_t *entry = &s->procs[frame->owner_pid].pt[frame->vpage];
        entry->valid = false;
        entry->frame_idx = -1;
        sim_log_event(s, EVENT_PAGE_EVICT, frame->owner_pid, NO_CORE, frame->vpage, f);

        frame->occupied = false;
        frame->owner_pid = NO_PROCESS;
    }

    s->total_frames = new_frames;
    sim_log_event(s, EVENT_RAM_SHRUNK, NO_PROCESS, NO_CORE, new_frames, 0);
    return true;
}

bool sim_inject_resource_fail(sim_t *s, int32_t resource_id) {
    if (!is_valid_resource(s, resource_id)) return false;

    s->resources[resource_id].available_instances = 0;
    sim_log_event(s, EVENT_FAULT_INJECTED, NO_PROCESS, NO_CORE, resource_id, 0);
    return true;
}

int32_t sim_add_resource(sim_t *s, const char *name, int32_t instances) {
    if (s->num_resources >= PRISM_MAX_RESOURCES) return -1;

    int32_t rid = s->num_resources++;
    sim_resource_t *res = &s->resources[rid];
    snprintf(res->name, sizeof(res->name), "%s", name ? name : "res");
    res->total_instances = instances > 0 ? instances : 1;
    res->available_instances = res->total_instances;
    res->owner_pid = NO_PROCESS;
    res->allocated_mask = 0;
    return rid;
}

static void grant_resource(sim_t *s, pcb_t *p, int32_t resource_id) {
    s->resources[resource_id].available_instances--;
    s->resources[resource_id].owner_pid = p->pid;
    s->resources[resource_id].allocated_mask |= 1ULL << p->pid;
    p->held_resources_mask |= 1 << resource_id;
    p->waiting_resource = -1;
}

bool sim_acquire_resource(sim_t *s, sim_pid_t pid, int32_t resource_id) {
    if (!is_valid_pid(s, pid) || !is_valid_resource(s, resource_id)) return false;

    pcb_t *p = &s->procs[pid];
    sim_log_event(s, EVENT_RESOURCE_REQUEST, pid, p->assigned_core, resource_id, 0);

    if (s->resources[resource_id].available_instances > 0) {
        grant_resource(s, p, resource_id);
        sim_log_event(s, EVENT_RESOURCE_ACQUIRED, pid, p->assigned_core, resource_id,
                      s->resources[resource_id].available_instances);
        return true;
    }

    p->state = PROC_BLOCKED_RESOURCE;
    p->waiting_resource = (int16_t)resource_id;
    sim_log_event(s, EVENT_RESOURCE_BLOCKED, pid, p->assigned_core, resource_id,
                  s->resources[resource_id].owner_pid);
    sim_detect_deadlock(s);
    return false;
}

static void wake_first_resource_waiter(sim_t *s, int32_t resource_id) {
    if (s->resources[resource_id].available_instances <= 0) return;

    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *waiter = &s->procs[i];
        bool waits_here = waiter->state == PROC_BLOCKED_RESOURCE &&
                          waiter->waiting_resource == resource_id;
        if (!waits_here) continue;

        grant_resource(s, waiter, resource_id);
        waiter->state = PROC_READY;
        waiter->curr_burst_idx++;
        sim_log_event(s, EVENT_RESOURCE_ACQUIRED, waiter->pid, NO_CORE, resource_id,
                      s->resources[resource_id].available_instances);
        notify_ready(s, waiter->pid);
        return;
    }
}

bool sim_release_resource(sim_t *s, sim_pid_t pid, int32_t resource_id) {
    if (!is_valid_pid(s, pid) || !is_valid_resource(s, resource_id)) return false;

    pcb_t *p = &s->procs[pid];
    if (!holds_resource(p, resource_id)) return false;

    p->held_resources_mask &= ~(1 << resource_id);
    s->resources[resource_id].allocated_mask &= ~(1ULL << pid);
    s->resources[resource_id].available_instances++;
    if (s->resources[resource_id].allocated_mask == 0) {
        s->resources[resource_id].owner_pid = NO_PROCESS;
    }
    sim_log_event(s, EVENT_RESOURCE_RELEASED, pid, p->assigned_core, resource_id,
                  s->resources[resource_id].available_instances);

    wake_first_resource_waiter(s, resource_id);
    sim_detect_deadlock(s);
    return true;
}

static void build_wait_for_graph(const sim_t *s, sim_pid_t waits_for[]) {
    for (int i = 0; i < s->num_processes; i++) {
        waits_for[i] = NO_PROCESS;

        const pcb_t *p = &s->procs[i];
        if (p->state != PROC_BLOCKED_RESOURCE || p->waiting_resource < 0) continue;

        sim_pid_t holder = s->resources[p->waiting_resource].owner_pid;
        if (holder >= 0 && holder != i) waits_for[i] = holder;
    }
}

static void record_deadlock(sim_t *s, sim_pid_t start, const sim_pid_t cycle[], int len) {
    s->deadlock_detected = true;
    s->deadlock_cycle_len = len;
    for (int k = 0; k < len; k++) {
        s->deadlock_cycle[k] = cycle[k];
    }
    s->total_deadlocks_detected++;
    sim_log_event(s, EVENT_DEADLOCK_DETECTED, start, NO_CORE, len, 0);
}

bool sim_detect_deadlock(sim_t *s) {
    s->deadlock_detected = false;
    s->deadlock_cycle_len = 0;

    sim_pid_t waits_for[PRISM_MAX_PROCESSES];
    build_wait_for_graph(s, waits_for);

    for (sim_pid_t start = 0; start < s->num_processes; start++) {
        if (waits_for[start] == NO_PROCESS) continue;

        bool visited[PRISM_MAX_PROCESSES] = {false};
        sim_pid_t path[PRISM_MAX_PROCESSES];
        int path_len = 0;

        for (sim_pid_t curr = start; curr != NO_PROCESS && !visited[curr]; curr = waits_for[curr]) {
            visited[curr] = true;
            path[path_len++] = curr;
            if (waits_for[curr] == start) {
                record_deadlock(s, start, path, path_len);
                return true;
            }
        }
    }
    return false;
}

void sim_release_all_resources(sim_t *s, sim_pid_t pid) {
    for (int r = 0; r < s->num_resources; r++) {
        if (holds_resource(&s->procs[pid], r)) sim_release_resource(s, pid, r);
    }
}

static sim_pid_t deadlock_victim(const sim_t *s) {
    return s->deadlock_cycle[s->deadlock_cycle_len - 1];
}

static bool has_deadlock(const sim_t *s) {
    return s->deadlock_detected && s->deadlock_cycle_len > 0;
}

bool sim_recover_deadlock_terminate(sim_t *s) {
    if (!has_deadlock(s)) return false;

    sim_pid_t victim = deadlock_victim(s);
    sim_release_all_resources(s, victim);
    sim_inject_kill(s, victim);

    s->total_deadlocks_recovered++;
    sim_log_event(s, EVENT_DEADLOCK_RESOLVED, victim, NO_CORE, 0, 0);
    sim_detect_deadlock(s);
    return true;
}

static int find_checkpoint_slot(const sim_t *s, sim_pid_t pid) {
    for (int i = 0; i < PRISM_MAX_CHECKPOINTS; i++) {
        if (s->checkpoints[i].valid && s->checkpoints[i].pid == pid) return i;
    }
    return -1;
}

bool sim_recover_deadlock_rollback(sim_t *s) {
    if (!has_deadlock(s)) return false;

    sim_pid_t victim = deadlock_victim(s);
    int slot = find_checkpoint_slot(s, victim);
    if (slot < 0) return sim_recover_deadlock_terminate(s);

    sim_release_all_resources(s, victim);
    sim_checkpoint_restore(s, victim, s->checkpoints[slot].checkpoint_id);

    s->total_deadlocks_recovered++;
    sim_log_event(s, EVENT_DEADLOCK_RESOLVED, victim, NO_CORE, 1, 0);
    sim_detect_deadlock(s);
    return true;
}

static void wake_ipc_receiver(sim_t *s, sim_pid_t sender, sim_pid_t receiver) {
    if (!is_valid_pid(s, receiver)) return;

    pcb_t *rp = &s->procs[receiver];
    bool waiting_for_sender = rp->waiting_ipc_pid == sender || rp->waiting_ipc_pid == NO_PROCESS;
    if (rp->state != PROC_BLOCKED_IPC || !waiting_for_sender) return;

    rp->state = PROC_READY;
    rp->waiting_ipc_pid = NO_PROCESS;
    rp->curr_burst_idx++;
    notify_ready(s, receiver);
}

bool sim_ipc_send(sim_t *s, sim_pid_t sender, sim_pid_t receiver, int16_t msg_id, int16_t data) {
    if (s->ipc_msg_count >= PRISM_MAX_MSG_QUEUE) return false;

    ipc_message_t *msg = &s->ipc_messages[s->ipc_msg_count++];
    msg->sender_pid = sender;
    msg->receiver_pid = receiver;
    msg->message_id = msg_id;
    msg->data = data;
    msg->timestamp = s->tick;

    sim_log_event(s, EVENT_IPC_SEND, sender, NO_CORE, receiver, data);
    wake_ipc_receiver(s, sender, receiver);
    return true;
}

static void remove_message(sim_t *s, int index) {
    for (int j = index; j < s->ipc_msg_count - 1; j++) {
        s->ipc_messages[j] = s->ipc_messages[j + 1];
    }
    s->ipc_msg_count--;
}

bool sim_ipc_recv(sim_t *s, sim_pid_t receiver, sim_pid_t expected_sender,
                  int16_t *msg_id_out, int16_t *data_out) {
    for (int i = 0; i < s->ipc_msg_count; i++) {
        bool for_receiver = s->ipc_messages[i].receiver_pid == receiver;
        bool from_sender = expected_sender == NO_PROCESS ||
                           s->ipc_messages[i].sender_pid == expected_sender;
        if (!for_receiver || !from_sender) continue;

        if (msg_id_out) *msg_id_out = s->ipc_messages[i].message_id;
        if (data_out) *data_out = s->ipc_messages[i].data;
        sim_log_event(s, EVENT_IPC_RECV, receiver, NO_CORE,
                      s->ipc_messages[i].sender_pid, s->ipc_messages[i].data);
        remove_message(s, i);
        return true;
    }
    return false;
}

static int free_checkpoint_slot(const sim_t *s) {
    for (int i = 0; i < PRISM_MAX_CHECKPOINTS; i++) {
        if (!s->checkpoints[i].valid) return i;
    }
    return 0;
}

uint32_t sim_checkpoint_create(sim_t *s, sim_pid_t pid) {
    if (!is_valid_pid(s, pid)) return 0;

    int slot = free_checkpoint_slot(s);
    uint32_t checkpoint_id = ++s->total_checkpoints;

    s->checkpoints[slot].valid = true;
    s->checkpoints[slot].pid = pid;
    s->checkpoints[slot].checkpoint_id = checkpoint_id;
    s->checkpoints[slot].saved_tick = s->tick;
    s->checkpoints[slot].saved_pcb = s->procs[pid];
    s->procs[pid].num_checkpoints++;

    sim_log_event(s, EVENT_CHECKPOINT_SAVED, pid, NO_CORE, (int32_t)checkpoint_id, 0);
    return checkpoint_id;
}

static void restore_progress(pcb_t *target, const pcb_t *saved) {
    target->curr_burst_idx = saved->curr_burst_idx;
    target->curr_page_ref_idx = saved->curr_page_ref_idx;
    for (int b = 0; b < target->num_bursts; b++) {
        target->bursts[b].remaining = saved->bursts[b].remaining;
    }
    target->state = PROC_READY;
    target->waiting_resource = -1;
    target->waiting_ipc_pid = NO_PROCESS;
    target->assigned_core = NO_CORE;
}

bool sim_checkpoint_restore(sim_t *s, sim_pid_t pid, uint32_t checkpoint_id) {
    if (!is_valid_pid(s, pid)) return false;

    for (int i = 0; i < PRISM_MAX_CHECKPOINTS; i++) {
        bool matches = s->checkpoints[i].valid && s->checkpoints[i].pid == pid &&
                       (checkpoint_id == 0 || s->checkpoints[i].checkpoint_id == checkpoint_id);
        if (!matches) continue;

        restore_progress(&s->procs[pid], &s->checkpoints[i].saved_pcb);
        notify_ready(s, pid);
        sim_log_event(s, EVENT_CHECKPOINT_RESTORED, pid, NO_CORE,
                      (int32_t)s->checkpoints[i].checkpoint_id, 0);
        return true;
    }
    return false;
}
