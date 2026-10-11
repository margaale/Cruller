// The host tests' checks and runner (each tests/test_*.c includes it; tests/run.sh builds and runs them):
// CHECK counts a check and says where one fails; RUN(test) runs a test and prints "ok" or "FAIL" with its
// name; main returns tests_done(), which prints the totals.

#pragma once

#include <stdio.h>
#include <string.h>

static int failures, checks, tests_run, tests_failed;
static const char *current; // the test running (for CHECK's message)

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, current, #cond); } } while (0)
#define CHECK_STR_HAS(s, sub) do { checks++; if (!strstr((s), (sub))) { failures++; \
    printf("  FAIL %s:%d in %s: \"%s\" lacks \"%s\"\n", __FILE__, __LINE__, current, (s), (sub)); } } while (0)

#define RUN(test) do { \
    const int before_ = failures; \
    current = #test; \
    test(); \
    tests_run++; \
    if (failures != before_) tests_failed++; \
    printf("%s %s\n", failures == before_ ? "ok  " : "FAIL", #test); \
} while (0)

static inline int tests_done(void) {
    printf("\n%d tests, %d checks, %d failed\n", tests_run, checks, tests_failed);
    return tests_failed ? 1 : 0;
}
