#ifndef TEST_DIAG_CHANNELS_H
#define TEST_DIAG_CHANNELS_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-channel diagnostics, CH1 selection and cross-channel isolation.
 * manager_queue is the already-running scale_manager's event queue. */
void test_diag_channels_run(QueueHandle_t manager_queue);

#ifdef __cplusplus
}
#endif

#endif /* TEST_DIAG_CHANNELS_H */
