#ifndef TEST_READER_SAFETY_H
#define TEST_READER_SAFETY_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dispensing-weight-source safety review fixes: single-protocol lock, resync
 * after any flush/overflow/stall, overload, strict pad, format guard, stable
 * inheritance, RAW CAPTURE. manager_queue is the running scale_manager queue
 * (NULL skips the end-to-end CH1 checks). */
void test_reader_safety_run(QueueHandle_t manager_queue);

#ifdef __cplusplus
}
#endif

#endif /* TEST_READER_SAFETY_H */
