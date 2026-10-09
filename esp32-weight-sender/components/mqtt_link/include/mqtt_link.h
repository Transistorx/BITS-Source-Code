#ifndef MQTT_LINK_H
#define MQTT_LINK_H

/*
 * MQTT link over esp-mqtt. Observability and operator-intent delivery only: the
 * weight path stays the WebSocket to the relay controller and never touches
 * this component.
 *
 * Threading: the esp-mqtt event callback only copies into a bounded queue.
 * The "mqtt_pub" task is the single place that publishes, subscribes, runs
 * the command handler and drives reconnect backoff.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqtt_link_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runs in the mqtt_pub task for each non-retained, complete message on
 * cas/{device}/commands. It must not block for long. */
typedef void (*mqtt_link_command_cb)(const char *payload, size_t len, bool retain,
                                     uint32_t recv_ms);

void mqtt_link_set_command_handler(mqtt_link_command_cb cb);

/* Needs NVS (device_id) and a network stack; safe to call before Wi-Fi is up
 * because reconnect is retried with 1-30 s backoff. */
esp_err_t mqtt_link_start(void);

/* Orderly stop: publishes retained {"online":false,"graceful":true} (bounded
 * wait, <~1 s), stops the client and pub task, frees the queue and outbox.
 * Idempotent; mqtt_link_start() may be called again afterwards. Registered as
 * an esp_register_shutdown_handler on first start. Not from the MQTT event task. */
esp_err_t mqtt_link_stop(void);

/* Queue one publish under cas/{device}/{topic_suffix}. Zero-timeout enqueue,
 * returns false when the queue is full or the link is not started. QoS 0
 * messages are dropped by the pub task while disconnected. */
bool mqtt_link_publish(const char *topic_suffix, const char *json, int qos, bool retain);

bool mqtt_link_connected(void);

/* Lock-free snapshot for the health line. Counters only; never blocks. */
typedef struct {
    bool connected;
    uint32_t published;       /* publishes handed to esp-mqtt */
    uint32_t publish_failed;  /* esp-mqtt refused the publish */
    uint32_t queue_depth;     /* items waiting for the pub task */
    uint32_t dropped_full;    /* enqueue refused: queue full */
    uint32_t dropped_other;   /* oversize / fragment / retained */
    uint32_t dropped_offline; /* QoS 0 refused while disconnected */
} mqtt_link_stats_t;
void mqtt_link_get_stats(mqtt_link_stats_t *out);
const char *mqtt_link_device_id(void);

/* Scale identity from NVS "scale_id" (default SCALE1), resolved in mqtt_link_start(). */
const char *mqtt_link_scale_id(void);

/* 8 hex chars from esp_random, generated once per boot, never persisted. */
const char *mqtt_link_boot_id(void);

/* fw/role strings (must outlive the link) carried in the birth message. Call
 * before mqtt_link_start(). */
void mqtt_link_set_identity(const char *fw, const char *role);

/* Offer one valid, real CAS sample. Publishes cas/{dev}/weight/ctl and
 * cas/{dev}/telemetry/weight once per new cas_seq, plus a telemetry/weight
 * heartbeat every MQTT_LINK_WEIGHT_HB_PERIOD_MS without a new cas_seq (QoS 0,
 * retain=false). Never blocks; drops are counted in the stats. Call only from
 * one task and only with WEIGHT_SOURCE_RESULT_REAL samples. */
void mqtt_link_weight_frame(const mqtt_link_weight_t *w, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LINK_H */
