#include "test_harness.h"
#include "prism/scenario.h"
#include "prism/sim_api.h"
#include "prism/workload.h"
#include <string.h>

#define BUFFER_SLOTS 3

static sim_t *new_sim(void) {
    sim_t *s = malloc(sizeof(sim_t));
    if (!s) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    sim_init(s, 1, 8, 3, SCHED_RR, REPL_LRU, 42);
    return s;
}

static sim_t *load_workload(const char *path) {
    sim_t *s = new_sim();
    if (!workload_load_file(path, s, NULL)) {
        free(s);
        return NULL;
    }
    sim_set_policy(s, s->sched_policy, s->repl_policy);
    return s;
}

static sim_pid_t add_process(sim_t *s, const char *name, const char *bursts) {
    proc_spec_t spec;
    proc_spec_init(&spec, name);
    if (!workload_parse_bursts(&spec, bursts, s)) return -1;
    return sim_spawn_process(s, &spec);
}

static void test_semaphore_wait_signal_fifo(void) {
    TEST_CASE("Semaphores: wait passes while value > 0, blocks at 0, signal wakes in FIFO order");

    sim_t *s = new_sim();
    int32_t gate = sim_add_semaphore(s, "gate", 1);
    ASSERT_EQ(gate, 0);
    ASSERT_EQ(sim_add_semaphore(s, "GATE", 2), -1);
    ASSERT_EQ(sim_add_semaphore(s, "neg", -1), -1);

    sim_pid_t a = add_process(s, "A", "C1,wait:gate,C3,signal:gate");
    sim_pid_t b = add_process(s, "B", "C1,wait:gate,C1,signal:gate");
    sim_pid_t c = add_process(s, "C", "C1,P:gate,C1,V:gate");
    ASSERT_TRUE(a >= 0 && b >= 0 && c >= 0);

    for (int t = 0; t < 4; t++) sim_step(s);
    ASSERT_EQ(s->semaphores[gate].value, 0);
    ASSERT_EQ(s->procs[b].state, PROC_BLOCKED_SEM);
    ASSERT_EQ(s->procs[c].state, PROC_BLOCKED_SEM);
    ASSERT_EQ(s->semaphores[gate].num_waiters, 2);
    ASSERT_EQ(s->semaphores[gate].waiters[0], b);

    sim_run(s);
    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->completed_processes, 3);
    ASSERT_EQ(s->semaphores[gate].value, 1);
    ASSERT_EQ(s->semaphores[gate].total_blocks, 2);
    ASSERT_TRUE(s->procs[b].finish_tick < s->procs[c].finish_tick);
    free(s);
    TEST_PASS();
}

static void test_consecutive_and_trailing_control_bursts(void) {
    TEST_CASE("Engine: back-to-back and trailing control bursts all run, then the process exits");

    sim_t *s = new_sim();
    int32_t sem = sim_add_semaphore(s, "count", 1);
    sim_pid_t p = add_process(s, "P", "C2,wait:count,signal:count,signal:count,signal:count");
    ASSERT_TRUE(p >= 0);

    sim_run(s);
    ASSERT_EQ(s->procs[p].state, PROC_TERMINATED);
    ASSERT_EQ(s->procs[p].finish_tick, 2);
    ASSERT_EQ(s->procs[p].total_cpu_time, 2);
    ASSERT_EQ(s->semaphores[sem].value, 3);
    free(s);
    TEST_PASS();
}

typedef struct {
    int32_t empty_id;
    int32_t full_id;
    int     items;
    int     min_items;
    int     max_items;
} buffer_watch_t;

static void watch_buffer(const sim_event_t *ev, void *user) {
    buffer_watch_t *w = user;
    if (ev->type != EVENT_SEM_SIGNAL) return;
    if (ev->detail1 == w->full_id) w->items++;
    if (ev->detail1 == w->empty_id) w->items--;
    if (w->items > w->max_items) w->max_items = w->items;
    if (w->items < w->min_items) w->min_items = w->items;
}

static void test_producer_consumer(void) {
    TEST_CASE("Producer-consumer: buffer stays within 0..3, every item consumed");

    sim_t *s = load_workload("workloads/producer_consumer.txt");
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->num_semaphores, 3);
    ASSERT_EQ(s->num_processes, 4);

    buffer_watch_t watch = {sim_find_semaphore(s, "empty"), sim_find_semaphore(s, "full"), 0, 0, 0};
    sim_set_observer(s, watch_buffer, &watch);
    sim_run(s);

    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->completed_processes, 4);
    ASSERT_EQ(watch.min_items, 0);
    ASSERT_EQ(watch.max_items, BUFFER_SLOTS);
    ASSERT_EQ(watch.items, 0);
    ASSERT_EQ(s->semaphores[sim_find_semaphore(s, "mutex")].value, 1);
    ASSERT_EQ(s->semaphores[watch.empty_id].value, BUFFER_SLOTS);
    ASSERT_EQ(s->semaphores[watch.full_id].value, 0);
    ASSERT_TRUE(s->semaphores[watch.empty_id].total_blocks > 0);
    free(s);
    TEST_PASS();
}

static int count_events(const sim_t *s, event_type_t type) {
    int count = 0;
    for (uint32_t i = 0; i < s->event_count; i++) count += s->event_log[i].type == type;
    return count;
}

static void test_dining_philosophers_deadlock_and_fix(void) {
    TEST_CASE("Dining philosophers: naive version stalls with all 5 blocked, asymmetric fix finishes");

    sim_t *s = load_workload("workloads/dining_philosophers.txt");
    ASSERT_TRUE(s != NULL);
    sim_run(s);
    ASSERT_TRUE(sim_is_stalled(s));
    ASSERT_TRUE(sim_is_done(s));
    ASSERT_EQ(s->completed_processes, 0);
    for (int i = 0; i < 5; i++) ASSERT_EQ(s->procs[i].state, PROC_BLOCKED_SEM);
    ASSERT_EQ(count_events(s, EVENT_STALLED), 1);

    tick_t stalled_at = s->tick;
    ASSERT_FALSE(sim_step(s));
    ASSERT_EQ(s->tick, stalled_at);
    free(s);

    s = load_workload("workloads/dining_philosophers_fixed.txt");
    ASSERT_TRUE(s != NULL);
    sim_run(s);
    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->completed_processes, 5);
    for (int i = 0; i < 5; i++) ASSERT_EQ(s->semaphores[i].value, 1);
    free(s);
    TEST_PASS();
}

typedef struct {
    int32_t room_id;
    int     seats_taken;
    int     max_seats;
} room_watch_t;

static void watch_room(const sim_event_t *ev, void *user) {
    room_watch_t *w = user;
    if (ev->detail1 != w->room_id) return;
    if (ev->type == EVENT_SEM_WAIT || ev->type == EVENT_SEM_WAKE) w->seats_taken++;
    if (ev->type == EVENT_SEM_SIGNAL) w->seats_taken--;
    if (w->seats_taken > w->max_seats) w->max_seats = w->seats_taken;
}

static void test_readers_writers(void) {
    TEST_CASE("Readers-writers: never more than 3 seats taken, everyone finishes");

    sim_t *s = load_workload("workloads/readers_writers.txt");
    ASSERT_TRUE(s != NULL);
    room_watch_t watch = {sim_find_semaphore(s, "room"), 0, 0};
    sim_set_observer(s, watch_room, &watch);
    sim_run(s);

    ASSERT_FALSE(sim_is_stalled(s));
    ASSERT_EQ(s->completed_processes, 6);
    ASSERT_EQ(watch.max_seats, 3);
    ASSERT_EQ(watch.seats_taken, 0);
    ASSERT_EQ(s->semaphores[watch.room_id].value, 3);
    free(s);
    TEST_PASS();
}

static void test_scenario_round_trip_and_reset(void) {
    TEST_CASE("Semaphores survive save/load and reset with an identical trace");

    sim_t *s = load_workload("workloads/producer_consumer.txt");
    ASSERT_TRUE(s != NULL);
    sim_run(s);
    uint64_t first_hash = s->log_hash;

    sim_reset(s);
    ASSERT_EQ(s->semaphores[1].value, BUFFER_SLOTS);
    sim_run(s);
    ASSERT_TRUE(s->log_hash == first_hash);

    const char *path = "build/test_sync_round_trip.txt";
    ASSERT_EQ(scenario_save(s, NULL, path), SIM_OK);
    sim_t *loaded = new_sim();
    ASSERT_EQ(scenario_load(loaded, NULL, path), SIM_OK);
    ASSERT_EQ(loaded->num_semaphores, 3);
    ASSERT_STR_EQ(loaded->semaphores[2].name, "full");
    ASSERT_EQ(loaded->semaphores[1].initial_value, BUFFER_SLOTS);
    ASSERT_EQ(loaded->procs[0].bursts[1].kind, BURST_SEM_WAIT);
    sim_run(loaded);
    ASSERT_TRUE(loaded->log_hash == first_hash);
    remove(path);
    free(loaded);
    free(s);
    TEST_PASS();
}

static void test_unknown_semaphore_rejected(void) {
    TEST_CASE("Semaphores: bursts naming an unknown semaphore are rejected");

    sim_t *s = new_sim();
    sim_add_semaphore(s, "known", 1);
    proc_spec_t spec;
    proc_spec_init(&spec, "P");
    ASSERT_FALSE(workload_parse_bursts(&spec, "C1,wait:missing", s));
    proc_spec_init(&spec, "P");
    ASSERT_FALSE(workload_parse_bursts(&spec, "C1,lock:known", s));
    proc_spec_init(&spec, "P");
    ASSERT_TRUE(workload_parse_bursts(&spec, "C1,wait:KNOWN,signal:known", s));
    ASSERT_EQ(spec.num_bursts, 3);
    free(s);
    TEST_PASS();
}

int main(void) {
    TEST_SUITE_BEGIN("PRISM synchronisation (semaphores, classic problems)");
    test_semaphore_wait_signal_fifo();
    test_consecutive_and_trailing_control_bursts();
    test_producer_consumer();
    test_dining_philosophers_deadlock_and_fix();
    test_readers_writers();
    test_scenario_round_trip_and_reset();
    test_unknown_semaphore_rejected();
    TEST_SUITE_SUMMARY();
    return g_tests_failed == 0 ? 0 : 1;
}
