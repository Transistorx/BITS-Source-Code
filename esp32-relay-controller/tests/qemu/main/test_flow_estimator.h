#pragma once

/* Flow estimator tests (components/flow_estimator). Pure logic, no hardware.
 * Contains a TEST-ONLY plant/scale simulator; it is not part of any
 * production component. Results are SIMULATED accuracy, not real-world. */
void test_flow_estimator_run(void);
