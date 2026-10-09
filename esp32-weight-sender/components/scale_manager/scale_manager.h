#ifndef SCALE_MANAGER_H
#define SCALE_MANAGER_H

#include "scale_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SCALE_CHANNELS 4

/* Start the scale manager task */
esp_err_t scale_manager_start(QueueHandle_t event_queue);

/* Thread-safe getter for a specific channel's status */
bool scale_manager_get_channel_status(uint8_t channel_id, scale_channel_status_t* out_status);

#ifdef __cplusplus
}
#endif

#endif // SCALE_MANAGER_H

