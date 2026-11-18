#include "test_harness.h"
#include "prism/metrics.h"
#include "prism/scenario.h"
#include "prism/sim_api.h"
#include "prism/workload.h"
#include <string.h>

static sim_t *new_sim(int cores, sched_policy_t policy) {
    sim_t *s = malloc(sizeof(sim_t));
    if (!s) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    sim_init(s, cores, 16, 3, policy, REPL_LRU, 42);
    return s;
}

static sim_pid_t spawn(sim_t *s, const char *name, int cpu, uint8_t affinity) {
    proc_spec_t spec;
    proc_spec_init(&spec, name);
    spec.affinity = affinity;
    proc_spec_add_burst(&spec, BURST_CPU, (uint16_t)cpu);
    return sim_spawn_process(s, &spec);
}

static int count_events(const sim_t *s, event_type_t type, sim_pid_t pid) {
    int count = 0;
    for (uint32_t i = 0; i < s->event_count; i++) {
        count += s->event_log[i].type == type && (pid < 0 || s->event_log[i].pid == pid);
    }
    return count;
}

static bool only_dispatched_on(const sim_t *s, sim_pid_t pid, int core) {
    for (uint32_t i = 0; i < s->event_count; i++) {
        const sim_event_t *e = &s->event_log[i];
        if (e->type == EVENT_PROC_DISPATCH && e->pid == pid && e->core_id != core) return false;
    }
    return true;
}

static void build_steal_case(sim_t *s, uint8_t pinned) {
    spawn(s, "P0", 8, pinned);
    spawn(s, "P1", 1, 0);
    spawn(s, "P2", 8, pinned);
    spawn(s, "P3", 1, 0);
    spawn(s, "P4", 8, pinned);
}

static void test_idle_core_steals(void) {
    TEST_CASE("SMP: arrivals spread over the least-loaded cores; an idle core steals from the busiest");

    sim_t *s = new_sim(2, SCHED_SMP);
    build_steal_case(s, 0);
    sim_step(s);
    ASSERT_EQ(s->running_pid[0], 0);
    ASSERT_EQ(s->ready_count[0], 2);
    ASSERT_EQ(s->ready_count[1], 1);
    ASSERT_TRUE(only_dispatched_on(s, 1, 1));

    sim_run(s);
    ASSERT_EQ(s->completed_processes, 5);
    ASSERT_EQ(count_events(s, EVENT_MIGRATE, -1), 2);
    ASSERT_EQ(count_events(s, EVENT_MIGRATE, 4), 1);
    const sim_event_t *steal = NULL;
    for (uint32_t i = 0; i < s->event_count && !steal; i++) {
        if (s->event_log[i].type == EVENT_MIGRATE) steal = &s->event_log[i];
    }
    ASSERT_EQ(steal->pid, 4);
    ASSERT_EQ(steal->tick, 2);
    ASSERT_EQ(steal->detail1, 0);
    ASSERT_EQ(steal->detail2, 1);
    ASSERT_TRUE(only_dispatched_on(s, 4, 1));
    free(s);
    TEST_PASS();
}

static void test_affinity_is_respected(void) {
    TEST_CASE("SMP: processes pinned to core 0 are never stolen, even when core 1 sits idle");

    sim_t *s = new_sim(2, SCHED_SMP);
    build_steal_case(s, 1u << 0);
    sim_run(s);
    ASSERT_EQ(s->completed_processes, 5);
    ASSERT_EQ(count_events(s, EVENT_MIGRATE, -1), 0);
    ASSERT_TRUE(only_dispatched_on(s, 0, 0));
    ASSERT_TRUE(only_dispatched_on(s, 2, 0));
    ASSERT_TRUE(only_dispatched_on(s, 4, 0));
    ASSERT_TRUE(s->core_busy_ticks[0] > 3 * s->core_busy_ticks[1]);
    free(s);
    TEST_PASS();
}

static sim_t *load(const char *path) {
    sim_t *s = new_sim(1, SCHED_RR);
    if (scenario_load(s, NULL, path) != SIM_OK) {
        free(s);
        return NULL;
    }
    return s;
}

static void test_fewer_core_changes_than_global_queue(void) {
    TEST_CASE("SMP keeps processes on their core: fewer core changes than one global RR queue");

    sim_t *smp = load("workloads/smp_affinity.txt");
    ASSERT_TRUE(smp != NULL);
    ASSERT_EQ(smp->num_cores, 4);
    ASSERT_EQ(smp->sched_policy, SCHED_SMP);
    ASSERT_EQ(smp->procs[0].affinity_mask, 1u << 0);
    ASSERT_EQ(smp->procs[4].affinity_mask, 1u << 3);

    sim_t *rr = malloc(sizeof(sim_t));
    if (!rr) exit(1);
    ASSERT_EQ(sim_prepare_variant(rr, smp, SCHED_RR, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP, SIM_VARIANT_KEEP), SIM_OK);

    sim_run(smp);
    sim_run(rr);
    ASSERT_EQ(smp->completed_processes, 10);
    ASSERT_EQ(rr->completed_processes, 10);
    ASSERT_TRUE(only_dispatched_on(smp, 0, 0));
    ASSERT_TRUE(only_dispatched_on(smp, 4, 3));
    ASSERT_TRUE(smp->total_migrations < rr->total_migrations);
    free(rr);
    free(smp);
    TEST_PASS();
}

static void test_live_switch_and_round_trip(void) {
    TEST_CASE("SMP: live switch from MLFQ keeps every process; affinity survives save/load");

    sim_t *s = load("workloads/smp_affinity.txt");
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(sim_set_sched(s, "mlfq", 0), SIM_OK);
    for (int t = 0; t < 8; t++) sim_step(s);
    ASSERT_EQ(sim_set_sched(s, "smp", 0), SIM_OK);
    sim_run(s);
    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->completed_processes, 10);

    const char *path = "build/test_smp_round_trip.txt";
    sim_reset(s);
    ASSERT_EQ(sim_set_sched(s, "smp", 0), SIM_OK);
    ASSERT_EQ(scenario_save(s, NULL, path), SIM_OK);
    sim_t *loaded = load(path);
    ASSERT_TRUE(loaded != NULL);
    ASSERT_EQ(loaded->procs[0].affinity_mask, 1u << 0);
    ASSERT_EQ(loaded->procs[4].affinity_mask, 1u << 3);
    ASSERT_EQ(loaded->procs[2].affinity_mask, 0);
    sim_run(s);
    sim_run(loaded);
    ASSERT_TRUE(s->log_hash == loaded->log_hash);
    remove(path);
    free(loaded);
    free(s);
    TEST_PASS();
}

int main(void) {
    TEST_SUITE_BEGIN("PRISM multi-core (per-core queues, affinity, balancing)");
    test_idle_core_steals();
    test_affinity_is_respected();
    test_fewer_core_changes_than_global_queue();
    test_live_switch_and_round_trip();
    TEST_SUITE_SUMMARY();
    return g_tests_failed == 0 ? 0 : 1;
}
