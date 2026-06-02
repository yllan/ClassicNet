#ifndef CN_TEST_H
#define CN_TEST_H

/*
 * Minimal, dependency-free unit-test harness for host builds.
 * Each test executable is a single translation unit with its own main().
 */

#include <stdio.h>

static int cn_tests_run = 0;
static int cn_tests_failed = 0;

#define CN_CHECK(cond)                                              \
    do {                                                           \
        cn_tests_run++;                                            \
        if (!(cond)) {                                             \
            cn_tests_failed++;                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                         \
    } while (0)

#define CN_RUN(fn)        \
    do {                  \
        printf("- %s\n", #fn); \
        fn();             \
    } while (0)

/* Use as: return CN_SUMMARY();  -- nonzero exit on any failure (for ctest). */
#define CN_SUMMARY()                                               \
    (printf("\n%d checks, %d failed\n", cn_tests_run, cn_tests_failed), \
     (cn_tests_failed == 0 ? 0 : 1))

#endif /* CN_TEST_H */
