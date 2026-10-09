#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

/*
 * Shared check sink for the QEMU suite. test_main.c owns the pass/fail
 * counters and the TEST_PASS / TEST_FAIL logging; additional test translation
 * units call test_check() so their results land in the same summary.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void test_check(bool condition, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* TEST_HARNESS_H */
