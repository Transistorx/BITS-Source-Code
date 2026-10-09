#pragma once

/* Phase 5 prerequisite batch 1 tests (PRE-02 pin validation, PRE-03 overweight
 * cut-off, PRE-04 relay write failure, PRE-05a settle freshness, PRE-06
 * watchdog priority). Uses a TEST-ONLY relay seam (BITS_QEMU_TEST_HOOKS). */
void test_prereq_batch1_run(void);
