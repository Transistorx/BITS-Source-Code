#include "mqtt_link_core.h"

#include <stdio.h>
#include <string.h>

#include "freertos/task.h"

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
                     "\"scale_id\":\"%s\",\"channel\":\"CH%u\",\"src_uart\":\"UART%u\",\"weight_g\":%ld,"
                     "\"stable\":%s,\"age_ms\":%lu,\"cas_seq\":%lu,\"source\":\"%s\"}",
                     boot_id, mid, (unsigned long)seq, (unsigned long)w->uptime_ms,
                     w->scale_id, (unsigned)w->src_uart, (unsigned)w->src_uart, (long)w->weight_g,
                     w->stable ? "true" : "false",
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

static void wgate_hb_stamp(mqtt_link_wgate_t *g, uint32_t now_ms)
{
    g->have_tel = true;
    g->last_tel_ms = now_ms;
}

bool mqtt_link_wgate_hb_due(mqtt_link_wgate_t *g, uint32_t now_ms, uint32_t period_ms)
{
    if (g == NULL) return false;
    if (g->have_tel && (uint32_t)(now_ms - g->last_tel_ms) < period_ms) return false;
    wgate_hb_stamp(g, now_ms);
    return true;
}

mqtt_link_wpub_t mqtt_link_wgate_step(mqtt_link_wgate_t *g, uint8_t src_uart, uint32_t cas_seq,
                                      uint32_t now_ms, uint32_t period_ms)
{
    if (g == NULL || (src_uart != 1U && src_uart != 2U)) return MQTT_LINK_WPUB_NONE;
    if (mqtt_link_wgate_new_frame(g, src_uart, cas_seq)) {
        wgate_hb_stamp(g, now_ms);
        return MQTT_LINK_WPUB_FRAME;
    }
    if (mqtt_link_wgate_hb_due(g, now_ms, period_ms)) return MQTT_LINK_WPUB_HEARTBEAT;
    return MQTT_LINK_WPUB_NONE;
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

typedef struct {
    uint32_t sample_ms;
    uint32_t uptime_ms;
    uint32_t seq;
    int32_t weight_g;
    uint32_t cas_seq;
    uint8_t src_uart;
    bool stable;
    bool with_message_id;
    char boot_id[MQTT_LINK_BOOT_ID_LEN + 1U];
    char scale_id[MQTT_LINK_SCALE_ID_MAX + 1U];
    char source[MQTT_LINK_SOURCE_MAX + 1U];
} weight_rec_t;

_Static_assert(sizeof(weight_rec_t) <= MQTT_LINK_PAYLOAD_MAX, "weight record must fit an item payload");

typedef enum { WAGE_FRESH = 0, WAGE_STALE, WAGE_FUTURE } wage_t;

static void queue_delete(QueueHandle_t *h)
{
    QueueHandle_t q = *h;
    *h = NULL;
    if (q != NULL) vQueueDelete(q);
}

static bool queues_ready(const mqtt_link_queues_t *q)
{
    return q->rx != NULL && q->pub != NULL && q->doorbell != NULL &&
           q->weight[MQTT_LINK_WEIGHT_SLOT_CTL] != NULL && q->weight[MQTT_LINK_WEIGHT_SLOT_TEL] != NULL;
}

static bool q_enter(mqtt_link_queues_t *q)
{
    if (q == NULL) return false;
    atomic_fetch_add(&q->users, 1U);
    if (atomic_load(&q->open) && queues_ready(q)) return true;
    atomic_fetch_sub(&q->users, 1U);
    return false;
}

static void q_leave(mqtt_link_queues_t *q)
{
    atomic_fetch_sub(&q->users, 1U);
}

static bool q_quiesce(mqtt_link_queues_t *q, uint32_t timeout_ms)
{
    TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    while (atomic_load(&q->users) != 0U) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= budget) return false;
        vTaskDelay(1);
    }
    return true;
}

bool mqtt_link_queues_deinit(mqtt_link_queues_t *q)
{
    if (q == NULL) return true;
    atomic_store(&q->open, false);
    SemaphoreHandle_t bell = q->doorbell;
    if (bell != NULL) (void)xSemaphoreGive(bell);
    if (!q_quiesce(q, MQTT_LINK_QUIESCE_MS)) return false;
    queue_delete(&q->rx);
    queue_delete(&q->pub);
    for (unsigned i = 0U; i < MQTT_LINK_WEIGHT_SLOTS; i++) queue_delete(&q->weight[i]);
    q->doorbell = NULL;
    if (bell != NULL) vSemaphoreDelete(bell);
    return true;
}

bool mqtt_link_queues_init(mqtt_link_queues_t *q, size_t rx_depth, size_t pub_depth)
{
    if (q == NULL || rx_depth == 0U || pub_depth == 0U) return false;
    atomic_store(&q->open, false);
    (void)q_quiesce(q, MQTT_LINK_QUIESCE_MS);
    atomic_store(&q->users, 0U);
    q->rx = NULL;
    q->pub = NULL;
    q->doorbell = NULL;
    for (unsigned i = 0U; i < MQTT_LINK_WEIGHT_SLOTS; i++) q->weight[i] = NULL;
    atomic_store(&q->rx_dropped_full, 0U);
    atomic_store(&q->pub_dropped_full, 0U);
    atomic_store(&q->weight_overwritten, 0U);
    atomic_store(&q->weight_dropped_stale, 0U);
    atomic_store(&q->weight_dropped_future, 0U);
    atomic_store(&q->dropped_other, 0U);

    q->rx = xQueueCreate((UBaseType_t)rx_depth, sizeof(mqtt_item_t));
    if (q->rx == NULL) goto cleanup;
    q->pub = xQueueCreate((UBaseType_t)pub_depth, sizeof(mqtt_item_t));
    if (q->pub == NULL) goto cleanup;
    for (unsigned i = 0U; i < MQTT_LINK_WEIGHT_SLOTS; i++) {
        q->weight[i] = xQueueCreate(1U, sizeof(mqtt_item_t));
        if (q->weight[i] == NULL) goto cleanup;
    }
    q->doorbell = xSemaphoreCreateBinaryStatic(&q->doorbell_buf);
    if (q->doorbell == NULL) goto cleanup;
    atomic_store(&q->open, true);
    return true;

cleanup:
    (void)mqtt_link_queues_deinit(q);
    return false;
}

static void ring(mqtt_link_queues_t *q)
{
    SemaphoreHandle_t bell = q->doorbell;
    if (bell != NULL) (void)xSemaphoreGive(bell);
}

static mqtt_enq_result_t push_item(mqtt_link_queues_t *q, QueueHandle_t h, _Atomic uint32_t *full,
                                   const mqtt_item_t *item)
{
    if (xQueueSend(h, item, 0) != pdTRUE) {
        atomic_fetch_add(full, 1U);
        return MQTT_ENQ_FULL;
    }
    ring(q);
    return MQTT_ENQ_OK;
}

static mqtt_enq_result_t push_rx_open(mqtt_link_queues_t *q, const char *topic, size_t topic_len,
                                      const char *data, size_t data_len, size_t total_len, bool retain,
                                      uint32_t now_ms)
{
    if (retain) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return MQTT_ENQ_RETAINED;
    }
    if (data_len != total_len) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return MQTT_ENQ_FRAGMENT;
    }
    if (topic_len >= MQTT_LINK_TOPIC_MAX || data_len > MQTT_LINK_PAYLOAD_MAX) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return MQTT_ENQ_TOO_BIG;
    }
    mqtt_item_t item;
    item.kind = MQTT_ITEM_RX;
    item.qos = 0U;
    item.retain = false;
    item.from_weight = false;
    item.len = (uint16_t)data_len;
    item.recv_ms = now_ms;
    memcpy(item.topic, topic, topic_len);
    item.topic[topic_len] = '\0';
    memcpy(item.data, data, data_len);
    item.data[data_len] = '\0';
    return push_item(q, q->rx, &q->rx_dropped_full, &item);
}

mqtt_enq_result_t mqtt_link_queue_push_rx(mqtt_link_queues_t *q,
                                          const char *topic, size_t topic_len,
                                          const char *data, size_t data_len,
                                          size_t total_len, bool retain,
                                          uint32_t now_ms)
{
    if (topic == NULL || data == NULL || topic_len == 0U || !q_enter(q)) return MQTT_ENQ_INVALID;
    mqtt_enq_result_t r = push_rx_open(q, topic, topic_len, data, data_len, total_len, retain, now_ms);
    q_leave(q);
    return r;
}

static mqtt_enq_result_t build_pub(mqtt_link_queues_t *q, mqtt_item_t *item, const char *topic,
                                   const char *data, int qos, bool retain, uint32_t stamp_ms)
{
    size_t tl = strlen(topic);
    size_t dl = strlen(data);
    if (tl == 0U || tl >= MQTT_LINK_TOPIC_MAX || dl > MQTT_LINK_PAYLOAD_MAX) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return MQTT_ENQ_TOO_BIG;
    }
    item->kind = MQTT_ITEM_PUB;
    item->qos = (uint8_t)qos;
    item->retain = retain;
    item->from_weight = false;
    item->len = (uint16_t)dl;
    item->recv_ms = stamp_ms;
    memcpy(item->topic, topic, tl + 1U);
    memcpy(item->data, data, dl + 1U);
    return MQTT_ENQ_OK;
}

mqtt_enq_result_t mqtt_link_queue_push_pub(mqtt_link_queues_t *q,
                                           const char *topic, const char *data,
                                           int qos, bool retain)
{
    if (topic == NULL || data == NULL || qos < 0 || qos > 2 || !q_enter(q)) return MQTT_ENQ_INVALID;
    mqtt_item_t item;
    mqtt_enq_result_t r = build_pub(q, &item, topic, data, qos, retain, 0U);
    if (r == MQTT_ENQ_OK) r = push_item(q, q->pub, &q->pub_dropped_full, &item);
    q_leave(q);
    return r;
}

static mqtt_enq_result_t put_weight_item(mqtt_link_queues_t *q, unsigned slot, const mqtt_item_t *item)
{
    QueueHandle_t h = q->weight[slot];
    if (uxQueueMessagesWaiting(h) != 0U) atomic_fetch_add(&q->weight_overwritten, 1U);
    (void)xQueueOverwrite(h, item);
    ring(q);
    return MQTT_ENQ_OK;
}

mqtt_enq_result_t mqtt_link_queue_put_weight(mqtt_link_queues_t *q, unsigned slot,
                                             const char *topic, const char *data,
                                             uint32_t now_ms)
{
    if (slot >= MQTT_LINK_WEIGHT_SLOTS || topic == NULL || data == NULL || !q_enter(q)) {
        return MQTT_ENQ_INVALID;
    }
    mqtt_item_t item;
    mqtt_enq_result_t r = build_pub(q, &item, topic, data, 0, false, now_ms);
    if (r == MQTT_ENQ_OK) r = put_weight_item(q, slot, &item);
    q_leave(q);
    return r;
}

static mqtt_enq_result_t build_weight_rec(mqtt_link_queues_t *q, mqtt_item_t *item, const char *topic,
                                          const char *boot_id, uint32_t seq,
                                          const mqtt_link_weight_t *w, bool with_message_id,
                                          uint32_t now_ms)
{
    mqtt_link_weight_t probe = *w;
    probe.age_ms = MQTT_LINK_WEIGHT_MAX_AGE_MS;
    size_t tl = strlen(topic);
    if (strnlen(w->source, MQTT_LINK_SOURCE_MAX + 1U) > MQTT_LINK_SOURCE_MAX ||
        mqtt_link_weight_json(item->data, sizeof(item->data), boot_id, seq, &probe, with_message_id) == 0U) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return MQTT_ENQ_INVALID;
    }
    if (tl == 0U || tl >= MQTT_LINK_TOPIC_MAX) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return MQTT_ENQ_TOO_BIG;
    }
    weight_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.sample_ms = now_ms - w->age_ms;
    rec.uptime_ms = w->uptime_ms;
    rec.seq = seq;
    rec.weight_g = w->weight_g;
    rec.cas_seq = w->cas_seq;
    rec.src_uart = w->src_uart;
    rec.stable = w->stable;
    rec.with_message_id = with_message_id;
    (void)strlcpy(rec.boot_id, boot_id, sizeof(rec.boot_id));
    (void)strlcpy(rec.scale_id, w->scale_id, sizeof(rec.scale_id));
    (void)strlcpy(rec.source, w->source, sizeof(rec.source));
    item->kind = MQTT_ITEM_WEIGHT;
    item->qos = 0U;
    item->retain = false;
    item->from_weight = true;
    item->len = (uint16_t)sizeof(rec);
    item->recv_ms = now_ms;
    memcpy(item->topic, topic, tl + 1U);
    memcpy(item->data, &rec, sizeof(rec));
    return MQTT_ENQ_OK;
}

mqtt_enq_result_t mqtt_link_queue_put_weight_sample(mqtt_link_queues_t *q, unsigned slot,
                                                    const char *topic, const char *boot_id,
                                                    uint32_t seq, const mqtt_link_weight_t *w,
                                                    bool with_message_id, uint32_t now_ms)
{
    if (slot >= MQTT_LINK_WEIGHT_SLOTS || topic == NULL || boot_id == NULL || w == NULL ||
        w->source == NULL || w->scale_id == NULL || !q_enter(q)) {
        return MQTT_ENQ_INVALID;
    }
    mqtt_item_t item;
    mqtt_enq_result_t r = build_weight_rec(q, &item, topic, boot_id, seq, w, with_message_id, now_ms);
    if (r == MQTT_ENQ_OK) r = put_weight_item(q, slot, &item);
    q_leave(q);
    return r;
}

static wage_t weight_age(uint32_t stamp_ms, uint32_t now_ms, uint32_t limit_ms)
{
    if ((uint32_t)(now_ms - stamp_ms) <= limit_ms) return WAGE_FRESH;
    if ((uint32_t)(stamp_ms - now_ms) <= MQTT_LINK_WEIGHT_FUTURE_TOL_MS) return WAGE_FRESH;
    return ((int32_t)(now_ms - stamp_ms) < 0) ? WAGE_FUTURE : WAGE_STALE;
}

static void count_age_drop(mqtt_link_queues_t *q, wage_t a)
{
    atomic_fetch_add(a == WAGE_FUTURE ? &q->weight_dropped_future : &q->weight_dropped_stale, 1U);
}

static bool render_weight(mqtt_link_queues_t *q, mqtt_item_t *out, uint32_t now_ms)
{
    weight_rec_t rec;
    memcpy(&rec, out->data, sizeof(rec));
    rec.boot_id[MQTT_LINK_BOOT_ID_LEN] = '\0';
    rec.scale_id[MQTT_LINK_SCALE_ID_MAX] = '\0';
    rec.source[MQTT_LINK_SOURCE_MAX] = '\0';
    wage_t a = weight_age(rec.sample_ms, now_ms, MQTT_LINK_WEIGHT_MAX_AGE_MS);
    if (a != WAGE_FRESH) {
        count_age_drop(q, a);
        return false;
    }
    uint32_t age = now_ms - rec.sample_ms;
    if (age > MQTT_LINK_WEIGHT_MAX_AGE_MS) age = 0U;
    mqtt_link_weight_t w = {
        .uptime_ms = rec.uptime_ms,
        .scale_id = rec.scale_id,
        .src_uart = rec.src_uart,
        .weight_g = rec.weight_g,
        .stable = rec.stable,
        .age_ms = age,
        .cas_seq = rec.cas_seq,
        .source = rec.source,
    };
    size_t n = mqtt_link_weight_json(out->data, sizeof(out->data), rec.boot_id, rec.seq, &w,
                                     rec.with_message_id);
    if (n == 0U) {
        atomic_fetch_add(&q->dropped_other, 1U);
        return false;
    }
    out->kind = MQTT_ITEM_PUB;
    out->len = (uint16_t)n;
    return true;
}

static bool take_ready(mqtt_link_queues_t *q, mqtt_item_t *out, uint32_t now_ms, uint32_t stale_ms)
{
    if (xQueueReceive(q->rx, out, 0) == pdTRUE || xQueueReceive(q->pub, out, 0) == pdTRUE) {
        out->from_weight = false;
        return true;
    }
    for (unsigned i = 0U; i < MQTT_LINK_WEIGHT_SLOTS; i++) {
        if (xQueueReceive(q->weight[i], out, 0) != pdTRUE) continue;
        out->from_weight = true;
        wage_t a = weight_age(out->recv_ms, now_ms, stale_ms);
        if (a != WAGE_FRESH) {
            count_age_drop(q, a);
            continue;
        }
        if (out->kind != MQTT_ITEM_WEIGHT || render_weight(q, out, now_ms)) return true;
    }
    return false;
}

bool mqtt_link_queue_next(mqtt_link_queues_t *q, mqtt_item_t *out, uint32_t timeout_ms,
                          uint32_t now_ms, uint32_t stale_ms)
{
    if (out == NULL || !q_enter(q)) return false;
    TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    uint32_t now = now_ms;
    bool got = false;
    while (atomic_load(&q->open)) {
        if (take_ready(q, out, now, stale_ms)) {
            got = true;
            break;
        }
        TickType_t used = xTaskGetTickCount() - start;
        if (used >= budget) break;
        (void)xSemaphoreTake(q->doorbell, budget - used);
        now = now_ms + (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - start);
    }
    q_leave(q);
    return got;
}

size_t mqtt_link_queue_pending(mqtt_link_queues_t *q)
{
    if (!q_enter(q)) return 0U;
    size_t n = (size_t)uxQueueMessagesWaiting(q->rx) + (size_t)uxQueueMessagesWaiting(q->pub);
    for (unsigned i = 0U; i < MQTT_LINK_WEIGHT_SLOTS; i++) {
        n += (size_t)uxQueueMessagesWaiting(q->weight[i]);
    }
    q_leave(q);
    return n;
}

uint16_t mqtt_link_sat16(uint32_t v)
{
    return v > UINT16_MAX ? (uint16_t)UINT16_MAX : (uint16_t)v;
}

void mqtt_link_queue_stats(mqtt_link_queues_t *q, mqtt_link_stats_t *out)
{
    if (q == NULL || out == NULL) return;
    out->queue_depth = (uint32_t)mqtt_link_queue_pending(q);
    out->rx_dropped_full = atomic_load(&q->rx_dropped_full);
    out->pub_dropped_full = atomic_load(&q->pub_dropped_full);
    out->dropped_full = out->rx_dropped_full + out->pub_dropped_full;
    out->dropped_other = atomic_load(&q->dropped_other);
    out->weight_overwritten = atomic_load(&q->weight_overwritten);
    out->weight_dropped_stale = atomic_load(&q->weight_dropped_stale);
    out->weight_dropped_future = atomic_load(&q->weight_dropped_future);
}

size_t mqtt_link_status_json(char *out, size_t cap, const mqtt_link_status_t *st,
                             const mqtt_link_stats_t *m)
{
    if (out == NULL || cap == 0U || st == NULL || m == NULL || !json_plain(st->role) ||
        !json_plain(st->boot_id) || !json_plain(st->firmware) || !json_plain(st->cas_link)) {
        return 0U;
    }
    int n = snprintf(out, cap,
                     "{\"role\":\"%s\",\"boot_id\":\"%s\",\"firmware\":\"%s\",\"cas_link\":\"%s\","
                     "\"cas_seq\":%lu,\"cas_age_ms\":%lu,\"ws_clients\":%d,\"uptime_ms\":%lu,"
                     "\"mqtt\":{\"pub\":%lu,\"pub_fail\":%lu,\"qdepth\":%lu,\"rx_full\":%lu,"
                     "\"pub_full\":%lu,\"w_pub\":%lu,\"w_over\":%lu,\"w_stale\":%lu,\"w_future\":%lu,"
                     "\"other\":%lu,\"offline\":%lu,\"heap_min_kb\":%u,\"pub_stuck\":%u,"
                     "\"deferred\":%u}}",
                     st->role, st->boot_id, st->firmware, st->cas_link, (unsigned long)st->cas_seq,
                     (unsigned long)st->cas_age_ms, st->ws_client ? 1 : 0,
                     (unsigned long)st->uptime_ms, (unsigned long)m->published,
                     (unsigned long)m->publish_failed, (unsigned long)m->queue_depth,
                     (unsigned long)m->rx_dropped_full, (unsigned long)m->pub_dropped_full,
                     (unsigned long)m->weight_published, (unsigned long)m->weight_overwritten,
                     (unsigned long)m->weight_dropped_stale, (unsigned long)m->weight_dropped_future,
                     (unsigned long)m->dropped_other, (unsigned long)m->dropped_offline,
                     (unsigned)m->heap_min_kb, (unsigned)m->stop_pub_stuck,
                     (unsigned)m->stop_deferred);
    if (n <= 0 || (size_t)n >= cap || (size_t)n > MQTT_LINK_PAYLOAD_MAX) return 0U;
    return (size_t)n;
}

static bool take_any(mqtt_link_queues_t *q, mqtt_item_t *out)
{
    if (xQueueReceive(q->rx, out, 0) == pdTRUE || xQueueReceive(q->pub, out, 0) == pdTRUE) {
        out->from_weight = false;
        return true;
    }
    for (unsigned i = 0U; i < MQTT_LINK_WEIGHT_SLOTS; i++) {
        if (xQueueReceive(q->weight[i], out, 0) == pdTRUE) {
            out->from_weight = true;
            return true;
        }
    }
    return false;
}

size_t mqtt_link_queue_flush(mqtt_link_queues_t *q, mqtt_item_t *scratch, size_t max_items,
                             mqtt_link_flush_cb cb, void *ctx)
{
    if (scratch == NULL || cb == NULL || !q_enter(q)) return 0U;
    size_t n = 0U;
    while (n < max_items && take_any(q, scratch)) {
        cb(scratch, ctx);
        n++;
    }
    q_leave(q);
    return n;
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

void mqtt_link_handoff_init(mqtt_link_handoff_t *h)
{
    if (h != NULL) atomic_store(&h->state, MQTT_HANDOFF_RUN);
}

bool mqtt_link_handoff_task_exit(mqtt_link_handoff_t *h)
{
    if (h == NULL) return false;
    int expected = MQTT_HANDOFF_RUN;
    return atomic_compare_exchange_strong(&h->state, &expected, MQTT_HANDOFF_DONE);
}

bool mqtt_link_handoff_wait(mqtt_link_handoff_t *h, uint32_t timeout_ms)
{
    if (h == NULL) return false;
    TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    TickType_t start = xTaskGetTickCount();
    while (atomic_load(&h->state) == MQTT_HANDOFF_RUN &&
           (TickType_t)(xTaskGetTickCount() - start) < budget) {
        vTaskDelay(1);
    }
    int expected = MQTT_HANDOFF_RUN;
    if (atomic_compare_exchange_strong(&h->state, &expected, MQTT_HANDOFF_ORPHAN)) return false;
    return expected == MQTT_HANDOFF_DONE;
}
