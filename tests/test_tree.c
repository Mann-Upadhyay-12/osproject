#include "test_harness.h"
#include "prism/scenario.h"
#include "prism/sim_api.h"
#include "prism/workload.h"
#include <string.h>

static sim_t *new_sim(void) {
    sim_t *s = malloc(sizeof(sim_t));
    if (!s) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    sim_init(s, 1, 16, 3, SCHED_FCFS, REPL_LRU, 42);
    return s;
}

static sim_pid_t spawn(sim_t *s, const char *name, sim_pid_t parent, const char *bursts) {
    proc_spec_t spec;
    proc_spec_init(&spec, name);
    spec.parent = (int16_t)parent;
    if (!workload_parse_bursts(&spec, bursts, s)) return -1;
    return sim_spawn_process(s, &spec);
}

static const sim_event_t *find(const sim_t *s, event_type_t type, sim_pid_t pid) {
    for (uint32_t i = 0; i < s->event_count; i++) {
        if (s->event_log[i].type == type && s->event_log[i].pid == pid) return &s->event_log[i];
    }
    return NULL;
}

static void test_fork_creates_child_at_fork_tick(void) {
    TEST_CASE("Process tree: a child only exists once its parent forks it");

    sim_t *s = new_sim();
    sim_pid_t parent = spawn(s, "Parent", -1, "C3,fork,C2");
    sim_pid_t child = spawn(s, "Child", parent, "C1");
    ASSERT_TRUE(parent >= 0 && child >= 0);
    ASSERT_TRUE(s->procs[child].awaiting_fork);

    for (int t = 0; t < 3; t++) sim_step(s);
    ASSERT_EQ(s->procs[child].state, PROC_READY);
    const sim_event_t *fork = find(s, EVENT_FORK, parent);
    ASSERT_TRUE(fork != NULL);
    ASSERT_EQ(fork->tick, 2);
    ASSERT_EQ(fork->detail1, child);
    ASSERT_EQ(s->procs[child].arrival_tick, 2);

    sim_run(s);
    ASSERT_EQ(s->completed_processes, 2);
    free(s);
    TEST_PASS();
}

static void test_zombie_until_reaped(void) {
    TEST_CASE("Process tree: a child that exits first is a zombie until wait() reaps it, without blocking");

    sim_t *s = new_sim();
    sim_pid_t parent = spawn(s, "Parent", -1, "C1,fork,I6,wait,C1");
    sim_pid_t child = spawn(s, "Child", parent, "C1");
    sim_run(s);

    const sim_event_t *zombie = find(s, EVENT_ZOMBIE, child);
    const sim_event_t *reap = find(s, EVENT_REAP, child);
    ASSERT_TRUE(zombie != NULL && reap != NULL);
    ASSERT_TRUE(reap->tick > zombie->tick);
    ASSERT_EQ(reap->detail1, parent);
    ASSERT_TRUE(find(s, EVENT_WAIT_BLOCKED, parent) == NULL);
    ASSERT_FALSE(s->procs[child].zombie);
    ASSERT_TRUE(s->procs[child].finish_tick < s->procs[parent].finish_tick);
    free(s);
    TEST_PASS();
}

static void test_wait_blocks_until_child_exits(void) {
    TEST_CASE("Process tree: wait() blocks while a child runs and wakes when it exits");

    sim_t *s = new_sim();
    sim_pid_t parent = spawn(s, "Parent", -1, "C1,fork,wait,C1");
    sim_pid_t child = spawn(s, "Child", parent, "C4");
    for (int t = 0; t < 2; t++) sim_step(s);
    ASSERT_EQ(s->procs[parent].state, PROC_BLOCKED_WAIT);

    sim_run(s);
    ASSERT_TRUE(find(s, EVENT_ZOMBIE, child) == NULL);
    ASSERT_EQ(find(s, EVENT_REAP, child)->detail1, parent);
    ASSERT_EQ(s->procs[child].finish_tick, 5);
    ASSERT_EQ(s->procs[parent].finish_tick, 6);
    free(s);
    TEST_PASS();
}

static void test_orphan_adopted_by_init(void) {
    TEST_CASE("Process tree: a running child of an exiting parent is adopted and reaped by init");

    sim_t *s = new_sim();
    sim_init(s, 2, 16, 3, SCHED_FCFS, REPL_LRU, 42);
    sim_pid_t parent = spawn(s, "Parent", -1, "C1,fork,C1");
    sim_pid_t child = spawn(s, "Child", parent, "C8");
    sim_run(s);

    const sim_event_t *orphan = find(s, EVENT_ORPHAN, child);
    ASSERT_TRUE(orphan != NULL);
    ASSERT_EQ(orphan->detail1, parent);
    ASSERT_TRUE(s->procs[child].orphaned);
    ASSERT_EQ(find(s, EVENT_REAP, child)->detail1, PRISM_INIT_PID);
    ASSERT_EQ(s->completed_processes, 2);
    free(s);
    TEST_PASS();
}

static void test_unforked_child_and_childless_wait(void) {
    TEST_CASE("Process tree: wait() with no children returns at once; never-forked children don't hang the run");

    sim_t *s = new_sim();
    sim_pid_t lonely = spawn(s, "Lonely", -1, "C1,wait,C1");
    sim_pid_t parent = spawn(s, "Parent", -1, "C2");
    sim_pid_t unborn = spawn(s, "Unborn", parent, "C5");
    sim_run(s);

    ASSERT_TRUE(sim_is_done(s));
    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->procs[lonely].finish_tick, 2);
    ASSERT_EQ(s->procs[unborn].state, PROC_NEW);
    ASSERT_EQ(s->completed_processes, 2);
    free(s);
    TEST_PASS();
}

static void test_tree_preset_round_trip(void) {
    TEST_CASE("Process tree preset: zombie, blocking wait, orphan; save/load and reset are identical");

    sim_t *s = new_sim();
    ASSERT_EQ(scenario_load(s, NULL, "workloads/process_tree.txt"), SIM_OK);
    ASSERT_EQ(s->procs[1].parent_pid, 0);
    ASSERT_EQ(s->procs[5].parent_pid, 3);
    sim_run(s);
    uint64_t hash = s->log_hash;

    ASSERT_TRUE(find(s, EVENT_ZOMBIE, 1) != NULL);
    ASSERT_TRUE(find(s, EVENT_WAIT_BLOCKED, 0) != NULL);
    ASSERT_TRUE(find(s, EVENT_ORPHAN, 3) != NULL);
    ASSERT_EQ(s->procs[4].state, PROC_NEW);

    const char *path = "build/test_tree_round_trip.txt";
    ASSERT_EQ(scenario_save(s, NULL, path), SIM_OK);
    sim_t *loaded = new_sim();
    ASSERT_EQ(scenario_load(loaded, NULL, path), SIM_OK);
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
    TEST_SUITE_BEGIN("PRISM process tree (fork, wait, zombies, orphans)");
    test_fork_creates_child_at_fork_tick();
    test_zombie_until_reaped();
    test_wait_blocks_until_child_exits();
    test_orphan_adopted_by_init();
    test_unforked_child_and_childless_wait();
    test_tree_preset_round_trip();
    TEST_SUITE_SUMMARY();
    return g_tests_failed == 0 ? 0 : 1;
}
