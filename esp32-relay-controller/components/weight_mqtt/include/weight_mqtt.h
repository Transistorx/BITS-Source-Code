#ifndef WEIGHT_MQTT_H
#define WEIGHT_MQTT_H

/*
 * Weight-over-MQTT intake (docs/telemetry/CONTRACT.md 8.2/8.3/9.11). OFF unless
 * weight_transport=mqtt AND peer_sender_id is a valid topic segment.
 *
 * SINGLE-SCALE operation: one CAS scale, one sender stream. This module only
 * VALIDATES and ORDERS messages from cas/<peer>/weight/ctl, keeps ONE cache keyed
 * by (scale_id, boot_id), and re-expresses each accepted message as ONE
 * single-scale sample (weight_g, stable, scale_id, boot_id, sequence, age_ms) for
 * weight_receiver_on_message(). The sender's src_uart tag is diagnostic only and is
 * NOT a pump: there is no channel, no per-pump slot and no neighbour bundling.
 * It does not touch safety_manager, relays, the 5000 ms age rule, the 5 s
 * no-valid-weight rule or link-loss handling: those stay in weight_receiver. MQTT
 * broker/connection state is never an input here; "link lost" is simply no accepted
 * message inside the receiver window.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WEIGHT_SEL_WS = 0,        /* default: WebSocket feeds the receiver */
    WEIGHT_SEL_MQTT,          /* mqtt selected and peer_sender_id valid */
    WEIGHT_SEL_MQTT_BLOCKED   /* mqtt selected, peer id missing/invalid: no weight at all */
} weight_sel_t;

/* Pure. Only the exact string "mqtt" selects MQTT; anything else is "ws". */
weight_sel_t weight_mqtt_select(const char *transport, const char *peer_sender_id);

typedef enum {
    WMQ_FED = 0,         /* accepted and handed to weight_receiver_on_message */
    WMQ_PENDING_BOOT,    /* new boot_id awaiting its 2nd consecutive message */
    WMQ_DROP_DISABLED,   /* transport is not mqtt */
    WMQ_DROP_INVALID,    /* schema / range / size / shape (incl. any `channel` field) */
    WMQ_DROP_OLD,        /* duplicate or older seq (timestamp not moved) */
    WMQ_DROP_TRANSIT,    /* > 300 ms over the sliding-window minimum offset */
    WMQ_DROP_SCALE       /* scale_id is not the configured expected scale_id */
} weight_mqtt_result_t;

typedef struct {
    uint32_t fed, pending, dropped_disabled, dropped_invalid, dropped_old, dropped_transit,
             dropped_scale;
} weight_mqtt_stats_t;

#define WEIGHT_MQTT_PAYLOAD_MAX   256U
#define WEIGHT_MQTT_TRANSIT_MAX_MS 300U   /* allowed excess over the window minimum */
#define WEIGHT_MQTT_SCALE_ID_DEFAULT "SCALE1"

void weight_mqtt_init(void);                 /* clears cache, counters, disables; expected id = SCALE1 */
void weight_mqtt_set_enabled(bool enabled);
bool weight_mqtt_enabled(void);

/* Expected scale_id (NVS `scale_id`, [A-Za-z0-9_-]{1,16}). A frame with another
 * scale_id is rejected and counted (dropped_scale). An invalid id installs an
 * expectation nothing can match, so NO weight is accepted (fail safe). */
bool weight_mqtt_set_expected_scale_id(const char *id);
const char *weight_mqtt_expected_scale_id(void);

/* recv_ms: local receive time (tick ms), the same domain as weight_receiver. */
weight_mqtt_result_t weight_mqtt_on_payload(const char *json, size_t len, uint32_t recv_ms);
void weight_mqtt_stats(weight_mqtt_stats_t *out);

#endif /* WEIGHT_MQTT_H */
