#pragma once

/* Auto-tune core tests (components/autotune_core). Pure logic, no hardware.
 * Contains a TEST-ONLY plant/scale simulator and the SIMULATION FIXTURE
 * limits. All numbers are illustrative simulation fixtures, not hardware
 * settings and not real-world accuracy. */
void test_autotune_run(void);
