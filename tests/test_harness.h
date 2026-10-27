#ifndef PRISM_TEST_HARNESS_H
#define PRISM_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define TEST_SUITE_BEGIN(name) \
    printf("\n=== Running Test Suite: %s ===\n", name)

#define TEST_CASE(name) \
    do { \
        printf("  [RUN]  %s ... ", name); \
        g_tests_run++; \
    } while(0)

#define TEST_PASS() \
    printf("\033[32mPASS\033[0m\n")

#define TEST_FAIL(msg, ...) \
    do { \
        printf("\033[31mFAIL\033[0m\n    " msg "\n", ##__VA_ARGS__); \
        g_tests_failed++; \
    } while(0)

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            TEST_FAIL("Assertion failed: (%s) at %s:%d", #cond, __FILE__, __LINE__); \
            return; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        long long _a = (long long)(a); \
        long long _b = (long long)(b); \
        if (_a != _b) { \
            TEST_FAIL("Expected %s == %s (%lld != %lld) at %s:%d", #a, #b, _a, _b, __FILE__, __LINE__); \
            return; \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            TEST_FAIL("Expected \"%s\" == \"%s\" at %s:%d", (a), (b), __FILE__, __LINE__); \
            return; \
        } \
    } while(0)

#define TEST_SUITE_SUMMARY() \
    do { \
        printf("\n--------------------------------------------------\n"); \
        if (g_tests_failed == 0) { \
            printf("\033[32mAll %d tests passed successfully!\033[0m\n", g_tests_run); \
        } else { \
            printf("\033[31m%d of %d tests failed!\033[0m\n", g_tests_failed, g_tests_run); \
        } \
        printf("--------------------------------------------------\n\n"); \
    } while(0)

#endif /* PRISM_TEST_HARNESS_H */
