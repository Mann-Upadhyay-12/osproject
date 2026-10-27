#include "prism/sim.h"
#include "prism/workload.h"
#include "test_harness.h"
#include <string.h>

static void test_determinism(void) {
    TEST_CASE("Log Determinism & FNV-1a Hash Invariance");

    sim_t sim1, sim2;
    sim_init(&sim1, 2, 4, 3, SCHED_RR, REPL_LRU, 12345);
    sim_init(&sim2, 2, 4, 3, SCHED_RR, REPL_LRU, 12345);

    workload_generate_mixed(&sim1, 4, 12345);
    workload_generate_mixed(&sim2, 4, 12345);

    sim_run(&sim1);
    sim_run(&sim2);

    ASSERT_EQ(sim1.tick, sim2.tick);
    ASSERT_EQ(sim1.total_context_switches, sim2.total_context_switches);
    ASSERT_EQ(sim1.total_page_faults, sim2.total_page_faults);
    ASSERT_EQ(sim1.log_hash, sim2.log_hash);

    TEST_PASS();
}

static void test_snapshot_rewind(void) {
    TEST_CASE("Flat State Memcpy Snapshot & Rewind Verification");

    sim_t sim_full, sim_snap, sim_resumed;
    sim_init(&sim_full, 1, 4, 2, SCHED_MLFQ, REPL_FIFO, 777);
    workload_generate_mixed(&sim_full, 3, 777);

    /* Step 10 ticks and snapshot */
    for (int t = 0; t < 10; t++) {
        sim_step(&sim_full);
    }
    sim_snapshot(&sim_full, &sim_snap);

    /* Finish full run */
    sim_run(&sim_full);
    uint64_t expected_hash = sim_full.log_hash;

    /* Restore from snapshot and run */
    sim_restore(&sim_resumed, &sim_snap);
    sim_run(&sim_resumed);

    ASSERT_EQ(expected_hash, sim_resumed.log_hash);
    TEST_PASS();
}

static void test_classic_page_replacement(void) {
    TEST_CASE("Textbook Classic Page Replacement (FIFO=15, LRU=12, OPT=9)");

    static const uint16_t refs[] = {7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1};
    size_t nrefs = sizeof(refs) / sizeof(refs[0]);

    /* Test FIFO */
    {
        sim_t sim;
        sim_init(&sim, 1, 3, 1, SCHED_FCFS, REPL_FIFO, 42);
        sim_pid_t pid = sim_add_process(&sim, "FIFO_P", 0);
        sim_add_burst(&sim, pid, BURST_CPU, (uint16_t)nrefs);
        for (size_t i = 0; i < nrefs; i++) sim_add_page_ref(&sim, pid, refs[i]);
        sim_run(&sim);
        ASSERT_EQ(sim.total_page_faults, 15);
    }

    /* Test LRU */
    {
        sim_t sim;
        sim_init(&sim, 1, 3, 1, SCHED_FCFS, REPL_LRU, 42);
        sim_pid_t pid = sim_add_process(&sim, "LRU_P", 0);
        sim_add_burst(&sim, pid, BURST_CPU, (uint16_t)nrefs);
        for (size_t i = 0; i < nrefs; i++) sim_add_page_ref(&sim, pid, refs[i]);
        sim_run(&sim);
        ASSERT_EQ(sim.total_page_faults, 12);
    }

    /* Test OPT */
    {
        sim_t sim;
        sim_init(&sim, 1, 3, 1, SCHED_FCFS, REPL_OPT, 42);
        sim_pid_t pid = sim_add_process(&sim, "OPT_P", 0);
        sim_add_burst(&sim, pid, BURST_CPU, (uint16_t)nrefs);
        for (size_t i = 0; i < nrefs; i++) sim_add_page_ref(&sim, pid, refs[i]);
        sim_run(&sim);
        ASSERT_EQ(sim.total_page_faults, 9);
    }

    TEST_PASS();
}

static void test_belady_anomaly(void) {
    TEST_CASE("Belady's Anomaly (FIFO 3 frames = 9 faults, 4 frames = 10 faults)");

    static const uint16_t refs[] = {1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5};
    size_t nrefs = sizeof(refs) / sizeof(refs[0]);

    uint32_t faults_3frames = 0;
    {
        sim_t sim;
        sim_init(&sim, 1, 3, 1, SCHED_FCFS, REPL_FIFO, 42);
        sim_pid_t pid = sim_add_process(&sim, "Belady3", 0);
        sim_add_burst(&sim, pid, BURST_CPU, (uint16_t)nrefs);
        for (size_t i = 0; i < nrefs; i++) sim_add_page_ref(&sim, pid, refs[i]);
        sim_run(&sim);
        faults_3frames = sim.total_page_faults;
    }

    uint32_t faults_4frames = 0;
    {
        sim_t sim;
        sim_init(&sim, 1, 4, 1, SCHED_FCFS, REPL_FIFO, 42);
        sim_pid_t pid = sim_add_process(&sim, "Belady4", 0);
        sim_add_burst(&sim, pid, BURST_CPU, (uint16_t)nrefs);
        for (size_t i = 0; i < nrefs; i++) sim_add_page_ref(&sim, pid, refs[i]);
        sim_run(&sim);
        faults_4frames = sim.total_page_faults;
    }

    ASSERT_EQ(faults_3frames, 9);
    ASSERT_EQ(faults_4frames, 10);
    ASSERT_TRUE(faults_4frames > faults_3frames);

    TEST_PASS();
}

static void test_fault_injection(void) {
    TEST_CASE("Fault Injection: Process Termination & Dynamic RAM Shrink");

    sim_t sim;
    sim_init(&sim, 2, 8, 2, SCHED_RR, REPL_LRU, 42);
    workload_generate_mixed(&sim, 4, 42);

    /* Step 5 ticks */
    for (int i = 0; i < 5; i++) sim_step(&sim);

    /* Kill process 1 */
    bool killed = sim_inject_kill(&sim, 1);
    ASSERT_TRUE(killed);
    ASSERT_EQ(sim.procs[1].state, PROC_TERMINATED);

    /* Shrink RAM from 8 frames to 4 */
    bool shrunk = sim_inject_shrink_ram(&sim, 4);
    ASSERT_TRUE(shrunk);
    ASSERT_EQ(sim.total_frames, 4);

    /* Ensure simulation runs to completion cleanly */
    sim_run(&sim);
    ASSERT_TRUE(sim_is_done(&sim));

    TEST_PASS();
}

static void test_sjf_srtf_schedulers(void) {
    TEST_CASE("Textbook SJF and SRTF Scheduling Verification");

    /*
     * Classic Textbook Problem:
     * P1 arrival=0 burst=8
     * P2 arrival=1 burst=4
     * P3 arrival=2 burst=9
     * P4 arrival=3 burst=5
     *
     * Non-preemptive SJF:
     * P1 runs [0..8]. At t=8, ready: P2(4), P4(5), P3(9) -> P2 runs [8..12]
     * At t=12, ready: P4(5), P3(9) -> P4 runs [12..17]
     * At t=17, ready: P3(9) -> P3 runs [17..26]
     * Finish: P1=8, P2=12, P4=17, P3=26.
     * Turnaround: P1=8, P2=11, P3=24, P4=14. Avg = (8+11+24+14)/4 = 14.25
     * Wait: P1=0, P2=7, P3=15, P4=9. Avg = (0+7+15+9)/4 = 7.75
     */
    {
        sim_t sim;
        sim_init(&sim, 1, 16, 1, SCHED_SJF, REPL_LRU, 1);
        sim_pid_t p1 = sim_add_process(&sim, "P1", 0); sim_add_burst(&sim, p1, BURST_CPU, 8);
        sim_pid_t p2 = sim_add_process(&sim, "P2", 1); sim_add_burst(&sim, p2, BURST_CPU, 4);
        sim_pid_t p3 = sim_add_process(&sim, "P3", 2); sim_add_burst(&sim, p3, BURST_CPU, 9);
        sim_pid_t p4 = sim_add_process(&sim, "P4", 3); sim_add_burst(&sim, p4, BURST_CPU, 5);
        sim_run(&sim);

        ASSERT_EQ(sim.procs[p1].finish_tick, 8);
        ASSERT_EQ(sim.procs[p2].finish_tick, 12);
        ASSERT_EQ(sim.procs[p4].finish_tick, 17);
        ASSERT_EQ(sim.procs[p3].finish_tick, 26);
        ASSERT_EQ(sim.procs[p1].total_wait_time, 0);
        ASSERT_EQ(sim.procs[p2].total_wait_time, 7);
        ASSERT_EQ(sim.procs[p4].total_wait_time, 9);
        ASSERT_EQ(sim.procs[p3].total_wait_time, 15);
    }

    /*
     * Preemptive SRTF:
     * P1 runs [0..1] (rem=7). At t=1, P2 arrives (rem=4 < 7) -> PREEMPTS P1!
     * P2 runs [1..5] (finishes at 5).
     * At t=5, ready: P4(rem 5), P1(rem 7), P3(rem 9). P4 runs [5..10] (finishes at 10).
     * At t=10, ready: P1(rem 7), P3(rem 9). P1 runs [10..17] (finishes at 17).
     * At t=17, ready: P3(rem 9). P3 runs [17..26] (finishes at 26).
     * Finish: P2=5, P4=10, P1=17, P3=26.
     * Wait: P1=(10-1)=9, P2=0, P4=(5-3)=2, P3=(17-2)=15.
     */
    {
        sim_t sim;
        sim_init(&sim, 1, 16, 1, SCHED_SRTF, REPL_LRU, 1);
        sim_pid_t p1 = sim_add_process(&sim, "P1", 0); sim_add_burst(&sim, p1, BURST_CPU, 8);
        sim_pid_t p2 = sim_add_process(&sim, "P2", 1); sim_add_burst(&sim, p2, BURST_CPU, 4);
        sim_pid_t p3 = sim_add_process(&sim, "P3", 2); sim_add_burst(&sim, p3, BURST_CPU, 9);
        sim_pid_t p4 = sim_add_process(&sim, "P4", 3); sim_add_burst(&sim, p4, BURST_CPU, 5);
        sim_run(&sim);

        ASSERT_EQ(sim.procs[p2].finish_tick, 5);
        ASSERT_EQ(sim.procs[p4].finish_tick, 10);
        ASSERT_EQ(sim.procs[p1].finish_tick, 17);
        ASSERT_EQ(sim.procs[p3].finish_tick, 26);
        ASSERT_EQ(sim.procs[p2].total_wait_time, 0);
        ASSERT_EQ(sim.procs[p4].total_wait_time, 2);
        ASSERT_EQ(sim.procs[p1].total_wait_time, 9);
        ASSERT_EQ(sim.procs[p3].total_wait_time, 15);
    }

    TEST_PASS();
}

static void test_cfs_bfs_schedulers(void) {
    TEST_CASE("CFS and BFS Schedulers Invariance & Fairness");

    /* CFS with two identical CPU bound tasks -> perfect interleaved share */
    {
        sim_t sim;
        sim_init(&sim, 1, 8, 1, SCHED_CFS, REPL_LRU, 42);
        sim_pid_t p1 = sim_add_process(&sim, "CFS_A", 0); sim_add_burst(&sim, p1, BURST_CPU, 10);
        sim_pid_t p2 = sim_add_process(&sim, "CFS_B", 0); sim_add_burst(&sim, p2, BURST_CPU, 10);
        sim_run(&sim);

        ASSERT_TRUE(sim.procs[p1].total_cpu_time == 10);
        ASSERT_TRUE(sim.procs[p2].total_cpu_time == 10);
        /* Both finish at tick 20 and have equal wait time due to fair 2-tick slices */
        ASSERT_EQ(sim.tick, 20);
        ASSERT_TRUE(sim.procs[p1].total_wait_time >= 8 && sim.procs[p1].total_wait_time <= 10);
        ASSERT_TRUE(sim.procs[p2].total_wait_time >= 8 && sim.procs[p2].total_wait_time <= 10);
    }

    /* BFS with high priority vs low priority deadline behavior */
    {
        sim_t sim;
        sim_init(&sim, 1, 8, 1, SCHED_BFS, REPL_LRU, 42);
        sim_pid_t p_high = sim_add_process(&sim, "BFS_High", 0);
        sim.procs[p_high].priority = 0; /* Earlier deadline */
        sim_add_burst(&sim, p_high, BURST_CPU, 6);

        sim_pid_t p_low = sim_add_process(&sim, "BFS_Low", 0);
        sim.procs[p_low].priority = 3;  /* Later deadline */
        sim_add_burst(&sim, p_low, BURST_CPU, 6);

        sim_run(&sim);
        ASSERT_EQ(sim.tick, 12);
        /* Higher priority process finishes earlier */
        ASSERT_TRUE(sim.procs[p_high].finish_tick < sim.procs[p_low].finish_tick);
    }

    TEST_PASS();
}

int main(void) {
    TEST_SUITE_BEGIN("PRISM Core, Scheduling & VM Suite");

    test_determinism();
    test_snapshot_rewind();
    test_classic_page_replacement();
    test_belady_anomaly();
    test_fault_injection();
    test_sjf_srtf_schedulers();
    test_cfs_bfs_schedulers();

    TEST_SUITE_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
