#include "prism/sim.h"
#include "prism/workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static void print_banner(void) {
    printf("====================================================================\n");
    printf("  PRISM: Process, Resource & Interprocess Simulation Model (v1.0)   \n");
    printf("  Deterministic OS Scheduler & Virtual Memory Engine               \n");
    printf("====================================================================\n");
}

static void print_gantt_chart(const sim_t *s) {
    printf("\n--- Core Execution Gantt Chart ---\n");
    uint32_t count = s->gantt_count;
    if (count > 60) count = 60; /* display first 60 ticks */

    for (int c = 0; c < s->num_cores; c++) {
        printf("Core %d: |", c);
        for (uint32_t t = 0; t < count; t++) {
            sim_pid_t p = s->gantt_history[t][c];
            if (p == -1) {
                printf(".");
            } else {
                printf("%c", 'A' + (p % 26));
            }
        }
        printf("|\n");
    }
    printf("Ticks :  ");
    for (uint32_t t = 0; t < count; t++) {
        if (t % 10 == 0) printf("%u", (t / 10) % 10);
        else printf(" ");
    }
    printf("\n");
}

static void print_metrics(const sim_t *s) {
    printf("\n--- Simulation Summary Metrics ---\n");
    printf("Total Ticks Executed : %" PRIu64 "\n", s->tick);
    printf("Total Context Switches: %u\n", s->total_context_switches);
    printf("Page Accesses / Faults: %u / %u (Fault Rate: %.1f%%)\n",
           s->total_page_accesses, s->total_page_faults,
           s->total_page_accesses > 0 ? (100.0 * s->total_page_faults / s->total_page_accesses) : 0.0);
    printf("Event Log FNV-1a Hash: 0x%016" PRIx64 "\n\n", s->log_hash);

    printf("%-6s %-12s %-6s %-6s %-6s %-6s %-6s %-6s\n",
           "PID", "Name", "Wait", "CPU", "I/O", "Fault", "Turn", "Faults");
    printf("------------------------------------------------------------\n");

    double avg_wait = 0, avg_turn = 0;
    for (int i = 0; i < s->num_processes; i++) {
        const pcb_t *p = &s->procs[i];
        int64_t turnaround = (p->finish_tick >= 0) ? (p->finish_tick - (int64_t)p->arrival_tick) : 0;
        avg_wait += p->total_wait_time;
        avg_turn += turnaround;

        printf("%-6d %-12s %-6u %-6u %-6u %-6u %-6" PRId64 " %-6u\n",
               p->pid, p->name, p->total_wait_time, p->total_cpu_time,
               p->total_io_time, p->total_fault_time, turnaround, p->num_page_faults);
    }

    if (s->num_processes > 0) {
        printf("------------------------------------------------------------\n");
        printf("Averages: Wait: %.2f | Turnaround: %.2f\n",
               avg_wait / s->num_processes, avg_turn / s->num_processes);
    }
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
            experiment_run = true;
        } else if (strcmp(argv[i], "--experiments") == 0 || strcmp(argv[i], "--all-exp") == 0) {
            run_experiment_1(workload_file);
            run_experiment_2();
            run_experiment_3();
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

    sim_run(&sim);

    if (csv_mode) {
        print_csv(&sim);
    } else {
        if (gantt_mode) print_gantt_chart(&sim);
        print_metrics(&sim);
    }

    return 0;
}
