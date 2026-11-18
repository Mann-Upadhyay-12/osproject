#include "cli.h"
#include "prism/workload.h"
#include <inttypes.h>
#include <stdlib.h>

#define NUM_SCHEDULERS   8
#define NUM_REPLACERS    4
#define WIDE_RULE "========================================================================================"

static const sched_policy_t ALL_SCHEDULERS[NUM_SCHEDULERS] = {
    SCHED_FCFS, SCHED_SJF, SCHED_SRTF, SCHED_RR,
    SCHED_MLFQ, SCHED_ADAPTIVE, SCHED_CFS, SCHED_BFS
};

static const repl_policy_t ALL_REPLACERS[NUM_REPLACERS] = {
    REPL_FIFO, REPL_LRU, REPL_CLOCK, REPL_OPT
};

typedef struct {
    double   avg_wait;
    double   avg_turnaround;
    double   avg_response;
    uint32_t ctx_switches;
    double   cpu_util;
} sched_result_t;

sim_t *cli_sim_alloc(void) {
    sim_t *sim = malloc(sizeof(*sim));
    if (!sim) {
        fprintf(stderr, "prism-cli: out of memory\n");
        exit(1);
    }
    return sim;
}

static double single_core_util(const sim_t *sim) {
    return sim->tick > 0 ? 100.0 * sim->core_busy_ticks[0] / sim->tick : 0;
}

static sched_result_t run_scheduler(sched_policy_t policy, int frames, const char *workload_path) {
    sim_t *sim = cli_sim_alloc();
    sim_init(sim, 1, frames, 3, policy, REPL_LRU, 42);

    if (workload_path) {
        workload_config_t cfg = {0};
        workload_load_file(workload_path, sim, &cfg);
    } else {
        workload_generate_mixed(sim, 6, 42);
    }
    sim->sched_policy = policy;
    sim_run(sim);

    double total_wait = 0, total_turnaround = 0, total_response = 0;
    for (int i = 0; i < sim->num_processes; i++) {
        const pcb_t *p = &sim->procs[i];
        total_wait += p->total_wait_time;
        total_turnaround += p->finish_tick - (int64_t)p->arrival_tick;
        total_response += p->start_tick - (int64_t)p->arrival_tick;
    }
    int count = sim->num_processes > 0 ? sim->num_processes : 1;

    sched_result_t result = {
        .avg_wait = total_wait / count,
        .avg_turnaround = total_turnaround / count,
        .avg_response = total_response / count,
        .ctx_switches = sim->total_context_switches,
        .cpu_util = single_core_util(sim),
    };
    free(sim);
    return result;
}

static uint32_t count_faults(repl_policy_t policy, int frames, const char *workload_path) {
    sim_t *sim = cli_sim_alloc();
    sim_init(sim, 1, frames, 2, SCHED_FCFS, policy, 42);

    if (workload_path) {
        workload_load_file(workload_path, sim, NULL);
        sim->repl_policy = policy;
    } else {
        workload_generate_belady(sim);
    }
    sim_run(sim);

    uint32_t faults = sim->total_page_faults;
    free(sim);
    return faults;
}

static void print_sched_header(void) {
    printf("%-12s %-10s %-10s %-10s %-12s %-10s\n",
           "Scheduler", "Avg Wait", "Avg Turn", "Avg Resp", "Ctx Switches", "CPU Util %");
    printf("----------------------------------------------------------------------\n");
}

static void print_sched_row(const char *name, const sched_result_t *r) {
    printf("%-12s %-10.2f %-10.2f %-10.2f %-12u %-9.1f%%\n",
           name, r->avg_wait, r->avg_turnaround, r->avg_response, r->ctx_switches, r->cpu_util);
}

void experiment_schedulers(const char *workload_path) {
    static const char *names[NUM_SCHEDULERS] = {
        "FCFS", "SJF", "SRTF", "Round Robin", "MLFQ", "Adaptive", "CFS", "BFS"
    };

    printf("\n=== EXPERIMENT 1: Scheduler Comparison on Mixed Workload ===\n");
    print_sched_header();
    for (int i = 0; i < NUM_SCHEDULERS; i++) {
        sched_result_t result = run_scheduler(ALL_SCHEDULERS[i], 4, workload_path);
        print_sched_row(names[i], &result);
    }
    printf("\n");
}

void experiment_page_replacement(void) {
    printf("\n=== EXPERIMENT 2: Page Replacement vs. Frame Count & Belady's Anomaly ===\n");
    printf("Workload: Belady Reference String [1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5]\n");
    printf("%-8s %-10s %-10s %-10s %-10s\n", "Frames", "FIFO", "LRU", "Clock", "OPT");
    printf("----------------------------------------------------\n");

    for (int frames = 3; frames <= 5; frames++) {
        uint32_t faults[NUM_REPLACERS];
        for (int r = 0; r < NUM_REPLACERS; r++) faults[r] = count_faults(ALL_REPLACERS[r], frames, NULL);

        bool anomaly = frames == 4 && faults[0] > 9;
        printf("%-8d %-10u %-10u %-10u %-10u %s\n",
               frames, faults[0], faults[1], faults[2], faults[3],
               anomaly ? "<-- Belady Anomaly! (FIFO 3f=9 vs 4f=10)" : "");
    }

    printf("\nWorkload: Classic Reference String [7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1] (3 frames)\n");
    uint32_t classic[NUM_REPLACERS];
    for (int r = 0; r < NUM_REPLACERS; r++) {
        classic[r] = count_faults(ALL_REPLACERS[r], 3, "workloads/classic_vm.txt");
    }
    printf("Results: FIFO=%u (expected 15) | LRU=%u (expected 12) | Clock=%u | OPT=%u (expected 9)\n\n",
           classic[0], classic[1], classic[2], classic[3]);
}

void experiment_thrashing(void) {
    static const int frame_counts[] = {12, 6, 3};
    static const sched_policy_t policies[] = {SCHED_RR, SCHED_ADAPTIVE};
    static const char *policy_names[] = {"RoundRobin", "Adaptive"};
    uint64_t ticks[3][2];

    printf("\n=== EXPERIMENT 3: RAM Shrinking & Thrashing (Round Robin vs Adaptive) ===\n");
    printf("Scenario: 4 processes with heavy memory footprints. Frame allocation throttled.\n");
    printf("%-10s %-12s %-12s %-12s %-12s\n", "Frames", "Policy", "CPU Util", "Total Faults", "Total Ticks");
    printf("------------------------------------------------------------\n");

    sim_t *sim = cli_sim_alloc();
    for (int f = 0; f < 3; f++) {
        for (int p = 0; p < 2; p++) {
            sim_init(sim, 1, frame_counts[f], 8, policies[p], REPL_LRU, 42);
            workload_generate_thrashing(sim, 4);
            sim_run(sim);
            ticks[f][p] = sim->tick;

            printf("%-10d %-12s %-11.1f%% %-12u %-12" PRIu64 "\n",
                   frame_counts[f], policy_names[p], single_core_util(sim), sim->total_page_faults, sim->tick);
        }
    }
    free(sim);

    printf("\nObservation: once the combined working set (12 pages) no longer fits in RAM, almost\n"
           "every reference faults and the run takes far longer: the time goes to the disk.\n");

    bool adaptive_helps = ticks[1][1] < ticks[1][0] || ticks[2][1] < ticks[2][0];
    if (adaptive_helps) {
        printf("The Adaptive scheduler finishes sooner than Round Robin under memory pressure.\n\n");
    } else {
        printf("The Adaptive scheduler does not help here: all four processes thrash equally, so\n"
               "demoting the thrashing ones changes nothing. Scheduling cannot fix a RAM shortage.\n\n");
    }
}

void demo_rewind(void) {
    sim_t *original = cli_sim_alloc();
    sim_t *snapshot = cli_sim_alloc();
    sim_t *restored = cli_sim_alloc();

    printf("\n=== STEP BACKWARD & DETERMINISM VERIFICATION DEMO ===\n");
    sim_init(original, 1, 4, 3, SCHED_RR, REPL_LRU, 999);
    workload_generate_mixed(original, 4, 999);

    for (int t = 0; t < 15; t++) sim_step(original);
    printf("Simulation at Tick 15: Hash = 0x%016" PRIx64 "\n", original->log_hash);

    sim_snapshot(original, snapshot);
    printf("--> Took flat memcpy snapshot at tick 15\n");

    sim_run(original);
    printf("Simulation finished at Tick %" PRIu64 ": Full Run Hash = 0x%016" PRIx64 "\n",
           original->tick, original->log_hash);

    sim_restore(restored, snapshot);
    printf("--> Restored state to Tick 15: Restored Hash = 0x%016" PRIx64 "\n", restored->log_hash);

    sim_run(restored);
    printf("Restored run finished at Tick %" PRIu64 ": Full Run Hash = 0x%016" PRIx64 "\n",
           restored->tick, restored->log_hash);

    if (original->log_hash == restored->log_hash) {
        printf("\033[32m[SUCCESS] Byte-identical execution verified via FNV-1a log hash!\033[0m\n\n");
    } else {
        printf("\033[31m[FAILED] Hash mismatch between original and rewind runs!\033[0m\n\n");
    }

    free(original);
    free(snapshot);
    free(restored);
}

static void print_title(const char *color, const char *title, const char *subtitle) {
    printf("\n%s%s" WIDE_RULE "%s\n", color, ANSI_BOLD, ANSI_RESET);
    printf("%s%s%s%s\n", color, ANSI_BOLD, title, ANSI_RESET);
    printf("%s%s%s\n", ANSI_DIM, subtitle, ANSI_RESET);
    printf("%s%s" WIDE_RULE "%s\n", color, ANSI_BOLD, ANSI_RESET);
}

static void print_bar(const char *glyph, int length) {
    for (int i = 0; i < length; i++) printf("%s", glyph);
}

void demo_compare_schedulers(const char *workload_path) {
    static const char *names[NUM_SCHEDULERS] = {
        "FCFS", "SJF", "SRTF", "RoundRobin", "MLFQ", "Adaptive", "CFS", "BFS"
    };
    char subtitle[160];
    snprintf(subtitle, sizeof(subtitle),
             "  Workload: %s | Frames: 6 | Latency: 3 ticks | Identical Seed: 42                    ",
             workload_path ? workload_path : "Synthetic Mixed (6 procs)");
    print_title(ANSI_CYAN,
                "  SIDE-BY-SIDE VISUAL COMPARISON: ALL 8 CPU SCHEDULERS                                ",
                subtitle);

    sched_result_t results[NUM_SCHEDULERS];
    for (int i = 0; i < NUM_SCHEDULERS; i++) results[i] = run_scheduler(ALL_SCHEDULERS[i], 6, workload_path);

    printf("\n");
    print_sched_header();
    for (int i = 0; i < NUM_SCHEDULERS; i++) print_sched_row(names[i], &results[i]);

    printf("\n%s%s--- Visual Bar Chart: Average Waiting Time (Lower is Better) ---%s\n", ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
    double longest_wait = 1.0;
    for (int i = 0; i < NUM_SCHEDULERS; i++) {
        if (results[i].avg_wait > longest_wait) longest_wait = results[i].avg_wait;
    }
    for (int i = 0; i < NUM_SCHEDULERS; i++) {
        printf("  %-10s | ", names[i]);
        print_bar("█", (int)((results[i].avg_wait / longest_wait) * 35.0));
        printf(" %.2f ticks\n", results[i].avg_wait);
    }
    printf("\n");
}

void demo_compare_replacement(void) {
    static const char *names[NUM_REPLACERS] = {"FIFO", "LRU", "Clock", "OPT"};
    enum { MIN_FRAMES = 2, MAX_FRAMES = 6 };
    uint32_t faults[MAX_FRAMES + 1][NUM_REPLACERS];

    print_title(ANSI_CYAN,
                "  SIDE-BY-SIDE VISUAL COMPARISON: PAGE REPLACEMENT POLICIES                            ",
                "  Sweeping physical frame allocation from 2 to 6 frames across FIFO, LRU, Clock, and OPT");

    for (int frames = MIN_FRAMES; frames <= MAX_FRAMES; frames++) {
        for (int r = 0; r < NUM_REPLACERS; r++) faults[frames][r] = count_faults(ALL_REPLACERS[r], frames, NULL);
    }

    printf("\n%-8s %-12s %-12s %-12s %-12s\n", "Frames", "FIFO", "LRU", "Clock", "OPT");
    printf("------------------------------------------------------------\n");
    for (int frames = MIN_FRAMES; frames <= MAX_FRAMES; frames++) {
        const uint32_t *row = faults[frames];
        bool anomaly = frames == 4 && row[0] > faults[frames - 1][0];
        printf("%-8d %-12u %-12u %-12u %-12u %s\n", frames, row[0], row[1], row[2], row[3],
               anomaly ? "<-- Belady Anomaly! (FIFO 3f=9 vs 4f=10)" : "");
    }

    printf("\n%s%s--- Visual Fault Curve Comparison on 3 Frames ---%s\n", ANSI_BOLD, ANSI_MAGENTA, ANSI_RESET);
    for (int r = 0; r < NUM_REPLACERS; r++) {
        printf("  %-6s | ", names[r]);
        print_bar("▓", (int)faults[3][r]);
        printf(" %u faults\n", faults[3][r]);
    }
    printf("\n");
}

static void add_acquire(sim_t *sim, sim_pid_t pid, int32_t resource) {
    sim_add_burst(sim, pid, BURST_ACQUIRE, 1);
    sim->procs[pid].bursts[sim->procs[pid].num_bursts - 1].target_id = (int16_t)resource;
}

static void build_deadlock_scenario(sim_t *sim) {
    sim_init(sim, 2, 8, 2, SCHED_RR, REPL_LRU, 42);
    int32_t disk = sim_add_resource(sim, "Disk_R0", 1);
    int32_t gpu = sim_add_resource(sim, "GPU_R1", 1);

    sim_pid_t a = sim_add_process(sim, "Proc_A", 0);
    sim_add_burst(sim, a, BURST_CHECKPOINT, 1);
    add_acquire(sim, a, disk);
    sim_add_burst(sim, a, BURST_CPU, 2);
    add_acquire(sim, a, gpu);
    sim_add_burst(sim, a, BURST_CPU, 5);

    sim_pid_t b = sim_add_process(sim, "Proc_B", 0);
    sim_add_burst(sim, b, BURST_CHECKPOINT, 1);
    sim_add_burst(sim, b, BURST_CPU, 1);
    add_acquire(sim, b, gpu);
    sim_add_burst(sim, b, BURST_CPU, 1);
    add_acquire(sim, b, disk);
    sim_add_burst(sim, b, BURST_CPU, 5);
}

void demo_deadlock(void) {
    print_title(ANSI_RED,
                "  DEADLOCK ENGINE & CHECKPOINT ROLLBACK RECOVERY DEMO                                  ",
                "  Demonstrating circular wait condition, WFG cycle detection, and checkpoint rollback  ");

    sim_t *sim = cli_sim_alloc();
    build_deadlock_scenario(sim);

    printf("\n%sStep 1: Advancing simulation into circular wait condition...%s\n", ANSI_BOLD, ANSI_RESET);
    for (int t = 0; t < 6; t++) sim_step(sim);
    report_resources(sim);

    if (sim->deadlock_detected) {
        printf("\n%s%sStep 2: Deadlock detected! Automatic Recovery via Checkpoint Rollback...%s\n",
               ANSI_BOLD, ANSI_YELLOW, ANSI_RESET);
        sim_recover_deadlock_rollback(sim);
        printf("%s--> Rolled back victim process to Checkpoint; released held resources.%s\n", ANSI_GREEN, ANSI_RESET);
        report_resources(sim);

        printf("\n%sStep 3: Resuming simulation to completion after recovery...%s\n", ANSI_BOLD, ANSI_RESET);
        sim_run(sim);
        printf("%s[SUCCESS] Simulation cleanly finished at tick %" PRIu64 " after resolving deadlock!%s\n\n",
               ANSI_GREEN, sim->tick, ANSI_RESET);
    }
    free(sim);
}
