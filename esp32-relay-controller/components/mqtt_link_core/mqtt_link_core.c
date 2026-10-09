#include "mqtt_link_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "freertos/task.h"

void mqtt_link_boot_id_format(char out[MQTT_LINK_BOOT_ID_LEN + 1U], uint32_t rnd)
{
    snprintf(out, MQTT_LINK_BOOT_ID_LEN + 1U, "%08lx", (unsigned long)rnd);
}

const char *mqtt_link_boot_id(void)
{
    /* 0 = unset, 1 = being generated, 2 = ready. Generated once per boot. */
    static char s_boot[MQTT_LINK_BOOT_ID_LEN + 1U];
    static atomic_int s_state;
    int expect = 0;
    if (atomic_compare_exchange_strong(&s_state, &expect, 1)) {
        mqtt_link_boot_id_format(s_boot, esp_random());
        atomic_store(&s_state, 2);
    } else {
        while (atomic_load(&s_state) != 2) vTaskDelay(1);
    }
    return s_boot;
}

bool mqtt_link_birth_build(char *out, size_t cap, const char *boot_id)
{
    if (out == NULL || boot_id == NULL) return false;
    int n = snprintf(out, cap, "{\"online\":true,\"boot_id\":\"%s\",\"caps\":[\"cmd_mqtt\",\"weight_mqtt\"]}",
                     boot_id);
    return n > 0 && (size_t)n < cap;
}

bool mqtt_link_topic_equals(const char *topic, size_t topic_len, const char *expected)
{
    return topic != NULL && expected != NULL && topic_len == strlen(expected) &&
           memcmp(topic, expected, topic_len) == 0;
}

bool mqtt_link_device_id_valid(const char *id)
{
    if (id == NULL) return false;
    size_t n = 0U;
    for (; id[n] != '\0'; n++) {
        char c = id[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || n >= MQTT_LINK_DEVICE_ID_MAX) return false;
    }
    return n >= 1U;
}

bool mqtt_link_topic(char *out, size_t cap, const char *device_id, const char *suffix)
{
    if (out == NULL || cap == 0U || suffix == NULL || !mqtt_link_device_id_valid(device_id)) {
        return false;
    }
    int n = snprintf(out, cap, "cas/%s/%s", device_id, suffix);
    return n > 0 && (size_t)n < cap && (size_t)n < MQTT_LINK_TOPIC_MAX;
}

bool mqtt_link_topic_is_run(const char *topic)
{
    if (topic == NULL || strncmp(topic, "cas/", 4U) != 0) return false;
    const char *s = strchr(topic + 4, '/');   /* device ids never contain '/' */
    return s != NULL && strncmp(s, "/run/", 5U) == 0;
}

bool mqtt_link_presence_plan(mqtt_link_presence_t *out, const char *device_id)
{
    if (out == NULL) return false;
    if (!mqtt_link_topic(out->lwt_topic, sizeof(out->lwt_topic), device_id, MQTT_SUFFIX_PRESENCE)) {
        return false;
    }
    /* LWT and birth share the one retained presence topic. */
    memcpy(out->birth_topic, out->lwt_topic, sizeof(out->birth_topic));
    out->lwt_payload = MQTT_LINK_LWT_JSON;
    out->birth_payload = MQTT_LINK_BIRTH_JSON;
    out->graceful_payload = MQTT_LINK_GRACEFUL_JSON;
    out->qos = 1;
    out->retain = true;
    return true;
}

bool mqtt_link_connect_plan(mqtt_link_connect_plan_t *out, const char *device_id)
{
    if (out == NULL) return false;
    if (!mqtt_link_topic(out->subscribe_topic, sizeof(out->subscribe_topic), device_id,
                         MQTT_SUFFIX_COMMANDS)) {
        return false;
    }
    out->subscribe_qos = 1;
    return mqtt_link_presence_plan(&out->presence, device_id);
}

uint32_t mqtt_link_backoff_ms(uint32_t attempt)
{
    uint32_t ms = MQTT_LINK_BACKOFF_MIN_MS;
    while (attempt-- > 0U && ms < MQTT_LINK_BACKOFF_MAX_MS) ms <<= 1;
    return ms > MQTT_LINK_BACKOFF_MAX_MS ? MQTT_LINK_BACKOFF_MAX_MS : ms;
}

void mqtt_link_reconnect_init(mqtt_link_reconnect_t *r)
{
    r->attempt = 0U;
    r->pending = false;
    r->due_us = 0;
}

uint32_t mqtt_link_reconnect_on_disconnect(mqtt_link_reconnect_t *r, int64_t now_us)
{
    uint32_t wait = mqtt_link_backoff_ms(r->attempt);
    if (r->attempt < 31U) r->attempt++;
    r->due_us = now_us + (int64_t)wait * 1000;
    r->pending = true;
    return wait;
}

bool mqtt_link_reconnect_due(mqtt_link_reconnect_t *r, int64_t now_us)
{
    if (!r->pending || now_us < r->due_us) return false;
    r->pending = false;
    return true;
}

void mqtt_link_reconnect_on_connected(mqtt_link_reconnect_t *r)
{
    r->attempt = 0U;
    r->pending = false;
}

void mqtt_link_uri_redact(char *out, size_t cap, const char *uri)
{
    if (out == NULL || cap == 0U) return;
    out[0] = '\0';
    if (uri == NULL) return;
    const char *auth = strstr(uri, "://");
    auth = (auth != NULL) ? auth + 3 : uri;
    const char *slash = strchr(auth, '/');
    const char *at = NULL;
    for (const char *p = auth; *p != '\0' && (slash == NULL || p < slash); p++) {
        if (*p == '@') at = p; /* last '@' in the authority ends the userinfo */
    }
    if (at == NULL) {
        snprintf(out, cap, "%s", uri);
    } else {
        snprintf(out, cap, "%.*s%s", (int)(auth - uri), uri, at + 1);
    }
}

bool mqtt_link_queue_init(mqtt_link_queue_t *queue, size_t rx_depth, size_t tx_depth)
{
    if (queue == NULL || rx_depth == 0U || tx_depth == 0U) return false;
    queue->rx_dropped_full = 0U;
    queue->tx_dropped_full = 0U;
    queue->tx_dropped_live = 0U;
    queue->dropped_other = 0U;
    queue->dropped_retained = 0U;
    queue->tx_dropped_net = 0U;
    queue->wrx = NULL;
    queue->wrx_dropped_full = 0U;
    queue->arx = NULL;
    queue->arx_dropped_full = 0U;
    queue->tx_deferred_run = 0U;
    queue->rx = xQueueCreate((UBaseType_t)rx_depth, sizeof(mqtt_rx_item_t));
    queue->tx = xQueueCreate((UBaseType_t)tx_depth, sizeof(mqtt_tx_item_t));
    if (queue->rx == NULL || queue->tx == NULL) {
        mqtt_link_queue_deinit(queue);
        return false;
    }
    return true;
}

mqtt_enq_result_t mqtt_link_queue_push_rx(mqtt_link_queue_t *queue,
                                          const char *topic, size_t topic_len,
                                          const char *data, size_t data_len,
                                          size_t total_len, bool retain,
                                          uint32_t now_ms, int64_t now_us)
{
    if (queue == NULL || queue->rx == NULL || topic == NULL || data == NULL ||
        topic_len == 0U) {
        return MQTT_ENQ_INVALID;
    }
    if (retain) {
        queue->dropped_other++;
        queue->dropped_retained++;
        return MQTT_ENQ_RETAINED;
    }
    if (data_len != total_len) {
        queue->dropped_other++;
        return MQTT_ENQ_FRAGMENT;
    }
    if (topic_len >= MQTT_LINK_TOPIC_MAX || data_len > MQTT_LINK_RX_PAYLOAD_MAX) {
        queue->dropped_other++;
        return MQTT_ENQ_TOO_BIG;
    }
    mqtt_rx_item_t item; /* on the caller's stack (~1.1 KB); no heap in the callback */
    item.len = (uint16_t)data_len;
    item.recv_ms = now_ms;
    item.recv_us = now_us;
    memcpy(item.topic, topic, topic_len);
    item.topic[topic_len] = '\0';
    memcpy(item.data, data, data_len);
    item.data[data_len] = '\0';
    if (xQueueSend(queue->rx, &item, 0) != pdTRUE) {
        queue->rx_dropped_full++;
        return MQTT_ENQ_FULL;
    }
    return MQTT_ENQ_OK;
}

bool mqtt_link_queue_weight_init(mqtt_link_queue_t *queue, size_t depth)
{
    if (queue == NULL || depth == 0U) return false;
    if (queue->wrx != NULL) return true;
    queue->wrx_dropped_full = 0U;
    queue->wrx = xQueueCreate((UBaseType_t)depth, sizeof(mqtt_wrx_item_t));
    return queue->wrx != NULL;
}

mqtt_enq_result_t mqtt_link_queue_push_weight(mqtt_link_queue_t *queue,
                                              const char *data, size_t data_len,
                                              size_t total_len, bool retain, uint32_t now_ms)
{
    if (queue == NULL || queue->wrx == NULL || data == NULL) return MQTT_ENQ_INVALID;
    if (retain) {
        queue->dropped_other++;
        queue->dropped_retained++;
        return MQTT_ENQ_RETAINED;
    }
    if (data_len != total_len) {
        queue->dropped_other++;
        return MQTT_ENQ_FRAGMENT;
    }
    if (data_len > MQTT_LINK_WRX_PAYLOAD_MAX) {
        queue->dropped_other++;
        return MQTT_ENQ_TOO_BIG;
    }
    mqtt_wrx_item_t item; /* ~270 B on the callback stack; no heap */
    item.len = (uint16_t)data_len;
    item.recv_ms = now_ms;
    memcpy(item.data, data, data_len);
    item.data[data_len] = '\0';
    if (xQueueSend(queue->wrx, &item, 0) != pdTRUE) {
        queue->wrx_dropped_full++;
        return MQTT_ENQ_FULL;
    }
    return MQTT_ENQ_OK;
}

bool mqtt_link_queue_pop_weight(mqtt_link_queue_t *queue, mqtt_wrx_item_t *out, uint32_t timeout_ms)
{
    if (queue == NULL || queue->wrx == NULL || out == NULL) return false;
    return xQueueReceive(queue->wrx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

size_t mqtt_link_queue_pending_weight(const mqtt_link_queue_t *queue)
{
    if (queue == NULL || queue->wrx == NULL) return 0U;
    return (size_t)uxQueueMessagesWaiting(queue->wrx);
}

bool mqtt_link_queue_ack_init(mqtt_link_queue_t *queue, size_t depth)
{
    if (queue == NULL || depth == 0U) return false;
    if (queue->arx != NULL) return true;
    queue->arx_dropped_full = 0U;
    queue->arx = xQueueCreate((UBaseType_t)depth, sizeof(mqtt_arx_item_t));
    return queue->arx != NULL;
}

mqtt_enq_result_t mqtt_link_queue_push_ack(mqtt_link_queue_t *queue,
                                           const char *data, size_t data_len,
                                           size_t total_len, bool retain)
{
    if (queue == NULL || queue->arx == NULL || data == NULL) return MQTT_ENQ_INVALID;
    if (retain) {
        queue->dropped_other++;
        queue->dropped_retained++;
        return MQTT_ENQ_RETAINED;
    }
    if (data_len != total_len) {
        queue->dropped_other++;
        return MQTT_ENQ_FRAGMENT;
    }
    if (data_len > MQTT_LINK_ARX_PAYLOAD_MAX) {
        queue->dropped_other++;
        return MQTT_ENQ_TOO_BIG;
    }
    mqtt_arx_item_t item; /* ~520 B on the callback stack; no heap */
    item.len = (uint16_t)data_len;
    memcpy(item.data, data, data_len);
    item.data[data_len] = '\0';
    if (xQueueSend(queue->arx, &item, 0) != pdTRUE) {
        queue->arx_dropped_full++;
        return MQTT_ENQ_FULL;
    }
    return MQTT_ENQ_OK;
}

bool mqtt_link_queue_pop_ack(mqtt_link_queue_t *queue, mqtt_arx_item_t *out, uint32_t timeout_ms)
{
    if (queue == NULL || queue->arx == NULL || out == NULL) return false;
    return xQueueReceive(queue->arx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

mqtt_enq_result_t mqtt_link_queue_push_event(mqtt_link_queue_t *queue,
                                             const char *weight_topic, const char *ack_topic,
                                             const char *topic, size_t topic_len,
                                             const char *data, size_t data_len,
                                             size_t total_len, bool retain,
                                             uint32_t now_ms, int64_t now_us)
{
    if (topic == NULL || data == NULL) return MQTT_ENQ_INVALID;
    if (weight_topic != NULL && mqtt_link_topic_equals(topic, topic_len, weight_topic)) {
        return mqtt_link_queue_push_weight(queue, data, data_len, total_len, retain, now_ms);
    }
    if (ack_topic != NULL && mqtt_link_topic_equals(topic, topic_len, ack_topic)) {
        return mqtt_link_queue_push_ack(queue, data, data_len, total_len, retain);
    }
    return mqtt_link_queue_push_rx(queue, topic, topic_len, data, data_len, total_len, retain,
                                   now_ms, now_us);
}

static mqtt_enq_result_t push_pub_limit(mqtt_link_queue_t *queue,
                                        const char *topic, const char *data,
                                        int qos, bool retain, UBaseType_t qos0_limit,
                                        volatile uint32_t *drop_ctr)
{
    if (queue == NULL || queue->tx == NULL || topic == NULL || data == NULL ||
        qos < 0 || qos > 2) {
        return MQTT_ENQ_INVALID;
    }
    size_t tl = strlen(topic);
    size_t dl = strlen(data);
    if (tl == 0U || tl >= MQTT_LINK_TOPIC_MAX || dl > MQTT_LINK_TX_PAYLOAD_MAX) {
        queue->dropped_other++;
        return MQTT_ENQ_TOO_BIG;
    }
    if (qos == 0 && uxQueueMessagesWaiting(queue->tx) >= qos0_limit) {
        (*drop_ctr)++; /* telemetry never takes the slots reserved for ACKs */
        return MQTT_ENQ_FULL;
    }
    mqtt_tx_item_t item;
    item.qos = (uint8_t)qos;
    item.retain = retain;
    item.len = (uint16_t)dl;
    memcpy(item.topic, topic, tl + 1U);
    item.data = (char *)malloc(dl + 1U);
    if (item.data == NULL) {
        (*drop_ctr)++;
        return MQTT_ENQ_FULL;
    }
    memcpy(item.data, data, dl + 1U);
    if (xQueueSend(queue->tx, &item, 0) != pdTRUE) {
        free(item.data);
        (*drop_ctr)++;
        return MQTT_ENQ_FULL;
    }
    return MQTT_ENQ_OK;
}

mqtt_enq_result_t mqtt_link_queue_push_pub(mqtt_link_queue_t *queue,
                                           const char *topic, const char *data,
                                           int qos, bool retain)
{
    return push_pub_limit(queue, topic, data, qos, retain, MQTT_LINK_TX_QOS0_LIMIT,
                          queue ? &queue->tx_dropped_full : NULL);
}

mqtt_enq_result_t mqtt_link_queue_push_live(mqtt_link_queue_t *queue,
                                            const char *topic, const char *data)
{
    return push_pub_limit(queue, topic, data, 0, false, MQTT_LINK_TX_LIVE_LIMIT,
                          queue ? &queue->tx_dropped_live : NULL);
}

bool mqtt_link_queue_pop_rx(mqtt_link_queue_t *queue, mqtt_rx_item_t *out, uint32_t timeout_ms)
{
    if (queue == NULL || queue->rx == NULL || out == NULL) return false;
    return xQueueReceive(queue->rx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool mqtt_link_queue_pop_tx(mqtt_link_queue_t *queue, mqtt_tx_item_t *out, uint32_t timeout_ms)
{
    if (queue == NULL || queue->tx == NULL || out == NULL) return false;
    return xQueueReceive(queue->tx, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void mqtt_link_tx_item_free(mqtt_tx_item_t *item)
{
    if (item == NULL || item->data == NULL) return;
    free(item->data);
    item->data = NULL;
}

size_t mqtt_link_queue_pending_rx(const mqtt_link_queue_t *queue)
{
    if (queue == NULL || queue->rx == NULL) return 0U;
    return (size_t)uxQueueMessagesWaiting(queue->rx);
}

size_t mqtt_link_queue_pending_tx(const mqtt_link_queue_t *queue)
{
    if (queue == NULL || queue->tx == NULL) return 0U;
    return (size_t)uxQueueMessagesWaiting(queue->tx);
}

size_t mqtt_link_queue_flush_tx(mqtt_link_queue_t *queue, size_t max_items,
                                mqtt_link_flush_cb cb, void *ctx)
{
    if (queue == NULL || queue->tx == NULL || cb == NULL) return 0U;
    size_t n = 0U;
    mqtt_tx_item_t item;
    while (n < max_items && mqtt_link_queue_pop_tx(queue, &item, 0U)) {
        cb(&item, ctx);
        mqtt_link_tx_item_free(&item);
        n++;
    }
    return n;
}

void mqtt_link_queue_reset(mqtt_link_queue_t *queue)
{
    if (queue == NULL) return;
    mqtt_tx_item_t tx;
    if (queue->tx != NULL) {
        while (mqtt_link_queue_pop_tx(queue, &tx, 0U)) mqtt_link_tx_item_free(&tx);
    }
    if (queue->rx != NULL) xQueueReset(queue->rx);
    if (queue->wrx != NULL) xQueueReset(queue->wrx);
    if (queue->arx != NULL) xQueueReset(queue->arx);
}

void mqtt_link_queue_deinit(mqtt_link_queue_t *queue)
{
    if (queue == NULL) return;
    QueueHandle_t rx = queue->rx;
    QueueHandle_t tx = queue->tx;
    QueueHandle_t wrx = queue->wrx;
    QueueHandle_t arx = queue->arx;
    mqtt_link_queue_reset(queue);
    queue->rx = NULL; /* pushes now refuse before the handles are freed */
    queue->tx = NULL;
    queue->wrx = NULL;
    queue->arx = NULL;
    if (arx != NULL) vQueueDelete(arx);
    if (wrx != NULL) vQueueDelete(wrx);
    if (rx != NULL) vQueueDelete(rx);
    if (tx != NULL) vQueueDelete(tx);
}

mqtt_tx_result_t mqtt_link_tx_dispatch(const mqtt_link_tx_ops_t *ops, mqtt_link_queue_t *queue,
                                       const mqtt_tx_item_t *item, bool connected,
                                       size_t outbox_limit)
{
    if (ops == NULL || ops->enqueue == NULL || item == NULL) return MQTT_TX_DROPPED_TELEMETRY;
    if (item->qos > 0 && mqtt_link_topic_is_run(item->topic)) {
        size_t used = ops->outbox_size != NULL ? ops->outbox_size(ops->ctx) : 0U;
        if (used + item->len + MQTT_LINK_ACK_RESERVE > outbox_limit) {
            if (queue != NULL) queue->tx_deferred_run++;
            return MQTT_TX_RUN_DEFERRED;
        }
    }
    if (item->qos > 0) {
        /* ACKs and presence: stored for the next connect if need be. */
        return ops->enqueue(ops->ctx, item->topic, item->data, item->len, item->qos,
                            item->retain) >= 0 ? MQTT_TX_ENQUEUED : MQTT_TX_ACK_REJECTED;
    }
    if (!connected) return MQTT_TX_DROPPED_OFFLINE;
    /* Telemetry stops short of the outbox limit; the tail is for ACKs/presence. */
    size_t used = ops->outbox_size != NULL ? ops->outbox_size(ops->ctx) : 0U;
    if (used + item->len + MQTT_LINK_ACK_RESERVE > outbox_limit ||
        ops->enqueue(ops->ctx, item->topic, item->data, item->len, 0, item->retain) < 0) {
        if (queue != NULL) queue->tx_dropped_net++;
        return MQTT_TX_DROPPED_TELEMETRY;
    }
    return MQTT_TX_ENQUEUED;
}

mqtt_stop_result_t mqtt_link_stop_sequence(const mqtt_link_stop_ops_t *ops, uint32_t task_wait_ms)
{
    ops->goodbye(ops->ctx);
    ops->client_stop(ops->ctx); /* aborts a publish the pub task may be stuck in */
    uint32_t w = 0U;
    while (!ops->task_exited(ops->ctx)) {
        if (w >= task_wait_ms) return MQTT_STOP_LEAKED; /* never destroy under a live task */
        ops->delay_ms(ops->ctx, 10U);
        w += 10U;
    }
    ops->client_destroy(ops->ctx);
    return MQTT_STOP_DESTROYED;
}
void mqtt_link_lc_init(mqtt_link_lc_t *lc)
{
    atomic_init(&lc->state, MQTT_LC_IDLE);
}

static bool lc_move(mqtt_link_lc_t *lc, int from, int to)
{
    return atomic_compare_exchange_strong(&lc->state, &from, to);
}

bool mqtt_link_lc_start(mqtt_link_lc_t *lc)
{
    return lc_move(lc, MQTT_LC_IDLE, MQTT_LC_RUNNING);
}

bool mqtt_link_lc_begin_stop(mqtt_link_lc_t *lc)
{
    return lc_move(lc, MQTT_LC_RUNNING, MQTT_LC_STOPPING);
}

void mqtt_link_lc_end_stop(mqtt_link_lc_t *lc)
{
    (void)lc_move(lc, MQTT_LC_STOPPING, MQTT_LC_IDLE);
}

bool mqtt_link_lc_running(const mqtt_link_lc_t *lc)
{
    return mqtt_link_lc_state(lc) == MQTT_LC_RUNNING;
}

mqtt_lc_state_t mqtt_link_lc_state(const mqtt_link_lc_t *lc)
{
    return (mqtt_lc_state_t)atomic_load((atomic_int *)&lc->state);
}
