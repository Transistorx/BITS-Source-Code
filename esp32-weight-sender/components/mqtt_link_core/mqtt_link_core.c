#include "mqtt_link_core.h"

#include <stdio.h>
#include <string.h>

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

uint32_t mqtt_link_backoff_ms(uint32_t attempt)
{
    uint32_t ms = MQTT_LINK_BACKOFF_MIN_MS;
    while (attempt-- > 0U && ms < MQTT_LINK_BACKOFF_MAX_MS) ms <<= 1;
    return ms > MQTT_LINK_BACKOFF_MAX_MS ? MQTT_LINK_BACKOFF_MAX_MS : ms;
}

uint32_t mqtt_link_backoff_jitter_ms(uint32_t attempt, uint32_t rnd)
{
    uint32_t base = mqtt_link_backoff_ms(attempt);
    uint32_t ms = base + rnd % (base / 4U + 1U);
    return ms > MQTT_LINK_BACKOFF_MAX_MS ? MQTT_LINK_BACKOFF_MAX_MS : ms;
}

void mqtt_link_boot_id_fmt(char out[MQTT_LINK_BOOT_ID_LEN + 1U], uint32_t rnd)
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0U; i < MQTT_LINK_BOOT_ID_LEN; i++) {
        out[i] = hex[(rnd >> (28U - 4U * i)) & 0xFU];
    }
    out[MQTT_LINK_BOOT_ID_LEN] = '\0';
}

bool mqtt_link_boot_id_valid(const char *id)
{
    if (id == NULL || strlen(id) != MQTT_LINK_BOOT_ID_LEN) return false;
    for (size_t i = 0U; i < MQTT_LINK_BOOT_ID_LEN; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}

/* Strings embedded in envelopes are fixed firmware constants; refuse anything
 * that would need escaping rather than emit broken JSON. */
static bool json_plain(const char *s)
{
    if (s == NULL) return false;
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\' || (unsigned char)*s < 0x20U) return false;
    }
    return true;
}

bool mqtt_link_scale_id_valid(const char *id)
{
    if (id == NULL) return false;
    size_t n = 0U;
    for (; id[n] != '\0'; n++) {
        char c = id[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= MQTT_LINK_SCALE_ID_MAX) return false;
    }
    return n >= 1U;
}

bool mqtt_link_birth_json(char *out, size_t cap, const char *boot_id, const char *caps_json,
                          const char *fw, const char *role, const char *scale_id)
{
    if (out == NULL || cap == 0U || !mqtt_link_boot_id_valid(boot_id) || caps_json == NULL ||
        !json_plain(fw) || !json_plain(role) || !mqtt_link_scale_id_valid(scale_id)) {
        return false;
    }
    int n = snprintf(out, cap,
                     "{\"online\":true,\"schema_version\":1,\"boot_id\":\"%s\",\"caps\":%s,"
                     "\"fw\":\"%s\",\"role\":\"%s\",\"scale_id\":\"%s\"}",
                     boot_id, caps_json, fw, role, scale_id);
    return n > 0 && (size_t)n < cap && (size_t)n <= MQTT_LINK_ENV_MAX;
}

bool mqtt_link_presence_set_birth(mqtt_link_presence_t *p, const char *boot_id,
                                  const char *caps_json, const char *fw, const char *role,
                                  const char *scale_id)
{
    if (p == NULL || !mqtt_link_birth_json(p->birth_buf, sizeof(p->birth_buf), boot_id, caps_json,
                                           fw, role, scale_id)) {
        return false;
    }
    p->birth_payload = p->birth_buf;
    return true;
}

size_t mqtt_link_weight_json(char *out, size_t cap, const char *boot_id, uint32_t seq,
                             const mqtt_link_weight_t *w, bool with_message_id)
{
    if (out == NULL || cap == 0U || w == NULL || !mqtt_link_boot_id_valid(boot_id) ||
        (w->src_uart != 1U && w->src_uart != 2U) || !mqtt_link_scale_id_valid(w->scale_id) ||
        !json_plain(w->source)) {
        return 0U;
    }
    char mid[48] = ""; /* worst case 35 chars + NUL */
    if (with_message_id) {
        (void)snprintf(mid, sizeof(mid), "\"message_id\":\"%s-%lu\",", boot_id,
                       (unsigned long)seq);
    }
    int n = snprintf(out, cap,
                     "{\"schema_version\":1,\"boot_id\":\"%s\",%s\"seq\":%lu,\"uptime_ms\":%lu,"
                     "\"scale_id\":\"%s\",\"src_uart\":\"UART%u\",\"weight_g\":%ld,\"stable\":%s,\"age_ms\":%lu,"
                     "\"cas_seq\":%lu,\"source\":\"%s\"}",
                     boot_id, mid, (unsigned long)seq, (unsigned long)w->uptime_ms,
                     w->scale_id, (unsigned)w->src_uart, (long)w->weight_g, w->stable ? "true" : "false",
                     (unsigned long)w->age_ms, (unsigned long)w->cas_seq, w->source);
    /* weight/ctl is capped at 256 B; the telemetry copy (message_id) is per section 7. */
    size_t limit = with_message_id ? MQTT_LINK_PAYLOAD_MAX : MQTT_LINK_ENV_MAX;
    if (n <= 0 || (size_t)n >= cap || (size_t)n > limit) return 0U;
    return (size_t)n;
}

void mqtt_link_wgate_init(mqtt_link_wgate_t *g)
{
    memset(g, 0, sizeof(*g));
}

bool mqtt_link_wgate_new_frame(mqtt_link_wgate_t *g, uint8_t src_uart, uint32_t cas_seq)
{
    if (src_uart != 1U && src_uart != 2U) return false;
    unsigned i = (unsigned)src_uart - 1U;
    if (g->have_cas_seq[i] && g->last_cas_seq[i] == cas_seq) return false;
    g->have_cas_seq[i] = true;
    g->last_cas_seq[i] = cas_seq;
    return true;
}

bool mqtt_link_wgate_tel_due(mqtt_link_wgate_t *g, uint32_t now_ms, uint32_t period_ms)
{
    if (g->have_tel && (uint32_t)(now_ms - g->last_tel_ms) < period_ms) return false;
    g->have_tel = true;
    g->last_tel_ms = now_ms;
    return true;
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

bool mqtt_link_queue_init(mqtt_link_queue_t *queue, size_t depth)
{
    if (queue == NULL || depth == 0U) return false;
    queue->dropped_full = 0U;
    queue->dropped_other = 0U;
    queue->q = xQueueCreate((UBaseType_t)depth, sizeof(mqtt_item_t));
    return queue->q != NULL;
}

static mqtt_enq_result_t push_item(mqtt_link_queue_t *queue, const mqtt_item_t *item)
{
    if (xQueueSend(queue->q, item, 0) != pdTRUE) {
        queue->dropped_full++;
        return MQTT_ENQ_FULL;
    }
    return MQTT_ENQ_OK;
}

mqtt_enq_result_t mqtt_link_queue_push_rx(mqtt_link_queue_t *queue,
                                          const char *topic, size_t topic_len,
                                          const char *data, size_t data_len,
                                          size_t total_len, bool retain,
                                          uint32_t now_ms)
{
    if (queue == NULL || queue->q == NULL || topic == NULL || data == NULL ||
        topic_len == 0U) {
        return MQTT_ENQ_INVALID;
    }
    if (retain) {
        queue->dropped_other++;
        return MQTT_ENQ_RETAINED;
    }
    if (data_len != total_len) {
        queue->dropped_other++;
        return MQTT_ENQ_FRAGMENT;
    }
    if (topic_len >= MQTT_LINK_TOPIC_MAX || data_len > MQTT_LINK_PAYLOAD_MAX) {
        queue->dropped_other++;
        return MQTT_ENQ_TOO_BIG;
    }
    mqtt_item_t item; /* on the caller's stack, 600 bytes; no heap in the callback */
    item.kind = MQTT_ITEM_RX;
    item.qos = 0U;
    item.retain = false;
    item.len = (uint16_t)data_len;
    item.recv_ms = now_ms;
    memcpy(item.topic, topic, topic_len);
    item.topic[topic_len] = '\0';
    memcpy(item.data, data, data_len);
    item.data[data_len] = '\0';
    return push_item(queue, &item);
}

mqtt_enq_result_t mqtt_link_queue_push_pub(mqtt_link_queue_t *queue,
                                           const char *topic, const char *data,
                                           int qos, bool retain)
{
    if (queue == NULL || queue->q == NULL || topic == NULL || data == NULL) {
        return MQTT_ENQ_INVALID;
    }
    size_t tl = strlen(topic);
    size_t dl = strlen(data);
    if (tl == 0U || tl >= MQTT_LINK_TOPIC_MAX || dl > MQTT_LINK_PAYLOAD_MAX) {
        queue->dropped_other++;
        return MQTT_ENQ_TOO_BIG;
    }
    mqtt_item_t item;
    item.kind = MQTT_ITEM_PUB;
    item.qos = (uint8_t)qos;
    item.retain = retain;
    item.len = (uint16_t)dl;
    item.recv_ms = 0U;
    memcpy(item.topic, topic, tl + 1U);
    memcpy(item.data, data, dl + 1U);
    return push_item(queue, &item);
}

bool mqtt_link_queue_pop(mqtt_link_queue_t *queue, mqtt_item_t *out, uint32_t timeout_ms)
{
    if (queue == NULL || queue->q == NULL || out == NULL) return false;
    return xQueueReceive(queue->q, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

size_t mqtt_link_queue_pending(const mqtt_link_queue_t *queue)
{
    if (queue == NULL || queue->q == NULL) return 0U;
    return (size_t)uxQueueMessagesWaiting(queue->q);
}

size_t mqtt_link_queue_flush(mqtt_link_queue_t *queue, mqtt_item_t *scratch, size_t max_items,
                             mqtt_link_flush_cb cb, void *ctx)
{
    if (queue == NULL || queue->q == NULL || scratch == NULL || cb == NULL) return 0U;
    size_t n = 0U;
    while (n < max_items && mqtt_link_queue_pop(queue, scratch, 0U)) {
        cb(scratch, ctx);
        n++;
    }
    return n;
}

void mqtt_link_queue_deinit(mqtt_link_queue_t *queue)
{
    if (queue == NULL || queue->q == NULL) return;
    QueueHandle_t q = queue->q;
    queue->q = NULL; /* pushes now refuse before the handle is freed */
    vQueueDelete(q);
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
