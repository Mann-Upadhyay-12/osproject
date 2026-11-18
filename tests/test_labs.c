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

int main(void) {
    TEST_SUITE_BEGIN("PRISM Labs Suite");

    test_refstring_textbook_counts();
    test_refstring_step_details();
    test_refstring_parse_errors();
    test_trace_import();

    TEST_SUITE_SUMMARY();
    return g_tests_failed == 0 ? 0 : 1;
}
