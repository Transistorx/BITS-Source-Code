#include "mqtt_link.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_config.h"
#include "sdkconfig.h"

static const char *TAG = "MQTT_LINK";

#ifndef CONFIG_MQTT_LINK_BROKER_URI
#define CONFIG_MQTT_LINK_BROKER_URI ""
#endif
#ifndef CONFIG_MQTT_LINK_DEVICE_ID_OVERRIDE
#define CONFIG_MQTT_LINK_DEVICE_ID_OVERRIDE ""
#endif
#ifndef CONFIG_MQTT_LINK_USERNAME
#define CONFIG_MQTT_LINK_USERNAME ""
#endif
#ifndef CONFIG_MQTT_LINK_PASSWORD
#define CONFIG_MQTT_LINK_PASSWORD ""
#endif
#ifndef CONFIG_MQTT_LINK_KEEPALIVE_S
#define CONFIG_MQTT_LINK_KEEPALIVE_S 30
#endif

/* esp-mqtt receive task: above live_status (2), equal to the HTTP telemetry
 * tasks (3), below every control task (4/5). It only reads the socket and
 * copies into the RX queue. The bulk publisher stays at 2 so telemetry can
 * never run ahead of anything that matters. */
#define MQTT_CLIENT_PRIO    3
#define MQTT_PUB_PRIO       2
#define MQTT_PUB_STACK      6144
#define MQTT_CLIENT_STACK   6144
#define MQTT_OUTBOX_LIMIT   8192U
#define MQTT_POLL_MS        100U

static mqtt_link_queue_t s_queue;
static esp_mqtt_client_handle_t s_client;
static char s_device_id[MQTT_LINK_DEVICE_ID_MAX + 1U];
static mqtt_link_connect_plan_t s_plan;
static atomic_bool s_connected;
static atomic_bool s_evt_connected;
static atomic_bool s_evt_disconnected;
static mqtt_link_lc_t s_lc;
static bool s_lc_ready;
static bool s_shutdown_hooked;
static atomic_bool s_pub_done;
static atomic_int s_published_id;
static atomic_uint s_connects;
static atomic_uint s_disconnects;
static mqtt_link_tx_ops_t s_txops;
/* Weight-over-MQTT (weight_transport=mqtt): exact peer topic, own bounded queue. */
static bool s_weight_on;
static char s_weight_topic[MQTT_LINK_TOPIC_MAX];
static char s_birth_json[160];
/* Run lifecycle acks (CONTRACT 9.2): exact topic, QoS 1, optional. */
#define MQTT_SUFFIX_RUN_ACK "run/ack"
static bool s_ack_on;
static char s_ack_topic[MQTT_LINK_TOPIC_MAX];

void mqtt_link_set_run_ack(bool on)
{
    s_ack_on = on;
}

bool mqtt_link_run_ack_pop(char *out, size_t cap, size_t *len, uint32_t timeout_ms)
{
    static mqtt_arx_item_t item;   /* single consumer: the run_mqtt task */
    if (out == NULL || cap == 0U || !s_lc_ready || !mqtt_link_lc_running(&s_lc) || s_queue.arx == NULL ||
        !mqtt_link_queue_pop_ack(&s_queue, &item, timeout_ms)) {
        return false;
    }
    if ((size_t)item.len >= cap) return false;
    memcpy(out, item.data, (size_t)item.len + 1U);
    if (len != NULL) *len = item.len;
    return true;
}

/* Stop budget: graceful PUBACK wait (400 ms) + esp_mqtt_client_stop + a bounded
 * (2 s) wait for the pub task. NOT strictly bounded: esp_mqtt_client_stop takes
 * the client API lock / waits on STOPPED_BIT with portMAX_DELAY, so while the
 * client task is inside esp_transport_connect it blocks until that returns
 * (~5-10 s). Worst case ~0.4 + 5-10 + 2 s. This runs only in the shutdown
 * handler / caller of mqtt_link_stop (the task calling esp_restart); control and
 * safety tasks are not stopped or waited on and keep running until the reset. */
#define MQTT_STOP_TASK_WAIT_MS 2000U
#define MQTT_STOP_ACK_WAIT_MS  400U
#define MQTT_STOP_FLUSH_MAX    MQTT_LINK_TX_DEPTH

bool mqtt_link_connected(void)
{
    return atomic_load(&s_connected);
}

const char *mqtt_link_device_id(void)
{
    return s_device_id;
}

void mqtt_link_stats(mqtt_link_stats_t *out)
{
    if (out == NULL) return;
    out->rx_dropped_full = s_queue.rx_dropped_full;
    out->tx_dropped_full = s_queue.tx_dropped_full;
    out->tx_dropped_live = s_queue.tx_dropped_live;
    out->dropped_other = s_queue.dropped_other;
    out->dropped_retained = s_queue.dropped_retained;
    out->telemetry_dropped_net = s_queue.tx_dropped_net;
    out->weight_dropped_full = s_queue.wrx_dropped_full;
    out->connects = atomic_load(&s_connects);
    out->disconnects = atomic_load(&s_disconnects);
}

bool mqtt_link_set_weight_peer(const char *peer_id)
{
    s_weight_on = false;
    if (peer_id == NULL || !mqtt_link_topic(s_weight_topic, sizeof(s_weight_topic), peer_id,
                                            MQTT_SUFFIX_WEIGHT_CTL)) {
        s_weight_topic[0] = '\0';
        return false;
    }
    s_weight_on = true;
    return true;
}

bool mqtt_link_weight_pop(mqtt_wrx_item_t *out, uint32_t timeout_ms)
{
    if (!s_lc_ready || !mqtt_link_lc_running(&s_lc) || s_queue.wrx == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms)); /* link off: keep the caller from spinning */
        return false;
    }
    return mqtt_link_queue_pop_weight(&s_queue, out, timeout_ms);
}

bool mqtt_link_publish(const char *topic_suffix, const char *json, int qos, bool retain)
{
    char topic[MQTT_LINK_TOPIC_MAX];
    if (!s_lc_ready || !mqtt_link_lc_running(&s_lc) ||
        !mqtt_link_topic(topic, sizeof(topic), s_device_id, topic_suffix)) {
        return false;
    }
    if (qos == 0 && !retain && strcmp(topic_suffix, MQTT_SUFFIX_LIVE) == 0) {
        return mqtt_link_queue_push_live(&s_queue, topic, json) == MQTT_ENQ_OK;
    }
    return mqtt_link_queue_push_pub(&s_queue, topic, json, qos, retain) == MQTT_ENQ_OK;
}

bool mqtt_link_rx_pop(mqtt_rx_item_t *out, uint32_t timeout_ms)
{
    if (!s_lc_ready || !mqtt_link_lc_running(&s_lc) || s_queue.rx == NULL) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms)); /* link off: keep the caller from spinning */
        return false;
    }
    /* run/ack never reaches this queue (own queue, see on_mqtt_event). A pop that
     * returns false therefore means "empty": a drain ends only when nothing is left.
     * A foreign topic (not subscribed, should not occur) is skipped, not a drain end. */
    uint32_t wait = timeout_ms;
    for (unsigned n = 0U; n < MQTT_LINK_RX_DEPTH; n++) {
        if (!mqtt_link_queue_pop_rx(&s_queue, out, wait)) return false;
        if (strcmp(out->topic, s_plan.subscribe_topic) == 0) return true;
        wait = 0U;
    }
    return false;
}

/*
 * esp-mqtt event callback (runs in the esp-mqtt task). Enqueue and flag only:
 * no JSON, no logging, no blocking. State changes are flags rather than queue
 * items so a saturated queue can never hide a disconnect.
 */
static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        atomic_store(&s_connected, true);
        atomic_store(&s_evt_connected, true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_connected, false);
        atomic_store(&s_evt_disconnected, true);
        break;
    case MQTT_EVENT_PUBLISHED:
        atomic_store(&s_published_id, ev->msg_id);
        break;
    case MQTT_EVENT_DATA:
        /* Only the first fragment carries the topic. A message split across
         * events is refused whole by the core (data_len != total_len). */
        if (ev->topic != NULL && ev->topic_len > 0 && ev->data_len >= 0 &&
            ev->total_data_len >= 0) {
            /* Weight and run/ack each have their OWN queue (zero timeout, drop-and-count);
             * only commands reach the RX queue. */
            (void)mqtt_link_queue_push_event(&s_queue, s_weight_on ? s_weight_topic : NULL,
                                             (s_ack_on && s_ack_topic[0] != '\0') ? s_ack_topic : NULL,
                                             ev->topic, (size_t)ev->topic_len,
                                             ev->data, (size_t)ev->data_len,
                                             (size_t)ev->total_data_len, ev->retain != 0,
                                             (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                                             esp_timer_get_time());
        }
        break;
    default:
        break;
    }
}

/* Both ops are non-blocking: esp-mqtt's task does the network write, so a
 * stalled TCP window can never hold the client lock the receive path needs. */
static int tx_enqueue(void *ctx, const char *topic, const char *data, int len, int qos, bool retain)
{
    (void)ctx;
    if (s_client == NULL) return -1;
    return esp_mqtt_client_enqueue(s_client, topic, data, len, qos, retain, true);
}

static size_t tx_outbox_size(void *ctx)
{
    (void)ctx;
    return s_client != NULL ? (size_t)esp_mqtt_client_get_outbox_size(s_client) : 0U;
}

static void publish_item(const mqtt_tx_item_t *item)
{
    if (s_client == NULL) return;
    /* QoS 0 telemetry is dropped (counted) under pressure or while offline;
     * QoS 1 command ACKs always wait in the outbox. They only report an
     * outcome: nothing is ever re-executed from them. */
    (void)mqtt_link_tx_dispatch(&s_txops, &s_queue, item, atomic_load(&s_connected),
                                MQTT_OUTBOX_LIMIT);
}

static void on_connected(void)
{
    ESP_LOGI(TAG, "connected as %s", s_device_id);
    /* Clean session: the broker forgot the subscription, so renew it on EVERY connect. */
    (void)esp_mqtt_client_subscribe_single(s_client, s_plan.subscribe_topic, s_plan.subscribe_qos);
    if (s_weight_on) (void)esp_mqtt_client_subscribe_single(s_client, s_weight_topic, 0);
    if (s_ack_on && s_ack_topic[0] != '\0') {
        (void)esp_mqtt_client_subscribe_single(s_client, s_ack_topic, 1);
    }
    /* Birth is published on connect only, never as a side effect of telemetry. */
    (void)esp_mqtt_client_enqueue(s_client, s_plan.presence.birth_topic,
                                  s_plan.presence.birth_payload,
                                  (int)strlen(s_plan.presence.birth_payload),
                                  s_plan.presence.qos, s_plan.presence.retain, true);
}

static void mqtt_pub_task(void *arg)
{
    (void)arg;
    static mqtt_tx_item_t item;
    mqtt_link_reconnect_t rc;
    mqtt_link_reconnect_init(&rc);

    while (mqtt_link_lc_running(&s_lc)) {
        bool got = mqtt_link_queue_pop_tx(&s_queue, &item, MQTT_POLL_MS);

        if (atomic_exchange(&s_evt_connected, false)) {
            mqtt_link_reconnect_on_connected(&rc);
            atomic_fetch_add(&s_connects, 1U);
            on_connected();
        }
        if (atomic_exchange(&s_evt_disconnected, false)) {
            uint32_t wait = mqtt_link_reconnect_on_disconnect(&rc, esp_timer_get_time());
            atomic_fetch_add(&s_disconnects, 1U);
            ESP_LOGW(TAG, "disconnected; reconnect in %lu ms", (unsigned long)wait);
        }
        if (mqtt_link_reconnect_due(&rc, esp_timer_get_time())) {
            (void)esp_mqtt_client_reconnect(s_client);
        }

        if (!got) continue;
        publish_item(&item);
        mqtt_link_tx_item_free(&item);
    }
    atomic_store(&s_pub_done, true);
    vTaskDelete(NULL);
}

static void flush_cb(const mqtt_tx_item_t *item, void *ctx)
{
    (void)ctx;
    /* Only acks (QoS 1) are worth a last send; QoS 0 telemetry is dropped. Run items
     * stay in the ring (RAM, resent), they are not worth the outbox at shutdown. */
    if (item->qos > 0 && !mqtt_link_topic_is_run(item->topic) && atomic_load(&s_connected)) {
        (void)esp_mqtt_client_enqueue(s_client, item->topic, item->data, item->len, item->qos,
                                      item->retain, true);
    }
}

/* Last words while the client is still up. Enqueue only: nothing here can block. */
static void stop_goodbye(void *ctx)
{
    (void)ctx;
    if (s_client == NULL || !atomic_load(&s_connected)) return;
    (void)mqtt_link_queue_flush_tx(&s_queue, MQTT_STOP_FLUSH_MAX, flush_cb, NULL);

    atomic_store(&s_published_id, -1);
    int id = esp_mqtt_client_enqueue(s_client, s_plan.presence.birth_topic,
                                     s_plan.presence.graceful_payload,
                                     (int)strlen(s_plan.presence.graceful_payload),
                                     s_plan.presence.qos, s_plan.presence.retain, true);
    for (uint32_t w = 0U; id > 0 && atomic_load(&s_published_id) != id &&
                          atomic_load(&s_connected) && w < MQTT_STOP_ACK_WAIT_MS; w += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (id <= 0 || atomic_load(&s_published_id) != id) {
        ESP_LOGW(TAG, "graceful offline not acknowledged; LWT covers it");
    }
}

static void stop_client(void *ctx)
{
    (void)ctx;
    if (s_client != NULL) (void)esp_mqtt_client_stop(s_client);
}

static bool stop_task_exited(void *ctx)
{
    (void)ctx;
    return atomic_load(&s_pub_done);
}

static void stop_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void stop_destroy(void *ctx)
{
    (void)ctx;
    if (s_client != NULL) {
        (void)esp_mqtt_client_destroy(s_client); /* frees the outbox and buffers */
        s_client = NULL;
    }
}

esp_err_t mqtt_link_stop(void)
{
    if (!s_lc_ready || !mqtt_link_lc_begin_stop(&s_lc)) return ESP_OK;

    /* The pub task sees STOPPING and exits; it is the only other user of s_client.
     * Stop the client, then wait for that task; destroy only once it is gone. */
    const mqtt_link_stop_ops_t ops = {
        .goodbye = stop_goodbye, .client_stop = stop_client, .task_exited = stop_task_exited,
        .delay_ms = stop_delay, .client_destroy = stop_destroy,
    };
    if (mqtt_link_stop_sequence(&ops, MQTT_STOP_TASK_WAIT_MS) == MQTT_STOP_LEAKED) {
        /* Leak the client rather than free it under a live task. The link stays
         * STOPPING so start is refused instead of racing the stuck task. */
        ESP_LOGE(TAG, "pub task did not exit; client leaked, link left stopped");
        atomic_store(&s_connected, false);
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(20)); /* let the idle task reap the deleted task */

    /* Keep the queues allocated (a command task may still be waiting on RX);
     * just drop whatever is pending. */
    mqtt_link_queue_reset(&s_queue);
    atomic_store(&s_connected, false);
    mqtt_link_lc_end_stop(&s_lc);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}
static void shutdown_handler(void)
{
    (void)mqtt_link_stop();
}

esp_err_t mqtt_link_start(const char *device_id)
{
    if (!s_lc_ready) {
        mqtt_link_lc_init(&s_lc);
        s_lc_ready = true;
    }
    if (mqtt_link_lc_state(&s_lc) != MQTT_LC_IDLE) return ESP_ERR_INVALID_STATE;
    if (CONFIG_MQTT_LINK_BROKER_URI[0] == '\0') {
        ESP_LOGW(TAG, "MQTT disabled: no broker URI configured");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_device_id[0] = '\0';
    if (CONFIG_MQTT_LINK_DEVICE_ID_OVERRIDE[0] != '\0') {
        strlcpy(s_device_id, CONFIG_MQTT_LINK_DEVICE_ID_OVERRIDE, sizeof(s_device_id));
    } else if (device_id != NULL && device_id[0] != '\0') {
        strlcpy(s_device_id, device_id, sizeof(s_device_id));
    } else {
        (void)nvs_config_get_str(NVS_KEY_DEVICE_ID, s_device_id, sizeof(s_device_id));
    }
    if (!mqtt_link_connect_plan(&s_plan, s_device_id)) {
        ESP_LOGE(TAG, "device id is not usable in MQTT topics");
        return ESP_ERR_INVALID_STATE;
    }
    if (!mqtt_link_topic(s_ack_topic, sizeof(s_ack_topic), s_device_id, MQTT_SUFFIX_RUN_ACK)) {
        s_ack_topic[0] = '\0';
    }
    if (s_queue.rx == NULL && !mqtt_link_queue_init(&s_queue, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH)) {
        return ESP_ERR_NO_MEM;
    }
    if (s_weight_on && !mqtt_link_queue_weight_init(&s_queue, MQTT_LINK_WRX_DEPTH)) {
        s_weight_on = false; /* no queue: never subscribe, weight stays stale (fail-safe) */
    }
    if (s_ack_on && !mqtt_link_queue_ack_init(&s_queue, MQTT_LINK_ARX_DEPTH)) {
        s_ack_on = false; /* no queue: no subscription; the ring only resends (fail-safe) */
    }
    mqtt_link_queue_reset(&s_queue);
    /* Birth carries boot_id and caps; the LWT stays {"online":false}. */
    if (mqtt_link_birth_build(s_birth_json, sizeof(s_birth_json), mqtt_link_boot_id())) {
        s_plan.presence.birth_payload = s_birth_json;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_LINK_BROKER_URI,
        .credentials.client_id = s_device_id,
        .session.keepalive = CONFIG_MQTT_LINK_KEEPALIVE_S,
        .session.last_will = {
            .topic = s_plan.presence.lwt_topic,
            .msg = s_plan.presence.lwt_payload,
            .msg_len = (int)strlen(s_plan.presence.lwt_payload),
            .qos = s_plan.presence.qos,
            .retain = s_plan.presence.retain,
        },
        /* Reconnect is driven by the pub task: 1 s doubling to 30 s. */
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = 5000,
        .task.priority = MQTT_CLIENT_PRIO,
        .task.stack_size = MQTT_CLIENT_STACK,
        .buffer.size = 1536,   /* a 1 KiB command arrives in one event */
        .buffer.out_size = 2048,
        .outbox.limit = MQTT_OUTBOX_LIMIT,
    };
    if (CONFIG_MQTT_LINK_USERNAME[0] != '\0') {
        cfg.credentials.username = CONFIG_MQTT_LINK_USERNAME;
        cfg.credentials.authentication.password = CONFIG_MQTT_LINK_PASSWORD;
    }

    atomic_store(&s_connected, false);
    atomic_store(&s_evt_connected, false);
    atomic_store(&s_evt_disconnected, false);
    atomic_store(&s_pub_done, false);

    s_txops = (mqtt_link_tx_ops_t){ .ctx = NULL, .enqueue = tx_enqueue, .outbox_size = tx_outbox_size };
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) return ESP_FAIL;
    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, NULL);
    if (err != ESP_OK) {
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    (void)mqtt_link_lc_start(&s_lc);
    if (xTaskCreate(mqtt_pub_task, "mqtt_pub", MQTT_PUB_STACK, NULL, MQTT_PUB_PRIO, NULL) !=
        pdPASS) {
        (void)mqtt_link_lc_begin_stop(&s_lc);
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        mqtt_link_lc_end_stop(&s_lc);
        return ESP_ERR_NO_MEM;
    }
    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        (void)mqtt_link_stop();
        return err;
    }
    if (!s_shutdown_hooked) {
        s_shutdown_hooked = (esp_register_shutdown_handler(shutdown_handler) == ESP_OK);
    }

    char shown[96];
    mqtt_link_uri_redact(shown, sizeof(shown), CONFIG_MQTT_LINK_BROKER_URI);
    ESP_LOGI(TAG, "started: broker %s device %s keepalive %ds", shown, s_device_id,
             CONFIG_MQTT_LINK_KEEPALIVE_S);
    return ESP_OK;
}
