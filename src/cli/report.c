#include "cli.h"
#include <inttypes.h>

#define SECTION(color, title) printf("\n%s%s" title "%s\n", ANSI_BOLD, color, ANSI_RESET)

#define GANTT_MAX_COLUMNS   70
#define READY_QUEUE_PREVIEW 8
#define MAILBOX_PREVIEW     4
#define FRAMES_PER_ROW      4
#define RECENT_EVENTS       6

void report_banner(void) {
    printf("%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s%s  PRISM :: Process, Resource & Interprocess Simulation Model (v1.0)                    %s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s  Deterministic OS Scheduler & Virtual Memory Engine :: Comprehensive CLI Explorer   %s\n", ANSI_DIM, ANSI_RESET);
    printf("%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
}

static int64_t turnaround_of(const pcb_t *p) {
    return p->finish_tick >= 0 ? p->finish_tick - (int64_t)p->arrival_tick : 0;
}

static const char *state_color(proc_state_t state) {
    switch (state) {
        case PROC_RUNNING:       return ANSI_GREEN;
        case PROC_READY:         return ANSI_YELLOW;
        case PROC_BLOCKED_IO:
        case PROC_BLOCKED_FAULT: return ANSI_RED;
        case PROC_TERMINATED:    return ANSI_DIM;
        default:                 return ANSI_RESET;
    }
}

static const char *process_name_or_system(const sim_t *s, sim_pid_t pid) {
    return pid >= 0 && pid < s->num_processes ? s->procs[pid].name : "system";
}

static void report_engine(const sim_t *s) {
    SECTION(ANSI_CYAN, "[1] SIMULATION ENGINE & GLOBAL CLOCK");
    printf("  Virtual Clock Tick : %s%" PRIu64 "%s\n", ANSI_BOLD, s->tick, ANSI_RESET);
    printf("  Cores Configured   : %d\n", s->num_cores);
    printf("  PRNG State / Seed  : 0x%016" PRIx64 " (Seed: %" PRIu64 ")\n", s->prng_state, s->seed);
    printf("  Deterministic Hash : %s0x%016" PRIx64 "%s (FNV-1a byte-identical verification)\n", ANSI_GREEN, s->log_hash, ANSI_RESET);
    printf("  Simulation Status  : %s\n", sim_is_done(s) ? "COMPLETED" : "IN-PROGRESS");
}

static void report_process_table(const sim_t *s) {
    SECTION(ANSI_BLUE, "[2] PROCESS MANAGER (PCB & LIFECYCLE)");
    printf("  %-4s %-12s %-10s %-8s %-6s %-6s %-6s %-6s %-6s %-7s %-8s\n",
           "PID", "Name", "State", "Arrival", "CPU", "I/O", "Fault", "Wait", "Turn", "Faults", "Switches");
    printf("  ------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        printf("  %-4d %-12s %s%-10s%s %-8" PRIu64 " %-6u %-6u %-6u %-6u %-6" PRId64 " %-7u %-8u\n",
               p->pid, p->name, state_color(p->state), proc_state_str(p->state), ANSI_RESET,
               p->arrival_tick, p->total_cpu_time, p->total_io_time,
               p->total_fault_time, p->total_wait_time, turnaround_of(p),
               p->num_page_faults, p->num_context_switches);
    }
}

static void report_core(const sim_t *s, int core) {
    sim_pid_t pid = s->running_pid[core];
    if (pid == -1) {
        printf("    Core %d: %s[ IDLE ]%s (Busy Ticks: %u)\n", core, ANSI_DIM, ANSI_RESET, s->core_busy_ticks[core]);
        return;
    }

    const pcb_t *p = &s->procs[pid];
    uint16_t burst_left = p->curr_burst_idx < p->num_bursts ? p->bursts[p->curr_burst_idx].remaining : 0;
    printf("    Core %d: %s[ RUNNING PID %d (%s) | Burst Rem: %u | Q-Rem: %u ]%s (Busy Ticks: %u)\n",
           core, ANSI_GREEN, pid, p->name, burst_left, p->quantum_remaining, ANSI_RESET, s->core_busy_ticks[core]);
}

static void report_ready_queue(const sim_t *s, int level) {
    printf("    Level %d (%u waiting): [ ", level, s->ready_count[level]);

    sim_pid_t pid = s->ready_head[level];
    for (int shown = 0; pid != -1 && shown < READY_QUEUE_PREVIEW; shown++) {
        printf("P%d(%s) ", pid, s->procs[pid].name);
        pid = s->procs[pid].next_proc;
    }
    if (pid != -1) printf("... ");
    printf("]\n");
}

static void report_scheduler(const sim_t *s) {
    SECTION(ANSI_GREEN, "[3] CPU SCHEDULER SUBSYSTEM");
    printf("  Active Policy       : %s%s%s\n", ANSI_BOLD, sched_get_ops(s->sched_policy)->name, ANSI_RESET);
    printf("  Total Ctx Switches  : %u\n", s->total_context_switches);

    printf("  Active Cores Status :\n");
    for (int c = 0; c < s->num_cores; c++) report_core(s, c);

    printf("  Ready Queue(s) Status:\n");
    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) report_ready_queue(s, q);
}

static void report_frame(const sim_t *s, int f) {
    const frame_entry_t *frame = &s->frames[f];
    if (!frame->occupied) {
        printf("[%02d: %sFREE%s] ", f, ANSI_DIM, ANSI_RESET);
    } else {
        printf("[%02d: %sP%d:v%u%s (ref:%d)] ",
               f, ANSI_MAGENTA, frame->owner_pid, frame->vpage, ANSI_RESET, frame->ref_bit ? 1 : 0);
    }
}

static void report_memory(const sim_t *s) {
    double fault_rate = s->total_page_accesses > 0 ? 100.0 * s->total_page_faults / s->total_page_accesses : 0.0;

    SECTION(ANSI_MAGENTA, "[4] VIRTUAL MEMORY & PAGE REPLACEMENT SUBSYSTEM");
    printf("  Replacement Policy  : %s%s%s\n", ANSI_BOLD, repl_get_ops(s->repl_policy)->name, ANSI_RESET);
    printf("  Disk Fault Latency  : %d ticks\n", s->disk_latency);
    printf("  Page Accesses       : %u\n", s->total_page_accesses);
    printf("  Page Faults / Rate  : %u (%.1f%%)\n", s->total_page_faults, fault_rate);

    printf("  Physical Frames (%d total):\n    ", s->total_frames);
    for (int f = 0; f < s->total_frames; f++) {
        report_frame(s, f);
        bool end_of_row = (f + 1) % FRAMES_PER_ROW == 0;
        bool more_frames = f + 1 < s->total_frames;
        if (end_of_row && more_frames) printf("\n    ");
    }
    printf("\n");
}

static void report_resource_holders(const sim_t *s, const sim_resource_t *res) {
    if (res->allocated_mask == 0) {
        printf("[ NONE ]\n");
        return;
    }
    printf("[ ");
    for (int pid = 0; pid < s->num_processes; pid++) {
        if (res->allocated_mask & (1ULL << pid)) printf("P%d(%s) ", pid, s->procs[pid].name);
    }
    printf("]\n");
}

static void report_deadlock_cycle(const sim_t *s) {
    printf("  %s%s[!!! DEADLOCK DETECTED !!!]%s Cycle: ", ANSI_BOLD, ANSI_RED, ANSI_RESET);
    for (int k = 0; k < s->deadlock_cycle_len; k++) {
        sim_pid_t pid = s->deadlock_cycle[k];
        printf("%sP%d(%s)%s -> ", ANSI_BOLD, pid, s->procs[pid].name, ANSI_RESET);
    }
    if (s->deadlock_cycle_len > 0) {
        sim_pid_t first = s->deadlock_cycle[0];
        printf("%sP%d(%s)%s", ANSI_BOLD, first, s->procs[first].name, ANSI_RESET);
    }
    printf("\n");
}

void report_resources(const sim_t *s) {
    SECTION(ANSI_RED, "[5] RESOURCE MANAGER & DEADLOCK ENGINE");
    printf("  Managed Resources   : %d\n", s->num_resources);
    for (int r = 0; r < s->num_resources; r++) {
        const sim_resource_t *res = &s->resources[r];
        printf("    R%d (%s): %d/%d avail | Held by: ", r, res->name, res->available_instances, res->total_instances);
        report_resource_holders(s, res);
    }

    if (s->deadlock_detected) {
        report_deadlock_cycle(s);
    } else {
        printf("  Deadlock State      : %sNO DEADLOCK DETECTED (WFG Acyclic)%s\n", ANSI_GREEN, ANSI_RESET);
    }
}

static void report_ipc(const sim_t *s) {
    SECTION(ANSI_YELLOW, "[6] IPC & CHECKPOINT / RESTORE");
    printf("  Pending Mailbox Msgs: %d\n", s->ipc_msg_count);
    for (int m = 0; m < s->ipc_msg_count && m < MAILBOX_PREVIEW; m++) {
        const ipc_message_t *msg = &s->ipc_messages[m];
        printf("    Msg #%d: P%d -> P%d (id:%d, data:%d at tick %" PRIu64 ")\n",
               m, msg->sender_pid, msg->receiver_pid, msg->message_id, msg->data, msg->timestamp);
    }
    printf("  Saved Checkpoints   : %u total\n", s->total_checkpoints);
}

static void report_gantt(const sim_t *s) {
    SECTION(ANSI_YELLOW, "[5] CORE EXECUTION TIMELINE (GANTT STRIP)");
    uint32_t columns = s->gantt_count < GANTT_MAX_COLUMNS ? s->gantt_count : GANTT_MAX_COLUMNS;

    for (int c = 0; c < s->num_cores; c++) {
        printf("  Core %d: |", c);
        for (uint32_t t = 0; t < columns; t++) {
            sim_pid_t pid = s->gantt_history[t][c];
            if (pid == -1) {
                printf("%s.%s", ANSI_DIM, ANSI_RESET);
            } else {
                printf("%c", 'A' + (pid % 26));
            }
        }
        printf("|\n");
    }

    printf("  Ticks :  ");
    for (uint32_t t = 0; t < columns; t++) {
        if (t % 10 == 0) printf("%u", (t / 10) % 10);
        else printf(" ");
    }
    printf("\n");
}

static void report_recent_events(const sim_t *s, uint32_t count) {
    printf("\n%s%s[6] EVENT LOG & STATE TRANSITIONS (Last %u Events)%s\n", ANSI_BOLD, ANSI_CYAN, count, ANSI_RESET);

    uint32_t total = s->event_count;
    uint32_t first = total > count ? total - count : 0;
    for (uint32_t i = first; i < total; i++) {
        const sim_event_t *ev = &s->event_log[i];
        printf("  [Tick %4" PRIu64 "] Event: %-12s | PID: %2d (%-11s) | Core: %2d | Info: (d1=%d, d2=%d)\n",
               ev->tick, event_type_str(ev->type), ev->pid, process_name_or_system(s, ev->pid),
               ev->core_id, ev->detail1, ev->detail2);
    }
}

void report_snapshot(const sim_t *s, bool show_gantt) {
    report_engine(s);
    report_process_table(s);
    report_scheduler(s);
    report_memory(s);
    report_resources(s);
    report_ipc(s);
    if (show_gantt) report_gantt(s);
    report_recent_events(s, RECENT_EVENTS);
}

void report_csv(const sim_t *s) {
    printf("pid,name,arrival,start,finish,wait,cpu,io,fault,turnaround,page_faults,context_switches\n");
    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        printf("%d,%s,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%u,%u,%u,%u,%" PRId64 ",%u,%u\n",
               p->pid, p->name, p->arrival_tick, p->start_tick, p->finish_tick,
               p->total_wait_time, p->total_cpu_time, p->total_io_time,
               p->total_fault_time, turnaround_of(p), p->num_page_faults, p->num_context_switches);
    }
}
