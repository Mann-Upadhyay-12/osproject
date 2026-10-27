#include "prism/sim.h"
#include "prism/hash.h"
#include "prism/prng.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

void sim_init(sim_t *s, int num_cores, int num_frames, int disk_latency,
              sched_policy_t sched, repl_policy_t repl, uint64_t seed) {
    memset(s, 0, sizeof(*s));

    s->tick = 0;
    s->seed = seed;
    s->prng_state = seed ? seed : 0x123456789abcdefULL;
    s->num_cores = (num_cores > 0 && num_cores <= PRISM_MAX_CORES) ? num_cores : 1;
    s->total_frames = (num_frames > 0 && num_frames <= PRISM_MAX_FRAMES) ? num_frames : 4;
    s->disk_latency = (disk_latency > 0) ? disk_latency : 5;
    s->sched_policy = sched;
    s->repl_policy = repl;
    s->default_quantum = 2;
    s->log_hash = fnv1a_init();

    for (int i = 0; i < PRISM_MAX_CORES; i++) {
        s->running_pid[i] = -1;
    }

    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        s->ready_head[q] = -1;
        s->ready_tail[q] = -1;
        s->ready_count[q] = 0;
    }

    for (int i = 0; i < PRISM_MAX_FRAMES; i++) {
        s->frames[i].occupied = false;
        s->frames[i].owner_pid = -1;
    }

    for (int r = 0; r < PRISM_MAX_RESOURCES; r++) {
        s->resources[r].owner_pid = -1;
        s->resources[r].available_instances = s->resources[r].total_instances;
        s->resources[r].allocated_mask = 0;
    }
    s->ipc_msg_count = 0;
    s->deadlock_detected = false;
    s->deadlock_cycle_len = 0;
    s->total_checkpoints = 0;

    const sched_ops_t *sops = sched_get_ops(s->sched_policy);
    if (sops && sops->init) sops->init(s);
    const repl_ops_t *rops = repl_get_ops(s->repl_policy);
    if (rops && rops->init) rops->init(s);
}

void sim_set_policy(sim_t *s, sched_policy_t sched, repl_policy_t repl) {
    s->sched_policy = sched;
    s->repl_policy = repl;
    const sched_ops_t *sops = sched_get_ops(s->sched_policy);
    if (sops && sops->init) sops->init(s);
    const repl_ops_t *rops = repl_get_ops(s->repl_policy);
    if (rops && rops->init) rops->init(s);
}

sim_pid_t sim_add_process(sim_t *s, const char *name, tick_t arrival_tick) {
    if (s->num_processes >= PRISM_MAX_PROCESSES) return -1;

    sim_pid_t pid = s->num_processes++;
    pcb_t *p = &s->procs[pid];
    memset(p, 0, sizeof(*p));

    p->pid = pid;
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "proc");
    p->state = PROC_NEW;
    p->arrival_tick = arrival_tick;
    p->start_tick = -1;
    p->finish_tick = -1;
    p->priority = 0;
    p->quantum_remaining = s->default_quantum;
    p->assigned_core = -1;
    p->waiting_resource = -1;
    p->waiting_ipc_pid = -1;
    p->next_proc = -1;

    for (int i = 0; i < PRISM_MAX_VIRTUAL_PAGES; i++) {
        p->pt[i].valid = false;
        p->pt[i].frame_idx = -1;
    }

    return pid;
}

bool sim_add_burst(sim_t *s, sim_pid_t pid, burst_kind_t kind, uint16_t duration) {
    if (pid < 0 || pid >= s->num_processes) return false;
    pcb_t *p = &s->procs[pid];
    if (p->num_bursts >= PRISM_MAX_BURSTS) return false;

    p->bursts[p->num_bursts].kind = (uint8_t)kind;
    p->bursts[p->num_bursts].duration = duration;
    p->bursts[p->num_bursts].remaining = duration;
    p->num_bursts++;
    return true;
}

bool sim_add_page_ref(sim_t *s, sim_pid_t pid, uint16_t vpage) {
    if (pid < 0 || pid >= s->num_processes) return false;
    pcb_t *p = &s->procs[pid];
    if (p->num_page_refs >= PRISM_MAX_PAGE_REFS) return false;

    p->page_refs[p->num_page_refs++] = vpage;
    return true;
}

/* Flat index-based queue operations */
void sim_queue_push(sim_t *s, int queue_idx, sim_pid_t pid) {
    if (queue_idx < 0 || queue_idx >= PRISM_MLFQ_QUEUES) return;
    if (pid < 0 || pid >= s->num_processes) return;

    pcb_t *p = &s->procs[pid];
    p->next_proc = -1;

    if (s->ready_head[queue_idx] == -1) {
        s->ready_head[queue_idx] = (int16_t)pid;
        s->ready_tail[queue_idx] = (int16_t)pid;
    } else {
        s->procs[s->ready_tail[queue_idx]].next_proc = (int16_t)pid;
        s->ready_tail[queue_idx] = (int16_t)pid;
    }
    s->ready_count[queue_idx]++;
}

sim_pid_t sim_queue_pop(sim_t *s, int queue_idx) {
    if (queue_idx < 0 || queue_idx >= PRISM_MLFQ_QUEUES) return -1;
    if (s->ready_head[queue_idx] == -1) return -1;

    sim_pid_t pid = s->ready_head[queue_idx];
    pcb_t *p = &s->procs[pid];
    s->ready_head[queue_idx] = p->next_proc;
    if (s->ready_head[queue_idx] == -1) {
        s->ready_tail[queue_idx] = -1;
    }
    p->next_proc = -1;
    s->ready_count[queue_idx]--;
    return pid;
}

bool sim_queue_remove(sim_t *s, int queue_idx, sim_pid_t pid) {
    if (queue_idx < 0 || queue_idx >= PRISM_MLFQ_QUEUES) return false;
    if (s->ready_head[queue_idx] == -1) return false;

    sim_pid_t curr = s->ready_head[queue_idx];
    sim_pid_t prev = -1;

    while (curr != -1) {
        if (curr == pid) {
            if (prev == -1) {
                s->ready_head[queue_idx] = s->procs[curr].next_proc;
                if (s->ready_head[queue_idx] == -1) {
                    s->ready_tail[queue_idx] = -1;
                }
            } else {
                s->procs[prev].next_proc = s->procs[curr].next_proc;
                if (s->ready_tail[queue_idx] == curr) {
                    s->ready_tail[queue_idx] = prev;
                }
            }
            s->procs[curr].next_proc = -1;
            s->ready_count[queue_idx]--;
            return true;
        }
        prev = curr;
        curr = s->procs[curr].next_proc;
    }
    return false;
}

void sim_log_event(sim_t *s, event_type_t type, sim_pid_t pid, int core, int d1, int d2) {
    sim_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.tick = s->tick;
    ev.type = type;
    ev.pid = pid;
    ev.core_id = core;
    ev.detail1 = d1;
    ev.detail2 = d2;

    if (s->event_count < PRISM_MAX_EVENT_LOG) {
        s->event_log[s->event_count++] = ev;
    }
    s->log_hash = fnv1a_update(s->log_hash, &ev, sizeof(ev));
}

bool sim_is_done(const sim_t *s) {
    if (s->num_processes == 0) return true;
    return s->completed_processes >= (uint32_t)s->num_processes;
}

const char *event_type_str(event_type_t type) {
    switch (type) {
        case EVENT_PROC_ARRIVE:        return "ARRIVE";
        case EVENT_PROC_DISPATCH:      return "DISPATCH";
        case EVENT_PROC_PREEMPT:       return "PREEMPT";
        case EVENT_PROC_BLOCK_IO:      return "BLOCK_IO";
        case EVENT_PROC_BLOCK_FAULT:   return "BLOCK_FAULT";
        case EVENT_PROC_WAKE_IO:       return "WAKE_IO";
        case EVENT_PROC_WAKE_FAULT:    return "WAKE_FAULT";
        case EVENT_PROC_TERMINATE:     return "TERMINATE";
        case EVENT_PAGE_ACCESS:        return "PAGE_ACCESS";
        case EVENT_PAGE_FAULT:         return "PAGE_FAULT";
        case EVENT_PAGE_EVICT:         return "PAGE_EVICT";
        case EVENT_RAM_SHRUNK:         return "RAM_SHRUNK";
        case EVENT_PROC_KILLED:        return "PROC_KILLED";
        case EVENT_RESOURCE_REQUEST:   return "RES_REQUEST";
        case EVENT_RESOURCE_ACQUIRED:  return "RES_ACQUIRED";
        case EVENT_RESOURCE_RELEASED:  return "RES_RELEASED";
        case EVENT_RESOURCE_BLOCKED:   return "RES_BLOCKED";
        case EVENT_DEADLOCK_DETECTED:  return "DEADLOCK_FOUND";
        case EVENT_DEADLOCK_RESOLVED:  return "DEADLOCK_RESOLVED";
        case EVENT_IPC_SEND:           return "IPC_SEND";
        case EVENT_IPC_RECV:           return "IPC_RECV";
        case EVENT_IPC_BLOCKED:        return "IPC_BLOCKED";
        case EVENT_CHECKPOINT_SAVED:    return "CKPT_SAVED";
        case EVENT_CHECKPOINT_RESTORED: return "CKPT_RESTORED";
        case EVENT_FAULT_INJECTED:     return "FAULT_INJECTED";
        default:                       return "UNKNOWN";
    }
}

const char *proc_state_str(proc_state_t st) {
    switch (st) {
        case PROC_NEW:              return "NEW";
        case PROC_READY:            return "READY";
        case PROC_RUNNING:          return "RUNNING";
        case PROC_BLOCKED_IO:       return "BLOCK_IO";
        case PROC_BLOCKED_FAULT:    return "BLOCK_VM";
        case PROC_BLOCKED_RESOURCE: return "BLOCK_RES";
        case PROC_BLOCKED_IPC:      return "BLOCK_IPC";
        case PROC_TERMINATED:       return "TERM";
        default:                    return "UNUSED";
    }
}

/* Service page allocation & eviction helper */
static void sim_service_page_fault(sim_t *s, sim_pid_t pid) {
    pcb_t *p = &s->procs[pid];
    uint16_t vpage = p->fault_vpage;
    const repl_ops_t *rops = repl_get_ops(s->repl_policy);

    /* 1. Try to find a free frame */
    int victim = -1;
    for (int i = 0; i < s->total_frames; i++) {
        if (!s->frames[i].occupied) {
            victim = i;
            break;
        }
    }

    /* 2. Evict if no free frame */
    if (victim == -1) {
        victim = rops->pick_victim(s);
        assert(victim >= 0 && victim < s->total_frames);
        frame_entry_t *vf = &s->frames[victim];
        if (vf->occupied && vf->owner_pid >= 0) {
            pcb_t *old_owner = &s->procs[vf->owner_pid];
            old_owner->pt[vf->vpage].valid = false;
            old_owner->pt[vf->vpage].frame_idx = -1;
            sim_log_event(s, EVENT_PAGE_EVICT, vf->owner_pid, -1, vf->vpage, victim);
        }
    }

    /* 3. Assign frame to faulting process */
    frame_entry_t *f = &s->frames[victim];
    f->occupied = true;
    f->owner_pid = pid;
    f->vpage = vpage;
    f->load_tick = s->tick;
    f->last_access_tick = s->tick;
    f->ref_bit = true;
    f->dirty_bit = false;

    p->pt[vpage].valid = true;
    p->pt[vpage].frame_idx = (int16_t)victim;
    p->pt[vpage].load_tick = s->tick;
    p->pt[vpage].last_access_tick = s->tick;
    p->pt[vpage].referenced = true;

    if (rops->on_load) {
        rops->on_load(s, pid, vpage, victim);
    }
}

bool sim_step(sim_t *s) {
    if (sim_is_done(s)) return false;

    const sched_ops_t *sops = sched_get_ops(s->sched_policy);
    const repl_ops_t  *rops = repl_get_ops(s->repl_policy);

    /* Phase 1: Process Arrivals */
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state == PROC_NEW && p->arrival_tick <= s->tick) {
            p->state = PROC_READY;
            sim_log_event(s, EVENT_PROC_ARRIVE, p->pid, -1, 0, 0);
            sops->on_ready(s, p->pid);
        }
    }

    /* Phase 2: Service I/O Blocked Processes */
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state == PROC_BLOCKED_IO) {
            p->total_io_time++;
            if (p->io_remaining > 0) {
                p->io_remaining--;
            }
            if (p->io_remaining == 0) {
                p->curr_burst_idx++;
                if (p->curr_burst_idx >= p->num_bursts) {
                    p->state = PROC_TERMINATED;
                    p->finish_tick = (int64_t)s->tick;
                    s->completed_processes++;
                    sim_log_event(s, EVENT_PROC_TERMINATE, p->pid, -1, 0, 0);
                } else {
                    p->state = PROC_READY;
                    sim_log_event(s, EVENT_PROC_WAKE_IO, p->pid, -1, 0, 0);
                    sops->on_ready(s, p->pid);
                }
            }
        }
    }

    /* Phase 3: Service Page Fault Blocked Processes */
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state == PROC_BLOCKED_FAULT) {
            p->total_fault_time++;
            if (p->fault_remaining > 0) {
                p->fault_remaining--;
            }
            if (p->fault_remaining == 0) {
                sim_service_page_fault(s, p->pid);
                p->state = PROC_READY;
                sim_log_event(s, EVENT_PROC_WAKE_FAULT, p->pid, -1, p->fault_vpage, 0);
                sops->on_ready(s, p->pid);
            }
        }
    }

    /* Phase 4: Core Dispatch */
    for (int c = 0; c < s->num_cores; c++) {
        /* If core is idle, try dispatching */
        if (s->running_pid[c] == -1) {
            sim_pid_t next_pid = sops->pick_next(s, c);
            if (next_pid != -1) {
                pcb_t *p = &s->procs[next_pid];
                assert(p->state == PROC_READY);
                p->state = PROC_RUNNING;
                p->assigned_core = (int16_t)c;
                s->running_pid[c] = next_pid;
                p->num_context_switches++;
                s->total_context_switches++;
                if (p->start_tick < 0) {
                    p->start_tick = (int64_t)s->tick;
                }
                sim_log_event(s, EVENT_PROC_DISPATCH, p->pid, c, 0, 0);
            }
        }
    }

    /* Phase 5: Wait time accounting for remaining READY processes */
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state == PROC_READY) {
            p->total_wait_time++;
        }
    }

    /* Phase 6: Core Execution */
    for (int c = 0; c < s->num_cores; c++) {

        /* If core is running a process */
        sim_pid_t curr_pid = s->running_pid[c];
        if (curr_pid != -1) {
            pcb_t *p = &s->procs[curr_pid];
            p->total_cpu_time++;
            s->core_busy_ticks[c]++;

            /* Record in timeline history */
            s->gantt_history[s->gantt_count % PRISM_GANTT_HISTORY][c] = curr_pid;

            /* Check virtual memory access for this CPU tick */
            bool faulted = false;
            if (p->curr_page_ref_idx < p->num_page_refs) {
                uint16_t vpage = p->page_refs[p->curr_page_ref_idx];
                s->total_page_accesses++;

                if (!p->pt[vpage].valid) {
                    /* Page Fault! */
                    faulted = true;
                    p->num_page_faults++;
                    s->total_page_faults++;
                    p->state = PROC_BLOCKED_FAULT;
                    p->fault_remaining = (uint16_t)s->disk_latency;
                    p->fault_vpage = vpage;
                    p->assigned_core = -1;
                    s->running_pid[c] = -1;

                    sim_log_event(s, EVENT_PAGE_FAULT, curr_pid, c, vpage, 0);
                    sim_log_event(s, EVENT_PROC_BLOCK_FAULT, curr_pid, c, vpage, 0);
                    sops->on_block(s, curr_pid);
                } else {
                    /* Page Hit */
                    int f = p->pt[vpage].frame_idx;
                    assert(f >= 0 && f < s->total_frames);
                    s->frames[f].last_access_tick = s->tick;
                    s->frames[f].ref_bit = true;
                    p->pt[vpage].last_access_tick = s->tick;
                    p->pt[vpage].referenced = true;
                    if (rops->on_access) {
                        rops->on_access(s, curr_pid, vpage, f);
                    }
                    sim_log_event(s, EVENT_PAGE_ACCESS, curr_pid, c, vpage, f);
                    p->curr_page_ref_idx++;
                }
            }

            if (!faulted) {
                /* Consume CPU burst tick */
                assert(p->bursts[p->curr_burst_idx].remaining > 0);
                p->bursts[p->curr_burst_idx].remaining--;

                /* Check burst completion */
                if (p->bursts[p->curr_burst_idx].remaining == 0) {
                    p->curr_burst_idx++;
                    if (p->curr_burst_idx >= p->num_bursts) {
                        /* Process finished execution */
                        p->state = PROC_TERMINATED;
                        p->finish_tick = (int64_t)(s->tick + 1);
                        p->assigned_core = -1;
                        s->running_pid[c] = -1;
                        s->completed_processes++;

                        /* Release frames */
                        for (int f = 0; f < s->total_frames; f++) {
                            if (s->frames[f].owner_pid == curr_pid) {
                                s->frames[f].occupied = false;
                                s->frames[f].owner_pid = -1;
                            }
                        }
                        sim_log_event(s, EVENT_PROC_TERMINATE, curr_pid, c, 0, 0);
                    } else if (p->bursts[p->curr_burst_idx].kind == BURST_IO) {
                        /* Next burst is I/O */
                        p->state = PROC_BLOCKED_IO;
                        p->io_remaining = p->bursts[p->curr_burst_idx].duration;
                        p->assigned_core = -1;
                        s->running_pid[c] = -1;
                        sim_log_event(s, EVENT_PROC_BLOCK_IO, curr_pid, c, p->io_remaining, 0);
                        sops->on_block(s, curr_pid);
                    } else if (p->bursts[p->curr_burst_idx].kind == BURST_ACQUIRE) {
                        int32_t rid = p->bursts[p->curr_burst_idx].target_id;
                        if (!sim_acquire_resource(s, curr_pid, rid)) {
                            /* Resource unavailable -> process blocks */
                            p->assigned_core = -1;
                            s->running_pid[c] = -1;
                            sops->on_block(s, curr_pid);
                        } else {
                            /* Immediately acquired; advance past this burst */
                            p->curr_burst_idx++;
                        }
                    } else if (p->bursts[p->curr_burst_idx].kind == BURST_RELEASE) {
                        int32_t rid = p->bursts[p->curr_burst_idx].target_id;
                        sim_release_resource(s, curr_pid, rid);
                        p->curr_burst_idx++;
                    } else if (p->bursts[p->curr_burst_idx].kind == BURST_IPC_SEND) {
                        sim_pid_t target = (sim_pid_t)p->bursts[p->curr_burst_idx].target_id;
                        int16_t data = p->bursts[p->curr_burst_idx].extra_data;
                        sim_ipc_send(s, curr_pid, target, 1, data);
                        p->curr_burst_idx++;
                    } else if (p->bursts[p->curr_burst_idx].kind == BURST_IPC_RECV) {
                        sim_pid_t target = (sim_pid_t)p->bursts[p->curr_burst_idx].target_id;
                        int16_t mid, mdata;
                        if (!sim_ipc_recv(s, curr_pid, target, &mid, &mdata)) {
                            /* No message available -> blocked on IPC */
                            p->state = PROC_BLOCKED_IPC;
                            p->waiting_ipc_pid = target;
                            p->assigned_core = -1;
                            s->running_pid[c] = -1;
                            sim_log_event(s, EVENT_IPC_BLOCKED, curr_pid, c, target, 0);
                            sops->on_block(s, curr_pid);
                        } else {
                            p->curr_burst_idx++;
                        }
                    } else if (p->bursts[p->curr_burst_idx].kind == BURST_CHECKPOINT) {
                        sim_checkpoint_create(s, curr_pid);
                        p->curr_burst_idx++;
                    }
                }

                /* If still running on core, invoke scheduler on_tick */
                if (s->running_pid[c] == curr_pid) {
                    sops->on_tick(s, c, curr_pid);
                }
            }
        } else {
            /* Core was idle */
            s->gantt_history[s->gantt_count % PRISM_GANTT_HISTORY][c] = -1;
        }
    }

    s->tick++;
    s->gantt_count++;
    return !sim_is_done(s);
}

void sim_run(sim_t *s) {
    while (sim_step(s)) {
        /* Loop until simulation finishes */
    }
}

bool sim_inject_kill(sim_t *s, sim_pid_t pid) {
    if (pid < 0 || pid >= s->num_processes) return false;
    pcb_t *p = &s->procs[pid];
    if (p->state == PROC_TERMINATED || p->state == PROC_UNUSED) return false;

    if (p->assigned_core >= 0) {
        s->running_pid[p->assigned_core] = -1;
        p->assigned_core = -1;
    }

    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        sim_queue_remove(s, q, pid);
    }

    p->state = PROC_TERMINATED;
    p->finish_tick = (int64_t)s->tick;
    s->completed_processes++;

    /* Release frames */
    for (int f = 0; f < s->total_frames; f++) {
        if (s->frames[f].owner_pid == pid) {
            s->frames[f].occupied = false;
            s->frames[f].owner_pid = -1;
        }
    }

    sim_log_event(s, EVENT_PROC_KILLED, pid, -1, 0, 0);
    return true;
}

bool sim_inject_shrink_ram(sim_t *s, int new_frames) {
    if (new_frames < 1 || new_frames >= s->total_frames) return false;

    /* Evict frames beyond new_frames */
    for (int f = new_frames; f < s->total_frames; f++) {
        if (s->frames[f].occupied && s->frames[f].owner_pid >= 0) {
            pcb_t *p = &s->procs[s->frames[f].owner_pid];
            p->pt[s->frames[f].vpage].valid = false;
            p->pt[s->frames[f].vpage].frame_idx = -1;
            sim_log_event(s, EVENT_PAGE_EVICT, s->frames[f].owner_pid, -1, s->frames[f].vpage, f);
            s->frames[f].occupied = false;
            s->frames[f].owner_pid = -1;
        }
    }

    s->total_frames = new_frames;
    sim_log_event(s, EVENT_RAM_SHRUNK, -1, -1, new_frames, 0);
    return true;
}

/* =========================================================================
 * Resource Manager Subsystem
 * ========================================================================= */
int32_t sim_add_resource(sim_t *s, const char *name, int32_t instances) {
    if (s->num_resources >= PRISM_MAX_RESOURCES) return -1;
    int32_t rid = s->num_resources++;
    snprintf(s->resources[rid].name, sizeof(s->resources[rid].name), "%s", name ? name : "res");
    s->resources[rid].total_instances = (instances > 0) ? instances : 1;
    s->resources[rid].available_instances = s->resources[rid].total_instances;
    s->resources[rid].owner_pid = -1;
    s->resources[rid].allocated_mask = 0;
    return rid;
}

bool sim_acquire_resource(sim_t *s, sim_pid_t pid, int32_t resource_id) {
    if (pid < 0 || pid >= s->num_processes || resource_id < 0 || resource_id >= s->num_resources) {
        return false;
    }
    pcb_t *p = &s->procs[pid];
    sim_log_event(s, EVENT_RESOURCE_REQUEST, pid, p->assigned_core, resource_id, 0);

    if (s->resources[resource_id].available_instances > 0) {
        s->resources[resource_id].available_instances--;
        s->resources[resource_id].owner_pid = pid;
        s->resources[resource_id].allocated_mask |= (1ULL << pid);
        p->held_resources_mask |= (1 << resource_id);
        p->waiting_resource = -1;
        sim_log_event(s, EVENT_RESOURCE_ACQUIRED, pid, p->assigned_core, resource_id, s->resources[resource_id].available_instances);
        return true;
    }

    /* Resource busy -> process blocks */
    p->state = PROC_BLOCKED_RESOURCE;
    p->waiting_resource = (int16_t)resource_id;
    sim_log_event(s, EVENT_RESOURCE_BLOCKED, pid, p->assigned_core, resource_id, s->resources[resource_id].owner_pid);

    /* Run deadlock detection whenever a process blocks on a resource */
    sim_detect_deadlock(s);
    return false;
}

bool sim_release_resource(sim_t *s, sim_pid_t pid, int32_t resource_id) {
    if (pid < 0 || pid >= s->num_processes || resource_id < 0 || resource_id >= s->num_resources) {
        return false;
    }
    pcb_t *p = &s->procs[pid];
    if (!(p->held_resources_mask & (1 << resource_id))) {
        return false;
    }

    p->held_resources_mask &= ~(1 << resource_id);
    s->resources[resource_id].allocated_mask &= ~(1ULL << pid);
    s->resources[resource_id].available_instances++;
    if (s->resources[resource_id].allocated_mask == 0) {
        s->resources[resource_id].owner_pid = -1;
    }
    sim_log_event(s, EVENT_RESOURCE_RELEASED, pid, p->assigned_core, resource_id, s->resources[resource_id].available_instances);

    /* Check if any process blocked on this resource can now wake up */
    const sched_ops_t *sops = sched_get_ops(s->sched_policy);
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *wp = &s->procs[i];
        if (wp->state == PROC_BLOCKED_RESOURCE && wp->waiting_resource == resource_id) {
            if (s->resources[resource_id].available_instances > 0) {
                s->resources[resource_id].available_instances--;
                s->resources[resource_id].owner_pid = wp->pid;
                s->resources[resource_id].allocated_mask |= (1ULL << wp->pid);
                wp->held_resources_mask |= (1 << resource_id);
                wp->waiting_resource = -1;
                wp->state = PROC_READY;
                wp->curr_burst_idx++; /* advance past the ACQUIRE burst */
                sim_log_event(s, EVENT_RESOURCE_ACQUIRED, wp->pid, -1, resource_id, s->resources[resource_id].available_instances);
                sops->on_ready(s, wp->pid);
                break;
            }
        }
    }

    /* Re-evaluate deadlock status */
    sim_detect_deadlock(s);
    return true;
}

/* =========================================================================
 * Deadlock Engine (Wait-For Graph Cycle Detection & Recovery)
 * ========================================================================= */
bool sim_detect_deadlock(sim_t *s) {
    s->deadlock_detected = false;
    s->deadlock_cycle_len = 0;

    /*
     * Build Wait-For Graph:
     * Process P waits for resource R.
     * Resource R is held by process Q.
     * Edge: P -> Q.
     */
    int wfg[PRISM_MAX_PROCESSES];
    for (int i = 0; i < s->num_processes; i++) {
        wfg[i] = -1;
        if (s->procs[i].state == PROC_BLOCKED_RESOURCE && s->procs[i].waiting_resource >= 0) {
            int r = s->procs[i].waiting_resource;
            sim_pid_t holder = s->resources[r].owner_pid;
            if (holder >= 0 && holder != i) {
                wfg[i] = holder;
            }
        }
    }

    /* Detect cycles in the directed graph */
    for (int start = 0; start < s->num_processes; start++) {
        if (wfg[start] == -1) continue;

        bool visited[PRISM_MAX_PROCESSES] = {false};
        sim_pid_t path[PRISM_MAX_PROCESSES];
        int path_len = 0;

        sim_pid_t curr = start;
        while (curr != -1 && !visited[curr]) {
            visited[curr] = true;
            path[path_len++] = curr;
            curr = wfg[curr];
            if (curr == start) {
                /* Cycle found! */
                s->deadlock_detected = true;
                s->deadlock_cycle_len = path_len;
                for (int k = 0; k < path_len; k++) {
                    s->deadlock_cycle[k] = path[k];
                }
                s->total_deadlocks_detected++;
                sim_log_event(s, EVENT_DEADLOCK_DETECTED, start, -1, path_len, 0);
                return true;
            }
        }
    }

    return false;
}

bool sim_recover_deadlock_terminate(sim_t *s) {
    if (!s->deadlock_detected || s->deadlock_cycle_len == 0) return false;

    /* Select victim: process in cycle with the lowest priority or highest PID */
    sim_pid_t victim = s->deadlock_cycle[s->deadlock_cycle_len - 1];

    /* Release all resources held by victim */
    for (int r = 0; r < s->num_resources; r++) {
        if (s->procs[victim].held_resources_mask & (1 << r)) {
            sim_release_resource(s, victim, r);
        }
    }

    sim_inject_kill(s, victim);
    s->total_deadlocks_recovered++;
    sim_log_event(s, EVENT_DEADLOCK_RESOLVED, victim, -1, 0, 0);

    /* Recheck deadlock */
    sim_detect_deadlock(s);
    return true;
}

bool sim_recover_deadlock_rollback(sim_t *s) {
    if (!s->deadlock_detected || s->deadlock_cycle_len == 0) return false;

    sim_pid_t victim = s->deadlock_cycle[s->deadlock_cycle_len - 1];

    /* Check if victim has a valid saved checkpoint */
    int best_ckpt = -1;
    for (int i = 0; i < PRISM_MAX_CHECKPOINTS; i++) {
        if (s->checkpoints[i].valid && s->checkpoints[i].pid == victim) {
            best_ckpt = i;
            break;
        }
    }

    if (best_ckpt != -1) {
        /* Release victim's currently held resources to break cycle */
        for (int r = 0; r < s->num_resources; r++) {
            if (s->procs[victim].held_resources_mask & (1 << r)) {
                sim_release_resource(s, victim, r);
            }
        }
        /* Restore victim from checkpoint */
        sim_checkpoint_restore(s, victim, s->checkpoints[best_ckpt].checkpoint_id);
        s->total_deadlocks_recovered++;
        sim_log_event(s, EVENT_DEADLOCK_RESOLVED, victim, -1, 1, 0);
        sim_detect_deadlock(s);
        return true;
    }

    /* Fallback to termination if no checkpoint */
    return sim_recover_deadlock_terminate(s);
}

/* =========================================================================
 * IPC Message Queue Subsystem
 * ========================================================================= */
bool sim_ipc_send(sim_t *s, sim_pid_t sender, sim_pid_t receiver, int16_t msg_id, int16_t data) {
    if (s->ipc_msg_count >= PRISM_MAX_MSG_QUEUE) return false;

    int idx = s->ipc_msg_count++;
    s->ipc_messages[idx].sender_pid = sender;
    s->ipc_messages[idx].receiver_pid = receiver;
    s->ipc_messages[idx].message_id = msg_id;
    s->ipc_messages[idx].data = data;
    s->ipc_messages[idx].timestamp = s->tick;

    sim_log_event(s, EVENT_IPC_SEND, sender, -1, receiver, data);

    /* Check if receiver is blocked waiting for this message */
    if (receiver >= 0 && receiver < s->num_processes) {
        pcb_t *rp = &s->procs[receiver];
        if (rp->state == PROC_BLOCKED_IPC && (rp->waiting_ipc_pid == sender || rp->waiting_ipc_pid == -1)) {
            rp->state = PROC_READY;
            rp->waiting_ipc_pid = -1;
            rp->curr_burst_idx++; /* consume the IPC_RECV burst */
            sched_get_ops(s->sched_policy)->on_ready(s, receiver);
        }
    }

    return true;
}

bool sim_ipc_recv(sim_t *s, sim_pid_t receiver, sim_pid_t expected_sender, int16_t *msg_id_out, int16_t *data_out) {
    for (int i = 0; i < s->ipc_msg_count; i++) {
        if (s->ipc_messages[i].receiver_pid == receiver &&
            (expected_sender == -1 || s->ipc_messages[i].sender_pid == expected_sender)) {
            if (msg_id_out) *msg_id_out = s->ipc_messages[i].message_id;
            if (data_out) *data_out = s->ipc_messages[i].data;
            sim_log_event(s, EVENT_IPC_RECV, receiver, -1, s->ipc_messages[i].sender_pid, s->ipc_messages[i].data);

            /* Remove message by shifting */
            for (int j = i; j < s->ipc_msg_count - 1; j++) {
                s->ipc_messages[j] = s->ipc_messages[j + 1];
            }
            s->ipc_msg_count--;
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Checkpoint / Restore API
 * ========================================================================= */
uint32_t sim_checkpoint_create(sim_t *s, sim_pid_t pid) {
    if (pid < 0 || pid >= s->num_processes) return 0;

    int slot = -1;
    for (int i = 0; i < PRISM_MAX_CHECKPOINTS; i++) {
        if (!s->checkpoints[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot == -1) slot = 0; /* overwrite oldest slot */

    uint32_t cid = ++s->total_checkpoints;
    s->checkpoints[slot].valid = true;
    s->checkpoints[slot].pid = pid;
    s->checkpoints[slot].checkpoint_id = cid;
    s->checkpoints[slot].saved_tick = s->tick;
    s->checkpoints[slot].saved_pcb = s->procs[pid];
    s->procs[pid].num_checkpoints++;

    sim_log_event(s, EVENT_CHECKPOINT_SAVED, pid, -1, (int32_t)cid, 0);
    return cid;
}

bool sim_checkpoint_restore(sim_t *s, sim_pid_t pid, uint32_t checkpoint_id) {
    if (pid < 0 || pid >= s->num_processes) return false;

    for (int i = 0; i < PRISM_MAX_CHECKPOINTS; i++) {
        if (s->checkpoints[i].valid && s->checkpoints[i].pid == pid &&
            (checkpoint_id == 0 || s->checkpoints[i].checkpoint_id == checkpoint_id)) {
            pcb_t *target = &s->procs[pid];
            pcb_t saved = s->checkpoints[i].saved_pcb;

            /* Restore process execution counters, bursts, and state to READY */
            target->curr_burst_idx = saved.curr_burst_idx;
            target->curr_page_ref_idx = saved.curr_page_ref_idx;
            for (int b = 0; b < target->num_bursts; b++) {
                target->bursts[b].remaining = saved.bursts[b].remaining;
            }
            target->state = PROC_READY;
            target->waiting_resource = -1;
            target->waiting_ipc_pid = -1;
            target->assigned_core = -1;

            sched_get_ops(s->sched_policy)->on_ready(s, pid);
            sim_log_event(s, EVENT_CHECKPOINT_RESTORED, pid, -1, (int32_t)s->checkpoints[i].checkpoint_id, 0);
            return true;
        }
    }
    return false;
}

bool sim_inject_resource_fail(sim_t *s, int32_t resource_id) {
    if (resource_id < 0 || resource_id >= s->num_resources) return false;
    s->resources[resource_id].available_instances = 0;
    sim_log_event(s, EVENT_FAULT_INJECTED, -1, -1, resource_id, 0);
    return true;
}
