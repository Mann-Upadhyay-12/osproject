#include "prism/metrics.h"
#include "prism/scenario.h"
#include "prism/sim_api.h"
#include "test_harness.h"
#include <math.h>
#include <string.h>

/* sim_t is ~0.5 MB; keep test instances off the stack. */
static sim_t g_base, g_v, g_w;

static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static double avg_wait_for(const char *path, sched_policy_t sp) {
    if (scenario_load(&g_base, NULL, path) != SIM_OK) return -1;
    if (sim_prepare_variant(&g_v, &g_base, sp, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP) != SIM_OK) return -1;
    sim_run_bounded(&g_v, 10000);
    sim_metrics_t m;
    sim_metrics_compute(&g_v, &m);
    return m.finished ? m.avg_wait : -1;
}

static uint32_t faults_for(const char *path, repl_policy_t rp, int frames) {
    if (scenario_load(&g_base, NULL, path) != SIM_OK) return 0;
    if (sim_prepare_variant(&g_v, &g_base, SIM_VARIANT_KEEP, rp, SIM_VARIANT_KEEP, frames) != SIM_OK) return 0;
    sim_run_bounded(&g_v, 10000);
    return g_v.total_page_faults;
}

static void test_textbook_schedulers(void) {
    TEST_CASE("textbook RR workload: average waiting time per scheduler");
    /* P1(0,8) P2(1,4) P3(2,9) P4(3,5), worked by hand. */
    ASSERT_TRUE(near(avg_wait_for("workloads/textbook_rr.txt", SCHED_FCFS), 35.0 / 4));
    ASSERT_TRUE(near(avg_wait_for("workloads/textbook_rr.txt", SCHED_SJF), 31.0 / 4));
    ASSERT_TRUE(near(avg_wait_for("workloads/textbook_rr.txt", SCHED_SRTF), 26.0 / 4));
    ASSERT_TRUE(near(avg_wait_for("workloads/textbook_rr.txt", SCHED_RR), 47.0 / 4));   /* q = 4 */
    TEST_PASS();
}

static void test_metrics_fields(void) {
    TEST_CASE("metrics: turnaround, response, utilisation, throughput");
    ASSERT_EQ(scenario_load(&g_base, NULL, "workloads/textbook_rr.txt"), SIM_OK);
    ASSERT_EQ(sim_prepare_variant(&g_v, &g_base, SCHED_FCFS, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP), SIM_OK);
    ASSERT_TRUE(sim_run_bounded(&g_v, 10000));
    sim_metrics_t m;
    sim_metrics_compute(&g_v, &m);
    ASSERT_EQ(m.processes, 4);
    ASSERT_EQ(m.completed, 4);
    ASSERT_TRUE(m.finished);
    ASSERT_EQ((int)m.makespan, 26);
    ASSERT_TRUE(near(m.avg_turnaround, (8 + 11 + 19 + 23) / 4.0));
    ASSERT_TRUE(near(m.avg_response, m.avg_wait));   /* non-preemptive, CPU only */
    ASSERT_EQ(m.max_wait, 18u);
    ASSERT_TRUE(near(m.cpu_util, 100.0));
    ASSERT_TRUE(near(m.throughput, 400.0 / 26));
    TEST_PASS();
}

static void test_replacement_textbook(void) {
    TEST_CASE("classic reference string: FIFO 15, LRU 12, OPT 9 faults (3 frames)");
    ASSERT_EQ(faults_for("workloads/classic_vm.txt", REPL_FIFO, 3), 15u);
    ASSERT_EQ(faults_for("workloads/classic_vm.txt", REPL_LRU, 3), 12u);
    ASSERT_EQ(faults_for("workloads/classic_vm.txt", REPL_OPT, 3), 9u);
    TEST_PASS();
}

static void test_belady_variant(void) {
    TEST_CASE("frame override shows Belady's anomaly (FIFO 3 -> 9, 4 -> 10)");
    ASSERT_EQ(faults_for("workloads/belady.txt", REPL_FIFO, 3), 9u);
    ASSERT_EQ(faults_for("workloads/belady.txt", REPL_FIFO, 4), 10u);
    ASSERT_TRUE(faults_for("workloads/belady.txt", REPL_LRU, 4) <= faults_for("workloads/belady.txt", REPL_LRU, 3));
    TEST_PASS();
}

static int g_obs_calls;
static void count_obs(const sim_event_t *ev, void *user) { (void)ev; (void)user; g_obs_calls++; }

static void test_variant_independent(void) {
    TEST_CASE("variant from a mid-run base = variant from a fresh base; base untouched");
    ASSERT_EQ(scenario_load(&g_base, NULL, "workloads/classic_vm.txt"), SIM_OK);
    sim_set_observer(&g_base, count_obs, NULL);

    ASSERT_EQ(sim_prepare_variant(&g_w, &g_base, SCHED_RR, REPL_FIFO, 3, 5), SIM_OK);
    sim_run_bounded(&g_w, 100000);

    sim_run_until(&g_base, 7);
    tick_t base_tick = g_base.tick;
    uint64_t base_hash = g_base.log_hash;
    int calls = g_obs_calls;

    ASSERT_EQ(sim_prepare_variant(&g_v, &g_base, SCHED_RR, REPL_FIFO, 3, 5), SIM_OK);
    ASSERT_EQ((int)g_v.tick, 0);
    ASSERT_EQ(g_v.default_quantum, 3);
    ASSERT_EQ(g_v.total_frames, 5);
    ASSERT_TRUE(g_v.observer == NULL);
    sim_run_bounded(&g_v, 100000);

    ASSERT_EQ(g_v.log_hash, g_w.log_hash);
    ASSERT_EQ(g_obs_calls, calls);              /* the variant never reports to the base's observer */
    ASSERT_EQ(g_base.tick, base_tick);
    ASSERT_EQ(g_base.log_hash, base_hash);
    ASSERT_EQ(sim_prepare_variant(&g_v, &g_base, SCHED_COUNT, SIM_VARIANT_KEEP, 0, SIM_VARIANT_KEEP), SIM_ERR_INVALID);
    ASSERT_EQ(sim_prepare_variant(&g_v, &g_base, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP, 0, PRISM_MAX_FRAMES + 1), SIM_ERR_INVALID);
    TEST_PASS();
}

static void test_bounded_run(void) {
    TEST_CASE("sim_run_bounded stops after max_ticks");
    ASSERT_EQ(scenario_load(&g_base, NULL, "workloads/textbook_rr.txt"), SIM_OK);
    ASSERT_FALSE(sim_run_bounded(&g_base, 5));
    ASSERT_EQ((int)g_base.tick, 5);
    sim_metrics_t m;
    sim_metrics_compute(&g_base, &m);
    ASSERT_FALSE(m.finished);
    ASSERT_EQ(m.completed, 0);
    TEST_PASS();
}

int main(void) {
    TEST_SUITE_BEGIN("Metrics & algorithm comparison");
    test_textbook_schedulers();
    test_metrics_fields();
    test_replacement_textbook();
    test_belady_variant();
    test_variant_independent();
    test_bounded_run();
    TEST_SUITE_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
