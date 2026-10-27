#include "prism/sim.h"
#include "prism/workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* ANSI Color escape sequences for clear component terminal presentation */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_BLUE    "\033[34m"
#define ANSI_DIM     "\033[2m"

static void print_banner(void) {
    printf("%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s%s  PRISM :: Process, Resource & Interprocess Simulation Model (v1.0)                    %s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s  Deterministic OS Scheduler & Virtual Memory Engine :: Comprehensive CLI Explorer   %s\n", ANSI_DIM, ANSI_RESET);
    printf("%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
}

/* 1. Component View: Simulation Engine & Clock */
static void print_component_engine(const sim_t *s) {
    printf("\n%s%s[1] SIMULATION ENGINE & GLOBAL CLOCK%s\n", ANSI_BOLD, ANSI_CYAN, ANSI_RESET);
    printf("  Virtual Clock Tick : %s%" PRIu64 "%s\n", ANSI_BOLD, s->tick, ANSI_RESET);
    printf("  Cores Configured   : %d\n", s->num_cores);
    printf("  PRNG State / Seed  : 0x%016" PRIx64 " (Seed: %" PRIu64 ")\n", s->prng_state, s->seed);
    printf("  Deterministic Hash : %s0x%016" PRIx64 "%s (FNV-1a byte-identical verification)\n", ANSI_GREEN, s->log_hash, ANSI_RESET);
    printf("  Simulation Status  : %s\n", sim_is_done(s) ? "COMPLETED" : "IN-PROGRESS");
}

/* 2. Component View: Process Manager */
static void print_component_process_table(const sim_t *s) {
    printf("\n%s%s[2] PROCESS MANAGER (PCB & LIFECYCLE)%s\n", ANSI_BOLD, ANSI_BLUE, ANSI_RESET);
    printf("  %-4s %-12s %-10s %-8s %-6s %-6s %-6s %-6s %-6s %-7s %-8s\n",
           "PID", "Name", "State", "Arrival", "CPU", "I/O", "Fault", "Wait", "Turn", "Faults", "Switches");
    printf("  ------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        int64_t turn = (p->finish_tick >= 0) ? (p->finish_tick - (int64_t)p->arrival_tick) : 0;
        const char *st_color = ANSI_RESET;
        if (p->state == PROC_RUNNING) st_color = ANSI_GREEN;
        else if (p->state == PROC_READY) st_color = ANSI_YELLOW;
        else if (p->state == PROC_BLOCKED_IO || p->state == PROC_BLOCKED_FAULT) st_color = ANSI_RED;
        else if (p->state == PROC_TERMINATED) st_color = ANSI_DIM;

        printf("  %-4d %-12s %s%-10s%s %-8" PRIu64 " %-6u %-6u %-6u %-6u %-6" PRId64 " %-7u %-8u\n",
               p->pid, p->name, st_color, proc_state_str(p->state), ANSI_RESET,
               p->arrival_tick, p->total_cpu_time, p->total_io_time,
               p->total_fault_time, p->total_wait_time, turn,
               p->num_page_faults, p->num_context_switches);
    }
}

/* 3. Component View: CPU Scheduler */
static void print_component_scheduler(const sim_t *s) {
    const sched_ops_t *ops = sched_get_ops(s->sched_policy);
    printf("\n%s%s[3] CPU SCHEDULER SUBSYSTEM%s\n", ANSI_BOLD, ANSI_GREEN, ANSI_RESET);
    printf("  Active Policy       : %s%s%s\n", ANSI_BOLD, ops->name, ANSI_RESET);
    printf("  Total Ctx Switches  : %u\n", s->total_context_switches);
    printf("  Active Cores Status :\n");

    for (int c = 0; c < s->num_cores; c++) {
        sim_pid_t pid = s->running_pid[c];
        if (pid == -1) {
            printf("    Core %d: %s[ IDLE ]%s (Busy Ticks: %u)\n", c, ANSI_DIM, ANSI_RESET, s->core_busy_ticks[c]);
        } else {
            const pcb_t *p = &s->procs[pid];
            uint16_t rem = (p->curr_burst_idx < p->num_bursts) ? p->bursts[p->curr_burst_idx].remaining : 0;
            printf("    Core %d: %s[ RUNNING PID %d (%s) | Burst Rem: %u | Q-Rem: %u ]%s (Busy Ticks: %u)\n",
                   c, ANSI_GREEN, pid, p->name, rem, p->quantum_remaining, ANSI_RESET, s->core_busy_ticks[c]);
        }
    }

    /* Ready Queues inspection */
    printf("  Ready Queue(s) Status:\n");
    for (int q = 0; q < PRISM_MLFQ_QUEUES; q++) {
        printf("    Level %d (%u waiting): [ ", q, s->ready_count[q]);
        sim_pid_t curr = s->ready_head[q];
        int count = 0;
        while (curr != -1 && count < 8) {
            printf("P%d(%s) ", curr, s->procs[curr].name);
            curr = s->procs[curr].next_proc;
            count++;
        }
        if (curr != -1) printf("... ");
        printf("]\n");
    }
}

/* 4. Component View: Virtual Memory & Page Replacement */
static void print_component_memory(const sim_t *s) {
    const repl_ops_t *rops = repl_get_ops(s->repl_policy);
    printf("\n%s%s[4] VIRTUAL MEMORY & PAGE REPLACEMENT SUBSYSTEM%s\n", ANSI_BOLD, ANSI_MAGENTA, ANSI_RESET);
    printf("  Replacement Policy  : %s%s%s\n", ANSI_BOLD, rops->name, ANSI_RESET);
    printf("  Disk Fault Latency  : %d ticks\n", s->disk_latency);
    printf("  Page Accesses       : %u\n", s->total_page_accesses);
    printf("  Page Faults / Rate  : %u (%.1f%%)\n",
           s->total_page_faults,
           s->total_page_accesses > 0 ? (100.0 * s->total_page_faults / s->total_page_accesses) : 0.0);

    /* Frame Table visualization */
    printf("  Physical Frames (%d total):\n    ", s->total_frames);
    for (int f = 0; f < s->total_frames; f++) {
        if (!s->frames[f].occupied) {
            printf("[%02d: %sFREE%s] ", f, ANSI_DIM, ANSI_RESET);
        } else {
            printf("[%02d: %sP%d:v%u%s (ref:%d)] ",
                   f, ANSI_MAGENTA, s->frames[f].owner_pid, s->frames[f].vpage,
                   ANSI_RESET, s->frames[f].ref_bit ? 1 : 0);
        }
        if ((f + 1) % 4 == 0 && (f + 1) < s->total_frames) {
            printf("\n    ");
        }
    }
    printf("\n");
}

/* 5. Component View: Resource Manager & Deadlock Engine */
static void print_component_resources(const sim_t *s) {
    printf("\n%s%s[5] RESOURCE MANAGER & DEADLOCK ENGINE%s\n", ANSI_BOLD, ANSI_RED, ANSI_RESET);
    printf("  Managed Resources   : %d\n", s->num_resources);
    for (int r = 0; r < s->num_resources; r++) {
        printf("    R%d (%s): %d/%d avail | Held by: ",
               r, s->resources[r].name,
               s->resources[r].available_instances, s->resources[r].total_instances);
        if (s->resources[r].allocated_mask == 0) {
            printf("[ NONE ]\n");
        } else {
            printf("[ ");
            for (int p = 0; p < s->num_processes; p++) {
                if (s->resources[r].allocated_mask & (1ULL << p)) {
                    printf("P%d(%s) ", p, s->procs[p].name);
                }
            }
            printf("]\n");
        }
    }

    /* Wait-For Graph (WFG) & Deadlock State */
    if (s->deadlock_detected) {
        printf("  %s%s[!!! DEADLOCK DETECTED !!!]%s Cycle: ", ANSI_BOLD, ANSI_RED, ANSI_RESET);
        for (int k = 0; k < s->deadlock_cycle_len; k++) {
            sim_pid_t p = s->deadlock_cycle[k];
            printf("%sP%d(%s)%s -> ", ANSI_BOLD, p, s->procs[p].name, ANSI_RESET);
        }
        if (s->deadlock_cycle_len > 0) {
            sim_pid_t p = s->deadlock_cycle[0];
            printf("%sP%d(%s)%s", ANSI_BOLD, p, s->procs[p].name, ANSI_RESET);
        }
        printf("\n");
    } else {
        printf("  Deadlock State      : %sNO DEADLOCK DETECTED (WFG Acyclic)%s\n", ANSI_GREEN, ANSI_RESET);
    }
}

/* 6. Component View: Inter-Process Communication (IPC) & Checkpoints */
static void print_component_ipc(const sim_t *s) {
    printf("\n%s%s[6] IPC & CHECKPOINT / RESTORE%s\n", ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
    printf("  Pending Mailbox Msgs: %d\n", s->ipc_msg_count);
    for (int m = 0; m < s->ipc_msg_count && m < 4; m++) {
        printf("    Msg #%d: P%d -> P%d (id:%d, data:%d at tick %" PRIu64 ")\n",
               m, s->ipc_messages[m].sender_pid, s->ipc_messages[m].receiver_pid,
               s->ipc_messages[m].message_id, s->ipc_messages[m].data, s->ipc_messages[m].timestamp);
    }
    printf("  Saved Checkpoints   : %u total\n", s->total_checkpoints);
}

/* 5. Component View: Timeline / Gantt Execution Strip */
static void print_gantt_chart(const sim_t *s) {
    printf("\n%s%s[5] CORE EXECUTION TIMELINE (GANTT STRIP)%s\n", ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
    uint32_t count = s->gantt_count;
    if (count > 70) count = 70; /* display first 70 ticks or last window */

    for (int c = 0; c < s->num_cores; c++) {
        printf("  Core %d: |", c);
        for (uint32_t t = 0; t < count; t++) {
            sim_pid_t p = s->gantt_history[t][c];
            if (p == -1) {
                printf("%s.%s", ANSI_DIM, ANSI_RESET);
            } else {
                printf("%c", 'A' + (p % 26));
            }
        }
        printf("|\n");
    }
    printf("  Ticks :  ");
    for (uint32_t t = 0; t < count; t++) {
        if (t % 10 == 0) printf("%u", (t / 10) % 10);
        else printf(" ");
    }
    printf("\n");
}

/* 6. Recent Event Trace */
static void print_recent_events(const sim_t *s, uint32_t count) {
    printf("\n%s%s[6] EVENT LOG & STATE TRANSITIONS (Last %u Events)%s\n", ANSI_BOLD, ANSI_CYAN, count, ANSI_RESET);
    uint32_t total = s->event_count;
    uint32_t start = (total > count) ? (total - count) : 0;

    for (uint32_t i = start; i < total; i++) {
        const sim_event_t *ev = &s->event_log[i];
        printf("  [Tick %4" PRIu64 "] Event: %-12s | PID: %2d (%-11s) | Core: %2d | Info: (d1=%d, d2=%d)\n",
               ev->tick, event_type_str(ev->type), ev->pid,
               (ev->pid >= 0 && ev->pid < s->num_processes) ? s->procs[ev->pid].name : "system",
               ev->core_id, ev->detail1, ev->detail2);
    }
}

/* Master Component Inspection View */
static void print_component_snapshot(const sim_t *s, bool show_gantt) {
    print_component_engine(s);
    print_component_process_table(s);
    print_component_scheduler(s);
    print_component_memory(s);
    print_component_resources(s);
    print_component_ipc(s);
    if (show_gantt) print_gantt_chart(s);
    print_recent_events(s, 6);
}

static void print_csv(const sim_t *s) {
    printf("pid,name,arrival,start,finish,wait,cpu,io,fault,turnaround,page_faults,context_switches\n");
    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        int64_t turnaround = (p->finish_tick >= 0) ? (p->finish_tick - (int64_t)p->arrival_tick) : 0;
        printf("%d,%s,%" PRIu64 ",%" PRId64 ",%" PRId64 ",%u,%u,%u,%u,%" PRId64 ",%u,%u\n",
               p->pid, p->name, p->arrival_tick, p->start_tick, p->finish_tick,
               p->total_wait_time, p->total_cpu_time, p->total_io_time,
               p->total_fault_time, turnaround, p->num_page_faults, p->num_context_switches);
    }
}

/* =========================================================================
 * Experiments
 * ========================================================================= */
static void run_experiment_1(const char *workload_path) {
    printf("\n=== EXPERIMENT 1: Scheduler Comparison on Mixed Workload ===\n");
    printf("%-12s %-10s %-10s %-10s %-12s %-10s\n",
           "Scheduler", "Avg Wait", "Avg Turn", "Avg Resp", "Ctx Switches", "CPU Util %%");
    printf("----------------------------------------------------------------------\n");

    sched_policy_t policies[] = {
        SCHED_FCFS, SCHED_SJF, SCHED_SRTF, SCHED_RR,
        SCHED_MLFQ, SCHED_ADAPTIVE, SCHED_CFS, SCHED_BFS
    };
    const char *names[] = {
        "FCFS", "SJF", "SRTF", "Round Robin",
        "MLFQ", "Adaptive", "CFS", "BFS"
    };

    for (size_t i = 0; i < sizeof(policies)/sizeof(policies[0]); i++) {
        sim_t sim;
        workload_config_t cfg = {0};
        sim_init(&sim, 1, 4, 3, policies[i], REPL_LRU, 42);

        if (workload_path) {
            workload_load_file(workload_path, &sim, &cfg);
            sim.sched_policy = policies[i];
        } else {
            workload_generate_mixed(&sim, 6, 42);
            sim.sched_policy = policies[i];
        }

        sim_run(&sim);

        double total_wait = 0, total_turn = 0, total_resp = 0;
        for (int p = 0; p < sim.num_processes; p++) {
            total_wait += sim.procs[p].total_wait_time;
            total_turn += (sim.procs[p].finish_tick - (int64_t)sim.procs[p].arrival_tick);
            total_resp += (sim.procs[p].start_tick - (int64_t)sim.procs[p].arrival_tick);
        }
        int np = sim.num_processes > 0 ? sim.num_processes : 1;
        double cpu_util = sim.tick > 0 ? (100.0 * sim.core_busy_ticks[0] / sim.tick) : 0;

        printf("%-12s %-10.2f %-10.2f %-10.2f %-12u %-9.1f%%\n",
               names[i], total_wait / np, total_turn / np, total_resp / np,
               sim.total_context_switches, cpu_util);
    }
    printf("\n");
}

static void run_experiment_2(void) {
    printf("\n=== EXPERIMENT 2: Page Replacement vs. Frame Count & Belady's Anomaly ===\n");
    printf("Workload: Belady Reference String [1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5]\n");
    printf("%-8s %-10s %-10s %-10s %-10s\n", "Frames", "FIFO", "LRU", "Clock", "OPT");
    printf("----------------------------------------------------\n");

    for (int f = 3; f <= 5; f++) {
        uint32_t faults[4] = {0};
        repl_policy_t repls[] = {REPL_FIFO, REPL_LRU, REPL_CLOCK, REPL_OPT};

        for (int r = 0; r < 4; r++) {
            sim_t sim;
            sim_init(&sim, 1, f, 2, SCHED_FCFS, repls[r], 42);
            workload_generate_belady(&sim);
            sim_run(&sim);
            faults[r] = sim.total_page_faults;
        }

        printf("%-8d %-10u %-10u %-10u %-10u %s\n",
               f, faults[0], faults[1], faults[2], faults[3],
               (f == 4 && faults[0] > 9) ? "<-- Belady Anomaly! (FIFO 3f=9 vs 4f=10)" : "");
    }

    printf("\nWorkload: Classic Reference String [7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1] (3 frames)\n");
    uint32_t classic_faults[4] = {0};
    repl_policy_t repls[] = {REPL_FIFO, REPL_LRU, REPL_CLOCK, REPL_OPT};
    for (int r = 0; r < 4; r++) {
        sim_t sim;
        sim_init(&sim, 1, 3, 2, SCHED_FCFS, repls[r], 42);
        workload_load_file("workloads/classic_vm.txt", &sim, NULL);
        sim.repl_policy = repls[r];
        sim_run(&sim);
        classic_faults[r] = sim.total_page_faults;
    }
    printf("Results: FIFO=%u (expected 15) | LRU=%u (expected 12) | Clock=%u | OPT=%u (expected 9)\n\n",
           classic_faults[0], classic_faults[1], classic_faults[2], classic_faults[3]);
}

static void run_experiment_3(void) {
    printf("\n=== EXPERIMENT 3: RAM Shrinking, Thrashing & Adaptive Mitigation ===\n");
    printf("Scenario: 4 processes with heavy memory footprints. Frame allocation throttled.\n");
    printf("%-10s %-12s %-12s %-12s %-12s\n",
           "Frames", "Policy", "CPU Util", "Total Faults", "Total Ticks");
    printf("------------------------------------------------------------\n");

    int test_frames[] = {12, 6, 3};
    sched_policy_t policies[] = {SCHED_RR, SCHED_ADAPTIVE};
    const char *pol_names[] = {"RoundRobin", "Adaptive"};

    for (size_t f = 0; f < 3; f++) {
        int frames = test_frames[f];
        for (size_t p = 0; p < 2; p++) {
            sim_t sim;
            sim_init(&sim, 1, frames, 8, policies[p], REPL_LRU, 42);
            workload_generate_thrashing(&sim, 4);
            sim_run(&sim);

            double cpu_util = sim.tick > 0 ? (100.0 * sim.core_busy_ticks[0] / sim.tick) : 0;
            printf("%-10d %-12s %-11.1f%% %-12u %-12" PRIu64 "\n",
                   frames, pol_names[p], cpu_util, sim.total_page_faults, sim.tick);
        }
    }
    printf("\nObservation: Shrinking RAM collapses CPU utilization as system spends vast majority\n"
           "of time stalled servicing page faults (thrashing). Adaptive scheduler mitigates stalls.\n\n");
}

static void run_rewind_demo(void) {
    printf("\n=== STEP BACKWARD & DETERMINISM VERIFICATION DEMO ===\n");
    sim_t sim1, sim_snapshot_tick15, sim_restored;
    sim_init(&sim1, 1, 4, 3, SCHED_RR, REPL_LRU, 999);
    workload_generate_mixed(&sim1, 4, 999);

    /* Run sim1 for 15 ticks */
    for (int t = 0; t < 15; t++) {
        sim_step(&sim1);
    }
    printf("Simulation at Tick 15: Hash = 0x%016" PRIx64 "\n", sim1.log_hash);

    /* Snapshot state */
    sim_snapshot(&sim1, &sim_snapshot_tick15);
    printf("--> Took flat memcpy snapshot at tick 15\n");

    /* Continue running sim1 to completion */
    sim_run(&sim1);
    uint64_t full_hash1 = sim1.log_hash;
    printf("Simulation finished at Tick %" PRIu64 ": Full Run Hash = 0x%016" PRIx64 "\n",
           sim1.tick, full_hash1);

    /* Restore from snapshot */
    sim_restore(&sim_restored, &sim_snapshot_tick15);
    printf("--> Restored state to Tick 15: Restored Hash = 0x%016" PRIx64 "\n", sim_restored.log_hash);

    /* Run restored sim forward to completion */
    sim_run(&sim_restored);
    uint64_t full_hash2 = sim_restored.log_hash;
    printf("Restored run finished at Tick %" PRIu64 ": Full Run Hash = 0x%016" PRIx64 "\n",
           sim_restored.tick, full_hash2);

    if (full_hash1 == full_hash2) {
        printf("\033[32m[SUCCESS] Byte-identical execution verified via FNV-1a log hash!\033[0m\n\n");
    } else {
        printf("\033[31m[FAILED] Hash mismatch between original and rewind runs!\033[0m\n\n");
    }
}

/* =========================================================================
 * Side-by-Side Comparison: Schedulers on Identical Workload
 * ========================================================================= */
static void run_compare_schedulers_visual(const char *workload_path) {
    printf("\n%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s%s  SIDE-BY-SIDE VISUAL COMPARISON: ALL 8 CPU SCHEDULERS                                %s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s  Workload: %s | Frames: 6 | Latency: 3 ticks | Identical Seed: 42                    %s\n",
           ANSI_DIM, workload_path ? workload_path : "Synthetic Mixed (6 procs)", ANSI_RESET);
    printf("%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);

    sched_policy_t policies[] = {
        SCHED_FCFS, SCHED_SJF, SCHED_SRTF, SCHED_RR,
        SCHED_MLFQ, SCHED_ADAPTIVE, SCHED_CFS, SCHED_BFS
    };
    const char *names[] = {
        "FCFS", "SJF", "SRTF", "RoundRobin",
        "MLFQ", "Adaptive", "CFS", "BFS"
    };

    double avg_waits[8], avg_turns[8], avg_resps[8], cpu_utils[8];
    uint32_t ctx_switches[8];

    for (size_t i = 0; i < 8; i++) {
        sim_t sim;
        workload_config_t cfg = {0};
        sim_init(&sim, 1, 6, 3, policies[i], REPL_LRU, 42);

        if (workload_path) {
            workload_load_file(workload_path, &sim, &cfg);
            sim.sched_policy = policies[i];
        } else {
            workload_generate_mixed(&sim, 6, 42);
            sim.sched_policy = policies[i];
        }

        sim_run(&sim);

        double total_wait = 0, total_turn = 0, total_resp = 0;
        for (int p = 0; p < sim.num_processes; p++) {
            total_wait += sim.procs[p].total_wait_time;
            total_turn += (sim.procs[p].finish_tick - (int64_t)sim.procs[p].arrival_tick);
            total_resp += (sim.procs[p].start_tick - (int64_t)sim.procs[p].arrival_tick);
        }
        int np = sim.num_processes > 0 ? sim.num_processes : 1;
        avg_waits[i] = total_wait / np;
        avg_turns[i] = total_turn / np;
        avg_resps[i] = total_resp / np;
        ctx_switches[i] = sim.total_context_switches;
        cpu_utils[i] = sim.tick > 0 ? (100.0 * sim.core_busy_ticks[0] / sim.tick) : 0;
    }

    printf("\n%-12s %-10s %-10s %-10s %-12s %-10s\n",
           "Scheduler", "Avg Wait", "Avg Turn", "Avg Resp", "Ctx Switches", "CPU Util %%");
    printf("----------------------------------------------------------------------\n");
    for (size_t i = 0; i < 8; i++) {
        printf("%-12s %-10.2f %-10.2f %-10.2f %-12u %-9.1f%%\n",
               names[i], avg_waits[i], avg_turns[i], avg_resps[i], ctx_switches[i], cpu_utils[i]);
    }

    /* ASCII Comparison Bar Chart for Average Waiting Time */
    printf("\n%s%s--- Visual Bar Chart: Average Waiting Time (Lower is Better) ---%s\n", ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
    double max_wait = 1.0;
    for (size_t i = 0; i < 8; i++) {
        if (avg_waits[i] > max_wait) max_wait = avg_waits[i];
    }
    for (size_t i = 0; i < 8; i++) {
        int bar_len = (int)((avg_waits[i] / max_wait) * 35.0);
        printf("  %-10s | ", names[i]);
        for (int b = 0; b < bar_len; b++) printf("█");
        printf(" %.2f ticks\n", avg_waits[i]);
    }
    printf("\n");
}

/* =========================================================================
 * Side-by-Side Comparison: Virtual Memory Replacement Policies
 * ========================================================================= */
static void run_compare_vm_visual(void) {
    printf("\n%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s%s  SIDE-BY-SIDE VISUAL COMPARISON: PAGE REPLACEMENT POLICIES                            %s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);
    printf("%s  Sweeping physical frame allocation from 2 to 6 frames across FIFO, LRU, Clock, and OPT%s\n", ANSI_DIM, ANSI_RESET);
    printf("%s%s========================================================================================%s\n", ANSI_CYAN, ANSI_BOLD, ANSI_RESET);

    repl_policy_t repls[] = {REPL_FIFO, REPL_LRU, REPL_CLOCK, REPL_OPT};
    const char *names[] = {"FIFO", "LRU", "Clock", "OPT"};
    uint32_t results[5][4];

    for (int f = 2; f <= 6; f++) {
        for (int r = 0; r < 4; r++) {
            sim_t sim;
            sim_init(&sim, 1, f, 2, SCHED_FCFS, repls[r], 42);
            workload_generate_belady(&sim);
            sim_run(&sim);
            results[f - 2][r] = sim.total_page_faults;
        }
    }

    printf("\n%-8s %-12s %-12s %-12s %-12s\n", "Frames", "FIFO", "LRU", "Clock", "OPT");
    printf("------------------------------------------------------------\n");
    for (int f = 2; f <= 6; f++) {
        printf("%-8d %-12u %-12u %-12u %-12u %s\n",
               f, results[f - 2][0], results[f - 2][1], results[f - 2][2], results[f - 2][3],
               (f == 4 && results[f - 2][0] > results[f - 3][0]) ? "<-- Belady Anomaly! (FIFO 3f=9 vs 4f=10)" : "");
    }

    printf("\n%s%s--- Visual Fault Curve Comparison on 3 Frames ---%s\n", ANSI_BOLD, ANSI_MAGENTA, ANSI_RESET);
    for (int r = 0; r < 4; r++) {
        printf("  %-6s | ", names[r]);
        for (uint32_t b = 0; b < results[1][r]; b++) printf("▓");
        printf(" %u faults\n", results[1][r]);
    }
    printf("\n");
}

/* =========================================================================
 * Deadlock & Checkpoint Rollback Demo
 * ========================================================================= */
static void run_deadlock_demo(void) {
    printf("\n%s%s========================================================================================%s\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
    printf("%s%s  DEADLOCK ENGINE & CHECKPOINT ROLLBACK RECOVERY DEMO                                  %s\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);
    printf("%s  Demonstrating circular wait condition, WFG cycle detection, and checkpoint rollback  %s\n", ANSI_DIM, ANSI_RESET);
    printf("%s%s========================================================================================%s\n", ANSI_RED, ANSI_BOLD, ANSI_RESET);

    sim_t sim;
    sim_init(&sim, 2, 8, 2, SCHED_RR, REPL_LRU, 42);

    /* Declare two resources: R0 (Disk) and R1 (GPU) */
    int32_t r0 = sim_add_resource(&sim, "Disk_R0", 1);
    int32_t r1 = sim_add_resource(&sim, "GPU_R1", 1);

    /* Process 0: Save checkpoint -> Acquire R0 -> Sleep 2 ticks -> Request R1 */
    sim_pid_t p0 = sim_add_process(&sim, "Proc_A", 0);
    sim_add_burst(&sim, p0, BURST_CHECKPOINT, 1);
    sim_add_burst(&sim, p0, BURST_ACQUIRE, 1);
    sim.procs[p0].bursts[1].target_id = (int16_t)r0;
    sim_add_burst(&sim, p0, BURST_CPU, 2);
    sim_add_burst(&sim, p0, BURST_ACQUIRE, 1);
    sim.procs[p0].bursts[3].target_id = (int16_t)r1;
    sim_add_burst(&sim, p0, BURST_CPU, 5);

    /* Process 1: Save checkpoint -> Sleep 1 tick -> Acquire R1 -> Request R0 (Circular wait!) */
    sim_pid_t p1 = sim_add_process(&sim, "Proc_B", 0);
    sim_add_burst(&sim, p1, BURST_CHECKPOINT, 1);
    sim_add_burst(&sim, p1, BURST_CPU, 1);
    sim_add_burst(&sim, p1, BURST_ACQUIRE, 1);
    sim.procs[p1].bursts[2].target_id = (int16_t)r1;
    sim_add_burst(&sim, p1, BURST_CPU, 1);
    sim_add_burst(&sim, p1, BURST_ACQUIRE, 1);
    sim.procs[p1].bursts[4].target_id = (int16_t)r0;
    sim_add_burst(&sim, p1, BURST_CPU, 5);

    printf("\n%sStep 1: Advancing simulation into circular wait condition...%s\n", ANSI_BOLD, ANSI_RESET);
    for (int t = 0; t < 6; t++) {
        sim_step(&sim);
    }

    print_component_resources(&sim);

    if (sim.deadlock_detected) {
        printf("\n%s%sStep 2: Deadlock detected! Automatic Recovery via Checkpoint Rollback...%s\n",
               ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
        sim_recover_deadlock_rollback(&sim);
        printf("%s--> Rolled back victim process to Checkpoint; released held resources.%s\n", ANSI_GREEN, ANSI_RESET);

        print_component_resources(&sim);

        printf("\n%sStep 3: Resuming simulation to completion after recovery...%s\n", ANSI_BOLD, ANSI_RESET);
        sim_run(&sim);
        printf("%s[SUCCESS] Simulation cleanly finished at tick %" PRIu64 " after resolving deadlock!%s\n\n",
               ANSI_GREEN, sim.tick, ANSI_RESET);
    }
}

int main(int argc, char **argv) {
    const char *workload_file = NULL;
    int cores = 1;
    int frames = 4;
    int latency = 3;
    sched_policy_t sched_pol = SCHED_RR;
    repl_policy_t repl_pol = REPL_LRU;
    uint64_t seed = 42;
    bool csv_mode = false;
    bool gantt_mode = true;
    bool experiment_run = false;
    bool step_trace = false;
    bool interactive_cli = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workload") == 0 && i + 1 < argc) {
            workload_file = argv[++i];
        } else if (strcmp(argv[i], "--cores") == 0 && i + 1 < argc) {
            cores = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            latency = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sched") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "fcfs") == 0) sched_pol = SCHED_FCFS;
            else if (strcmp(argv[i], "rr") == 0) sched_pol = SCHED_RR;
            else if (strcmp(argv[i], "sjf") == 0) sched_pol = SCHED_SJF;
            else if (strcmp(argv[i], "srtf") == 0) sched_pol = SCHED_SRTF;
            else if (strcmp(argv[i], "mlfq") == 0) sched_pol = SCHED_MLFQ;
            else if (strcmp(argv[i], "adaptive") == 0) sched_pol = SCHED_ADAPTIVE;
            else if (strcmp(argv[i], "cfs") == 0) sched_pol = SCHED_CFS;
            else if (strcmp(argv[i], "bfs") == 0) sched_pol = SCHED_BFS;
        } else if (strcmp(argv[i], "--repl") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "fifo") == 0) repl_pol = REPL_FIFO;
            else if (strcmp(argv[i], "lru") == 0) repl_pol = REPL_LRU;
            else if (strcmp(argv[i], "clock") == 0) repl_pol = REPL_CLOCK;
            else if (strcmp(argv[i], "opt") == 0) repl_pol = REPL_OPT;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--csv") == 0) {
            csv_mode = true;
        } else if (strcmp(argv[i], "--no-gantt") == 0) {
            gantt_mode = false;
        } else if (strcmp(argv[i], "--step") == 0 || strcmp(argv[i], "-s") == 0) {
            step_trace = true;
        } else if (strcmp(argv[i], "--interactive") == 0 || strcmp(argv[i], "-i") == 0) {
            interactive_cli = true;
        } else if (strcmp(argv[i], "--exp1") == 0) {
            run_experiment_1(workload_file);
            experiment_run = true;
        } else if (strcmp(argv[i], "--exp2") == 0) {
            run_experiment_2();
            experiment_run = true;
        } else if (strcmp(argv[i], "--exp3") == 0) {
            run_experiment_3();
            experiment_run = true;
        } else if (strcmp(argv[i], "--rewind-demo") == 0) {
            run_rewind_demo();
        } else if (strcmp(argv[i], "--compare-sched") == 0) {
            run_compare_schedulers_visual(workload_file);
            experiment_run = true;
        } else if (strcmp(argv[i], "--compare-vm") == 0) {
            run_compare_vm_visual();
            experiment_run = true;
        } else if (strcmp(argv[i], "--deadlock-demo") == 0) {
            run_deadlock_demo();
            experiment_run = true;
        } else if (strcmp(argv[i], "--experiments") == 0 || strcmp(argv[i], "--all-exp") == 0) {
            run_compare_schedulers_visual(workload_file);
            run_compare_vm_visual();
            run_deadlock_demo();
            run_rewind_demo();
            experiment_run = true;
        }
    }

    if (experiment_run) return 0;

    if (!csv_mode) print_banner();

    sim_t sim;
    workload_config_t cfg = {0};
    sim_init(&sim, cores, frames, latency, sched_pol, repl_pol, seed);

    if (workload_file) {
        if (!workload_load_file(workload_file, &sim, &cfg)) {
            fprintf(stderr, "Error: Failed to load workload file '%s'\n", workload_file);
            return 1;
        }
    } else {
        /* Default to mixed synthetic workload */
        workload_generate_mixed(&sim, 4, seed);
    }

    /* History snapshots buffer for CLI interactive rewind */
    #define CLI_MAX_HISTORY 500
    sim_t *cli_history = NULL;
    int cli_hist_count = 0;
    if (interactive_cli) {
        cli_history = malloc(CLI_MAX_HISTORY * sizeof(sim_t));
        if (cli_history) {
            sim_snapshot(&sim, &cli_history[cli_hist_count++]);
        }
    }

    if (interactive_cli) {
        printf("\n%s%s[CLI INTERACTIVE CONTROL SHELL]%s\n", ANSI_BOLD, ANSI_GREEN, ANSI_RESET);
        printf("Commands: [Enter/s] Step | [r] Run to End | [b] Step Back / Rewind | [k PID] Kill | [d N] Shrink RAM | [q] Quit\n");
        print_component_snapshot(&sim, gantt_mode);

        char line[128];
        while (!sim_is_done(&sim)) {
            printf("\n%sPRISM (tick %" PRIu64 ") > %s", ANSI_CYAN, sim.tick, ANSI_RESET);
            if (!fgets(line, sizeof(line), stdin)) break;

            if (line[0] == 'q' || line[0] == 'Q') {
                break;
            } else if (line[0] == 'r' || line[0] == 'R') {
                sim_run(&sim);
                print_component_snapshot(&sim, gantt_mode);
                break;
            } else if (line[0] == 'b' || line[0] == 'B') {
                if (cli_hist_count > 1) {
                    cli_hist_count--;
                    sim_restore(&sim, &cli_history[cli_hist_count - 1]);
                    printf("%s[Rewound to tick %" PRIu64 "]%s\n", ANSI_YELLOW, sim.tick, ANSI_RESET);
                    print_component_snapshot(&sim, gantt_mode);
                } else {
                    printf("%s[Cannot rewind further: at initial state]%s\n", ANSI_RED, ANSI_RESET);
                }
            } else if (line[0] == 'k' || line[0] == 'K') {
                int kpid = atoi(line + 1);
                if (sim_inject_kill(&sim, kpid)) {
                    printf("%s[Injected Fault: Process %d Terminated]%s\n", ANSI_RED, kpid, ANSI_RESET);
                } else {
                    printf("%s[Failed to kill PID %d]%s\n", ANSI_RED, kpid, ANSI_RESET);
                }
                print_component_snapshot(&sim, gantt_mode);
            } else if (line[0] == 'd' || line[0] == 'D') {
                int new_f = atoi(line + 1);
                if (new_f > 0 && sim_inject_shrink_ram(&sim, new_f)) {
                    printf("%s[Injected Fault: RAM shrunk to %d frames]%s\n", ANSI_MAGENTA, new_f, ANSI_RESET);
                } else {
                    printf("%s[Failed to shrink RAM to %d frames]%s\n", ANSI_RED, new_f, ANSI_RESET);
                }
                print_component_snapshot(&sim, gantt_mode);
            } else {
                /* Default action: Step 1 tick */
                if (cli_history && cli_hist_count < CLI_MAX_HISTORY) {
                    sim_snapshot(&sim, &cli_history[cli_hist_count++]);
                }
                sim_step(&sim);
                print_component_snapshot(&sim, gantt_mode);
            }
        }
        if (cli_history) free(cli_history);
        return 0;
    }

    if (step_trace) {
        printf("\n%s%s[EXECUTING STEP TRACE]%s\n", ANSI_BOLD, ANSI_CYAN, ANSI_RESET);
        while (sim_step(&sim)) {
            printf("\n%s--- ADVANCED TO TICK %" PRIu64 " ---%s\n", ANSI_BOLD, sim.tick, ANSI_RESET);
            print_component_snapshot(&sim, gantt_mode);
        }
    } else {
        sim_run(&sim);
    }

    if (csv_mode) {
        print_csv(&sim);
    } else {
        print_component_snapshot(&sim, gantt_mode);
    }

    return 0;
}
