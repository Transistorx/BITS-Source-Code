#include "mqtt_link.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "broker_cfg.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if defined(CONFIG_MQTT_LINK_TLS_BUNDLE)
#include "esp_crt_bundle.h"
#endif
#include "log_util.h"
#include "mqtt_client.h"
#include "mqtt_link_core.h"
#include "nvs.h"
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
#ifndef CONFIG_MQTT_LINK_WEIGHT_HB_MS
#define CONFIG_MQTT_LINK_WEIGHT_HB_MS MQTT_LINK_WEIGHT_HB_PERIOD_MS
#endif
#define MQTT_HB_PERIOD_MS   ((uint32_t)CONFIG_MQTT_LINK_WEIGHT_HB_MS)

/* Everything MQTT runs below the weight path (weight_tx 3, scale reader 4+). */
#define MQTT_TASK_PRIO      2
#define MQTT_PUB_STACK      6144
#define MQTT_CLIENT_STACK   6144
#define MQTT_OUTBOX_LIMIT   8192U
#define MQTT_POLL_MS        100U

static mqtt_link_queues_t s_queues;
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
static mqtt_link_handoff_t s_pub_handoff;
static atomic_bool s_teardown_pending;
static atomic_int s_published_id;
static atomic_uint s_published_count;  /* publishes handed to esp-mqtt, for the health line */
static atomic_uint s_publish_failed;
static atomic_uint s_dropped_offline;  /* QoS 0 refused while the broker is down */
static atomic_uint s_weight_published;
static atomic_uint s_stop_pub_stuck;
static atomic_uint s_stop_deferred;
static atomic_bool s_pub_stuck;
static char s_boot_id[MQTT_LINK_BOOT_ID_LEN + 1U];
static char s_scale_id[MQTT_LINK_SCALE_ID_MAX + 1U] = MQTT_LINK_SCALE_ID_DEFAULT;
static const char *s_fw = "unknown";
static const char *s_role = "weight_sender";
static mqtt_link_wgate_t s_wgate;      /* weight_tx task only */
static broker_cfg_rec_t s_cfg_rec;
static broker_cfg_eff_t s_cfg_eff;
static broker_cfg_src_t s_cfg_src;
/* Last esp-mqtt connect error, stored lock-free by the event callback and
 * only formatted by the pub task. */
static atomic_int s_err_type;
static atomic_int s_err_code;
/* Quiet period between "connect failed" summaries while the broker is down. */
#define MQTT_FAIL_LOG_INTERVAL_MS 30000U

/* Stop budget: pub task exit + flush + graceful PUBACK wait stay under ~1 s. */
#define MQTT_STOP_TASK_WAIT_MS 300U
#define MQTT_STOP_ACK_WAIT_MS  400U
#define MQTT_STOP_FLUSH_MAX    (MQTT_LINK_RX_DEPTH + MQTT_LINK_PUB_DEPTH + MQTT_LINK_WEIGHT_SLOTS)

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
    memset(out, 0, sizeof(*out));
    mqtt_link_queue_stats(&s_queues, out);
    out->connected = atomic_load(&s_connected);
    out->published = atomic_load(&s_published_count);
    out->publish_failed = atomic_load(&s_publish_failed);
    out->dropped_offline = atomic_load(&s_dropped_offline);
    out->weight_published = atomic_load(&s_weight_published);
    out->heap_min_kb = mqtt_link_sat16((uint32_t)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024U));
    out->link_state = s_lc_ready ? (uint8_t)mqtt_link_lc_state(&s_lc) : (uint8_t)MQTT_LC_IDLE;
    out->pub_stuck = atomic_load(&s_pub_stuck);
    out->teardown_pending = atomic_load(&s_teardown_pending);
    out->stop_pub_stuck = mqtt_link_sat16(atomic_load(&s_stop_pub_stuck));
    out->stop_deferred = mqtt_link_sat16(atomic_load(&s_stop_deferred));
}

const char *mqtt_link_device_id(void)
{
    return s_device_id;
}

broker_cfg_src_t mqtt_link_config_source(void)
{
    return s_cfg_src;
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

static bool publish_gate(char *topic, size_t cap, const char *topic_suffix, int qos)
{
    if (!s_lc_ready || !mqtt_link_lc_running(&s_lc) ||
        !mqtt_link_topic(topic, cap, s_device_id, topic_suffix)) {
        return false;
    }
    if (qos == 0 && !atomic_load(&s_connected)) {
        atomic_fetch_add(&s_dropped_offline, 1U);
        return false;
    }
    return true;
}

bool mqtt_link_publish(const char *topic_suffix, const char *json, int qos, bool retain)
{
    char topic[MQTT_LINK_TOPIC_MAX];
    if (!publish_gate(topic, sizeof(topic), topic_suffix, qos)) return false;
    return mqtt_link_queue_push_pub(&s_queues, topic, json, qos, retain) == MQTT_ENQ_OK;
}

static bool weight_put(unsigned slot, const char *topic_suffix, uint32_t seq,
                       const mqtt_link_weight_t *w, bool with_message_id, uint32_t now_ms)
{
    char topic[MQTT_LINK_TOPIC_MAX];
    if (!publish_gate(topic, sizeof(topic), topic_suffix, 0)) return false;
    return mqtt_link_queue_put_weight_sample(&s_queues, slot, topic, mqtt_link_boot_id(), seq, w,
                                             with_message_id, now_ms) == MQTT_ENQ_OK;
}

void mqtt_link_weight_frame(const mqtt_link_weight_t *w, uint32_t now_ms)
{
    if (w == NULL) return;
    mqtt_link_weight_t f = *w;
    f.scale_id = s_scale_id;
    w = &f;
    mqtt_link_wpub_t step = mqtt_link_wgate_step(&s_wgate, w->src_uart, w->cas_seq, now_ms,
                                                 MQTT_HB_PERIOD_MS);
    if (step == MQTT_LINK_WPUB_NONE) return;
    if (step == MQTT_LINK_WPUB_FRAME &&
        weight_put(MQTT_LINK_WEIGHT_SLOT_CTL, MQTT_SUFFIX_WEIGHT_CTL, s_wgate.seq_ctl, w, false, now_ms)) {
        s_wgate.seq_ctl++;
    }
    if (weight_put(MQTT_LINK_WEIGHT_SLOT_TEL, MQTT_SUFFIX_WEIGHT_TEL, s_wgate.seq_tel, w, true, now_ms)) {
        s_wgate.seq_tel++;
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
            (void)mqtt_link_queue_push_rx(&s_queues, ev->topic, (size_t)ev->topic_len,
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
            if (item->from_weight) atomic_fetch_add(&s_weight_published, 1U);
        } else {
            atomic_fetch_add(&s_publish_failed, 1U);
        }
    } else if (item->qos > 0) {
        /* Command ACKs wait in the (size-limited) outbox for the next connect. */
        if (esp_mqtt_client_enqueue(s_client, item->topic, item->data, item->len,
                                    item->qos, item->retain, true) < 0) {
            atomic_fetch_add(&s_publish_failed, 1U);
        }
    } else {
        atomic_fetch_add(&s_dropped_offline, 1U);
    }
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

static bool link_teardown(void)
{
    if (s_client != NULL) {
        (void)esp_mqtt_client_stop(s_client);
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    atomic_store(&s_connected, false);
    if (!mqtt_link_queues_deinit(&s_queues)) {
        atomic_store(&s_teardown_pending, true);
        atomic_fetch_add(&s_stop_deferred, 1U);
        ESP_LOGE(TAG, "queues still in use after %ums; kept allocated, link held stopped reason=MQTT_STOP_QUEUES_BUSY",
                 (unsigned)MQTT_LINK_QUIESCE_MS);
        return false;
    }
    atomic_store(&s_teardown_pending, false);
    mqtt_link_lc_end_stop(&s_lc);
    ESP_LOGI(TAG, "stopped");
    return true;
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
        uint32_t poll_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool got = mqtt_link_queue_next(&s_queues, &item, MQTT_POLL_MS, poll_ms,
                                        MQTT_HB_PERIOD_MS);

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
    if (!mqtt_link_handoff_task_exit(&s_pub_handoff)) {
        atomic_store(&s_pub_stuck, false);
        ESP_LOGW(TAG, "pub task exited after stop gave up; finishing teardown reason=MQTT_STOP_LATE_EXIT");
        (void)link_teardown();
    }
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
    if (!mqtt_link_handoff_wait(&s_pub_handoff, MQTT_STOP_TASK_WAIT_MS)) {
        atomic_store(&s_pub_stuck, true);
        atomic_fetch_add(&s_stop_pub_stuck, 1U);
        ESP_LOGE(TAG, "pub task did not exit in %ums; teardown deferred to it reason=MQTT_STOP_PUB_STUCK",
                 (unsigned)MQTT_STOP_TASK_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    vTaskDelay(pdMS_TO_TICKS(20)); /* let the idle task reap the deleted task */

    if (s_client != NULL && atomic_load(&s_connected)) {
        static mqtt_item_t scratch;
        (void)mqtt_link_queue_flush(&s_queues, &scratch, MQTT_STOP_FLUSH_MAX, flush_cb, NULL);

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

    return link_teardown() ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void queues_release(void)
{
    if (!mqtt_link_queues_deinit(&s_queues)) {
        ESP_LOGE(TAG, "queues still in use; kept allocated reason=MQTT_STOP_QUEUES_BUSY");
    }
}

static void resolve_broker_cfg(void)
{
    uint8_t blob[sizeof(broker_cfg_rec_t)];
    size_t len = sizeof(blob);
    broker_cfg_src_t src;
    esp_err_t err = nvs_config_get_blob(NVS_KEY_MQTT_CFG, blob, &len);

    broker_cfg_wipe(&s_cfg_rec);
    if (err == ESP_OK) {
        src = broker_cfg_decode(blob, len, &s_cfg_rec);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        src = BROKER_CFG_SRC_FALLBACK_ABSENT;
    } else if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        src = BROKER_CFG_SRC_FALLBACK_VERSION;
    } else {
        ESP_LOGW(TAG, "mqtt_cfg read failed: %s", esp_err_to_name(err));
        src = BROKER_CFG_SRC_FALLBACK_INVALID;
    }
    memset(blob, 0, sizeof(blob));
    broker_cfg_effective(src, &s_cfg_rec, CONFIG_MQTT_LINK_BROKER_URI, CONFIG_MQTT_LINK_USERNAME,
                         CONFIG_MQTT_LINK_PASSWORD, &s_cfg_eff);
    s_cfg_src = s_cfg_eff.src;
    if (s_cfg_src != BROKER_CFG_SRC_NVS) {
        broker_cfg_wipe(&s_cfg_rec);
    }
}

static bool tls_bundle_available(void)
{
#if defined(CONFIG_MQTT_LINK_TLS_BUNDLE)
    return true;
#else
    return false;
#endif
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
    if (mqtt_link_lc_state(&s_lc) == MQTT_LC_STOPPING && atomic_load(&s_teardown_pending) &&
        mqtt_link_queues_deinit(&s_queues)) {
        atomic_store(&s_teardown_pending, false);
        mqtt_link_lc_end_stop(&s_lc);
        ESP_LOGI(TAG, "deferred teardown completed");
    }
    if (mqtt_link_lc_state(&s_lc) != MQTT_LC_IDLE) return ESP_ERR_INVALID_STATE;
    resolve_broker_cfg();
    if (s_cfg_eff.uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT OFF reason=MQTT_CFG_EMPTY source=%s", broker_cfg_src_name(s_cfg_src));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_cfg_eff.tls && !tls_bundle_available()) {
        ESP_LOGE(TAG, "MQTT OFF reason=MQTT_CFG_INVALID source=%s mqtts without a trust anchor",
                 broker_cfg_src_name(s_cfg_src));
        broker_cfg_wipe(&s_cfg_rec);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_cfg_src != BROKER_CFG_SRC_NVS && s_cfg_src != BROKER_CFG_SRC_KCONFIG) {
        ESP_LOGW(TAG, "mqtt_cfg unusable, using Kconfig: reason=%s", broker_cfg_src_name(s_cfg_src));
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
        broker_cfg_wipe(&s_cfg_rec);
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
    if (!mqtt_link_queues_init(&s_queues, MQTT_LINK_RX_DEPTH, MQTT_LINK_PUB_DEPTH)) {
        broker_cfg_wipe(&s_cfg_rec);
        return ESP_ERR_NO_MEM;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_cfg_eff.uri,
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
    bool auth = s_cfg_eff.user[0] != '\0';
    if (auth) {
        cfg.credentials.username = s_cfg_eff.user;
        cfg.credentials.authentication.password = s_cfg_eff.pass;
    }
#if defined(CONFIG_MQTT_LINK_TLS_BUNDLE)
    if (s_cfg_eff.tls) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
#endif

    atomic_store(&s_connected, false);
    atomic_store(&s_evt_connected, false);
    atomic_store(&s_evt_disconnected, false);
    mqtt_link_handoff_init(&s_pub_handoff);

    char shown[96];
    mqtt_link_uri_redact(shown, sizeof(shown), s_cfg_eff.uri);
    s_client = esp_mqtt_client_init(&cfg);
    broker_cfg_wipe(&s_cfg_rec);
    memset(&cfg, 0, sizeof(cfg));
    if (s_client == NULL) {
        ESP_LOGE(TAG, "MQTT OFF reason=MQTT_INIT_FAIL source=%s", broker_cfg_src_name(s_cfg_src));
        queues_release();
        return ESP_FAIL;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, NULL);
    if (err != ESP_OK) {
        (void)esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        queues_release();
        return err;
    }

    (void)mqtt_link_lc_start(&s_lc);
    if (xTaskCreate(mqtt_pub_task, "mqtt_pub", MQTT_PUB_STACK, NULL, MQTT_TASK_PRIO, NULL) !=
        pdPASS) {
        (void)mqtt_link_lc_begin_stop(&s_lc);
        (void)link_teardown();
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

    ESP_LOGI(TAG, "STARTED broker=%s source=%s tls=%u auth=%u device=%s keepalive=%ds", shown,
             broker_cfg_src_name(s_cfg_src), (unsigned)s_cfg_eff.tls, (unsigned)auth, s_device_id,
             CONFIG_MQTT_LINK_KEEPALIVE_S);
    return ESP_OK;
}
