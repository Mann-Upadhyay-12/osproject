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
        default:                       return "UNKNOWN";
    }
}

const char *proc_state_str(proc_state_t st) {
    switch (st) {
        case PROC_NEW:            return "NEW";
        case PROC_READY:          return "READY";
        case PROC_RUNNING:        return "RUNNING";
        case PROC_BLOCKED_IO:     return "BLOCK_IO";
        case PROC_BLOCKED_FAULT:  return "BLOCK_VM";
        case PROC_TERMINATED:     return "TERM";
        default:                  return "UNUSED";
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
