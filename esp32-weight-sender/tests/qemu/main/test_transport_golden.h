#ifndef TEST_TRANSPORT_GOLDEN_H
#define TEST_TRANSPORT_GOLDEN_H

/*
 * Golden-reference tests for the RS232 scale path.
 *
 * These pin the behaviour recorded in
 * docs/superpowers/baseline/rs232-golden/GOLDEN_BASELINE.md BEFORE the
 * transport abstraction existed. They must pass unchanged after the refactor
 * and after any future RS485 backend is added: if a golden frame starts
 * parsing differently, something changed the frozen contract.
 */

#ifdef __cplusplus
extern "C" {
#endif

void test_transport_golden_run(void);

#ifdef __cplusplus
}
#endif

#endif /* TEST_TRANSPORT_GOLDEN_H */
