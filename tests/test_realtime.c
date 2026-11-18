#include "test_harness.h"
#include "prism/realtime.h"
#include "prism/scenario.h"
#include "prism/sim_api.h"
#include "prism/workload.h"
#include <math.h>
#include <string.h>

static sim_t *load(const char *path, const char *sched) {
    sim_t *s = malloc(sizeof(sim_t));
    if (!s) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    sim_init(s, 1, 8, 3, SCHED_RR, REPL_LRU, 42);
    if (!workload_load_file(path, s, NULL)) {
        free(s);
        return NULL;
    }
    sim_set_policy(s, s->sched_policy, s->repl_policy);
    if (sched) sim_set_sched(s, sched, 0);
    return s;
}

static const sim_event_t *find_event(const sim_t *s, event_type_t type, sim_pid_t pid, int job) {
    for (uint32_t i = 0; i < s->event_count; i++) {
        const sim_event_t *e = &s->event_log[i];
        if (e->type == type && e->pid == pid && e->detail1 == job) return e;
    }
    return NULL;
}

static void test_rm_textbook_schedulable(void) {
    TEST_CASE("RM: periods 50/100, bursts 20/35 meet every deadline; P2's first job ends at t75");

    sim_t *s = load("workloads/rt_rate_monotonic.txt", NULL);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->sched_policy, SCHED_RM);
    sim_run(s);

    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->completed_processes, 2);
    ASSERT_EQ(s->total_deadline_misses, 0);
    ASSERT_EQ(s->procs[0].jobs_done, 8);
    ASSERT_EQ(s->procs[1].jobs_done, 4);

    const sim_event_t *p2_job1 = find_event(s, EVENT_JOB_COMPLETE, 1, 1);
    ASSERT_TRUE(p2_job1 != NULL);
    ASSERT_EQ(p2_job1->detail2, 75 - 100);
    const sim_event_t *p1_job2 = find_event(s, EVENT_JOB_RELEASE, 0, 2);
    ASSERT_TRUE(p1_job2 != NULL);
    ASSERT_EQ(p1_job2->tick, 50);
    ASSERT_EQ(p1_job2->detail2, 100);
    free(s);
    TEST_PASS();
}

static void test_rm_misses_edf_does_not(void) {
    TEST_CASE("RM: periods 50/80, bursts 25/35: P2 misses at t80 and ends at t85; EDF misses nothing");

    sim_t *s = load("workloads/rt_deadline_miss.txt", NULL);
    ASSERT_TRUE(s != NULL);
    sim_run(s);
    const sim_event_t *miss = find_event(s, EVENT_DEADLINE_MISS, 1, 1);
    ASSERT_TRUE(miss != NULL);
    ASSERT_EQ(miss->tick, 80);
    ASSERT_EQ(miss->detail2, 80);
    const sim_event_t *late = find_event(s, EVENT_JOB_COMPLETE, 1, 1);
    ASSERT_TRUE(late != NULL);
    ASSERT_EQ(late->detail2, 5);
    ASSERT_TRUE(s->total_deadline_misses >= 1);
    free(s);

    s = load("workloads/rt_deadline_miss.txt", "edf");
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->sched_policy, SCHED_EDF);
    sim_run(s);
    ASSERT_EQ(s->total_deadline_misses, 0);
    ASSERT_EQ(s->procs[0].jobs_done, 8);
    ASSERT_EQ(s->procs[1].jobs_done, 5);
    ASSERT_TRUE(s->tick <= 400);
    free(s);
    TEST_PASS();
}

static void test_schedulability_analysis(void) {
    TEST_CASE("Analysis: U 0.75 under the RM bound 0.828; U 0.9375 above it but EDF-feasible");

    rt_analysis_t a;
    sim_t *s = load("workloads/rt_rate_monotonic.txt", NULL);
    rt_analyze(s, &a);
    ASSERT_EQ(a.tasks, 2);
    ASSERT_TRUE(fabs(a.utilization - 0.75) < 1e-9);
    ASSERT_TRUE(fabs(a.rm_bound - 2.0 * (sqrt(2.0) - 1.0)) < 1e-9);
    ASSERT_TRUE(a.rm_guaranteed && a.edf_feasible);
    ASSERT_TRUE(a.hyperperiod == 100);
    free(s);

    s = load("workloads/rt_deadline_miss.txt", NULL);
    rt_analyze(s, &a);
    ASSERT_TRUE(fabs(a.utilization - 0.9375) < 1e-9);
    ASSERT_FALSE(a.rm_guaranteed);
    ASSERT_TRUE(a.edf_feasible);
    ASSERT_TRUE(a.hyperperiod == 400);
    free(s);
    TEST_PASS();
}

static sim_t *new_sim(sched_policy_t policy) {
    sim_t *s = malloc(sizeof(sim_t));
    if (!s) exit(1);
    sim_init(s, 1, 8, 3, policy, REPL_LRU, 42);
    return s;
}

static sim_pid_t spawn(sim_t *s, const char *name, tick_t arrival, int period, int jobs, int cpu) {
    proc_spec_t spec;
    proc_spec_init(&spec, name);
    spec.arrival = arrival;
    spec.period = (uint16_t)period;
    spec.jobs = (uint16_t)jobs;
    proc_spec_add_burst(&spec, BURST_CPU, (uint16_t)cpu);
    return sim_spawn_process(s, &spec);
}

static void test_background_and_sleeping(void) {
    TEST_CASE("RM/EDF: aperiodic work runs only in idle time; periodic tasks sleep between jobs");

    sim_t *s = new_sim(SCHED_EDF);
    sim_pid_t task = spawn(s, "Task", 0, 10, 3, 4);
    sim_pid_t batch = spawn(s, "Batch", 0, 0, 0, 12);
    ASSERT_TRUE(task >= 0 && batch >= 0);

    for (int t = 0; t < 4; t++) sim_step(s);
    ASSERT_EQ(s->procs[task].state, PROC_SLEEPING);
    ASSERT_EQ(s->procs[batch].total_cpu_time, 0);
    for (int t = 4; t < 11; t++) sim_step(s);
    ASSERT_EQ(s->procs[task].jobs_done, 1);
    ASSERT_EQ(s->running_pid[0], task);
    ASSERT_EQ(s->procs[batch].total_cpu_time, 6);

    sim_run(s);
    ASSERT_EQ(s->procs[task].jobs_done, 3);
    ASSERT_EQ(s->procs[task].finish_tick, 24);
    ASSERT_EQ(s->procs[batch].finish_tick, 20);
    ASSERT_EQ(s->total_deadline_misses, 0);
    free(s);
    TEST_PASS();
}

static void test_periodic_spec_round_trip(void) {
    TEST_CASE("Periodic tasks: validation, save/load, reset keep period, deadline and jobs");

    proc_spec_t spec;
    proc_spec_init(&spec, "Bad");
    proc_spec_add_burst(&spec, BURST_CPU, 2);
    spec.jobs = 3;
    ASSERT_EQ(proc_spec_validate(&spec, NULL, 0), SIM_ERR_INVALID);
    spec.jobs = 0;
    spec.deadline = 4;
    ASSERT_EQ(proc_spec_validate(&spec, NULL, 0), SIM_ERR_INVALID);

    sim_t *s = new_sim(SCHED_EDF);
    proc_spec_init(&spec, "Tight");
    proc_spec_add_burst(&spec, BURST_CPU, 2);
    spec.period = 6;
    spec.deadline = 3;
    spec.jobs = 4;
    ASSERT_TRUE(sim_spawn_process(s, &spec) >= 0);
    spawn(s, "Other", 0, 4, 6, 1);
    sim_run(s);
    uint64_t hash = s->log_hash;

    const char *path = "build/test_realtime_round_trip.txt";
    ASSERT_EQ(scenario_save(s, NULL, path), SIM_OK);
    sim_t *loaded = new_sim(SCHED_FCFS);
    ASSERT_EQ(scenario_load(loaded, NULL, path), SIM_OK);
    ASSERT_EQ(loaded->procs[0].period, 6);
    ASSERT_EQ(loaded->procs[0].relative_deadline, 3);
    ASSERT_EQ(loaded->procs[0].num_jobs, 4);
    ASSERT_EQ(loaded->procs[1].relative_deadline, 4);
    sim_run(loaded);
    ASSERT_TRUE(loaded->log_hash == hash);

    sim_reset(s);
    sim_run(s);
    ASSERT_TRUE(s->log_hash == hash);
    remove(path);
    free(loaded);
    free(s);
    TEST_PASS();
}

int main(void) {
    TEST_SUITE_BEGIN("PRISM real-time scheduling (RM, EDF, periodic tasks)");
    test_rm_textbook_schedulable();
    test_rm_misses_edf_does_not();
    test_schedulability_analysis();
    test_background_and_sleeping();
    test_periodic_spec_round_trip();
    TEST_SUITE_SUMMARY();
    return g_tests_failed == 0 ? 0 : 1;
}
