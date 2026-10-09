#include "mqtt_link.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_util.h"
#include "mqtt_client.h"
#include "mqtt_link_core.h"
#include "nvs_config.h"
#include "sdkconfig.h"

static const char *TAG = "MQTT";

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

/* Everything MQTT runs below the weight path (weight_tx 3, scale reader 4+). */
#define MQTT_TASK_PRIO      2
#define MQTT_PUB_STACK      6144
#define MQTT_CLIENT_STACK   6144
#define MQTT_OUTBOX_LIMIT   8192U
#define MQTT_POLL_MS        100U

static mqtt_link_queue_t s_queue;
static esp_mqtt_client_handle_t s_client;
static mqtt_link_command_cb s_command_cb;
static char s_device_id[MQTT_LINK_DEVICE_ID_MAX + 1U];
static char s_topic_commands[MQTT_LINK_TOPIC_MAX];
static mqtt_link_presence_t s_presence;
static atomic_bool s_connected;
static atomic_bool s_evt_connected;
static atomic_bool s_evt_disconnected;
static mqtt_link_lc_t s_lc;
static bool s_lc_ready;
static bool s_shutdown_hooked;
static atomic_bool s_pub_done;
static atomic_int s_published_id;
static atomic_uint s_published_count;  /* publishes handed to esp-mqtt, for the health line */
static atomic_uint s_publish_failed;
static atomic_uint s_dropped_offline;  /* QoS 0 refused while the broker is down */
static char s_boot_id[MQTT_LINK_BOOT_ID_LEN + 1U];
static char s_scale_id[MQTT_LINK_SCALE_ID_MAX + 1U] = MQTT_LINK_SCALE_ID_DEFAULT;
static const char *s_fw = "unknown";
static const char *s_role = "weight_sender";
static mqtt_link_wgate_t s_wgate;      /* weight_tx task only */
/* Last esp-mqtt connect error, stored lock-free by the event callback and
 * only formatted by the pub task. */
static atomic_int s_err_type;
static atomic_int s_err_code;
/* Quiet period between "connect failed" summaries while the broker is down. */
#define MQTT_FAIL_LOG_INTERVAL_MS 30000U

/* Stop budget: pub task exit + flush + graceful PUBACK wait stay under ~1 s. */
#define MQTT_STOP_TASK_WAIT_MS 300U
#define MQTT_STOP_ACK_WAIT_MS  400U
#define MQTT_STOP_FLUSH_MAX    MQTT_LINK_QUEUE_DEPTH

void mqtt_link_set_command_handler(mqtt_link_command_cb cb)
{
    s_command_cb = cb;
}

bool mqtt_link_connected(void)
{
    return atomic_load(&s_connected);
}

void mqtt_link_get_stats(mqtt_link_stats_t *out)
{
    if (out == NULL) return;
    out->connected = atomic_load(&s_connected);
    out->published = atomic_load(&s_published_count);
    out->publish_failed = atomic_load(&s_publish_failed);
    out->queue_depth = (uint32_t)mqtt_link_queue_pending(&s_queue);
    out->dropped_full = s_queue.dropped_full;
    out->dropped_other = s_queue.dropped_other;
    out->dropped_offline = atomic_load(&s_dropped_offline);
}

const char *mqtt_link_device_id(void)
{
    return s_device_id;
}

const char *mqtt_link_scale_id(void)
{
    return s_scale_id;
}

const char *mqtt_link_boot_id(void)
{
    /* First call is from single-threaded startup (remote_cmd_start). */
    if (s_boot_id[0] == '\0') mqtt_link_boot_id_fmt(s_boot_id, esp_random());
    return s_boot_id;
}

void mqtt_link_set_identity(const char *fw, const char *role)
{
    if (fw != NULL) s_fw = fw;
    if (role != NULL) s_role = role;
}

bool mqtt_link_publish(const char *topic_suffix, const char *json, int qos, bool retain)
{
    char topic[MQTT_LINK_TOPIC_MAX];
    if (!s_lc_ready || !mqtt_link_lc_running(&s_lc) ||
        !mqtt_link_topic(topic, sizeof(topic), s_device_id, topic_suffix)) {
        return false;
    }
    /* QoS 0 is never accumulated for an offline broker: drop at the door. */
    if (qos == 0 && !atomic_load(&s_connected)) {
        atomic_fetch_add(&s_dropped_offline, 1U);
        return false;
    }
    return mqtt_link_queue_push_pub(&s_queue, topic, json, qos, retain) == MQTT_ENQ_OK;
}

/* Called by the weight_tx task for every valid, real CAS sample. Zero-timeout
 * enqueue only; a full queue or a down broker drops (counted), never waits. */
void mqtt_link_weight_frame(const mqtt_link_weight_t *w, uint32_t now_ms)
{
    char json[320];
    if (w == NULL) return;
    mqtt_link_weight_t f = *w;
    f.scale_id = s_scale_id;
    w = &f;
    if (mqtt_link_wgate_new_frame(&s_wgate, w->src_uart, w->cas_seq)) {
        size_t n = mqtt_link_weight_json(json, sizeof(json), mqtt_link_boot_id(), s_wgate.seq_ctl,
                                         w, false);
        if (n != 0U && mqtt_link_publish(MQTT_SUFFIX_WEIGHT_CTL, json, 0, false)) {
            s_wgate.seq_ctl++;
        }
    }
    if (mqtt_link_wgate_tel_due(&s_wgate, now_ms, MQTT_LINK_WEIGHT_TEL_PERIOD_MS)) {
        size_t n = mqtt_link_weight_json(json, sizeof(json), mqtt_link_boot_id(), s_wgate.seq_tel,
                                         w, true);
        if (n != 0U && mqtt_link_publish(MQTT_SUFFIX_WEIGHT_TEL, json, 0, false)) {
            s_wgate.seq_tel++;
        }
    }
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
    case MQTT_EVENT_ERROR:
        if (ev->error_handle != NULL) {
            atomic_store(&s_err_type, (int)ev->error_handle->error_type);
            atomic_store(&s_err_code,
                         ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED
                             ? (int)ev->error_handle->connect_return_code
                             : ev->error_handle->esp_transport_sock_errno);
        }
        break;
    case MQTT_EVENT_DATA:
        /* Only the first fragment carries the topic. A message split across
         * events is refused whole by the core (data_len != total_len). */
        if (ev->topic != NULL && ev->topic_len > 0 && ev->data_len >= 0 &&
            ev->total_data_len >= 0) {
            (void)mqtt_link_queue_push_rx(&s_queue, ev->topic, (size_t)ev->topic_len,
                                          ev->data, (size_t)ev->data_len,
                                          (size_t)ev->total_data_len, ev->retain != 0,
                                          (uint32_t)(esp_timer_get_time() / 1000));
        }
        break;
    default:
        break;
    }
}

static void publish_item(const mqtt_item_t *item)
{
    if (s_client == NULL) return;
#if LOG_UTIL_MQTT_TRACE
    ESP_LOGD(TAG, "PUB %s %.*s", item->topic, (int)item->len, item->data);
#endif
    if (atomic_load(&s_connected)) {
        if (esp_mqtt_client_publish(s_client, item->topic, item->data, item->len,
                                    item->qos, item->retain) >= 0) {
            atomic_fetch_add(&s_published_count, 1U);
        } else {
            atomic_fetch_add(&s_publish_failed, 1U);
        }
    } else if (item->qos > 0) {
        /* Command ACKs wait in the (size-limited) outbox for the next connect. */
        (void)esp_mqtt_client_enqueue(s_client, item->topic, item->data, item->len,
                                      item->qos, item->retain, true);
    } /* QoS 0 telemetry while disconnected is dropped on purpose. */
}

static void on_connected(void)
{
    ESP_LOGI(TAG, "CONNECTED");
    if (esp_mqtt_client_subscribe_single(s_client, s_topic_commands, 1) >= 0) {
        ESP_LOGI(TAG, "SUBSCRIBED %s qos=1", s_topic_commands);
    } else {
        ESP_LOGW(TAG, "subscribe failed %s", s_topic_commands);
    }
    /* Birth is published on connect only, never as a side effect of telemetry. */
    (void)esp_mqtt_client_publish(s_client, s_presence.birth_topic, s_presence.birth_payload,
                                  (int)strlen(s_presence.birth_payload), s_presence.qos,
                                  s_presence.retain);
}

/* Short text for the last esp-mqtt error; pub task only. */
static const char *mqtt_error_text(void)
{
    static char text[40];
    int type = atomic_load(&s_err_type);
    int code = atomic_load(&s_err_code);
    switch ((esp_mqtt_error_type_t)type) {
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        snprintf(text, sizeof(text), "tcp errno=%d", code);
        break;
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        snprintf(text, sizeof(text), "refused rc=%d", code);
        break;
    default:
        snprintf(text, sizeof(text), "none");
        break;
    }
    return text;
}

static void mqtt_pub_task(void *arg)
{
    (void)arg;
    static mqtt_item_t item;
    uint32_t attempt = 0U;
    bool reconnect_pending = false;
    bool was_up = false;
    int64_t reconnect_at_us = 0;
    log_ratelimit_t fail_rl;
    log_ratelimit_init(&fail_rl);

    while (mqtt_link_lc_running(&s_lc)) {
        bool got = mqtt_link_queue_pop(&s_queue, &item, MQTT_POLL_MS);

        if (atomic_exchange(&s_evt_connected, false)) {
            attempt = 0U;
            reconnect_pending = false;
            was_up = true;
            log_ratelimit_init(&fail_rl);
            on_connected();
        }
        if (atomic_exchange(&s_evt_disconnected, false)) {
            uint32_t wait = mqtt_link_backoff_jitter_ms(attempt++, esp_random());
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t folded = 0U;
            if (was_up) {
                /* Transition: logged once, then failures are summarised. */
                ESP_LOGW(TAG, "DISCONNECTED retry=%lus error=%s", (unsigned long)(wait / 1000U),
                         mqtt_error_text());
                was_up = false;
                log_ratelimit_init(&fail_rl);
                (void)log_ratelimit_event(&fail_rl, now_ms, MQTT_FAIL_LOG_INTERVAL_MS, &folded);
            } else if (log_ratelimit_event(&fail_rl, now_ms, MQTT_FAIL_LOG_INTERVAL_MS, &folded)) {
                char times[16] = "";
                if (folded > 1U) snprintf(times, sizeof(times), " (x%lu)", (unsigned long)folded);
                ESP_LOGW(TAG, "connect failed attempt=%lu retry=%lus error=%s%s",
                         (unsigned long)attempt, (unsigned long)(wait / 1000U),
                         mqtt_error_text(), times);
            }
            reconnect_at_us = esp_timer_get_time() + (int64_t)wait * 1000;
            reconnect_pending = true;
        }
        if (reconnect_pending && esp_timer_get_time() >= reconnect_at_us) {
            reconnect_pending = false;
            (void)esp_mqtt_client_reconnect(s_client);
        }

        if (!got) continue;
        if (item.kind == MQTT_ITEM_PUB) {
            publish_item(&item);
        } else if (strcmp(item.topic, s_topic_commands) == 0) {
#if LOG_UTIL_MQTT_TRACE
            ESP_LOGD(TAG, "RX %s %.*s", item.topic, (int)item.len, item.data);
#endif
            if (s_command_cb != NULL) s_command_cb(item.data, item.len, item.retain, item.recv_ms);
        }
    }
    atomic_store(&s_pub_done, true);
    vTaskDelete(NULL);
}

static void flush_cb(const mqtt_item_t *item, void *ctx)
{
    (void)ctx;
    /* Only acks (QoS 1) are worth a last send; QoS 0 telemetry is dropped. */
    if (item->kind == MQTT_ITEM_PUB && item->qos > 0 && atomic_load(&s_connected)) {
        (void)esp_mqtt_client_publish(s_client, item->topic, item->data, item->len, item->qos,
                                      item->retain);
    }
}

/* Orderly stop: graceful offline presence, bounded wait, then tear everything
 * down so mqtt_link_start() can run again. Idempotent; never blocks > ~1 s plus
 * the esp-mqtt client stop. Must not be called from the esp-mqtt event task. */
esp_err_t mqtt_link_stop(void)
{
    if (!s_lc_ready || !mqtt_link_lc_begin_stop(&s_lc)) return ESP_OK;

    /* Pub task sees STOPPING and exits; it is the only other publisher. */
    for (uint32_t w = 0U; !atomic_load(&s_pub_done) && w < MQTT_STOP_TASK_WAIT_MS; w += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!atomic_load(&s_pub_done)) ESP_LOGW(TAG, "pub task did not exit in time");
    vTaskDelay(pdMS_TO_TICKS(20)); /* let the idle task reap the deleted task */

    if (s_client != NULL && atomic_load(&s_connected)) {
        static mqtt_item_t scratch;
        (void)mqtt_link_queue_flush(&s_queue, &scratch, MQTT_STOP_FLUSH_MAX, flush_cb, NULL);

        atomic_store(&s_published_id, -1);
        int id = esp_mqtt_client_publish(s_client, s_presence.birth_topic,
                                         s_presence.graceful_payload,
                                         (int)strlen(s_presence.graceful_payload),
                                         s_presence.qos, s_presence.retain);
        for (uint32_t w = 0U; id > 0 && atomic_load(&s_published_id) != id &&
                              atomic_load(&s_connected) && w < MQTT_STOP_ACK_WAIT_MS; w += 10U) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (id <= 0 || atomic_load(&s_published_id) != id) {
            ESP_LOGW(TAG, "graceful offline not acknowledged; LWT covers it");
        }
    }

    if (s_client != NULL) {
        (void)esp_mqtt_client_stop(s_client);
        (void)esp_mqtt_client_destroy(s_client); /* frees the outbox and buffers */
        s_client = NULL;
    }
    mqtt_link_queue_deinit(&s_queue);
    atomic_store(&s_connected, false);
    mqtt_link_lc_end_stop(&s_lc);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

static void shutdown_handler(void)
{
    (void)mqtt_link_stop();
}

esp_err_t mqtt_link_start(void)
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
    } else {
        (void)nvs_config_get_str(NVS_KEY_DEVICE_ID, s_device_id, sizeof(s_device_id));
    }
    if (!mqtt_link_device_id_valid(s_device_id) ||
        !mqtt_link_topic(s_topic_commands, sizeof(s_topic_commands), s_device_id,
                         MQTT_SUFFIX_COMMANDS) ||
        !mqtt_link_presence_plan(&s_presence, s_device_id)) {
        ESP_LOGE(TAG, "device id is not usable in MQTT topics");
        return ESP_ERR_INVALID_STATE;
    }
    /* scale_id from NVS; absent or invalid falls back to the default, never a bad id. */
    char sid[MQTT_LINK_SCALE_ID_MAX + 2U] = "";
    if (nvs_config_get_str(NVS_KEY_SCALE_ID, sid, sizeof(sid)) == ESP_OK &&
        mqtt_link_scale_id_valid(sid)) {
        strlcpy(s_scale_id, sid, sizeof(s_scale_id));
    } else {
        ESP_LOGW(TAG, "scale_id missing or invalid; using %s", MQTT_LINK_SCALE_ID_DEFAULT);
        strlcpy(s_scale_id, MQTT_LINK_SCALE_ID_DEFAULT, sizeof(s_scale_id));
    }
    /* Enriched birth; on failure the plain {"online":true} birth is kept. */
    if (!mqtt_link_presence_set_birth(&s_presence, mqtt_link_boot_id(), "[\"weight_mqtt\"]", s_fw,
                                      s_role, s_scale_id)) {
        ESP_LOGW(TAG, "birth envelope not built; plain birth used");
    }
    if (!mqtt_link_queue_init(&s_queue, MQTT_LINK_QUEUE_DEPTH)) return ESP_ERR_NO_MEM;

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_LINK_BROKER_URI,
        .credentials.client_id = s_device_id,
        .session.keepalive = CONFIG_MQTT_LINK_KEEPALIVE_S,
        .session.last_will = {
            .topic = s_presence.lwt_topic,
            .msg = s_presence.lwt_payload,
            .msg_len = (int)strlen(s_presence.lwt_payload),
            .qos = s_presence.qos,
            .retain = s_presence.retain,
        },
        /* Reconnect is driven by the pub task: 1 s doubling to 30 s. */
        .network.disable_auto_reconnect = true,
        .network.timeout_ms = 5000,
        .task.priority = MQTT_TASK_PRIO,
        .task.stack_size = MQTT_CLIENT_STACK,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
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

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        mqtt_link_queue_deinit(&s_queue);
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, NULL);
    if (err != ESP_OK) {
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        mqtt_link_queue_deinit(&s_queue);
        return err;
    }

    (void)mqtt_link_lc_start(&s_lc);
    if (xTaskCreate(mqtt_pub_task, "mqtt_pub", MQTT_PUB_STACK, NULL, MQTT_TASK_PRIO, NULL) !=
        pdPASS) {
        (void)mqtt_link_lc_begin_stop(&s_lc);
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        mqtt_link_queue_deinit(&s_queue);
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
    ESP_LOGI(TAG, "STARTED broker=%s device=%s keepalive=%ds", shown, s_device_id,
             CONFIG_MQTT_LINK_KEEPALIVE_S);
    return ESP_OK;
}
