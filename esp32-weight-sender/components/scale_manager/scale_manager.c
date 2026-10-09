#include "scale_manager.h"
#include "scale_events.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "RS485";

static const char *channel_state_name(channel_state_t s)
{
    switch (s) {
    case CHANNEL_STATE_DISABLED: return "DISABLED";
    case CHANNEL_STATE_OFFLINE:  return "OFFLINE";
    case CHANNEL_STATE_ONLINE:   return "ONLINE";
    case CHANNEL_STATE_ERROR:    return "ERROR";
    default:                     return "UNKNOWN";
    }
}

static scale_channel_status_t channel_states[MAX_SCALE_CHANNELS];
static SemaphoreHandle_t manager_mutex = NULL;

static void scale_manager_task(void *arg) {
    QueueHandle_t event_queue = (QueueHandle_t)arg;
    scale_event_t ev;

    while (1) {
        if (xQueueReceive(event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            if (ev.channel_id >= MAX_SCALE_CHANNELS) continue;

            xSemaphoreTake(manager_mutex, portMAX_DELAY);
            
            scale_channel_status_t* status = &channel_states[ev.channel_id];
            status->channel_id = ev.channel_id;
            channel_state_t previous_state = status->state;
            
            switch (ev.type) {
                case SCALE_EVENT_ONLINE:
                    status->state = CHANNEL_STATE_ONLINE;
                    break;
                case SCALE_EVENT_OFFLINE:
                    status->state = CHANNEL_STATE_OFFLINE;
                    break;
                case SCALE_EVENT_PROTOCOL_ERROR:
                case SCALE_EVENT_TRANSPORT_ERROR:
                    status->state = CHANNEL_STATE_ERROR;
                    break;
                case SCALE_EVENT_READING:
                    {
                    /* Keep acquisition age through queue backlog. Dequeue time
                     * must never make an old UART frame fresh again. */
                    uint32_t reading_time_ms = ev.reading.timestamp_ms;
                    if (ev.reading.has_gross || ev.reading.has_display) {
                        status->weight_sequence++;
                    }
                    if (ev.reading.has_gross) {
                        status->latest_reading.gross = ev.reading.gross;
                        status->latest_reading.has_gross = true;
                        status->latest_reading.gross_is_physical = ev.reading.gross_is_physical;
                        status->gross_update_ms = reading_time_ms;
                    }
                    if (ev.reading.has_net) {
                        status->latest_reading.net = ev.reading.net;
                        status->latest_reading.has_net = true;
                    }
                    if (ev.reading.has_tare) {
                        status->latest_reading.tare = ev.reading.tare;
                        status->latest_reading.has_tare = true;
                    }
                    if (ev.reading.has_display) {
                        status->latest_reading.display = ev.reading.display;
                        status->latest_reading.has_display = true;
                        status->latest_reading.display_is_physical = ev.reading.display_is_physical;
                        status->display_update_ms = reading_time_ms;
                    }
                    if (ev.reading.has_unit) {
                        strncpy(status->latest_reading.unit, ev.reading.unit, sizeof(status->latest_reading.unit) - 1);
                        status->latest_reading.unit[sizeof(status->latest_reading.unit) - 1] = '\0';
                        status->latest_reading.has_unit = true;
                    }
                    if (ev.reading.has_status) {
                        status->latest_reading.stable = ev.reading.stable;
                        status->latest_reading.overload = ev.reading.overload;
                        status->latest_reading.underload = ev.reading.underload;
                        status->latest_reading.error = ev.reading.error;
                        status->latest_reading.zero = ev.reading.zero;
                        status->latest_reading.center_zero = ev.reading.center_zero;
                        status->latest_reading.display_net = ev.reading.display_net;
                        status->latest_reading.raw_status = ev.reading.raw_status;
                        status->latest_reading.raw_error = ev.reading.raw_error;
                        status->latest_reading.has_status = true;
                        status->status_update_ms = reading_time_ms;
                    } else if (ev.reading.has_gross || ev.reading.has_display) {
                        /* A weight with no status of its own must NEVER inherit
                         * the previous frame's stable/overload flags: unknown
                         * status is "not stable". */
                        status->latest_reading.has_status = false;
                        status->latest_reading.stable = false;
                        status->latest_reading.overload = false;
                        status->latest_reading.underload = false;
                        status->latest_reading.error = false;
                        status->latest_reading.zero = false;
                        status->latest_reading.center_zero = false;
                        status->status_update_ms = 0U;
                    }
                    status->latest_reading.valid = true;
                    status->last_update_ms = reading_time_ms;
                    status->state = CHANNEL_STATE_ONLINE;
                    break;
                    }
            }
            
            channel_state_t next_state = status->state;
            xSemaphoreGive(manager_mutex);
            if (previous_state != next_state) {
                /* Logged after the mutex is released. Transitions only. */
                ESP_LOGI(TAG, "CH%u link %s -> %s", (unsigned)ev.channel_id + 1U,
                         channel_state_name(previous_state), channel_state_name(next_state));
            }
        }
    }
}

esp_err_t scale_manager_start(QueueHandle_t event_queue) {
    if (!event_queue) return ESP_FAIL;

    manager_mutex = xSemaphoreCreateMutex();
    if (!manager_mutex) return ESP_FAIL;

    for (int i = 0; i < MAX_SCALE_CHANNELS; i++) {
        channel_states[i].channel_id = i;
        channel_states[i].state = CHANNEL_STATE_DISABLED;
        memset(&channel_states[i].latest_reading, 0, sizeof(scale_reading_t));
    }

    BaseType_t ret = xTaskCreate(scale_manager_task, "scale_manager", 3072, event_queue, 4, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool scale_manager_get_channel_status(uint8_t channel_id, scale_channel_status_t* out_status) {
    if (channel_id >= MAX_SCALE_CHANNELS || !out_status || !manager_mutex) return false;

    xSemaphoreTake(manager_mutex, portMAX_DELAY);
    *out_status = channel_states[channel_id];
    xSemaphoreGive(manager_mutex);

    return true;
}
