#pragma once

/*
 * tests.h — a deliberately tiny assertion harness.
 *
 * mshell cross-compiles to Windows, so its own code cannot run here. What CAN
 * run is the logic that has no Windows in it, and those units (match.c,
 * layout_math.c) are compiled natively by `make test` and exercised directly.
 * No framework, no dependency: a counter, a macro, and a non-zero exit code.
 */

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int tests_run, tests_failed;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        tests_run++;                                                          \
        if (!(cond)) {                                                        \
            tests_failed++;                                                   \
            printf("  FAIL  %s:%d  ", __FILE__, __LINE__);                    \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static int tests_report(const char *suite) {
    if (tests_failed)
        printf("%s: %d/%d failed\n", suite, tests_failed, tests_run);
    else
        printf("  ok    %s (%d assertions)\n", suite, tests_run);
    return tests_failed ? 1 : 0;
}
