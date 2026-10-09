#ifndef MQTT_LINK_CORE_H
#define MQTT_LINK_CORE_H

/*
 * Pure, esp-mqtt-free logic of the MQTT link: topic building, presence
 * (LWT/birth) payloads, reconnect backoff and the bounded message queue that
 * decouples the esp-mqtt event callback from everything else.
 *
 * Kept separate from components/mqtt_link so the QEMU suite can test it
 * without pulling the managed esp-mqtt component. Nothing here reads Kconfig.
 *
 * The MQTT link is observability and operator-intent delivery only. It is NOT
 * the weight path: the weight WebSocket to the relay controller is untouched.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_LINK_DEVICE_ID_MAX  64U
#define MQTT_LINK_TOPIC_MAX      96U
#define MQTT_LINK_PAYLOAD_MAX    512U
#define MQTT_LINK_RX_DEPTH       4U
#define MQTT_LINK_PUB_DEPTH      6U
#define MQTT_LINK_WEIGHT_SLOT_CTL 0U
#define MQTT_LINK_WEIGHT_SLOT_TEL 1U
#define MQTT_LINK_WEIGHT_SLOTS   2U
#define MQTT_LINK_ENV_MAX        256U  /* birth and weight/ctl size limit */

#define MQTT_LINK_WEIGHT_FUTURE_TOL_MS 100U
#define MQTT_LINK_WEIGHT_MAX_AGE_MS 3000U
#define MQTT_LINK_QUIESCE_MS 200U
#define MQTT_LINK_SOURCE_MAX 31U

#define MQTT_LINK_BACKOFF_MIN_MS 1000U
#define MQTT_LINK_BACKOFF_MAX_MS 30000U

/* Topic suffixes under cas/{device}/ (docs/telemetry/CONTRACT.md section 7). */
#define MQTT_SUFFIX_PRESENCE "status"
#define MQTT_SUFFIX_STATUS   "telemetry/status"
#define MQTT_SUFFIX_COMMANDS "commands"
#define MQTT_SUFFIX_ACK      "commands/ack"

/* Presence payloads. Retained, QoS 1. Birth is published on CONNECT only,
 * never as a side effect of telemetry/status. */
#define MQTT_LINK_LWT_JSON   "{\"online\":false}"
#define MQTT_LINK_BIRTH_JSON "{\"online\":true}"

/* Published (same topic, retained QoS 1) before an orderly disconnect. The
 * contract deliberately uses online:false so it never reads as still-online. */
#define MQTT_LINK_GRACEFUL_JSON "{\"online\":false,\"graceful\":true}"

typedef struct {
    char lwt_topic[MQTT_LINK_TOPIC_MAX];
    char birth_topic[MQTT_LINK_TOPIC_MAX];
    const char *lwt_payload;
    const char *birth_payload;
    const char *graceful_payload;
    char birth_buf[MQTT_LINK_ENV_MAX + 1U];  /* backs birth_payload once enriched */
    int qos;
    bool retain;
} mqtt_link_presence_t;

/* ---- MQTT v1 envelope (docs/telemetry/CONTRACT.md section 8) ---- */
#define MQTT_SUFFIX_WEIGHT_CTL "weight/ctl"
#define MQTT_SUFFIX_WEIGHT_TEL "telemetry/weight"
#define MQTT_LINK_BOOT_ID_LEN  8U
#define MQTT_LINK_WEIGHT_HB_PERIOD_MS 1000U

/* 8 lowercase hex chars from a random word (esp_random at the call site). */
void mqtt_link_boot_id_fmt(char out[MQTT_LINK_BOOT_ID_LEN + 1U], uint32_t rnd);
/* True for exactly 8 lowercase hex chars. */
bool mqtt_link_boot_id_valid(const char *id);

/* Single-scale identity: one physical scale moved between pump stations. The
 * id names the scale, never a pump. [A-Za-z0-9_-]{1,16}; NVS key "scale_id". */
#define MQTT_LINK_SCALE_ID_MAX     16U
#define MQTT_LINK_SCALE_ID_DEFAULT "SCALE1"
bool mqtt_link_scale_id_valid(const char *id);

/* Retained birth: {"online":true,"schema_version":1,"boot_id":..,"caps":[..],
 * "fw":..,"role":..,"scale_id":..}. caps_json is a ready JSON array. Fails
 * (false) if the result would exceed MQTT_LINK_ENV_MAX, scale_id is invalid or
 * any string needs JSON escaping. */
bool mqtt_link_birth_json(char *out, size_t cap, const char *boot_id, const char *caps_json,
                          const char *fw, const char *role, const char *scale_id);

typedef struct {
    uint32_t uptime_ms;
    const char *scale_id; /* validated; filled by mqtt_link_weight_frame */
    uint8_t src_uart;     /* diagnostic only: UART/channel the sample arrived on, 1 or 2 */
    int32_t weight_g;
    bool stable;
    uint32_t age_ms;
    uint32_t cas_seq;
    const char *source;   /* compiled transport, e.g. "CAS_RS485" */
} mqtt_link_weight_t;

/* weight/ctl (no message_id) or telemetry/weight (with_message_id) body. One
 * scale, no per-pump fields. Returns the length, or 0 if it does not fit
 * MQTT_LINK_ENV_MAX / bad input (invalid scale_id, boot_id, src_uart). */
size_t mqtt_link_weight_json(char *out, size_t cap, const char *boot_id, uint32_t seq,
                             const mqtt_link_weight_t *w, bool with_message_id);

/* Publish gating for the weight topics; touched by one task only. */
typedef struct {
    bool have_cas_seq[2];       /* dedupe per UART: index src_uart - 1 */
    uint32_t last_cas_seq[2];
    bool have_tel;
    uint32_t last_tel_ms;
    uint32_t seq_ctl;     /* ONE counter per boot for the scale; advanced only on enqueue */
    uint32_t seq_tel;
} mqtt_link_wgate_t;

void mqtt_link_wgate_init(mqtt_link_wgate_t *g);
/* True once per distinct cas_seq per UART (dedupe); marks it seen either way.
 * A src_uart other than 1/2 is never new. */
bool mqtt_link_wgate_new_frame(mqtt_link_wgate_t *g, uint8_t src_uart, uint32_t cas_seq);
bool mqtt_link_wgate_hb_due(mqtt_link_wgate_t *g, uint32_t now_ms, uint32_t period_ms);

typedef enum {
    MQTT_LINK_WPUB_NONE = 0,
    MQTT_LINK_WPUB_FRAME,
    MQTT_LINK_WPUB_HEARTBEAT
} mqtt_link_wpub_t;

mqtt_link_wpub_t mqtt_link_wgate_step(mqtt_link_wgate_t *g, uint8_t src_uart, uint32_t cas_seq,
                                      uint32_t now_ms, uint32_t period_ms);

/* [A-Za-z0-9._-]{1,64}. The topic device id is authoritative on the server. */
bool mqtt_link_device_id_valid(const char *id);

/* "cas/{device}/{suffix}". False on invalid id or truncation. */
bool mqtt_link_topic(char *out, size_t cap, const char *device_id, const char *suffix);

bool mqtt_link_presence_plan(mqtt_link_presence_t *out, const char *device_id);

/* Replaces the plain birth with the enriched one (LWT stays {"online":false}). */
bool mqtt_link_presence_set_birth(mqtt_link_presence_t *p, const char *boot_id,
                                  const char *caps_json, const char *fw, const char *role,
                                  const char *scale_id);

/* 1 s doubling to a 30 s ceiling: attempt 0 -> 1000, 1 -> 2000 ... 5+ -> 30000. */
uint32_t mqtt_link_backoff_ms(uint32_t attempt);

/* Backoff plus up to 25% jitter from rnd (never below the plain backoff, and
 * still capped at MQTT_LINK_BACKOFF_MAX_MS). */
uint32_t mqtt_link_backoff_jitter_ms(uint32_t attempt, uint32_t rnd);

/* Copies uri into out with any "user:password@" userinfo removed, so a broker
 * URI can be logged without leaking credentials. */
void mqtt_link_uri_redact(char *out, size_t cap, const char *uri);

typedef enum {
    MQTT_ITEM_PUB = 0,  /* outbound: published by the mqtt_pub task */
    MQTT_ITEM_RX,       /* inbound: delivered to the command handler */
    MQTT_ITEM_WEIGHT
} mqtt_item_kind_t;

typedef struct {
    uint8_t  kind;
    uint8_t  qos;
    bool     retain;
    bool     from_weight;
    uint16_t len;
    uint32_t recv_ms;
    char     topic[MQTT_LINK_TOPIC_MAX];
    char     data[MQTT_LINK_PAYLOAD_MAX + 1U];
} mqtt_item_t;

typedef enum {
    MQTT_ENQ_OK = 0,
    MQTT_ENQ_FULL,       /* queue saturated: dropped, never blocked */
    MQTT_ENQ_TOO_BIG,
    MQTT_ENQ_FRAGMENT,   /* partial message: never acted on */
    MQTT_ENQ_RETAINED,   /* retain=1 inbound: ignored, never acted on */
    MQTT_ENQ_INVALID
} mqtt_enq_result_t;

typedef struct {
    QueueHandle_t rx;
    QueueHandle_t pub;
    QueueHandle_t weight[MQTT_LINK_WEIGHT_SLOTS];
    SemaphoreHandle_t doorbell;
    StaticSemaphore_t doorbell_buf;
    atomic_bool open;
    atomic_uint users;
    _Atomic uint32_t rx_dropped_full;
    _Atomic uint32_t pub_dropped_full;
    _Atomic uint32_t weight_overwritten;
    _Atomic uint32_t weight_dropped_stale;
    _Atomic uint32_t weight_dropped_future;
    _Atomic uint32_t dropped_other;
} mqtt_link_queues_t;

bool mqtt_link_queues_init(mqtt_link_queues_t *q, size_t rx_depth, size_t pub_depth);

mqtt_enq_result_t mqtt_link_queue_push_rx(mqtt_link_queues_t *q,
                                          const char *topic, size_t topic_len,
                                          const char *data, size_t data_len,
                                          size_t total_len, bool retain,
                                          uint32_t now_ms);

mqtt_enq_result_t mqtt_link_queue_push_pub(mqtt_link_queues_t *q,
                                           const char *topic, const char *data,
                                           int qos, bool retain);

mqtt_enq_result_t mqtt_link_queue_put_weight(mqtt_link_queues_t *q, unsigned slot,
                                             const char *topic, const char *data,
                                             uint32_t now_ms);

mqtt_enq_result_t mqtt_link_queue_put_weight_sample(mqtt_link_queues_t *q, unsigned slot,
                                                    const char *topic, const char *boot_id,
                                                    uint32_t seq, const mqtt_link_weight_t *w,
                                                    bool with_message_id, uint32_t now_ms);

bool mqtt_link_queue_next(mqtt_link_queues_t *q, mqtt_item_t *out, uint32_t timeout_ms,
                          uint32_t now_ms, uint32_t stale_ms);

size_t mqtt_link_queue_pending(mqtt_link_queues_t *q);

typedef struct {
    bool connected;
    uint32_t published;
    uint32_t publish_failed;
    uint32_t queue_depth;
    uint32_t dropped_full;
    uint32_t dropped_other;
    uint32_t dropped_offline;
    uint32_t rx_dropped_full;
    uint32_t pub_dropped_full;
    uint32_t weight_published;
    uint32_t weight_overwritten;
    uint32_t weight_dropped_stale;
    uint32_t weight_dropped_future;
    uint16_t heap_min_kb;
    uint8_t link_state;
    bool pub_stuck;
    bool teardown_pending;
    uint16_t stop_pub_stuck;
    uint16_t stop_deferred;
} mqtt_link_stats_t;

void mqtt_link_queue_stats(mqtt_link_queues_t *q, mqtt_link_stats_t *out);

uint16_t mqtt_link_sat16(uint32_t v);

typedef struct {
    const char *role;
    const char *boot_id;
    const char *firmware;
    const char *cas_link;
    uint32_t cas_seq;
    uint32_t cas_age_ms;
    bool ws_client;
    uint32_t uptime_ms;
    const uint32_t *hb;
    uint8_t hb_n;
} mqtt_link_status_t;

#define MQTT_LINK_STATUS_HB_MAX 3U

size_t mqtt_link_status_json(char *out, size_t cap, const mqtt_link_status_t *st,
                             const mqtt_link_stats_t *m);

typedef void (*mqtt_link_flush_cb)(const mqtt_item_t *item, void *ctx);
size_t mqtt_link_queue_flush(mqtt_link_queues_t *q, mqtt_item_t *scratch, size_t max_items,
                             mqtt_link_flush_cb cb, void *ctx);

bool mqtt_link_queues_deinit(mqtt_link_queues_t *q);

/* Link lifecycle: IDLE -> RUNNING -> STOPPING -> IDLE. begin_stop succeeds once
 * per run, so stop is idempotent; start is refused until stop has finished. */
typedef enum { MQTT_LC_IDLE = 0, MQTT_LC_RUNNING, MQTT_LC_STOPPING } mqtt_lc_state_t;
typedef struct { atomic_int state; } mqtt_link_lc_t;

void mqtt_link_lc_init(mqtt_link_lc_t *lc);
bool mqtt_link_lc_start(mqtt_link_lc_t *lc);       /* IDLE -> RUNNING */
bool mqtt_link_lc_begin_stop(mqtt_link_lc_t *lc);  /* RUNNING -> STOPPING, once */
void mqtt_link_lc_end_stop(mqtt_link_lc_t *lc);    /* STOPPING -> IDLE */
bool mqtt_link_lc_running(const mqtt_link_lc_t *lc);
mqtt_lc_state_t mqtt_link_lc_state(const mqtt_link_lc_t *lc);

typedef enum { MQTT_HANDOFF_RUN = 0, MQTT_HANDOFF_DONE, MQTT_HANDOFF_ORPHAN } mqtt_handoff_state_t;
typedef struct { atomic_int state; } mqtt_link_handoff_t;

void mqtt_link_handoff_init(mqtt_link_handoff_t *h);
bool mqtt_link_handoff_task_exit(mqtt_link_handoff_t *h);
bool mqtt_link_handoff_wait(mqtt_link_handoff_t *h, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LINK_CORE_H */
