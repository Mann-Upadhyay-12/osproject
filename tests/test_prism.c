#include "prism/sim.h"
#include "prism/workload.h"
#include "test_harness.h"
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t faults_for_reference_string(repl_policy_t policy, uint32_t frames, const uint16_t *refs, size_t count) {
    static sim_t sim;
    sim_init(&sim, 1, frames, 1, SCHED_FCFS, policy, 42);
    sim_pid_t pid = sim_add_process(&sim, "RefString", 0);
    sim_add_burst(&sim, pid, BURST_CPU, (uint16_t)count);
    for (size_t i = 0; i < count; i++) sim_add_page_ref(&sim, pid, refs[i]);
    sim_run(&sim);
    return sim.total_page_faults;
}

static sim_pid_t add_cpu_process(sim_t *sim, const char *name, tick_t arrival, uint16_t burst) {
    sim_pid_t pid = sim_add_process(sim, name, arrival);
    sim_add_burst(sim, pid, BURST_CPU, burst);
    return pid;
}

static void add_targeted_burst(sim_t *sim, sim_pid_t pid, burst_kind_t kind, int target) {
    sim_add_burst(sim, pid, kind, 1);
    pcb_t *p = &sim->procs[pid];
    p->bursts[p->num_bursts - 1].target_id = (int16_t)target;
}

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

    for (int t = 0; t < 10; t++) {
        sim_step(&sim_full);
    }
    sim_snapshot(&sim_full, &sim_snap);

    sim_run(&sim_full);
    uint64_t expected_hash = sim_full.log_hash;

    sim_restore(&sim_resumed, &sim_snap);
    sim_run(&sim_resumed);

    ASSERT_EQ(expected_hash, sim_resumed.log_hash);
    TEST_PASS();
}

static void test_classic_page_replacement(void) {
    TEST_CASE("Textbook Classic Page Replacement (FIFO=15, LRU=12, OPT=9)");

    static const uint16_t refs[] = {7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1};

    ASSERT_EQ(faults_for_reference_string(REPL_FIFO, 3, refs, ARRAY_LEN(refs)), 15);
    ASSERT_EQ(faults_for_reference_string(REPL_LRU, 3, refs, ARRAY_LEN(refs)), 12);
    ASSERT_EQ(faults_for_reference_string(REPL_OPT, 3, refs, ARRAY_LEN(refs)), 9);

    TEST_PASS();
}

static void test_belady_anomaly(void) {
    TEST_CASE("Belady's Anomaly (FIFO 3 frames = 9 faults, 4 frames = 10 faults)");

    static const uint16_t refs[] = {1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5};

    uint32_t faults_3frames = faults_for_reference_string(REPL_FIFO, 3, refs, ARRAY_LEN(refs));
    uint32_t faults_4frames = faults_for_reference_string(REPL_FIFO, 4, refs, ARRAY_LEN(refs));

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

    for (int i = 0; i < 5; i++) sim_step(&sim);

    bool killed = sim_inject_kill(&sim, 1);
    ASSERT_TRUE(killed);
    ASSERT_EQ(sim.procs[1].state, PROC_TERMINATED);

    bool shrunk = sim_inject_shrink_ram(&sim, 4);
    ASSERT_TRUE(shrunk);
    ASSERT_EQ(sim.total_frames, 4);

    sim_run(&sim);
    ASSERT_TRUE(sim_is_done(&sim));

    TEST_PASS();
}

static void test_sjf_srtf_schedulers(void) {
    TEST_CASE("Textbook SJF and SRTF Scheduling Verification");

    {
        sim_t sim;
        sim_init(&sim, 1, 16, 1, SCHED_SJF, REPL_LRU, 1);
        sim_pid_t p1 = add_cpu_process(&sim, "P1", 0, 8);
        sim_pid_t p2 = add_cpu_process(&sim, "P2", 1, 4);
        sim_pid_t p3 = add_cpu_process(&sim, "P3", 2, 9);
        sim_pid_t p4 = add_cpu_process(&sim, "P4", 3, 5);
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

    {
        sim_t sim;
        sim_init(&sim, 1, 16, 1, SCHED_SRTF, REPL_LRU, 1);
        sim_pid_t p1 = add_cpu_process(&sim, "P1", 0, 8);
        sim_pid_t p2 = add_cpu_process(&sim, "P2", 1, 4);
        sim_pid_t p3 = add_cpu_process(&sim, "P3", 2, 9);
        sim_pid_t p4 = add_cpu_process(&sim, "P4", 3, 5);
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

    {
        sim_t sim;
        sim_init(&sim, 1, 8, 1, SCHED_CFS, REPL_LRU, 42);
        sim_pid_t p1 = add_cpu_process(&sim, "CFS_A", 0, 10);
        sim_pid_t p2 = add_cpu_process(&sim, "CFS_B", 0, 10);
        sim_run(&sim);

        ASSERT_TRUE(sim.procs[p1].total_cpu_time == 10);
        ASSERT_TRUE(sim.procs[p2].total_cpu_time == 10);
        ASSERT_EQ(sim.tick, 20);
        ASSERT_TRUE(sim.procs[p1].total_wait_time >= 8 && sim.procs[p1].total_wait_time <= 10);
        ASSERT_TRUE(sim.procs[p2].total_wait_time >= 8 && sim.procs[p2].total_wait_time <= 10);
    }

    {
        sim_t sim;
        sim_init(&sim, 1, 8, 1, SCHED_BFS, REPL_LRU, 42);
        sim_pid_t p_high = sim_add_process(&sim, "BFS_High", 0);
        sim.procs[p_high].priority = 0;
        sim_add_burst(&sim, p_high, BURST_CPU, 6);

        sim_pid_t p_low = sim_add_process(&sim, "BFS_Low", 0);
        sim.procs[p_low].priority = 3;
        sim_add_burst(&sim, p_low, BURST_CPU, 6);

        sim_run(&sim);
        ASSERT_EQ(sim.tick, 12);
        ASSERT_TRUE(sim.procs[p_high].finish_tick < sim.procs[p_low].finish_tick);
    }

    TEST_PASS();
}

static void test_cfs_weights_and_sleepers(void) {
    TEST_CASE("CFS: CPU share follows weight; a waking sleeper is not sent to the back");

    {
        sim_t sim;
        sim_init(&sim, 1, 8, 1, SCHED_CFS, REPL_LRU, 42);
        sim_pid_t hi = sim_add_process(&sim, "W_Hi", 0);
        sim_add_burst(&sim, hi, BURST_CPU, 64);
        sim_pid_t lo = sim_add_process(&sim, "W_Lo", 0);
        sim.procs[lo].priority = 4;
        sim_add_burst(&sim, lo, BURST_CPU, 64);

        while (sim.procs[hi].state != PROC_TERMINATED && sim_step(&sim)) { }
        ASSERT_TRUE(sim.procs[hi].state == PROC_TERMINATED);
        ASSERT_TRUE(sim.procs[lo].total_cpu_time >= 18 && sim.procs[lo].total_cpu_time <= 22);
        sim_run(&sim);
        ASSERT_EQ(sim.tick, 128);
    }

    {
        sim_t sim;
        sim_init(&sim, 1, 8, 1, SCHED_CFS, REPL_LRU, 42);
        sim_pid_t a = add_cpu_process(&sim, "Hog_A", 0, 40);
        sim_pid_t b = add_cpu_process(&sim, "Hog_B", 0, 40);
        sim_pid_t sl = sim_add_process(&sim, "Sleeper", 0);
        for (int i = 0; i < 6; i++) {
            sim_add_burst(&sim, sl, BURST_CPU, 1);
            if (i < 5) sim_add_burst(&sim, sl, BURST_IO, 7);
        }

        uint64_t waited = 0, longest = 0;
        while (sim.procs[sl].state != PROC_TERMINATED) {
            bool waiting_after_wake = sim.procs[sl].state == PROC_READY && sim.procs[sl].total_cpu_time > 0;
            waited = waiting_after_wake ? waited + 1 : 0;
            if (waited > longest) longest = waited;
            if (!sim_step(&sim)) break;
        }
        ASSERT_TRUE(sim.procs[sl].state == PROC_TERMINATED);
        ASSERT_TRUE(sim.procs[a].state != PROC_TERMINATED && sim.procs[b].state != PROC_TERMINATED);
        ASSERT_TRUE(longest <= 2);
    }

    TEST_PASS();
}

static void test_resource_manager_and_deadlock(void) {
    TEST_CASE("Resource Manager, WFG Cycle Detection & Checkpoint Rollback");

    sim_t sim;
    sim_init(&sim, 2, 8, 2, SCHED_RR, REPL_LRU, 42);

    int32_t r0 = sim_add_resource(&sim, "Disk", 1);
    int32_t r1 = sim_add_resource(&sim, "GPU", 1);
    ASSERT_EQ(r0, 0);
    ASSERT_EQ(r1, 1);

    sim_pid_t p0 = sim_add_process(&sim, "P0", 0);
    sim_add_burst(&sim, p0, BURST_CHECKPOINT, 1);
    add_targeted_burst(&sim, p0, BURST_ACQUIRE, r0);
    sim_add_burst(&sim, p0, BURST_CPU, 2);
    add_targeted_burst(&sim, p0, BURST_ACQUIRE, r1);
    sim_add_burst(&sim, p0, BURST_CPU, 4);

    sim_pid_t p1 = sim_add_process(&sim, "P1", 0);
    sim_add_burst(&sim, p1, BURST_CHECKPOINT, 1);
    sim_add_burst(&sim, p1, BURST_CPU, 1);
    add_targeted_burst(&sim, p1, BURST_ACQUIRE, r1);
    sim_add_burst(&sim, p1, BURST_CPU, 1);
    add_targeted_burst(&sim, p1, BURST_ACQUIRE, r0);
    sim_add_burst(&sim, p1, BURST_CPU, 4);

    for (int t = 0; t < 6; t++) {
        sim_step(&sim);
    }

    ASSERT_TRUE(sim.deadlock_detected);
    ASSERT_EQ(sim.deadlock_cycle_len, 2);

    bool recovered = sim_recover_deadlock_rollback(&sim);
    ASSERT_TRUE(recovered);
    ASSERT_FALSE(sim.deadlock_detected);

    sim_run(&sim);
    ASSERT_TRUE(sim_is_done(&sim));

    TEST_PASS();
}

static void test_ipc_message_queue(void) {
    TEST_CASE("IPC Message Queue Producer-Consumer Synchronization");

    sim_t sim;
    sim_init(&sim, 1, 8, 2, SCHED_RR, REPL_LRU, 42);

    sim_pid_t p_prod = sim_add_process(&sim, "Producer", 0);
    sim_pid_t p_cons = sim_add_process(&sim, "Consumer", 0);

    add_targeted_burst(&sim, p_cons, BURST_IPC_RECV, p_prod);
    sim_add_burst(&sim, p_cons, BURST_CPU, 3);

    sim_add_burst(&sim, p_prod, BURST_CPU, 2);
    add_targeted_burst(&sim, p_prod, BURST_IPC_SEND, p_cons);
    sim.procs[p_prod].bursts[1].extra_data = 999;
    sim_add_burst(&sim, p_prod, BURST_CPU, 2);

    sim_run(&sim);
    ASSERT_TRUE(sim_is_done(&sim));
    ASSERT_EQ(sim.procs[p_cons].state, PROC_TERMINATED);
    ASSERT_EQ(sim.procs[p_prod].state, PROC_TERMINATED);

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
    test_cfs_weights_and_sleepers();
    test_resource_manager_and_deadlock();
    test_ipc_message_queue();

    TEST_SUITE_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
