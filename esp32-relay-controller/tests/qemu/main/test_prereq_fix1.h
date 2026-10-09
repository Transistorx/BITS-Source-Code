#pragma once

/* Phase 5 batch 1 fix round 1 (H1, M1..M6, L1..L3, L6, L8). Names are prefixed
 * fix1_. Software only: the PCF8574 is a TEST-ONLY fake behind the
 * BITS_QEMU_TEST_HOOKS seams; no hardware is ever actuated. */
void test_prereq_fix1_run(void);

/* Runs fn to completion in a task with a normal-sized stack (the QEMU project's
 * main task stack is only 3584 bytes). Same core and priority as the caller. */
void test_run_on_big_stack(void (*fn)(void));
