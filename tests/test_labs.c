#include "prism/contig.h"
#include "prism/disk.h"
#include "prism/refstring.h"
#include "test_harness.h"
#include <string.h>

static const char *TEXTBOOK_REFS = "7 0 1 2 0 3 0 4 2 3 0 3 2 1 2 0 1 7 0 1";

static int faults_for(repl_policy_t policy, int frames, const char *text) {
    static uint16_t refs[REFSTR_MAX_REFS];
    static refstr_result_t result;
    int count;
    if (refstr_parse(text, refs, REFSTR_MAX_REFS, &count) != REFSTR_OK) return -1;
    if (refstr_simulate(policy, frames, refs, count, &result) != REFSTR_OK) return -1;
    return result.faults;
}

static void test_refstring_textbook_counts(void) {
    TEST_CASE("Reference string lab: FIFO 15, LRU 12, Clock 14, OPT 9 (3 frames)");

    ASSERT_EQ(faults_for(REPL_FIFO, 3, TEXTBOOK_REFS), 15);
    ASSERT_EQ(faults_for(REPL_LRU, 3, TEXTBOOK_REFS), 12);
    ASSERT_EQ(faults_for(REPL_CLOCK, 3, TEXTBOOK_REFS), 14);
    ASSERT_EQ(faults_for(REPL_OPT, 3, TEXTBOOK_REFS), 9);

    ASSERT_EQ(faults_for(REPL_FIFO, 3, "1,2,3,4,1,2,5,1,2,3,4,5"), 9);
    ASSERT_EQ(faults_for(REPL_FIFO, 4, "1,2,3,4,1,2,5,1,2,3,4,5"), 10);
    TEST_PASS();
}

static void test_refstring_step_details(void) {
    TEST_CASE("Reference string lab: per-step frames, hits and victims");

    static uint16_t refs[REFSTR_MAX_REFS];
    static refstr_result_t r;
    int count;
    ASSERT_EQ(refstr_parse(TEXTBOOK_REFS, refs, REFSTR_MAX_REFS, &count), REFSTR_OK);
    ASSERT_EQ(count, 20);
    ASSERT_EQ(refstr_simulate(REPL_FIFO, 3, refs, count, &r), REFSTR_OK);
    ASSERT_EQ(r.num_steps, 20);

    ASSERT_TRUE(r.steps[0].fault);
    ASSERT_EQ(r.steps[0].victim, REFSTR_EMPTY);
    ASSERT_EQ(r.steps[0].frames[0], 7);
    ASSERT_EQ(r.steps[0].frames[1], REFSTR_EMPTY);

    ASSERT_TRUE(r.steps[3].fault);
    ASSERT_EQ(r.steps[3].victim, 7);
    ASSERT_EQ(r.steps[3].frames[0], 2);
    ASSERT_EQ(r.steps[3].frames[1], 0);
    ASSERT_EQ(r.steps[3].frames[2], 1);

    ASSERT_FALSE(r.steps[4].fault);
    ASSERT_EQ(r.steps[4].frame, 1);
    TEST_PASS();
}

static void test_refstring_parse_errors(void) {
    TEST_CASE("Reference string lab: input validation");

    uint16_t refs[8];
    int count;
    ASSERT_EQ(refstr_parse("", refs, 8, &count), REFSTR_ERR_EMPTY);
    ASSERT_EQ(refstr_parse("1 2 x", refs, 8, &count), REFSTR_ERR_SYNTAX);
    ASSERT_EQ(refstr_parse("1 64", refs, 8, &count), REFSTR_ERR_PAGE_RANGE);
    ASSERT_EQ(refstr_parse("1 2 3 4 5 6 7 8 9", refs, 8, &count), REFSTR_ERR_TOO_MANY);

    static refstr_result_t r;
    uint16_t one[1] = {1};
    ASSERT_EQ(refstr_simulate(REPL_LRU, 0, one, 1, &r), REFSTR_ERR_FRAMES);
    ASSERT_EQ(refstr_simulate(REPL_LRU, 17, one, 1, &r), REFSTR_ERR_FRAMES);
    TEST_PASS();
}

static void test_trace_import(void) {
    TEST_CASE("Trace import: lackey records and plain addresses map to small page numbers");

    const char *trace =
        "==123== Lackey header\n"
        "I  0400d7d4,8\n"
        " L 04f6b868,8\n"
        " S 04f6b86c,4\n"
        " M 0421c7f0,4\n"
        "# comment\n"
        "0x2000\n"
        "8192\n";
    uint16_t refs[16];
    trace_info_t info;
    ASSERT_EQ(refstr_parse_trace_text(trace, 4096, refs, 16, &info), REFSTR_OK);
    ASSERT_EQ(info.count, 5);
    ASSERT_EQ(info.distinct_pages, 3);
    ASSERT_FALSE(info.truncated);
    ASSERT_EQ(refs[0], 0);
    ASSERT_EQ(refs[1], 0);
    ASSERT_EQ(refs[2], 1);
    ASSERT_EQ(refs[3], 2);
    ASSERT_EQ(refs[4], 2);

    ASSERT_EQ(refstr_parse_trace_text("0x0\n0x1000\n0x2000\n", 4096, refs, 2, &info), REFSTR_OK);
    ASSERT_EQ(info.count, 2);
    ASSERT_TRUE(info.truncated);
    TEST_PASS();
}

static disk_input_t textbook_disk(disk_direction_t direction) {
    disk_input_t in;
    memset(&in, 0, sizeof(in));
    in.cylinders = 200;
    in.head = 53;
    in.direction = direction;
    disk_parse_requests("98 183 37 122 14 124 65 67", &in);
    return in;
}

static int disk_total(disk_policy_t policy, disk_direction_t direction) {
    disk_input_t in = textbook_disk(direction);
    disk_result_t out;
    if (disk_schedule(policy, &in, &out) != DISK_OK) return -1;
    return out.total_movement;
}

static void test_disk_textbook_totals(void) {
    TEST_CASE("Disk lab: textbook head movement (FCFS 640, SSTF 236, SCAN 236, C-SCAN 382, LOOK 299, C-LOOK 322)");

    ASSERT_EQ(disk_total(DISK_FCFS, DISK_TOWARD_HIGH), 640);
    ASSERT_EQ(disk_total(DISK_SSTF, DISK_TOWARD_HIGH), 236);
    ASSERT_EQ(disk_total(DISK_SCAN, DISK_TOWARD_LOW), 236);
    ASSERT_EQ(disk_total(DISK_SCAN, DISK_TOWARD_HIGH), 331);
    ASSERT_EQ(disk_total(DISK_CSCAN, DISK_TOWARD_HIGH), 382);
    ASSERT_EQ(disk_total(DISK_LOOK, DISK_TOWARD_HIGH), 299);
    ASSERT_EQ(disk_total(DISK_CLOOK, DISK_TOWARD_HIGH), 322);
    TEST_PASS();
}

static void test_disk_service_order(void) {
    TEST_CASE("Disk lab: service order, edges and return jumps");

    disk_input_t in = textbook_disk(DISK_TOWARD_HIGH);
    disk_result_t out;

    ASSERT_EQ(disk_schedule(DISK_SSTF, &in, &out), DISK_OK);
    static const int sstf_order[] = {65, 67, 37, 14, 98, 122, 124, 183};
    ASSERT_EQ(out.num_stops, 8);
    for (int i = 0; i < 8; i++) ASSERT_EQ(out.stops[i].cylinder, sstf_order[i]);

    ASSERT_EQ(disk_schedule(DISK_CSCAN, &in, &out), DISK_OK);
    ASSERT_EQ(out.return_movement, 199);
    ASSERT_EQ(out.stops[6].kind, DISK_STOP_EDGE);
    ASSERT_EQ(out.stops[6].cylinder, 199);
    ASSERT_EQ(out.stops[7].kind, DISK_STOP_JUMP);
    ASSERT_EQ(out.stops[7].cylinder, 0);

    ASSERT_EQ(disk_schedule(DISK_CLOOK, &in, &out), DISK_OK);
    ASSERT_EQ(out.return_movement, 169);

    in = textbook_disk(DISK_TOWARD_LOW);
    ASSERT_EQ(disk_schedule(DISK_SCAN, &in, &out), DISK_OK);
    ASSERT_EQ(out.stops[2].kind, DISK_STOP_EDGE);
    ASSERT_EQ(out.stops[2].cylinder, 0);
    ASSERT_EQ(disk_distance_moved(&out, 3), 53);
    TEST_PASS();
}

static void test_disk_edge_cases(void) {
    TEST_CASE("Disk lab: ties, one-sided queues and validation");

    disk_input_t in;
    memset(&in, 0, sizeof(in));
    in.cylinders = 100;
    in.head = 50;
    disk_result_t out;

    disk_parse_requests("60 40", &in);
    ASSERT_EQ(disk_schedule(DISK_SSTF, &in, &out), DISK_OK);
    ASSERT_EQ(out.stops[0].cylinder, 40);

    disk_parse_requests("70 90", &in);
    in.direction = DISK_TOWARD_HIGH;
    ASSERT_EQ(disk_schedule(DISK_SCAN, &in, &out), DISK_OK);
    ASSERT_EQ(out.num_stops, 2);
    ASSERT_EQ(out.total_movement, 40);
    ASSERT_EQ(disk_schedule(DISK_CSCAN, &in, &out), DISK_OK);
    ASSERT_EQ(out.return_movement, 0);

    ASSERT_EQ(disk_parse_requests("12 x", &in), DISK_ERR_SYNTAX);
    ASSERT_EQ(disk_parse_requests("", &in), DISK_ERR_EMPTY);
    disk_parse_requests("150", &in);
    ASSERT_EQ(disk_schedule(DISK_FCFS, &in, &out), DISK_ERR_RANGE);
    in.head = 100;
    disk_parse_requests("10", &in);
    ASSERT_EQ(disk_schedule(DISK_FCFS, &in, &out), DISK_ERR_HEAD);

    disk_policy_t policy;
    ASSERT_TRUE(disk_lookup("c-scan", &policy));
    ASSERT_EQ(policy, DISK_CSCAN);
    ASSERT_FALSE(disk_lookup("elevator", &policy));
    TEST_PASS();
}

static const char *TEXTBOOK_PARTITIONS =
    "h1=100 #1=10 h2=500 #2=10 h3=200 #3=10 h4=300 #4=10 h5=600 "
    "free h1 free h2 free h3 free h4 free h5 P1=212 P2=417 P3=112 P4=426";

static const contig_result_t *run_contig(contig_policy_t policy, int memory, const char *script) {
    static contig_op_t ops[CONTIG_MAX_OPS];
    static contig_result_t result;
    int count;
    if (contig_parse_script(script, ops, CONTIG_MAX_OPS, &count, NULL, 0) != CONTIG_OK) return NULL;
    if (contig_run(policy, memory, ops, count, &result) != CONTIG_OK) return NULL;
    return &result;
}

static int placed_at(const contig_result_t *r, int step) {
    return r->steps[step].ok ? r->steps[step].address : -1;
}

static void test_contig_textbook_partitions(void) {
    TEST_CASE("Memory lab: textbook 212/417/112/426 into holes 100/500/200/300/600");

    const contig_result_t *r = run_contig(CONTIG_FIRST_FIT, 1740, TEXTBOOK_PARTITIONS);
    ASSERT_TRUE(r != NULL);
    ASSERT_EQ(r->num_steps, 18);
    ASSERT_EQ(placed_at(r, 14), 110);
    ASSERT_EQ(placed_at(r, 15), 1140);
    ASSERT_EQ(placed_at(r, 16), 322);
    ASSERT_FALSE(r->steps[17].ok);
    ASSERT_EQ(r->failed, 1);

    r = run_contig(CONTIG_BEST_FIT, 1740, TEXTBOOK_PARTITIONS);
    ASSERT_EQ(placed_at(r, 14), 830);
    ASSERT_EQ(placed_at(r, 15), 110);
    ASSERT_EQ(placed_at(r, 16), 620);
    ASSERT_EQ(placed_at(r, 17), 1140);
    ASSERT_EQ(r->failed, 0);

    r = run_contig(CONTIG_WORST_FIT, 1740, TEXTBOOK_PARTITIONS);
    ASSERT_EQ(placed_at(r, 14), 1140);
    ASSERT_EQ(placed_at(r, 15), 110);
    ASSERT_EQ(placed_at(r, 16), 1352);
    ASSERT_FALSE(r->steps[17].ok);

    r = run_contig(CONTIG_NEXT_FIT, 1740, TEXTBOOK_PARTITIONS);
    ASSERT_EQ(placed_at(r, 14), 110);
    ASSERT_EQ(placed_at(r, 15), 1140);
    ASSERT_EQ(placed_at(r, 16), 1557);
    ASSERT_FALSE(r->steps[17].ok);
    TEST_PASS();
}

static void test_contig_free_merge_and_compact(void) {
    TEST_CASE("Memory lab: freeing merges holes, compaction leaves one hole");

    const contig_result_t *r = run_contig(CONTIG_FIRST_FIT, 100, "A=30 B=20 C=30 free B compact -A D=60");
    ASSERT_TRUE(r != NULL);

    const contig_step_t *after_free = &r->steps[3];
    ASSERT_EQ(after_free->stats.num_holes, 2);
    ASSERT_EQ(after_free->stats.free_total, 40);
    ASSERT_EQ(after_free->stats.largest_hole, 20);

    const contig_step_t *after_compact = &r->steps[4];
    ASSERT_EQ(after_compact->moved, 30);
    ASSERT_EQ(after_compact->stats.num_holes, 1);
    ASSERT_EQ(after_compact->memory.blocks[1].start, 30);

    const contig_step_t *after_free_a = &r->steps[5];
    ASSERT_EQ(after_free_a->stats.num_holes, 2);
    ASSERT_FALSE(r->steps[6].ok);
    ASSERT_TRUE(r->steps[6].stats.free_total >= 60);
    ASSERT_TRUE(r->steps[6].stats.external_fragmentation > 0.4);

    r = run_contig(CONTIG_FIRST_FIT, 100, "A=50 -A B=100");
    ASSERT_EQ(r->steps[1].stats.num_holes, 1);
    ASSERT_TRUE(r->steps[2].ok);
    TEST_PASS();
}

static void test_contig_parse_errors(void) {
    TEST_CASE("Memory lab: script validation");

    contig_op_t ops[4];
    int count;
    char bad[64];
    ASSERT_EQ(contig_parse_script("A=10 hello", ops, 4, &count, bad, sizeof(bad)), CONTIG_ERR_SYNTAX);
    ASSERT_STR_EQ(bad, "hello");
    ASSERT_EQ(contig_parse_script("A=0", ops, 4, &count, NULL, 0), CONTIG_ERR_SIZE);
    ASSERT_EQ(contig_parse_script("A!=5", ops, 4, &count, NULL, 0), CONTIG_ERR_NAME);
    ASSERT_EQ(contig_parse_script("", ops, 4, &count, NULL, 0), CONTIG_ERR_EMPTY);
    ASSERT_EQ(contig_parse_script("A=1 B=1 C=1 D=1 E=1", ops, 4, &count, NULL, 0), CONTIG_ERR_TOO_MANY);
    ASSERT_EQ(contig_parse_script("A=5 free A compact", ops, 4, &count, NULL, 0), CONTIG_OK);
    ASSERT_EQ(count, 3);
    ASSERT_EQ(ops[1].kind, CONTIG_OP_FREE);
    ASSERT_EQ(ops[2].kind, CONTIG_OP_COMPACT);

    static contig_result_t r;
    ASSERT_EQ(contig_run(CONTIG_FIRST_FIT, 0, ops, count, &r), CONTIG_ERR_MEMORY_SIZE);
    TEST_PASS();
}

int main(void) {
    TEST_SUITE_BEGIN("PRISM Labs Suite");

    test_refstring_textbook_counts();
    test_refstring_step_details();
    test_refstring_parse_errors();
    test_trace_import();
    test_disk_textbook_totals();
    test_disk_service_order();
    test_disk_edge_cases();
    test_contig_textbook_partitions();
    test_contig_free_merge_and_compact();
    test_contig_parse_errors();

    TEST_SUITE_SUMMARY();
    return g_tests_failed == 0 ? 0 : 1;
}
