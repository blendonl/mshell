#pragma once

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
