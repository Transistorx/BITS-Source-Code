#ifndef MQTT_LINK_H
#define MQTT_LINK_H

/*
 * MQTT link over esp-mqtt for the relay controller. Observability and
 * operator-intent delivery only: the weight path stays the WebSocket and never
 * touches this component, and the server never drives a relay through it.
 *
 * Threading:
 *  - The esp-mqtt event callback only copies into a bounded RX queue.
 *  - "mqtt_pub" is the single place that publishes and drives reconnect
 *    backoff. A slow or stalled publish can delay telemetry and ACKs only.
 *  - Commands are consumed from the RX queue by the caller's own task (via
 *    mqtt_link_rx_pop), never by mqtt_pub, so a STOP never waits behind a
 *    publish.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mqtt_link_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* device_id NULL/empty: Kconfig override, else NVS. Safe to call before Wi-Fi
 * is up: reconnect is retried with 1-30 s backoff. */
esp_err_t mqtt_link_start(const char *device_id);

/* Orderly stop: publishes retained {"online":false,"graceful":true} (bounded
 * wait, <~1 s), stops the client and pub task, drops queued work. Idempotent;
 * mqtt_link_start() may be called again afterwards. Registered as an
 * esp_register_shutdown_handler on first start. Not from the MQTT event task. */
esp_err_t mqtt_link_stop(void);

/* Queue one publish under cas/{device}/{topic_suffix}. Zero-timeout enqueue,
 * returns false when the queue is full, the body is too big or the link is not
 * started. QoS 0 messages are dropped by the pub task while disconnected. */
bool mqtt_link_publish(const char *topic_suffix, const char *json, int qos, bool retain);

/* Next complete, non-retained message on cas/{device}/commands. Waits up to
 * timeout_ms (sleeps that long when the link is not running). */
bool mqtt_link_rx_pop(mqtt_rx_item_t *out, uint32_t timeout_ms);

/* weight_transport=mqtt: call BEFORE mqtt_link_start with the NVS peer_sender_id.
 * Subscribes (QoS 0) to exactly cas/<peer>/weight/ctl. False (and weight stays
 * off) when the id is missing or not a valid topic segment. */
bool mqtt_link_set_weight_peer(const char *peer_id);
/* Next complete, non-retained weight message from the separate weight queue. */
bool mqtt_link_weight_pop(mqtt_wrx_item_t *out, uint32_t timeout_ms);

/* Run lifecycle (CONTRACT 9.2): call BEFORE mqtt_link_start. Subscribes (QoS 1) to
 * exactly cas/{device}/run/ack on every connect. Acks land in their OWN small bounded
 * queue (zero-timeout push in the callback, drop+count when full) and are read with
 * mqtt_link_run_ack_pop by the run task; they never enter the command RX queue, so
 * mqtt_link_rx_pop is never ended or delayed by an ack. Not set = no subscription. */
void mqtt_link_set_run_ack(bool on);
/* Next complete, non-retained run/ack body (NUL-terminated, *len bytes) or false. */
bool mqtt_link_run_ack_pop(char *out, size_t cap, size_t *len, uint32_t timeout_ms);

bool mqtt_link_connected(void);
const char *mqtt_link_device_id(void);

typedef struct {
    uint32_t rx_dropped_full;
    uint32_t tx_dropped_full;
    uint32_t tx_dropped_live;       /* telemetry/live drops only */
    uint32_t dropped_other;
    uint32_t dropped_retained;
    uint32_t telemetry_dropped_net; /* QoS 0 dropped instead of blocking the link */
    uint32_t weight_dropped_full;   /* weight queue full: dropped, never blocking */
    uint32_t connects;
    uint32_t disconnects;
} mqtt_link_stats_t;
void mqtt_link_stats(mqtt_link_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LINK_H */
