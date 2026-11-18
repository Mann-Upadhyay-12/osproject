#include "prism/sim.h"
#include "prism/text.h"
#include <stdio.h>
#include <string.h>

#define NO_CORE (-1)

static bool is_valid_semaphore(const sim_t *s, int32_t sem_id) {
    return sem_id >= 0 && sem_id < s->num_semaphores;
}

static bool is_valid_pid(const sim_t *s, sim_pid_t pid) {
    return pid >= 0 && pid < s->num_processes;
}

int32_t sim_add_semaphore(sim_t *s, const char *name, int32_t initial_value) {
    if (s->num_semaphores >= PRISM_MAX_SEMAPHORES || initial_value < 0) return -1;
    if (name && sim_find_semaphore(s, name) >= 0) return -1;

    int32_t id = s->num_semaphores++;
    sim_semaphore_t *sem = &s->semaphores[id];
    memset(sem, 0, sizeof(*sem));
    snprintf(sem->name, sizeof(sem->name), "%s", name && *name ? name : "sem");
    sem->initial_value = initial_value;
    sem->value = initial_value;
    return id;
}

int32_t sim_find_semaphore(const sim_t *s, const char *name) {
    for (int32_t i = 0; i < s->num_semaphores; i++) {
        if (text_equals_ignore_case(s->semaphores[i].name, name)) return i;
    }
    return -1;
}

static void enqueue_waiter(sim_semaphore_t *sem, sim_pid_t pid) {
    if (sem->num_waiters < PRISM_MAX_PROCESSES) sem->waiters[sem->num_waiters++] = pid;
}

static sim_pid_t dequeue_waiter(sim_semaphore_t *sem) {
    sim_pid_t first = sem->waiters[0];
    memmove(&sem->waiters[0], &sem->waiters[1], (size_t)(sem->num_waiters - 1) * sizeof(sem->waiters[0]));
    sem->num_waiters--;
    return first;
}

bool sim_sem_wait(sim_t *s, sim_pid_t pid, int32_t sem_id) {
    if (!is_valid_pid(s, pid) || !is_valid_semaphore(s, sem_id)) return true;

    sim_semaphore_t *sem = &s->semaphores[sem_id];
    pcb_t *p = &s->procs[pid];
    sem->total_waits++;

    if (sem->value > 0) {
        sem->value--;
        sim_log_event(s, EVENT_SEM_WAIT, pid, p->assigned_core, sem_id, sem->value);
        return true;
    }

    sem->total_blocks++;
    p->state = PROC_BLOCKED_SEM;
    p->waiting_semaphore = (int16_t)sem_id;
    enqueue_waiter(sem, pid);
    sim_log_event(s, EVENT_SEM_BLOCKED, pid, p->assigned_core, sem_id, sem->num_waiters);
    return false;
}

static void wake_waiter(sim_t *s, sim_semaphore_t *sem, int32_t sem_id, sim_pid_t signaller) {
    pcb_t *woken = &s->procs[dequeue_waiter(sem)];
    woken->waiting_semaphore = -1;
    woken->curr_burst_idx++;
    woken->state = PROC_READY;
    sim_log_event(s, EVENT_SEM_WAKE, woken->pid, NO_CORE, sem_id, signaller);
    sched_get_ops(s->sched_policy)->on_ready(s, woken->pid);
}

bool sim_sem_signal(sim_t *s, sim_pid_t pid, int32_t sem_id) {
    if (!is_valid_pid(s, pid) || !is_valid_semaphore(s, sem_id)) return false;

    sim_semaphore_t *sem = &s->semaphores[sem_id];
    if (sem->num_waiters == 0) sem->value++;
    sim_log_event(s, EVENT_SEM_SIGNAL, pid, s->procs[pid].assigned_core, sem_id, sem->value);
    if (sem->num_waiters > 0) wake_waiter(s, sem, sem_id, pid);
    return true;
}

void sim_sem_forget(sim_t *s, sim_pid_t pid) {
    for (int32_t i = 0; i < s->num_semaphores; i++) {
        sim_semaphore_t *sem = &s->semaphores[i];
        int kept = 0;
        for (int w = 0; w < sem->num_waiters; w++) {
            if (sem->waiters[w] != pid) sem->waiters[kept++] = sem->waiters[w];
        }
        sem->num_waiters = kept;
    }
    if (is_valid_pid(s, pid)) s->procs[pid].waiting_semaphore = -1;
}
