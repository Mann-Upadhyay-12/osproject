#include "prism/sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define NO_PROCESS (-1)

const char *sim_strerror(int err) {
    switch (err) {
        case SIM_OK:          return "ok";
        case SIM_ERR_FULL:    return "process table is full";
        case SIM_ERR_INVALID: return "invalid argument";
        case SIM_ERR_STATE:   return "not allowed in the process's current state";
        case SIM_ERR_NO_PID:  return "no such process";
        case SIM_ERR_NAME:    return "unknown policy name";
        case SIM_ERR_IO:      return "file I/O error";
        default:              return (err >= 0) ? "ok" : "unknown error";
    }
}

void proc_spec_init(proc_spec_t *spec, const char *name) {
    memset(spec, 0, sizeof(*spec));
    snprintf(spec->name, sizeof(spec->name), "%s", (name && *name) ? name : "proc");
}

int proc_spec_add_burst(proc_spec_t *spec, burst_kind_t kind, uint16_t duration) {
    if (spec->num_bursts >= PRISM_MAX_BURSTS) return SIM_ERR_FULL;
    burst_t *b = &spec->bursts[spec->num_bursts++];
    memset(b, 0, sizeof(*b));
    b->kind = (uint8_t)kind;
    b->duration = duration;
    b->remaining = duration;
    return SIM_OK;
}

int proc_spec_add_ref(proc_spec_t *spec, uint16_t vpage) {
    if (spec->num_page_refs >= PRISM_MAX_PAGE_REFS) return SIM_ERR_FULL;
    if (vpage >= PRISM_MAX_VIRTUAL_PAGES) return SIM_ERR_INVALID;
    spec->page_refs[spec->num_page_refs++] = vpage;
    return SIM_OK;
}

#define SPEC_FAIL(...) do { if (msg && msglen) snprintf(msg, msglen, __VA_ARGS__); return SIM_ERR_INVALID; } while (0)

int proc_spec_validate(const proc_spec_t *spec, char *msg, size_t msglen) {
    if (!spec) SPEC_FAIL("no spec");
    if (spec->name[0] == '\0') SPEC_FAIL("name is empty");
    if (spec->num_bursts == 0) SPEC_FAIL("process needs at least one burst");
    if (spec->num_bursts > PRISM_MAX_BURSTS) SPEC_FAIL("more than %d bursts", PRISM_MAX_BURSTS);
    if (spec->bursts[0].kind != BURST_CPU) SPEC_FAIL("first burst must be cpu");
    if (spec->period == 0 && spec->jobs > 1) SPEC_FAIL("repeating jobs need a period");
    if (spec->period == 0 && spec->deadline > 0) SPEC_FAIL("a deadline needs a period");
    for (int i = 0; i < spec->num_bursts; i++) {
        const burst_t *b = &spec->bursts[i];
        if (b->kind > BURST_SEM_SIGNAL) SPEC_FAIL("burst %d has unknown kind", i + 1);
        bool uses_semaphore = b->kind == BURST_SEM_WAIT || b->kind == BURST_SEM_SIGNAL;
        if (uses_semaphore && b->target_id < 0) SPEC_FAIL("burst %d names no semaphore", i + 1);
        if ((b->kind == BURST_CPU || b->kind == BURST_IO) && b->duration == 0)
            SPEC_FAIL("burst %d has zero duration", i + 1);
    }
    if (spec->num_page_refs > PRISM_MAX_PAGE_REFS) SPEC_FAIL("more than %d page references", PRISM_MAX_PAGE_REFS);
    for (int i = 0; i < spec->num_page_refs; i++) {
        if (spec->page_refs[i] >= PRISM_MAX_VIRTUAL_PAGES)
            SPEC_FAIL("page %u out of range (max %d)", spec->page_refs[i], PRISM_MAX_VIRTUAL_PAGES - 1);
    }
    return SIM_OK;
}

static void copy_bursts_unstarted(burst_t *dst, const burst_t *src, int count) {
    for (int i = 0; i < count; i++) {
        dst[i] = src[i];
        dst[i].remaining = src[i].duration;
    }
}

static void replace_spaces(char *name) {
    for (char *c = name; *c; c++) {
        if (isspace((unsigned char)*c)) *c = '_';
    }
}

static void pcb_apply_spec(pcb_t *p, const proc_spec_t *spec, tick_t arrival) {
    snprintf(p->name, sizeof(p->name), "%s", spec->name);
    replace_spaces(p->name);

    p->arrival_tick = arrival;
    p->priority = spec->priority;
    p->base_priority = spec->priority;
    p->period = spec->period;
    p->relative_deadline = spec->deadline ? spec->deadline : spec->period;
    p->num_jobs = spec->jobs ? spec->jobs : 1;

    p->num_bursts = spec->num_bursts;
    copy_bursts_unstarted(p->bursts, spec->bursts, spec->num_bursts);
    p->curr_burst_idx = 0;

    p->num_page_refs = spec->num_page_refs;
    memcpy(p->page_refs, spec->page_refs, spec->num_page_refs * sizeof(spec->page_refs[0]));
    p->curr_page_ref_idx = 0;
}

static bool pid_live(const sim_t *s, sim_pid_t pid) {
    return pid >= 0 && pid < s->num_processes && s->procs[pid].state != PROC_UNUSED;
}

static tick_t arrival_not_in_past(const sim_t *s, const proc_spec_t *spec) {
    return spec->arrival > s->tick ? spec->arrival : s->tick;
}

int sim_spawn_process(sim_t *s, const proc_spec_t *spec) {
    if (proc_spec_validate(spec, NULL, 0) != SIM_OK) return SIM_ERR_INVALID;
    if (s->num_processes >= PRISM_MAX_PROCESSES) return SIM_ERR_FULL;

    tick_t arrival = arrival_not_in_past(s, spec);
    sim_pid_t pid = sim_add_process(s, spec->name, arrival);
    if (pid < 0) return SIM_ERR_FULL;
    pcb_apply_spec(&s->procs[pid], spec, arrival);
    return pid;
}

int sim_remove_process(sim_t *s, sim_pid_t pid) {
    if (!pid_live(s, pid)) return SIM_ERR_NO_PID;

    pcb_t *p = &s->procs[pid];
    bool not_started = p->state == PROC_NEW;
    bool finished = p->state == PROC_TERMINATED;
    if (!not_started && !finished) return SIM_ERR_STATE;

    if (finished) s->completed_processes--;
    sim_release_frames(s, pid);
    p->state = PROC_UNUSED;
    p->next_proc = NO_PROCESS;
    s->num_removed++;
    return SIM_OK;
}

int sim_update_process(sim_t *s, sim_pid_t pid, const proc_spec_t *spec) {
    if (!pid_live(s, pid)) return SIM_ERR_NO_PID;
    if (proc_spec_validate(spec, NULL, 0) != SIM_OK) return SIM_ERR_INVALID;

    pcb_t *p = &s->procs[pid];
    bool has_started = p->state != PROC_NEW || p->start_tick >= 0;
    if (has_started) return SIM_ERR_STATE;

    pcb_apply_spec(p, spec, arrival_not_in_past(s, spec));
    return SIM_OK;
}

int sim_get_process_spec(const sim_t *s, sim_pid_t pid, proc_spec_t *out) {
    if (!pid_live(s, pid)) return SIM_ERR_NO_PID;

    const pcb_t *p = &s->procs[pid];
    proc_spec_init(out, p->name);
    out->arrival = p->arrival_tick;
    out->priority = p->base_priority;
    out->period = p->period;
    out->deadline = p->relative_deadline;
    out->jobs = p->num_jobs;
    out->num_bursts = p->num_bursts;
    copy_bursts_unstarted(out->bursts, p->bursts, p->num_bursts);
    out->num_page_refs = p->num_page_refs;
    memcpy(out->page_refs, p->page_refs, p->num_page_refs * sizeof(p->page_refs[0]));
    return SIM_OK;
}

static void readd_process(sim_t *fresh, const sim_t *old_sim, sim_pid_t old_pid) {
    const pcb_t *old = &old_sim->procs[old_pid];
    sim_pid_t pid = sim_add_process(fresh, old->name, old->arrival_tick);
    pcb_t *p = &fresh->procs[pid];

    if (old->state == PROC_UNUSED) {
        p->state = PROC_UNUSED;
        fresh->num_removed++;
        return;
    }

    proc_spec_t spec;
    sim_get_process_spec(old_sim, old_pid, &spec);
    pcb_apply_spec(p, &spec, old->arrival_tick);
}

void sim_reset(sim_t *s) {
    sim_t *fresh = malloc(sizeof(*fresh));
    if (!fresh) {
        fprintf(stderr, "prism: out of memory in sim_reset\n");
        exit(1);
    }

    int frames = s->configured_frames > 0 ? s->configured_frames : s->total_frames;
    sim_init(fresh, s->num_cores, frames, s->disk_latency, s->sched_policy, s->repl_policy, s->seed);
    fresh->num_cores = s->num_cores;
    fresh->disk_latency = s->disk_latency;
    fresh->prng_state = s->seed ? s->seed : fresh->prng_state;
    fresh->default_quantum = s->default_quantum;

    for (int r = 0; r < s->num_resources; r++) {
        sim_add_resource(fresh, s->resources[r].name, s->resources[r].total_instances);
    }
    for (int i = 0; i < s->num_semaphores; i++) {
        sim_add_semaphore(fresh, s->semaphores[i].name, s->semaphores[i].initial_value);
    }

    for (sim_pid_t pid = 0; pid < s->num_processes; pid++) {
        readd_process(fresh, s, pid);
    }

    fresh->observer = s->observer;
    fresh->observer_user = s->observer_user;
    *s = *fresh;
    free(fresh);
}

static int drain_ready_queues(sim_t *s, sim_pid_t out[PRISM_MAX_PROCESSES]) {
    int count = 0;
    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        sim_pid_t pid;
        while (count < PRISM_MAX_PROCESSES && (pid = sim_queue_pop(s, q)) != NO_PROCESS) {
            out[count++] = pid;
        }
    }
    return count;
}

int sim_set_sched(sim_t *s, const char *name, int quantum) {
    sched_policy_t policy;
    if (!sched_lookup(name, &policy)) return SIM_ERR_NAME;
    if (quantum > UINT16_MAX) return SIM_ERR_INVALID;
    if (quantum > 0) s->default_quantum = (uint16_t)quantum;

    sim_pid_t ready[PRISM_MAX_PROCESSES];
    int num_ready = drain_ready_queues(s, ready);

    s->sched_policy = policy;
    const sched_ops_t *ops = sched_get_ops(policy);
    if (ops->init) ops->init(s);

    for (int i = 0; i < num_ready; i++) {
        pcb_t *p = &s->procs[ready[i]];
        p->priority = p->base_priority;
        ops->on_ready(s, p->pid);
    }
    return SIM_OK;
}

int sim_set_repl(sim_t *s, const char *name) {
    repl_policy_t policy;
    if (!repl_lookup(name, &policy)) return SIM_ERR_NAME;

    s->repl_policy = policy;
    const repl_ops_t *ops = repl_get_ops(policy);
    if (ops->init) ops->init(s);
    return SIM_OK;
}

int sim_set_frames(sim_t *s, int nframes) {
    if (nframes < 1 || nframes > PRISM_MAX_FRAMES) return SIM_ERR_INVALID;
    s->configured_frames = nframes;
    sim_reset(s);
    return SIM_OK;
}

int sim_set_cores(sim_t *s, int ncores) {
    if (ncores < 1 || ncores > PRISM_MAX_CORES) return SIM_ERR_INVALID;
    s->num_cores = ncores;
    sim_reset(s);
    return SIM_OK;
}

int sim_set_disk_latency(sim_t *s, int ticks) {
    if (ticks < 1 || ticks > UINT16_MAX) return SIM_ERR_INVALID;
    s->disk_latency = ticks;
    return SIM_OK;
}

bool sim_run_until(sim_t *s, tick_t t) {
    while (s->tick < t && !sim_is_done(s)) {
        sim_step(s);
    }
    return !sim_is_done(s);
}

bool sim_event_is_notable(event_type_t type) {
    return type != EVENT_NONE && type != EVENT_PAGE_ACCESS;
}

bool sim_step_event(sim_t *s) {
    uint64_t events_before = s->notable_events;
    for (int i = 0; i < PRISM_STEP_EVENT_MAX_TICKS; i++) {
        bool something_happened = s->notable_events != events_before;
        if (sim_is_done(s) || something_happened) break;
        sim_step(s);
    }
    return !sim_is_done(s);
}

void sim_set_observer(sim_t *s, sim_observer_fn fn, void *user) {
    s->observer = fn;
    s->observer_user = user;
}
