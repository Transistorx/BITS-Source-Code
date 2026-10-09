#ifndef MQTT_LINK_CORE_H
#define MQTT_LINK_CORE_H

/*
 * Pure, esp-mqtt-free logic of the relay controller's MQTT link: topic
 * building, presence (LWT/birth) payloads, reconnect backoff and the two
 * bounded queues that decouple the esp-mqtt event callback from everything
 * else. Mirrors esp32-weight-sender/components/mqtt_link_core, with one
 * deliberate difference: the relay publishes status/sample bodies of several
 * KiB, so the outbound queue carries a heap copy of the body (allocated by the
 * publishing task, never by the callback) while the inbound queue keeps the
 * fixed in-line item.
 *
 * Kept separate from components/mqtt_link so the QEMU suite can test it
 * without pulling the managed esp-mqtt component. Nothing here reads Kconfig.
 *
 * The MQTT link is observability and operator-intent delivery only. It is NOT
 * the weight path: the weight WebSocket is untouched.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_LINK_DEVICE_ID_MAX   64U
#define MQTT_LINK_TOPIC_MAX       96U
#define MQTT_LINK_RX_PAYLOAD_MAX  1024U  /* largest command (JOB + pinned profile) */
#define MQTT_LINK_TX_PAYLOAD_MAX  4864U  /* largest sample batch (BODY_CAP 4800) */
#define MQTT_LINK_RX_DEPTH        8U
#define MQTT_LINK_TX_DEPTH        8U
/* QoS 0 telemetry may only fill this many TX slots; the rest stay free for
 * QoS 1 command ACKs so a telemetry burst can never starve an ACK. */
#define MQTT_LINK_TX_QOS0_LIMIT   3U
/* Live-weight (5 Hz) is droppable first: it may only use this many slots, so one
 * QoS 0 slot always stays free for samples/events. */
#define MQTT_LINK_TX_LIVE_LIMIT   2U

/* Outbox bytes telemetry may never take: room for QoS 1 ACKs and presence. */
#define MQTT_LINK_ACK_RESERVE     2048U

#define MQTT_LINK_BACKOFF_MIN_MS  1000U
#define MQTT_LINK_BACKOFF_MAX_MS  30000U

/* Topic suffixes under cas/{device}/ (docs/telemetry/CONTRACT.md section 7). */
#define MQTT_SUFFIX_PRESENCE "status"
#define MQTT_SUFFIX_STATUS   "telemetry/status"
#define MQTT_SUFFIX_SAMPLES  "telemetry/samples"
#define MQTT_SUFFIX_EVENTS   "telemetry/events"
#define MQTT_SUFFIX_LIVE     "telemetry/live"   /* QoS 0, not retained, 5 Hz weight only */
#define MQTT_SUFFIX_COMMANDS "commands"
#define MQTT_SUFFIX_ACK      "commands/ack"
/* Peer sender weight topic (cas/<peer>/weight/ctl): subscribed only when
 * weight_transport=mqtt; exact topic, never a wildcard. */
#define MQTT_SUFFIX_WEIGHT_CTL "weight/ctl"

/* Presence payloads. Retained, QoS 1. Birth is published on CONNECT only,
 * never as a side effect of telemetry. */
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
    int qos;
    bool retain;
} mqtt_link_presence_t;

/* What the link does on EVERY (re)connect: clean session means the broker has
 * forgotten the subscription, so it is renewed each time, then birth is sent. */
typedef struct {
    char subscribe_topic[MQTT_LINK_TOPIC_MAX];
    int subscribe_qos;
    mqtt_link_presence_t presence;
} mqtt_link_connect_plan_t;

/* 8 lowercase hex from esp_random, generated once per boot (not persisted). */
#define MQTT_LINK_BOOT_ID_LEN 8U
const char *mqtt_link_boot_id(void);
void mqtt_link_boot_id_format(char out[MQTT_LINK_BOOT_ID_LEN + 1U], uint32_t rnd);
/* Birth body: {"online":true,"boot_id":"..","caps":["cmd_mqtt","weight_mqtt"]}. */
bool mqtt_link_birth_build(char *out, size_t cap, const char *boot_id);

/* [A-Za-z0-9._-]{1,64}. The topic device id is authoritative on the server. */
bool mqtt_link_device_id_valid(const char *id);

/* "cas/{device}/{suffix}". False on invalid id or truncation. */
bool mqtt_link_topic(char *out, size_t cap, const char *device_id, const char *suffix);

bool mqtt_link_presence_plan(mqtt_link_presence_t *out, const char *device_id);
bool mqtt_link_connect_plan(mqtt_link_connect_plan_t *out, const char *device_id);

/* 1 s doubling to a 30 s ceiling: attempt 0 -> 1000, 1 -> 2000 ... 5+ -> 30000. */
uint32_t mqtt_link_backoff_ms(uint32_t attempt);

/* Reconnect schedule driven by the pub task. */
typedef struct {
    uint32_t attempt;
    bool pending;
    int64_t due_us;
} mqtt_link_reconnect_t;

void mqtt_link_reconnect_init(mqtt_link_reconnect_t *r);
/* Disconnect seen: schedules the next attempt, returns the wait in ms. */
uint32_t mqtt_link_reconnect_on_disconnect(mqtt_link_reconnect_t *r, int64_t now_us);
/* True exactly once when the scheduled attempt is due. */
bool mqtt_link_reconnect_due(mqtt_link_reconnect_t *r, int64_t now_us);
/* Connected: the backoff starts over. */
void mqtt_link_reconnect_on_connected(mqtt_link_reconnect_t *r);

/* Copies uri into out with any "user:password@" userinfo removed, so a broker
 * URI can be logged without leaking credentials. */
void mqtt_link_uri_redact(char *out, size_t cap, const char *uri);

/* Inbound item: copied in-line by the esp-mqtt callback (no heap there). */
typedef struct {
    uint16_t len;
    uint32_t recv_ms;   /* xTaskGetTickCount() * portTICK_PERIOD_MS: command ttl domain */
    int64_t  recv_us;   /* esp_timer: queue->dispatch latency */
    char     topic[MQTT_LINK_TOPIC_MAX];
    char     data[MQTT_LINK_RX_PAYLOAD_MAX + 1U];
} mqtt_rx_item_t;

/* Weight (cas/<peer>/weight/ctl) has its OWN small queue so ~22 Hz weight can
 * never starve command/ACK handling. Payloads are capped at 256 B (contract 8.2). */
#define MQTT_LINK_WRX_DEPTH        4U
#define MQTT_LINK_WRX_PAYLOAD_MAX  256U
typedef struct {
    uint16_t len;
    uint32_t recv_ms;   /* xTaskGetTickCount() * portTICK_PERIOD_MS */
    char     data[MQTT_LINK_WRX_PAYLOAD_MAX + 1U];
} mqtt_wrx_item_t;

/* Run lifecycle acks (cas/<dev>/run/ack, CONTRACT 9.2) have their OWN small queue,
 * so an ack can never sit in, or end a drain of, the command RX queue (STOP-first). */
#define MQTT_LINK_ARX_DEPTH        6U
#define MQTT_LINK_ARX_PAYLOAD_MAX  512U
typedef struct {
    uint16_t len;
    char     data[MQTT_LINK_ARX_PAYLOAD_MAX + 1U];
} mqtt_arx_item_t;

/* Outbound item: `data` is a heap copy owned by the item until
 * mqtt_link_tx_item_free(). */
typedef struct {
    uint8_t  qos;
    bool     retain;
    uint16_t len;
    char     topic[MQTT_LINK_TOPIC_MAX];
    char    *data;
} mqtt_tx_item_t;

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
    QueueHandle_t tx;
    volatile uint32_t rx_dropped_full;
    volatile uint32_t tx_dropped_full;
    volatile uint32_t tx_dropped_live;  /* telemetry/live drops (kept out of tx_dropped_full) */
    volatile uint32_t dropped_other;
    volatile uint32_t dropped_retained;   /* inbound retain=1 messages refused (never dispatched) */
    volatile uint32_t tx_dropped_net;   /* QoS 0 telemetry dropped instead of blocking */
    QueueHandle_t wrx;                  /* NULL unless weight_transport=mqtt */
    volatile uint32_t wrx_dropped_full;
    QueueHandle_t arx;                  /* NULL unless run_mqtt is on */
    volatile uint32_t arx_dropped_full;
    volatile uint32_t tx_deferred_run;  /* run items refused by the outbox budget (resent later) */
} mqtt_link_queue_t;

bool mqtt_link_queue_init(mqtt_link_queue_t *queue, size_t rx_depth, size_t tx_depth);

/*
 * Called from the esp-mqtt event callback. Copies into the bounded RX queue
 * with a ZERO timeout: no JSON, no logging, no blocking. A full queue drops.
 * data_len < total_len (fragment), retain=1 and oversize are refused here.
 */
mqtt_enq_result_t mqtt_link_queue_push_rx(mqtt_link_queue_t *queue,
                                          const char *topic, size_t topic_len,
                                          const char *data, size_t data_len,
                                          size_t total_len, bool retain,
                                          uint32_t now_ms, int64_t now_us);

/* Optional weight queue (only created when weight_transport=mqtt). */
bool mqtt_link_queue_weight_init(mqtt_link_queue_t *queue, size_t depth);
/* esp-mqtt callback context, ZERO timeout, drop-and-count when full. Retained,
 * fragmented and oversized (> 256 B) messages are refused. */
mqtt_enq_result_t mqtt_link_queue_push_weight(mqtt_link_queue_t *queue,
                                              const char *data, size_t data_len,
                                              size_t total_len, bool retain, uint32_t now_ms);
bool mqtt_link_queue_pop_weight(mqtt_link_queue_t *queue, mqtt_wrx_item_t *out, uint32_t timeout_ms);
size_t mqtt_link_queue_pending_weight(const mqtt_link_queue_t *queue);
/* True when the event topic (not NUL-terminated) is exactly `expected`. */
bool mqtt_link_topic_equals(const char *topic, size_t topic_len, const char *expected);

/* Optional run-ack queue (only created when run_mqtt is on). */
bool mqtt_link_queue_ack_init(mqtt_link_queue_t *queue, size_t depth);
/* esp-mqtt callback context, ZERO timeout, drop-and-count when full. Retained,
 * fragmented and oversized (> 512 B) messages are refused. */
mqtt_enq_result_t mqtt_link_queue_push_ack(mqtt_link_queue_t *queue,
                                           const char *data, size_t data_len,
                                           size_t total_len, bool retain);
bool mqtt_link_queue_pop_ack(mqtt_link_queue_t *queue, mqtt_arx_item_t *out, uint32_t timeout_ms);

/* The esp-mqtt MQTT_EVENT_DATA routing, kept here so QEMU can test it: weight topic ->
 * weight queue, ack topic -> ack queue (each only when its topic is non-NULL), anything
 * else -> the command RX queue. Zero timeout everywhere. */
mqtt_enq_result_t mqtt_link_queue_push_event(mqtt_link_queue_t *queue,
                                             const char *weight_topic, const char *ack_topic,
                                             const char *topic, size_t topic_len,
                                             const char *data, size_t data_len,
                                             size_t total_len, bool retain,
                                             uint32_t now_ms, int64_t now_us);

/* Outbound enqueue (publisher task context, not the callback). Zero timeout.
 * Copies the body to the heap; QoS 0 is capped at MQTT_LINK_TX_QOS0_LIMIT. */
mqtt_enq_result_t mqtt_link_queue_push_pub(mqtt_link_queue_t *queue,
                                           const char *topic, const char *data,
                                           int qos, bool retain);

/* Same as push_pub (QoS 0, not retained) but dropped once MQTT_LINK_TX_LIVE_LIMIT
 * items are queued, so live weight never takes the last telemetry slot. */
mqtt_enq_result_t mqtt_link_queue_push_live(mqtt_link_queue_t *queue,
                                            const char *topic, const char *data);

bool mqtt_link_queue_pop_rx(mqtt_link_queue_t *queue, mqtt_rx_item_t *out, uint32_t timeout_ms);
/* The caller owns out->data afterwards: call mqtt_link_tx_item_free(). */
bool mqtt_link_queue_pop_tx(mqtt_link_queue_t *queue, mqtt_tx_item_t *out, uint32_t timeout_ms);
void mqtt_link_tx_item_free(mqtt_tx_item_t *item);

size_t mqtt_link_queue_pending_rx(const mqtt_link_queue_t *queue);
size_t mqtt_link_queue_pending_tx(const mqtt_link_queue_t *queue);

/* Pops at most max_items TX items (zero timeout), hands each to cb, then frees
 * its body. Bounded so a shutdown never drains an unbounded backlog. */
typedef void (*mqtt_link_flush_cb)(const mqtt_tx_item_t *item, void *ctx);
size_t mqtt_link_queue_flush_tx(mqtt_link_queue_t *queue, size_t max_items,
                                mqtt_link_flush_cb cb, void *ctx);

/* Drops everything pending (frees TX bodies) but keeps the queues usable. */
void mqtt_link_queue_reset(mqtt_link_queue_t *queue);

/* Frees the queues. Idempotent and NULL-safe. Afterwards every push is INVALID. */
void mqtt_link_queue_deinit(mqtt_link_queue_t *queue);

/*
 * Non-blocking send policy (the pub task's only way onto the wire). esp-mqtt's
 * blocking publish holds the client lock its receive task also needs, so a
 * stalled TCP window could delay a STOP received over MQTT. Everything goes
 * through the enqueue hook instead (the esp-mqtt task does the network write).
 *  - QoS 0 telemetry: only while connected and only while it fits below the
 *    outbox limit minus MQTT_LINK_ACK_RESERVE; otherwise dropped and counted
 *    (tx_dropped_net).
 *  - QoS 1 ACKs / presence: always enqueued (stored), even while disconnected;
 *    the reserve at the top of the outbox is theirs.
 *  - QoS 1 run items (cas/<dev>/run/...): stored like ACKs but under their OWN byte
 *    budget: refused (MQTT_TX_RUN_DEFERRED, counted in tx_deferred_run) when the outbox
 *    would pass outbox_limit - MQTT_LINK_ACK_RESERVE. The run ring still holds the item
 *    and resends it, so resends on a half-open link can never eat the ACK reserve.
 */
typedef struct {
    void *ctx;
    /* Non-blocking: <0 = rejected (outbox full, no client). Always stores. */
    int (*enqueue)(void *ctx, const char *topic, const char *data, int len, int qos, bool retain);
    size_t (*outbox_size)(void *ctx);
} mqtt_link_tx_ops_t;

typedef enum {
    MQTT_TX_ENQUEUED = 0,
    MQTT_TX_DROPPED_TELEMETRY,   /* QoS 0: outbox pressure or enqueue refused (counted) */
    MQTT_TX_DROPPED_OFFLINE,     /* QoS 0 while disconnected (not counted) */
    MQTT_TX_ACK_REJECTED,        /* QoS 1 refused: HTTP re-serves the command */
    MQTT_TX_RUN_DEFERRED         /* QoS 1 run item over its outbox budget: ring resends it */
} mqtt_tx_result_t;

/* True for a "cas/<dev>/run/<x>" topic (run lifecycle publish, not run/ack). */
bool mqtt_link_topic_is_run(const char *topic);

mqtt_tx_result_t mqtt_link_tx_dispatch(const mqtt_link_tx_ops_t *ops, mqtt_link_queue_t *queue,
                                       const mqtt_tx_item_t *item, bool connected,
                                       size_t outbox_limit);

/*
 * Orderly stop sequence. Order is the contract: say goodbye while the client is
 * still up, stop the client (which aborts any publish the pub task is blocked
 * in), WAIT for the pub task to exit, and only then destroy the client. If the
 * task never exits the client is leaked, never destroyed under it.
 */
typedef struct {
    void *ctx;
    void (*goodbye)(void *ctx);          /* flush ACKs + graceful offline (enqueue only) */
    void (*client_stop)(void *ctx);
    bool (*task_exited)(void *ctx);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void (*client_destroy)(void *ctx);
} mqtt_link_stop_ops_t;

typedef enum { MQTT_STOP_DESTROYED = 0, MQTT_STOP_LEAKED } mqtt_stop_result_t;

mqtt_stop_result_t mqtt_link_stop_sequence(const mqtt_link_stop_ops_t *ops, uint32_t task_wait_ms);

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

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LINK_CORE_H */
