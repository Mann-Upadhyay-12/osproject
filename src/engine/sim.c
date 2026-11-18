#include "prism/sim.h"
#include "prism/hash.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NO_PROCESS (-1)
#define NO_CORE    (-1)
#define NO_FRAME   (-1)

static void init_policies(sim_t *s) {
    const sched_ops_t *sched = sched_get_ops(s->sched_policy);
    if (sched && sched->init) sched->init(s);

    const repl_ops_t *repl = repl_get_ops(s->repl_policy);
    if (repl && repl->init) repl->init(s);
}

void sim_init(sim_t *s, int num_cores, int num_frames, int disk_latency,
              sched_policy_t sched, repl_policy_t repl, uint64_t seed) {
    memset(s, 0, sizeof(*s));

    bool cores_valid  = num_cores > 0 && num_cores <= PRISM_MAX_CORES;
    bool frames_valid = num_frames > 0 && num_frames <= PRISM_MAX_FRAMES;

    s->seed = seed;
    s->prng_state = seed ? seed : 0x123456789abcdefULL;
    s->num_cores = cores_valid ? num_cores : 1;
    s->total_frames = frames_valid ? num_frames : 4;
    s->configured_frames = s->total_frames;
    s->disk_latency = disk_latency > 0 ? disk_latency : 5;
    s->sched_policy = sched;
    s->repl_policy = repl;
    s->default_quantum = 2;
    s->log_hash = fnv1a_init();

    for (int c = 0; c < PRISM_MAX_CORES; c++) {
        s->running_pid[c] = NO_PROCESS;
    }
    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        s->ready_head[q] = NO_PROCESS;
        s->ready_tail[q] = NO_PROCESS;
    }
    for (int f = 0; f < PRISM_MAX_FRAMES; f++) {
        s->frames[f].owner_pid = NO_PROCESS;
    }
    for (int r = 0; r < PRISM_MAX_RESOURCES; r++) {
        s->resources[r].owner_pid = NO_PROCESS;
    }

    init_policies(s);
}

void sim_set_policy(sim_t *s, sched_policy_t sched, repl_policy_t repl) {
    s->sched_policy = sched;
    s->repl_policy = repl;
    init_policies(s);
}

static bool is_valid_pid(const sim_t *s, sim_pid_t pid) {
    return pid >= 0 && pid < s->num_processes;
}

sim_pid_t sim_add_process(sim_t *s, const char *name, tick_t arrival_tick) {
    if (s->num_processes >= PRISM_MAX_PROCESSES) return NO_PROCESS;

    sim_pid_t pid = s->num_processes++;
    pcb_t *p = &s->procs[pid];
    memset(p, 0, sizeof(*p));

    p->pid = pid;
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "proc");
    p->state = PROC_NEW;
    p->arrival_tick = arrival_tick;
    p->start_tick = -1;
    p->finish_tick = -1;
    p->quantum_remaining = s->default_quantum;
    p->assigned_core = NO_CORE;
    p->waiting_resource = -1;
    p->waiting_ipc_pid = NO_PROCESS;
    p->waiting_semaphore = -1;
    p->num_jobs = 1;
    p->next_proc = NO_PROCESS;

    for (int page = 0; page < PRISM_MAX_VIRTUAL_PAGES; page++) {
        p->pt[page].frame_idx = NO_FRAME;
    }
    return pid;
}

bool sim_add_burst(sim_t *s, sim_pid_t pid, burst_kind_t kind, uint16_t duration) {
    if (!is_valid_pid(s, pid)) return false;

    pcb_t *p = &s->procs[pid];
    if (p->num_bursts >= PRISM_MAX_BURSTS) return false;

    burst_t *burst = &p->bursts[p->num_bursts++];
    burst->kind = (uint8_t)kind;
    burst->duration = duration;
    burst->remaining = duration;
    return true;
}

bool sim_add_page_ref(sim_t *s, sim_pid_t pid, uint16_t vpage) {
    if (!is_valid_pid(s, pid)) return false;

    pcb_t *p = &s->procs[pid];
    if (p->num_page_refs >= PRISM_MAX_PAGE_REFS) return false;

    p->page_refs[p->num_page_refs++] = vpage;
    return true;
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
    if (type != EVENT_PAGE_ACCESS) s->notable_events++;
    if (s->observer) s->observer(&ev, s->observer_user);
}

static bool all_processes_finished(const sim_t *s) {
    return s->completed_processes + s->num_removed >= (uint32_t)s->num_processes;
}

static bool can_still_progress(proc_state_t state) {
    switch (state) {
        case PROC_NEW:
        case PROC_READY:
        case PROC_RUNNING:
        case PROC_BLOCKED_IO:
        case PROC_BLOCKED_FAULT:
        case PROC_SLEEPING:
            return true;
        default:
            return false;
    }
}

bool sim_is_stalled(const sim_t *s) {
    if (s->num_processes == 0 || all_processes_finished(s)) return false;
    for (int i = 0; i < s->num_processes; i++) {
        if (can_still_progress(s->procs[i].state)) return false;
    }
    return true;
}

bool sim_is_done(const sim_t *s) {
    if (s->num_processes == 0) return true;
    return all_processes_finished(s) || sim_is_stalled(s);
}

static void free_frame(frame_entry_t *frame) {
    frame->occupied = false;
    frame->owner_pid = NO_PROCESS;
}

void sim_release_frames(sim_t *s, sim_pid_t pid) {
    for (int f = 0; f < s->total_frames; f++) {
        if (s->frames[f].occupied && s->frames[f].owner_pid == pid) {
            free_frame(&s->frames[f]);
        }
    }
}

static int find_free_frame(const sim_t *s) {
    for (int f = 0; f < s->total_frames; f++) {
        if (!s->frames[f].occupied) return f;
    }
    return NO_FRAME;
}

static void evict_frame(sim_t *s, int frame_idx) {
    frame_entry_t *frame = &s->frames[frame_idx];
    if (!frame->occupied || frame->owner_pid < 0) return;

    page_table_entry_t *entry = &s->procs[frame->owner_pid].pt[frame->vpage];
    entry->valid = false;
    entry->frame_idx = NO_FRAME;
    sim_log_event(s, EVENT_PAGE_EVICT, frame->owner_pid, NO_CORE, frame->vpage, frame_idx);
}

static void load_page_into_frame(sim_t *s, sim_pid_t pid, uint16_t vpage, int frame_idx) {
    frame_entry_t *frame = &s->frames[frame_idx];
    frame->occupied = true;
    frame->owner_pid = pid;
    frame->vpage = vpage;
    frame->load_tick = s->tick;
    frame->last_access_tick = s->tick;
    frame->ref_bit = true;
    frame->dirty_bit = false;

    page_table_entry_t *entry = &s->procs[pid].pt[vpage];
    entry->valid = true;
    entry->frame_idx = (int16_t)frame_idx;
    entry->load_tick = s->tick;
    entry->last_access_tick = s->tick;
    entry->referenced = true;
}

static void service_page_fault(sim_t *s, sim_pid_t pid) {
    const repl_ops_t *repl = repl_get_ops(s->repl_policy);
    uint16_t vpage = s->procs[pid].fault_vpage;

    int frame_idx = find_free_frame(s);
    if (frame_idx == NO_FRAME) {
        frame_idx = repl->pick_victim(s);
        assert(frame_idx >= 0 && frame_idx < s->total_frames);
        evict_frame(s, frame_idx);
    }

    load_page_into_frame(s, pid, vpage, frame_idx);
    if (repl->on_load) repl->on_load(s, pid, vpage, frame_idx);
}

static void enter_ready_queue(sim_t *s, pcb_t *p) {
    p->state = PROC_READY;
    sched_get_ops(s->sched_policy)->on_ready(s, p->pid);
}

static void make_ready(sim_t *s, pcb_t *p, event_type_t reason, int detail) {
    sim_log_event(s, reason, p->pid, NO_CORE, detail, 0);
    enter_ready_queue(s, p);
}

static void vacate_core(sim_t *s, pcb_t *p) {
    if (p->assigned_core >= 0) s->running_pid[p->assigned_core] = NO_PROCESS;
    p->assigned_core = NO_CORE;
}

static void terminate(sim_t *s, pcb_t *p, tick_t finish_tick, int core) {
    p->state = PROC_TERMINATED;
    p->finish_tick = (int64_t)finish_tick;
    vacate_core(s, p);
    s->completed_processes++;
    sim_release_frames(s, p->pid);
    sim_release_all_resources(s, p->pid);
    sim_log_event(s, EVENT_PROC_TERMINATE, p->pid, core, 0, 0);
}

static bool has_more_bursts(const pcb_t *p) {
    return p->curr_burst_idx < p->num_bursts;
}

static bool is_periodic(const pcb_t *p) {
    return p->period > 0;
}

static void release_job(sim_t *s, pcb_t *p, tick_t release) {
    p->job_release = release;
    p->job_deadline = release + p->relative_deadline;
    p->job_missed = false;
    sim_log_event(s, EVENT_JOB_RELEASE, p->pid, NO_CORE, p->jobs_done + 1, (int32_t)p->job_deadline);
    enter_ready_queue(s, p);
}

static void admit_arrivals(sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state != PROC_NEW || p->arrival_tick > s->tick) continue;

        if (is_periodic(p)) {
            sim_log_event(s, EVENT_PROC_ARRIVE, p->pid, NO_CORE, 0, 0);
            release_job(s, p, p->arrival_tick);
        } else {
            make_ready(s, p, EVENT_PROC_ARRIVE, 0);
        }
    }
}

static void release_periodic_jobs(sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state == PROC_SLEEPING && p->job_release <= s->tick) release_job(s, p, p->job_release);
    }
}

static bool job_in_progress(const pcb_t *p) {
    return p->state != PROC_NEW && p->state != PROC_SLEEPING && p->state != PROC_TERMINATED && p->state != PROC_UNUSED;
}

static void check_deadlines(sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        bool overdue = is_periodic(p) && job_in_progress(p) && !p->job_missed && s->tick >= p->job_deadline;
        if (!overdue) continue;

        p->job_missed = true;
        p->deadline_misses++;
        s->total_deadline_misses++;
        sim_log_event(s, EVENT_DEADLINE_MISS, p->pid, p->assigned_core, p->jobs_done + 1, (int32_t)p->job_deadline);
    }
}

static void rewind_bursts(pcb_t *p) {
    for (int b = 0; b < p->num_bursts; b++) p->bursts[b].remaining = p->bursts[b].duration;
    p->curr_burst_idx = 0;
    p->curr_page_ref_idx = 0;
}

static void sleep_until_next_job(sim_t *s, pcb_t *p, int core) {
    rewind_bursts(p);
    p->state = PROC_SLEEPING;
    p->job_release += p->period;
    vacate_core(s, p);
    if (core != NO_CORE) sched_get_ops(s->sched_policy)->on_block(s, p->pid);
}

static void finish_job(sim_t *s, pcb_t *p, tick_t finish_tick, int core) {
    if (!is_periodic(p)) {
        terminate(s, p, finish_tick, core);
        return;
    }

    p->jobs_done++;
    int32_t lateness = (int32_t)((int64_t)finish_tick - (int64_t)p->job_deadline);
    sim_log_event(s, EVENT_JOB_COMPLETE, p->pid, core, p->jobs_done, lateness);
    if (p->jobs_done >= p->num_jobs) terminate(s, p, finish_tick, core);
    else sleep_until_next_job(s, p, core);
}

static void advance_io(sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state != PROC_BLOCKED_IO) continue;

        p->total_io_time++;
        if (p->io_remaining > 0) p->io_remaining--;
        if (p->io_remaining > 0) continue;

        p->curr_burst_idx++;
        if (has_more_bursts(p)) {
            make_ready(s, p, EVENT_PROC_WAKE_IO, 0);
        } else {
            finish_job(s, p, s->tick, NO_CORE);
        }
    }
}

static void advance_fault_service(sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        pcb_t *p = &s->procs[i];
        if (p->state != PROC_BLOCKED_FAULT) continue;

        p->total_fault_time++;
        if (p->fault_remaining > 0) p->fault_remaining--;
        if (p->fault_remaining > 0) continue;

        service_page_fault(s, p->pid);
        make_ready(s, p, EVENT_PROC_WAKE_FAULT, p->fault_vpage);
    }
}

static void dispatch_idle_cores(sim_t *s) {
    const sched_ops_t *sched = sched_get_ops(s->sched_policy);

    for (int c = 0; c < s->num_cores; c++) {
        if (s->running_pid[c] != NO_PROCESS) continue;

        sim_pid_t pid = sched->pick_next(s, c);
        if (pid == NO_PROCESS) continue;

        pcb_t *p = &s->procs[pid];
        assert(p->state == PROC_READY);
        p->state = PROC_RUNNING;
        p->assigned_core = (int16_t)c;
        s->running_pid[c] = pid;
        p->num_context_switches++;
        s->total_context_switches++;
        if (p->start_tick < 0) p->start_tick = (int64_t)s->tick;
        sim_log_event(s, EVENT_PROC_DISPATCH, pid, c, 0, 0);
    }
}

static void account_waiting(sim_t *s) {
    for (int i = 0; i < s->num_processes; i++) {
        if (s->procs[i].state == PROC_READY) s->procs[i].total_wait_time++;
    }
}

static void block_on_page_fault(sim_t *s, pcb_t *p, int core, uint16_t vpage) {
    p->num_page_faults++;
    s->total_page_faults++;
    p->state = PROC_BLOCKED_FAULT;
    p->fault_remaining = (uint16_t)s->disk_latency;
    p->fault_vpage = vpage;
    vacate_core(s, p);

    sim_log_event(s, EVENT_PAGE_FAULT, p->pid, core, vpage, 0);
    sim_log_event(s, EVENT_PROC_BLOCK_FAULT, p->pid, core, vpage, 0);
    sched_get_ops(s->sched_policy)->on_block(s, p->pid);
}

static void record_page_hit(sim_t *s, pcb_t *p, int core, uint16_t vpage) {
    page_table_entry_t *entry = &p->pt[vpage];
    int frame_idx = entry->frame_idx;
    assert(frame_idx >= 0 && frame_idx < s->total_frames);

    s->frames[frame_idx].last_access_tick = s->tick;
    s->frames[frame_idx].ref_bit = true;
    entry->last_access_tick = s->tick;
    entry->referenced = true;

    const repl_ops_t *repl = repl_get_ops(s->repl_policy);
    if (repl->on_access) repl->on_access(s, p->pid, vpage, frame_idx);

    sim_log_event(s, EVENT_PAGE_ACCESS, p->pid, core, vpage, frame_idx);
    p->curr_page_ref_idx++;
}

static bool touch_next_page(sim_t *s, pcb_t *p, int core) {
    if (p->curr_page_ref_idx >= p->num_page_refs) return true;

    uint16_t vpage = p->page_refs[p->curr_page_ref_idx];
    s->total_page_accesses++;

    if (!p->pt[vpage].valid) {
        block_on_page_fault(s, p, core, vpage);
        return false;
    }
    record_page_hit(s, p, core, vpage);
    return true;
}

static void block_and_yield(sim_t *s, pcb_t *p) {
    vacate_core(s, p);
    sched_get_ops(s->sched_policy)->on_block(s, p->pid);
}

static void start_io_burst(sim_t *s, pcb_t *p, int core, const burst_t *burst) {
    p->state = PROC_BLOCKED_IO;
    p->io_remaining = burst->duration;
    vacate_core(s, p);
    sim_log_event(s, EVENT_PROC_BLOCK_IO, p->pid, core, p->io_remaining, 0);
    sched_get_ops(s->sched_policy)->on_block(s, p->pid);
}

static void start_ipc_recv(sim_t *s, pcb_t *p, int core, sim_pid_t sender) {
    int16_t msg_id, data;
    if (sim_ipc_recv(s, p->pid, sender, &msg_id, &data)) {
        p->curr_burst_idx++;
        return;
    }
    p->state = PROC_BLOCKED_IPC;
    p->waiting_ipc_pid = sender;
    vacate_core(s, p);
    sim_log_event(s, EVENT_IPC_BLOCKED, p->pid, core, sender, 0);
    sched_get_ops(s->sched_policy)->on_block(s, p->pid);
}

static void start_next_burst(sim_t *s, pcb_t *p, int core) {
    const burst_t *burst = &p->bursts[p->curr_burst_idx];

    switch ((burst_kind_t)burst->kind) {
        case BURST_IO:
            start_io_burst(s, p, core, burst);
            break;
        case BURST_ACQUIRE:
            if (sim_acquire_resource(s, p->pid, burst->target_id)) {
                p->curr_burst_idx++;
            } else {
                block_and_yield(s, p);
            }
            break;
        case BURST_RELEASE:
            sim_release_resource(s, p->pid, burst->target_id);
            p->curr_burst_idx++;
            break;
        case BURST_IPC_SEND:
            sim_ipc_send(s, p->pid, burst->target_id, 1, burst->extra_data);
            p->curr_burst_idx++;
            break;
        case BURST_IPC_RECV:
            start_ipc_recv(s, p, core, burst->target_id);
            break;
        case BURST_CHECKPOINT:
            sim_checkpoint_create(s, p->pid);
            p->curr_burst_idx++;
            break;
        case BURST_SEM_WAIT:
            if (sim_sem_wait(s, p->pid, burst->target_id)) {
                p->curr_burst_idx++;
            } else {
                block_and_yield(s, p);
            }
            break;
        case BURST_SEM_SIGNAL:
            sim_sem_signal(s, p->pid, burst->target_id);
            p->curr_burst_idx++;
            break;
        case BURST_CPU:
            break;
    }
}

static bool at_cpu_burst(const pcb_t *p) {
    return has_more_bursts(p) && p->bursts[p->curr_burst_idx].kind == BURST_CPU;
}

static bool run_control_bursts(sim_t *s, pcb_t *p, int core, tick_t finish_tick) {
    while (has_more_bursts(p) && !at_cpu_burst(p)) {
        start_next_burst(s, p, core);
        if (p->state != PROC_RUNNING) return false;
    }
    if (has_more_bursts(p)) return true;

    finish_job(s, p, finish_tick, core);
    return false;
}

static void run_one_cpu_tick(sim_t *s, pcb_t *p, int core) {
    burst_t *burst = &p->bursts[p->curr_burst_idx];
    assert(burst->remaining > 0);
    burst->remaining--;
    if (burst->remaining > 0) return;

    p->curr_burst_idx++;
    run_control_bursts(s, p, core, s->tick + 1);
}

static void execute_core(sim_t *s, int core) {
    sim_pid_t pid = s->running_pid[core];
    s->gantt_history[s->gantt_count % PRISM_GANTT_HISTORY][core] = pid;
    if (pid == NO_PROCESS) return;

    pcb_t *p = &s->procs[pid];
    if (!at_cpu_burst(p) && !run_control_bursts(s, p, core, s->tick)) return;

    p->total_cpu_time++;
    s->core_busy_ticks[core]++;

    if (!touch_next_page(s, p, core)) return;

    run_one_cpu_tick(s, p, core);

    bool still_on_core = s->running_pid[core] == pid;
    if (still_on_core) sched_get_ops(s->sched_policy)->on_tick(s, core, pid);
}

static int count_blocked(const sim_t *s) {
    int blocked = 0;
    for (int i = 0; i < s->num_processes; i++) {
        proc_state_t state = s->procs[i].state;
        blocked += state == PROC_BLOCKED_SEM || state == PROC_BLOCKED_RESOURCE || state == PROC_BLOCKED_IPC;
    }
    return blocked;
}

bool sim_step(sim_t *s) {
    if (sim_is_done(s)) return false;

    admit_arrivals(s);
    release_periodic_jobs(s);
    advance_io(s);
    advance_fault_service(s);
    check_deadlines(s);
    dispatch_idle_cores(s);
    account_waiting(s);
    for (int c = 0; c < s->num_cores; c++) {
        execute_core(s, c);
    }

    s->tick++;
    s->gantt_count++;
    if (sim_is_stalled(s)) sim_log_event(s, EVENT_STALLED, NO_PROCESS, NO_CORE, count_blocked(s), 0);
    return !sim_is_done(s);
}

void sim_run(sim_t *s) {
    while (sim_step(s)) {
    }
}
