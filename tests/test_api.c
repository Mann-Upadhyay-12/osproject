#include "prism/sim_api.h"
#include "prism/scenario.h"
#include "prism/workload.h"
#include "test_harness.h"
#include <string.h>
#include <stdio.h>

/* sim_t is ~0.5 MB; keep test instances off the stack. */
static sim_t g_a, g_b;
static layout_t g_la, g_lb;

static char g_tmpdir[256] = "build";

static void tmp_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", g_tmpdir, name);
}

/* A small, fully specified scenario used by most tests. */
static void build_base(sim_t *s, sched_policy_t sp) {
    sim_init(s, 1, 4, 3, sp, REPL_LRU, 7);
    s->default_quantum = 3;
    proc_spec_t spec;

    proc_spec_init(&spec, "CPU");
    proc_spec_add_burst(&spec, BURST_CPU, 10);
    for (int i = 0; i < 10; i++) proc_spec_add_ref(&spec, (uint16_t)(i % 4));
    sim_spawn_process(s, &spec);

    proc_spec_init(&spec, "IO");
    spec.arrival = 1;
    proc_spec_add_burst(&spec, BURST_CPU, 3);
    proc_spec_add_burst(&spec, BURST_IO, 4);
    proc_spec_add_burst(&spec, BURST_CPU, 3);
    for (int i = 0; i < 6; i++) proc_spec_add_ref(&spec, (uint16_t)(i % 2));
    sim_spawn_process(s, &spec);

    proc_spec_init(&spec, "Late");
    spec.arrival = 6;
    spec.priority = 2;
    proc_spec_add_burst(&spec, BURST_CPU, 5);
    sim_spawn_process(s, &spec);
}

static void make_injected(proc_spec_t *spec) {
    proc_spec_init(spec, "Injected");
    spec->arrival = 0;               /* in the past: must be clamped to "now" */
    proc_spec_add_burst(spec, BURST_CPU, 4);
    proc_spec_add_burst(spec, BURST_IO, 2);
    proc_spec_add_burst(spec, BURST_CPU, 2);
    proc_spec_add_ref(spec, 5);
    proc_spec_add_ref(spec, 6);
}

static void test_spec_validation(void) {
    TEST_CASE("proc_spec validation rejects bad scripts");
    proc_spec_t spec;
    char msg[128];

    proc_spec_init(&spec, "Empty");
    ASSERT_EQ(proc_spec_validate(&spec, msg, sizeof(msg)), SIM_ERR_INVALID);

    proc_spec_init(&spec, "IOFirst");
    proc_spec_add_burst(&spec, BURST_IO, 3);
    ASSERT_EQ(proc_spec_validate(&spec, msg, sizeof(msg)), SIM_ERR_INVALID);
    ASSERT_TRUE(strstr(msg, "first burst") != NULL);

    proc_spec_init(&spec, "Zero");
    proc_spec_add_burst(&spec, BURST_CPU, 0);
    ASSERT_EQ(proc_spec_validate(&spec, NULL, 0), SIM_ERR_INVALID);

    proc_spec_init(&spec, "BadPage");
    ASSERT_EQ(proc_spec_add_ref(&spec, PRISM_MAX_VIRTUAL_PAGES), SIM_ERR_INVALID);

    proc_spec_init(&spec, "Ok");
    proc_spec_add_burst(&spec, BURST_CPU, 2);
    ASSERT_EQ(proc_spec_validate(&spec, NULL, 0), SIM_OK);

    sim_init(&g_a, 1, 4, 3, SCHED_FCFS, REPL_FIFO, 1);
    proc_spec_init(&spec, "IOFirst");
    proc_spec_add_burst(&spec, BURST_IO, 3);
    ASSERT_EQ(sim_spawn_process(&g_a, &spec), SIM_ERR_INVALID);
    ASSERT_EQ(g_a.num_processes, 0);
    TEST_PASS();
}

static void test_spawn_mid_run(void) {
    TEST_CASE("Spawn process mid-run arrives now and completes");
    build_base(&g_a, SCHED_RR);
    sim_run_until(&g_a, 8);
    ASSERT_EQ(g_a.tick, 8);

    proc_spec_t spec;
    make_injected(&spec);
    int pid = sim_spawn_process(&g_a, &spec);
    ASSERT_EQ(pid, 3);
    ASSERT_EQ(g_a.procs[pid].arrival_tick, 8);
    ASSERT_EQ(g_a.procs[pid].state, PROC_NEW);

    sim_step(&g_a);
    ASSERT_TRUE(g_a.procs[pid].state != PROC_NEW);   /* admitted on the very next tick */

    sim_run(&g_a);
    ASSERT_TRUE(sim_is_done(&g_a));
    ASSERT_EQ(g_a.procs[pid].state, PROC_TERMINATED);
    ASSERT_TRUE(g_a.procs[pid].start_tick >= 8);
    ASSERT_EQ(g_a.completed_processes, 4);

    /* Spawning into a finished run revives it. */
    make_injected(&spec);
    pid = sim_spawn_process(&g_a, &spec);
    ASSERT_TRUE(!sim_is_done(&g_a));
    sim_run(&g_a);
    ASSERT_EQ(g_a.procs[pid].state, PROC_TERMINATED);
    TEST_PASS();
}

static void test_reset_rerun_identical(void) {
    TEST_CASE("Reset then rerun reproduces metrics and trace hash");
    build_base(&g_a, SCHED_MLFQ);
    sim_run_until(&g_a, 5);
    proc_spec_t spec;
    make_injected(&spec);
    sim_spawn_process(&g_a, &spec);
    sim_run(&g_a);

    tick_t ticks = g_a.tick;
    uint32_t faults = g_a.total_page_faults, cs = g_a.total_context_switches;
    uint64_t hash = g_a.log_hash;
    uint32_t wait3 = g_a.procs[3].total_wait_time;
    uint64_t fin2 = (uint64_t)g_a.procs[2].finish_tick;

    sim_reset(&g_a);
    ASSERT_EQ(g_a.tick, 0);
    ASSERT_EQ(g_a.event_count, 0);
    ASSERT_EQ(g_a.num_processes, 4);
    ASSERT_EQ(g_a.procs[3].arrival_tick, 5);
    ASSERT_EQ(g_a.procs[2].priority, 2);       /* base priority restored after MLFQ changes */
    for (int i = 0; i < g_a.total_frames; i++) ASSERT_TRUE(!g_a.frames[i].occupied);

    sim_run(&g_a);
    ASSERT_EQ(g_a.tick, ticks);
    ASSERT_EQ(g_a.total_page_faults, faults);
    ASSERT_EQ(g_a.total_context_switches, cs);
    ASSERT_EQ(g_a.procs[3].total_wait_time, wait3);
    ASSERT_EQ((uint64_t)g_a.procs[2].finish_tick, fin2);
    ASSERT_EQ(g_a.log_hash, hash);
    TEST_PASS();
}

static void test_remove_rules(void) {
    TEST_CASE("Remove: only NEW/TERMINATED, pids never reused");
    build_base(&g_a, SCHED_FCFS);
    sim_run_until(&g_a, 2);
    /* P0 is mid-flight (running or blocked on its first fault), P1 has arrived. */
    ASSERT_TRUE(g_a.procs[0].state != PROC_NEW && g_a.procs[0].state != PROC_TERMINATED);
    ASSERT_TRUE(g_a.procs[1].state != PROC_NEW && g_a.procs[1].state != PROC_TERMINATED);
    ASSERT_EQ(sim_remove_process(&g_a, 0), SIM_ERR_STATE);
    ASSERT_EQ(sim_remove_process(&g_a, 1), SIM_ERR_STATE);
    ASSERT_EQ(sim_remove_process(&g_a, 99), SIM_ERR_NO_PID);
    ASSERT_EQ(sim_remove_process(&g_a, 2), SIM_OK);          /* still NEW */
    ASSERT_EQ(g_a.procs[2].state, PROC_UNUSED);
    ASSERT_EQ(sim_remove_process(&g_a, 2), SIM_ERR_NO_PID);  /* already a hole */

    proc_spec_t spec;
    make_injected(&spec);
    ASSERT_EQ(sim_spawn_process(&g_a, &spec), 3);            /* hole at 2 is not reused */

    sim_run(&g_a);
    ASSERT_TRUE(sim_is_done(&g_a));
    ASSERT_EQ(g_a.completed_processes, 3);
    ASSERT_EQ(g_a.procs[2].state, PROC_UNUSED);              /* never ran */
    ASSERT_EQ(g_a.procs[2].start_tick, -1);

    /* TERMINATED may be removed; done-ness stays consistent. */
    ASSERT_EQ(sim_remove_process(&g_a, 0), SIM_OK);
    ASSERT_EQ(g_a.completed_processes, 2);
    ASSERT_TRUE(sim_is_done(&g_a));

    /* Holes survive reset. */
    sim_reset(&g_a);
    ASSERT_EQ(g_a.procs[0].state, PROC_UNUSED);
    ASSERT_EQ(g_a.procs[2].state, PROC_UNUSED);
    ASSERT_EQ(g_a.procs[1].state, PROC_NEW);
    sim_run(&g_a);
    ASSERT_TRUE(sim_is_done(&g_a));
    ASSERT_EQ(g_a.completed_processes, 2);
    TEST_PASS();
}

static void test_update_rules(void) {
    TEST_CASE("Update: allowed before first run only");
    build_base(&g_a, SCHED_FCFS);
    proc_spec_t spec;
    ASSERT_EQ(sim_get_process_spec(&g_a, 2, &spec), SIM_OK);
    ASSERT_STR_EQ(spec.name, "Late");
    ASSERT_EQ(spec.num_bursts, 1);
    spec.bursts[0].duration = 9;
    proc_spec_add_burst(&spec, BURST_IO, 2);
    proc_spec_add_burst(&spec, BURST_CPU, 1);
    ASSERT_EQ(sim_update_process(&g_a, 2, &spec), SIM_OK);
    ASSERT_EQ(g_a.procs[2].num_bursts, 3);
    ASSERT_EQ(g_a.procs[2].bursts[0].remaining, 9);

    sim_run_until(&g_a, 1);
    ASSERT_EQ(sim_get_process_spec(&g_a, 0, &spec), SIM_OK);
    ASSERT_EQ(sim_update_process(&g_a, 0, &spec), SIM_ERR_STATE);  /* already running */

    proc_spec_init(&spec, "Bad");
    ASSERT_EQ(sim_update_process(&g_a, 2, &spec), SIM_ERR_INVALID);
    sim_run(&g_a);
    ASSERT_EQ(g_a.procs[2].total_cpu_time, 10);
    TEST_PASS();
}

/* Observer that records into a buffer. */
#define OBS_CAP 4096
static sim_event_t g_obs[OBS_CAP];
static uint32_t g_obs_n;
static void record_obs(const sim_event_t *ev, void *user) {
    (void)user;
    if (g_obs_n < OBS_CAP) g_obs[g_obs_n] = *ev;
    g_obs_n++;
}

static void test_observer_order(void) {
    TEST_CASE("Observer sees every event in trace order; no effect on hash");
    build_base(&g_b, SCHED_RR);
    sim_run(&g_b);
    uint64_t hash_no_obs = g_b.log_hash;

    build_base(&g_a, SCHED_RR);
    g_obs_n = 0;
    sim_set_observer(&g_a, record_obs, NULL);
    sim_run(&g_a);

    ASSERT_EQ(g_a.log_hash, hash_no_obs);
    ASSERT_EQ(g_obs_n, g_a.event_count);
    ASSERT_TRUE(g_obs_n < OBS_CAP);
    for (uint32_t i = 0; i < g_obs_n; i++) {
        ASSERT_TRUE(memcmp(&g_obs[i], &g_a.event_log[i], sizeof(sim_event_t)) == 0);
    }

    /* Observer survives reset; detaching stops notifications. */
    sim_reset(&g_a);
    g_obs_n = 0;
    sim_step(&g_a);
    ASSERT_TRUE(g_obs_n > 0);
    sim_set_observer(&g_a, NULL, NULL);
    uint32_t before = g_obs_n;
    sim_run(&g_a);
    ASSERT_EQ(g_obs_n, before);
    TEST_PASS();
}

static void test_step_event(void) {
    TEST_CASE("step_event advances to the next notable event");
    build_base(&g_a, SCHED_RR);
    int steps = 0;
    while (!sim_is_done(&g_a)) {
        uint64_t before = g_a.notable_events;
        sim_step_event(&g_a);
        ASSERT_TRUE(g_a.notable_events > before || sim_is_done(&g_a));
        steps++;
        ASSERT_TRUE(steps < 10000);
    }
    build_base(&g_b, SCHED_RR);
    sim_run(&g_b);
    ASSERT_EQ(g_a.log_hash, g_b.log_hash);   /* stepping granularity does not change the run */

    /* A process arriving far in the future: one call jumps straight to it. */
    sim_init(&g_a, 1, 4, 3, SCHED_FCFS, REPL_FIFO, 1);
    proc_spec_t spec;
    proc_spec_init(&spec, "Far");
    spec.arrival = 50;
    proc_spec_add_burst(&spec, BURST_CPU, 2);
    sim_spawn_process(&g_a, &spec);
    sim_step_event(&g_a);
    ASSERT_EQ(g_a.tick, 51);
    ASSERT_EQ(g_a.event_log[0].type, EVENT_PROC_ARRIVE);
    TEST_PASS();
}

static void test_policy_switch(void) {
    TEST_CASE("Live scheduler switch keeps every ready process; names resolve");
    ASSERT_EQ(sim_set_sched(&g_a, "nope", 0), SIM_ERR_NAME);
    ASSERT_EQ(sim_set_repl(&g_a, "nope"), SIM_ERR_NAME);

    /* Six CPU-bound processes under MLFQ get spread over levels Q0..Q2. */
    sim_init(&g_a, 1, 8, 2, SCHED_MLFQ, REPL_LRU, 3);
    proc_spec_t spec;
    for (int i = 0; i < 6; i++) {
        char name[16];
        snprintf(name, sizeof(name), "W%d", i);
        proc_spec_init(&spec, name);
        proc_spec_add_burst(&spec, BURST_CPU, 30);
        sim_spawn_process(&g_a, &spec);
    }
    sim_run_until(&g_a, 40);
    ASSERT_TRUE(g_a.ready_count[1] + g_a.ready_count[2] > 0);
    int ready_before = g_a.ready_count[0] + g_a.ready_count[1] + g_a.ready_count[2];

    ASSERT_EQ(sim_set_sched(&g_a, "fcfs", 0), SIM_OK);
    ASSERT_EQ(g_a.sched_policy, SCHED_FCFS);
    ASSERT_EQ(g_a.ready_count[0], ready_before);
    ASSERT_EQ(g_a.ready_count[1] + g_a.ready_count[2], 0);

    ASSERT_EQ(sim_set_sched(&g_a, "RoundRobin", 5), SIM_OK);   /* display name works too */
    ASSERT_EQ(g_a.default_quantum, 5);
    ASSERT_EQ(sim_set_repl(&g_a, "CLOCK"), SIM_OK);
    ASSERT_EQ(g_a.repl_policy, REPL_CLOCK);
    sim_run(&g_a);
    ASSERT_TRUE(sim_is_done(&g_a));
    ASSERT_EQ(g_a.completed_processes, 6);
    TEST_PASS();
}

static void test_set_frames(void) {
    TEST_CASE("set_frames validates, resets, and changes fault counts");
    build_base(&g_a, SCHED_FCFS);
    sim_run_until(&g_a, 10);
    ASSERT_EQ(sim_set_frames(&g_a, 0), SIM_ERR_INVALID);
    ASSERT_EQ(sim_set_frames(&g_a, PRISM_MAX_FRAMES + 1), SIM_ERR_INVALID);
    ASSERT_EQ(g_a.tick, 10);                     /* rejected call does not reset */

    ASSERT_EQ(sim_set_frames(&g_a, 1), SIM_OK);
    ASSERT_EQ(g_a.tick, 0);
    ASSERT_EQ(g_a.total_frames, 1);
    sim_run(&g_a);
    uint32_t faults_small = g_a.total_page_faults;

    ASSERT_EQ(sim_set_frames(&g_a, 16), SIM_OK);
    sim_run(&g_a);
    ASSERT_TRUE(g_a.total_page_faults < faults_small);

    /* RAM shrink by fault injection is undone by reset. */
    sim_reset(&g_a);
    sim_inject_shrink_ram(&g_a, 2);
    ASSERT_EQ(g_a.total_frames, 2);
    sim_reset(&g_a);
    ASSERT_EQ(g_a.total_frames, 16);
    TEST_PASS();
}

static void test_scenario_roundtrip(void) {
    TEST_CASE("Scenario save/load round-trips script, config and layout");
    char path[300];
    tmp_path(path, sizeof(path), "test_api_roundtrip.wl");

    build_base(&g_a, SCHED_SRTF);
    sim_add_resource(&g_a, "printer", 2);
    sim_set_repl(&g_a, "clock");
    proc_spec_t spec;
    proc_spec_init(&spec, "Random");
    proc_spec_add_burst(&spec, BURST_CPU, 12);
    ASSERT_TRUE(workload_parse_pages(&spec, "random:0:9:12", 7, 3));
    sim_spawn_process(&g_a, &spec);
    sim_remove_process(&g_a, 1);       /* hole: skipped on save */

    layout_init(&g_la);
    ASSERT_EQ(layout_set(&g_la, "cpu", 640, 200), SIM_OK);
    ASSERT_EQ(layout_set(&g_la, "Late", 120.5f, 80), SIM_OK);
    ASSERT_EQ(layout_set(&g_la, "bad key", 1, 1), SIM_ERR_INVALID);
    ASSERT_EQ(layout_set(&g_la, "cpu", 650, 210), SIM_OK);       /* update in place */
    ASSERT_EQ(g_la.count, 2);

    ASSERT_EQ(scenario_save(&g_a, &g_la, path), SIM_OK);

    g_obs_n = 0;
    sim_set_observer(&g_b, record_obs, NULL);
    ASSERT_EQ(scenario_load(&g_b, &g_lb, path), SIM_OK);
    ASSERT_TRUE(g_b.observer == record_obs);     /* kept across load */
    sim_set_observer(&g_b, NULL, NULL);

    ASSERT_EQ(g_b.num_processes, 3);
    ASSERT_STR_EQ(g_b.procs[1].name, "Late");
    ASSERT_EQ(g_b.procs[1].base_priority, 2);
    ASSERT_EQ(g_b.procs[1].arrival_tick, 6);
    ASSERT_EQ(g_b.sched_policy, SCHED_SRTF);
    ASSERT_EQ(g_b.repl_policy, REPL_CLOCK);
    ASSERT_EQ(g_b.default_quantum, 3);
    ASSERT_EQ(g_b.seed, 7);
    ASSERT_EQ(g_b.num_resources, 1);
    ASSERT_EQ(g_b.resources[0].total_instances, 2);
    ASSERT_EQ(g_b.procs[2].num_page_refs, 12);
    for (int i = 0; i < 12; i++) ASSERT_EQ(g_b.procs[2].page_refs[i], g_a.procs[3].page_refs[i]);

    float x = 0, y = 0;
    ASSERT_EQ(g_lb.count, 2);
    ASSERT_TRUE(layout_get(&g_lb, "cpu", &x, &y));
    ASSERT_TRUE(x == 650.0f && y == 210.0f);
    ASSERT_TRUE(layout_get(&g_lb, "Late", &x, &y));
    ASSERT_TRUE(x == 120.5f);

    /* Saved -> loaded -> saved again is byte-identical, and both runs match. */
    char path2[300];
    tmp_path(path2, sizeof(path2), "test_api_roundtrip2.wl");
    ASSERT_EQ(scenario_save(&g_b, &g_lb, path2), SIM_OK);
    sim_t *c = &g_a;
    ASSERT_EQ(scenario_load(c, NULL, path2), SIM_OK);
    sim_run(c);
    sim_run(&g_b);
    ASSERT_EQ(c->log_hash, g_b.log_hash);

    FILE *f1 = fopen(path, "r"), *f2 = fopen(path2, "r");
    ASSERT_TRUE(f1 && f2);
    int a, b;
    do { a = fgetc(f1); b = fgetc(f2); } while (a == b && a != EOF);
    fclose(f1); fclose(f2);
    ASSERT_EQ(a, b);
    remove(path);
    remove(path2);

    ASSERT_EQ(scenario_load(&g_b, &g_lb, "no/such/file.wl"), SIM_ERR_IO);
    TEST_PASS();
}

static void test_old_workloads_load(void) {
    TEST_CASE("Old workload files load via scenario_load (Belady still 9 faults)");
    ASSERT_EQ(scenario_load(&g_a, &g_la, "workloads/belady.txt"), SIM_OK);
    ASSERT_EQ(g_la.count, 0);
    ASSERT_EQ(g_a.total_frames, 3);
    ASSERT_EQ(g_a.repl_policy, REPL_FIFO);
    sim_run(&g_a);
    ASSERT_EQ(g_a.total_page_faults, 9);

    ASSERT_EQ(scenario_load(&g_a, NULL, "workloads/mixed_demo.txt"), SIM_OK);
    ASSERT_EQ(g_a.num_processes, 4);
    ASSERT_EQ(g_a.sched_policy, SCHED_MLFQ);
    sim_run(&g_a);
    ASSERT_TRUE(sim_is_done(&g_a));
    TEST_PASS();
}

int main(int argc, char **argv) {
    if (argc > 1) snprintf(g_tmpdir, sizeof(g_tmpdir), "%s", argv[1]);
    TEST_SUITE_BEGIN("PRISM Interactive API (sim_api / scenario)");

    test_spec_validation();
    test_spawn_mid_run();
    test_reset_rerun_identical();
    test_remove_rules();
    test_update_rules();
    test_observer_order();
    test_step_event();
    test_policy_switch();
    test_set_frames();
    test_scenario_roundtrip();
    test_old_workloads_load();

    TEST_SUITE_SUMMARY();
    return (g_tests_failed == 0) ? 0 : 1;
}
