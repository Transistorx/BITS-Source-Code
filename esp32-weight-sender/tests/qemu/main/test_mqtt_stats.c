#include "test_mqtt_stats.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_link_core.h"
#include "test_harness.h"

#define ST_DEV "bits-a4cf12ab34cd"
#define ST_BOOT "a1b2c3d4"
#define ST_TEL "cas/" ST_DEV "/telemetry/weight"
#define ST_CTL "cas/" ST_DEV "/weight/ctl"
#define ST_CMD "cas/" ST_DEV "/commands"
#define ST_STATUS "cas/" ST_DEV "/telemetry/status"
#define ST_FW "weight-sender 7.0-cas-physical"
#define ST_CAS_LONGEST "ONLINE / RECEIVING REAL RS232 DATA"

static const char *TAG = "TEST_STATS";

static unsigned s_flush_weight;
static unsigned s_flush_other;

static void st_init(mqtt_link_queues_t *q)
{
    memset(q, 0, sizeof(*q));
    (void)mqtt_link_queues_init(q, MQTT_LINK_RX_DEPTH, MQTT_LINK_PUB_DEPTH);
}

static mqtt_link_weight_t st_w(uint32_t uptime, uint32_t age)
{
    mqtt_link_weight_t w = {
        .uptime_ms = uptime, .scale_id = "SCALE1", .src_uart = 1U, .weight_g = 2500, .stable = true,
        .age_ms = age, .cas_seq = 77U, .source = "CAS_RS485",
    };
    return w;
}

static mqtt_enq_result_t st_rx(mqtt_link_queues_t *q)
{
    static const char cmd[] = "{\"command_id\":1}";
    return mqtt_link_queue_push_rx(q, ST_CMD, strlen(ST_CMD), cmd, strlen(cmd), strlen(cmd), false,
                                   1000U);
}

static mqtt_enq_result_t st_sample(mqtt_link_queues_t *q, unsigned slot, uint32_t seq,
                                   const mqtt_link_weight_t *w, uint32_t now)
{
    bool tel = slot == MQTT_LINK_WEIGHT_SLOT_TEL;
    return mqtt_link_queue_put_weight_sample(q, slot, tel ? ST_TEL : ST_CTL, ST_BOOT, seq, w, tel, now);
}

static void test_counters_snapshot(void)
{
    mqtt_link_queues_t q;
    mqtt_item_t it;
    st_init(&q);
    mqtt_link_stats_t m;
    memset(&m, 0, sizeof(m));
    for (unsigned i = 0U; i < MQTT_LINK_RX_DEPTH + 1U; i++) (void)st_rx(&q);
    for (unsigned i = 0U; i < MQTT_LINK_PUB_DEPTH + 1U; i++) {
        (void)mqtt_link_queue_push_pub(&q, ST_STATUS, "{\"s\":1}", 0, false);
    }
    mqtt_link_weight_t w = st_w(1000U, 0U);
    (void)st_sample(&q, MQTT_LINK_WEIGHT_SLOT_TEL, 1U, &w, 1000U);
    (void)st_sample(&q, MQTT_LINK_WEIGHT_SLOT_TEL, 2U, &w, 1000U);
    (void)mqtt_link_queue_push_rx(&q, ST_CMD, strlen(ST_CMD), "{}", 2U, 2U, true, 1000U);

    mqtt_link_queue_stats(&q, &m);
    test_check(m.queue_depth == MQTT_LINK_RX_DEPTH + MQTT_LINK_PUB_DEPTH + 1U,
               "REQ-WMQ-05_stats_queue_depth_counts_rx_pub_and_weight_slots");
    test_check(m.rx_dropped_full == 1U && m.pub_dropped_full == 1U && m.dropped_full == 2U,
               "REQ-WMQ-05_stats_rx_and_pub_dropped_full_reported_separately_and_summed");
    test_check(m.weight_overwritten == 1U, "REQ-WMQ-05_stats_weight_overwritten_reported");
    test_check(m.dropped_other == 1U, "REQ-WMQ-05_stats_dropped_other_reported");

    for (unsigned i = 0U; i < MQTT_LINK_RX_DEPTH + MQTT_LINK_PUB_DEPTH; i++) {
        (void)mqtt_link_queue_next(&q, &it, 0U, 1000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    }
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 1000U + MQTT_LINK_WEIGHT_HB_PERIOD_MS + 1U,
                                     MQTT_LINK_WEIGHT_HB_PERIOD_MS),
               "REQ-WMQ-04_stats_stale_weight_not_delivered");
    (void)st_sample(&q, MQTT_LINK_WEIGHT_SLOT_CTL, 3U, &w, 5000U);
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 4000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS),
               "REQ-WMQ-04_stats_future_weight_not_delivered");
    mqtt_link_queue_stats(&q, &m);
    test_check(m.weight_dropped_stale == 1U && m.weight_dropped_future == 1U && m.queue_depth == 0U,
               "REQ-WMQ-05_stats_weight_dropped_stale_and_future_reported");
    test_check(m.rx_dropped_full == 1U && m.pub_dropped_full == 1U && m.weight_overwritten == 1U,
               "REQ-WMQ-05_stats_counters_survive_drain");

    for (unsigned i = 0U; i < MQTT_LINK_PUB_DEPTH; i++) {
        (void)mqtt_link_queue_push_pub(&q, ST_STATUS, "{\"s\":1}", 0, false);
    }
    test_check(st_rx(&q) == MQTT_ENQ_OK, "REQ-WMQ-03_stats_rx_accepted_while_pub_full");

    mqtt_link_stats_t h;
    memset(&h, 0, sizeof(h));
    h.published = 77U;
    h.publish_failed = 3U;
    h.weight_published = 5U;
    h.dropped_offline = 9U;
    h.heap_min_kb = 120U;
    h.stop_pub_stuck = 2U;
    h.stop_deferred = 1U;
    h.pub_stuck = true;
    h.teardown_pending = true;
    h.link_state = (uint8_t)MQTT_LC_STOPPING;
    h.connected = true;
    mqtt_link_queue_stats(&q, &h);
    test_check(h.published == 77U && h.publish_failed == 3U && h.weight_published == 5U &&
                   h.dropped_offline == 9U && h.heap_min_kb == 120U && h.stop_pub_stuck == 2U &&
                   h.stop_deferred == 1U && h.pub_stuck && h.teardown_pending &&
                   h.link_state == (uint8_t)MQTT_LC_STOPPING && h.connected,
               "REQ-WMQ-05_stats_queue_snapshot_leaves_link_fields_untouched");

    (void)mqtt_link_queues_deinit(&q);
    mqtt_link_queue_stats(&q, &m);
    test_check(m.queue_depth == 0U && m.rx_dropped_full == 1U && m.weight_dropped_future == 1U,
               "REQ-WMQ-05_stats_readable_after_deinit_depth_zero");
    mqtt_link_queue_stats(NULL, &m);
    mqtt_link_queue_stats(&q, NULL);
    test_check(true, "REQ-WMQ-05_stats_null_args_safe");
}

static void test_from_weight_flag(void)
{
    mqtt_link_queues_t q;
    mqtt_item_t it;
    st_init(&q);
    (void)st_rx(&q);
    (void)mqtt_link_queue_push_pub(&q, ST_STATUS, "{\"s\":1}", 0, false);
    mqtt_link_weight_t w = st_w(2000U, 10U);
    (void)st_sample(&q, MQTT_LINK_WEIGHT_SLOT_CTL, 1U, &w, 2000U);
    (void)mqtt_link_queue_put_weight(&q, MQTT_LINK_WEIGHT_SLOT_TEL, ST_TEL, "{\"raw\":1}", 2000U);

    memset(&it, 0xA5, sizeof(it));
    bool a = mqtt_link_queue_next(&q, &it, 0U, 2000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    test_check(a && it.kind == MQTT_ITEM_RX && !it.from_weight, "REQ-WMQ-05_stats_rx_item_not_weight");
    memset(&it, 0xA5, sizeof(it));
    a = mqtt_link_queue_next(&q, &it, 0U, 2000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    test_check(a && it.kind == MQTT_ITEM_PUB && !it.from_weight, "REQ-WMQ-05_stats_pub_item_not_weight");
    memset(&it, 0, sizeof(it));
    a = mqtt_link_queue_next(&q, &it, 0U, 2000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    test_check(a && it.kind == MQTT_ITEM_PUB && it.from_weight && strcmp(it.topic, ST_CTL) == 0,
               "REQ-WMQ-05_stats_rendered_weight_sample_flagged_weight");
    memset(&it, 0, sizeof(it));
    a = mqtt_link_queue_next(&q, &it, 0U, 2000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    test_check(a && it.kind == MQTT_ITEM_PUB && it.from_weight && strcmp(it.data, "{\"raw\":1}") == 0,
               "REQ-WMQ-05_stats_raw_weight_slot_item_flagged_weight");
    (void)mqtt_link_queues_deinit(&q);
}

static void st_flush_cb(const mqtt_item_t *item, void *ctx)
{
    (void)ctx;
    if (item->from_weight) {
        s_flush_weight++;
    } else {
        s_flush_other++;
    }
}

static void test_flush_flag(void)
{
    mqtt_link_queues_t q;
    mqtt_item_t it;
    st_init(&q);
    (void)st_rx(&q);
    (void)mqtt_link_queue_push_pub(&q, ST_STATUS, "{\"s\":1}", 1, false);
    mqtt_link_weight_t w = st_w(3000U, 0U);
    (void)st_sample(&q, MQTT_LINK_WEIGHT_SLOT_CTL, 1U, &w, 3000U);
    (void)st_sample(&q, MQTT_LINK_WEIGHT_SLOT_TEL, 1U, &w, 3000U);
    s_flush_weight = 0U;
    s_flush_other = 0U;
    memset(&it, 0xA5, sizeof(it));
    size_t n = mqtt_link_queue_flush(&q, &it, 8U, st_flush_cb, NULL);
    test_check(n == 4U && s_flush_weight == 2U && s_flush_other == 2U,
               "REQ-WMQ-05_stats_flush_marks_weight_slot_items");
    (void)mqtt_link_queues_deinit(&q);
}

static void test_sat16(void)
{
    test_check(mqtt_link_sat16(0U) == 0U && mqtt_link_sat16(65535U) == 65535U &&
                   mqtt_link_sat16(65536U) == 65535U && mqtt_link_sat16(UINT32_MAX) == 65535U,
               "REQ-WMQ-05_stats_sat16_clamps");
}

static mqtt_link_status_t st_status(void)
{
    mqtt_link_status_t st = {
        .role = "weight_sender", .boot_id = ST_BOOT, .firmware = ST_FW, .cas_link = "STALE",
        .cas_seq = 42U, .cas_age_ms = 3100U, .ws_client = true, .uptime_ms = 123456U,
    };
    return st;
}

static mqtt_link_stats_t st_stats(void)
{
    mqtt_link_stats_t m;
    memset(&m, 0, sizeof(m));
    m.published = 1U;
    m.publish_failed = 2U;
    m.queue_depth = 3U;
    m.rx_dropped_full = 4U;
    m.pub_dropped_full = 5U;
    m.dropped_full = 9U;
    m.weight_published = 6U;
    m.weight_overwritten = 7U;
    m.weight_dropped_stale = 8U;
    m.weight_dropped_future = 10U;
    m.dropped_other = 11U;
    m.dropped_offline = 12U;
    m.heap_min_kb = 13U;
    m.stop_pub_stuck = 14U;
    m.stop_deferred = 15U;
    return m;
}

static void test_status_json(void)
{
    mqtt_link_queues_t q;
    mqtt_item_t it;
    static const char golden[] =
        "{\"role\":\"weight_sender\",\"boot_id\":\"a1b2c3d4\",\"firmware\":\"" ST_FW "\","
        "\"cas_link\":\"STALE\",\"cas_seq\":42,\"cas_age_ms\":3100,\"ws_clients\":1,\"uptime_ms\":123456,"
        "\"mqtt\":{\"pub\":1,\"pub_fail\":2,\"qdepth\":3,\"rx_full\":4,\"pub_full\":5,\"w_pub\":6,"
        "\"w_over\":7,\"w_stale\":8,\"w_future\":10,\"other\":11,\"offline\":12,\"heap_min_kb\":13,"
        "\"pub_stuck\":14,\"deferred\":15}}";
    char body[MQTT_LINK_PAYLOAD_MAX + 1U];
    mqtt_link_status_t st = st_status();
    mqtt_link_stats_t m = st_stats();
    size_t n = mqtt_link_status_json(body, sizeof(body), &st, &m);
    bool ok = n == strlen(golden) && strcmp(body, golden) == 0;
    test_check(ok, "REQ-WMQ-05_status_json_golden");
    if (!ok) ESP_LOGI(TAG, "got %s", body);

    st.ws_client = false;
    st.cas_age_ms = UINT32_MAX;
    n = mqtt_link_status_json(body, sizeof(body), &st, &m);
    test_check(n > 0U && strstr(body, "\"ws_clients\":0,") != NULL &&
                   strstr(body, "\"cas_age_ms\":4294967295,") != NULL,
               "REQ-WMQ-05_status_json_no_client_and_no_cas_sentinel");

    mqtt_link_status_t worst = {
        .role = "weight_sender", .boot_id = "ffffffff", .firmware = ST_FW, .cas_link = ST_CAS_LONGEST,
        .cas_seq = UINT32_MAX, .cas_age_ms = UINT32_MAX, .ws_client = true, .uptime_ms = UINT32_MAX,
    };
    mqtt_link_stats_t mw;
    memset(&mw, 0xFF, sizeof(mw));
    n = mqtt_link_status_json(body, sizeof(body), &worst, &mw);
    ESP_LOGI(TAG, "status worst case %u of %u bytes", (unsigned)n, (unsigned)MQTT_LINK_PAYLOAD_MAX);
    test_check(n > 0U && n <= MQTT_LINK_PAYLOAD_MAX && strlen(body) == n,
               "REQ-WMQ-05_status_json_worst_case_fits_payload_max");
    st_init(&q);
    test_check(n > 0U && mqtt_link_queue_push_pub(&q, ST_STATUS, body, 0, false) == MQTT_ENQ_OK,
               "REQ-WMQ-05_status_json_worst_case_accepted_by_pub_queue");
    (void)mqtt_link_queues_deinit(&q);

    memset(body, 0x5A, sizeof(body));
    n = mqtt_link_status_json(body, 100U, &st, &m);
    test_check(n == 0U && (unsigned char)body[100] == 0x5AU,
               "REQ-WMQ-05_status_json_small_cap_refused_no_overrun");

    memset(it.data, 'a', 299U);
    it.data[299] = '\0';
    mqtt_link_status_t lf = st_status();
    lf.firmware = it.data;
    n = mqtt_link_status_json(body, sizeof(body), &lf, &m);
    test_check(n == 0U, "REQ-WMQ-05_status_json_over_payload_max_refused");

    mqtt_link_status_t bad = st_status();
    bad.firmware = "fw\"x";
    test_check(mqtt_link_status_json(body, sizeof(body), &bad, &m) == 0U,
               "REQ-WMQ-05_status_json_quote_in_string_refused");
    bad = st_status();
    bad.cas_link = NULL;
    test_check(mqtt_link_status_json(body, sizeof(body), &bad, &m) == 0U &&
                   mqtt_link_status_json(body, sizeof(body), &st, NULL) == 0U &&
                   mqtt_link_status_json(body, sizeof(body), NULL, &m) == 0U &&
                   mqtt_link_status_json(NULL, sizeof(body), &st, &m) == 0U &&
                   mqtt_link_status_json(body, 0U, &st, &m) == 0U,
               "REQ-WMQ-05_status_json_null_and_zero_cap_refused");
}

void test_mqtt_stats_run(void)
{
    test_counters_snapshot();
    test_from_weight_flag();
    test_flush_flag();
    test_sat16();
    test_status_json();
}
