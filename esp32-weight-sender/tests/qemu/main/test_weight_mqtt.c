#include "test_weight_mqtt.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "broker_cfg.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_link_core.h"
#include "scale_cmd.h"
#include "scale_events.h"
#include "task_hb.h"
#include "test_harness.h"
#include "weight_source.h"

#define DEV "bits-a4cf12ab34cd"
#define BOOT "a1b2c3d4"

static void put_str(mqtt_link_queues_t *q, unsigned slot, const char *data, uint32_t now)
{
    (void)mqtt_link_queue_put_weight(q, slot, "cas/" DEV "/telemetry/weight", data, now);
}

static void queues_free(mqtt_link_queues_t *q)
{
    if (q->rx != NULL) vQueueDelete(q->rx);
    if (q->pub != NULL) vQueueDelete(q->pub);
    if (q->weight[0] != NULL) vQueueDelete(q->weight[0]);
    if (q->weight[1] != NULL) vQueueDelete(q->weight[1]);
}

static mqtt_enq_result_t rx_one(mqtt_link_queues_t *q, const char *data)
{
    const char *topic = "cas/" DEV "/commands";
    return mqtt_link_queue_push_rx(q, topic, strlen(topic), data, strlen(data), strlen(data), false, 10U);
}

static void test_queues(void)
{
    static mqtt_link_queues_t q;
    static mqtt_item_t it;
    memset(&q, 0, sizeof(q));
    test_check(mqtt_link_queues_init(&q, 4U, 6U), "REQ-WMQ-03_queues_init_rx4_pub6");

    int64_t t0 = esp_timer_get_time();
    int ok = 0;
    int full = 0;
    for (int i = 0; i < 1000; i++) {
        mqtt_enq_result_t r = mqtt_link_queue_push_pub(&q, "cas/" DEV "/status", "{}", 0, false);
        if (r == MQTT_ENQ_OK) ok++;
        if (r == MQTT_ENQ_FULL) full++;
    }
    int64_t dt = esp_timer_get_time() - t0;
    test_check(ok == 6 && full == 994, "REQ-WMQ-01_pub_full_returns_full_never_blocks");
    test_check(dt < 1000000, "REQ-WMQ-01_1000_enqueues_on_full_queue_under_1ms_each");
    test_check(q.pub_dropped_full == 994U, "REQ-WMQ-01_pub_dropped_full_counts_994");

    test_check(rx_one(&q, "{\"command_id\":1}") == MQTT_ENQ_OK, "REQ-WMQ-03_rx_ok_while_pub_full");
    test_check(q.rx_dropped_full == 0U, "REQ-WMQ-03_rx_not_counted_dropped_when_pub_full");

    test_check(rx_one(&q, "{\"command_id\":2}") == MQTT_ENQ_OK &&
                   rx_one(&q, "{\"command_id\":3}") == MQTT_ENQ_OK &&
                   rx_one(&q, "{\"command_id\":4}") == MQTT_ENQ_OK,
               "REQ-WMQ-03_rx_depth_4_accepts_four");
    test_check(rx_one(&q, "{\"command_id\":5}") == MQTT_ENQ_FULL && q.rx_dropped_full == 1U,
               "REQ-WMQ-03_rx_fifth_full_and_counted");
    test_check(mqtt_link_queue_push_pub(&q, "t", "{}", 0, false) == MQTT_ENQ_FULL,
               "REQ-WMQ-03_rx_full_does_not_free_pub_slot");

    test_check(mqtt_link_queue_next(&q, &it, 0U, 20U, 1000U) && it.kind == MQTT_ITEM_RX &&
                   strstr(it.data, "\"command_id\":1") != NULL,
               "REQ-WMQ-03_next_pops_rx_before_pub");
    for (int i = 0; i < 3; i++) (void)mqtt_link_queue_next(&q, &it, 0U, 20U, 1000U);
    test_check(mqtt_link_queue_next(&q, &it, 0U, 20U, 1000U) && it.kind == MQTT_ITEM_PUB &&
                   strcmp(it.topic, "cas/" DEV "/status") == 0 && it.qos == 0 && !it.retain,
               "REQ-WMQ-03_next_pops_pub_after_rx_empty");
    queues_free(&q);

    memset(&q, 0, sizeof(q));
    (void)mqtt_link_queues_init(&q, 4U, 6U);
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 100U, 1000U), "REQ-WMQ-04_next_empty_returns_false");
    put_str(&q, 1U, "{\"w\":1}", 100U);
    put_str(&q, 1U, "{\"w\":2}", 200U);
    put_str(&q, 1U, "{\"w\":3}", 300U);
    test_check(q.weight_overwritten == 2U, "REQ-WMQ-04_overwrite_counts_replaced_values");
    test_check(mqtt_link_queue_next(&q, &it, 0U, 350U, 1000U) && strcmp(it.data, "{\"w\":3}") == 0 &&
                   it.kind == MQTT_ITEM_PUB && it.recv_ms == 300U,
               "REQ-WMQ-04_latest_value_wins_with_enqueue_stamp");
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 350U, 1000U), "REQ-WMQ-04_mailbox_empty_after_pop");

    put_str(&q, 1U, "{\"w\":4}", 1000U);
    test_check(mqtt_link_queue_next(&q, &it, 0U, 2000U, 1000U) && strcmp(it.data, "{\"w\":4}") == 0,
               "REQ-WMQ-04_age_equal_to_stale_ms_still_delivered");
    put_str(&q, 1U, "{\"w\":5}", 1000U);
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 2001U, 1000U) && q.weight_dropped_stale == 1U,
               "REQ-WMQ-04_age_over_stale_ms_dropped_and_counted");
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 2001U, 1000U) && q.weight_dropped_stale == 1U,
               "REQ-WMQ-04_stale_drop_counted_once");

    put_str(&q, 1U, "{\"w\":6}", 0xFFFFFF00U);
    test_check(mqtt_link_queue_next(&q, &it, 0U, 0x000000FFU, 1000U) && strcmp(it.data, "{\"w\":6}") == 0,
               "REQ-WMQ-04_stale_check_wrap_safe_fresh");
    put_str(&q, 1U, "{\"w\":7}", 0xFFFFFF00U);
    test_check(!mqtt_link_queue_next(&q, &it, 0U, 0x00000800U, 1000U) && q.weight_dropped_stale == 2U,
               "REQ-WMQ-04_stale_check_wrap_safe_stale");

    test_check(mqtt_link_queue_push_pub(&q, "cas/" DEV "/status", "{\"p\":1}", 0, false) == MQTT_ENQ_OK,
               "REQ-WMQ-04_pub_enqueue_ok");
    put_str(&q, 0U, "{\"c\":1}", 5000U);
    put_str(&q, 1U, "{\"t\":1}", 5000U);
    test_check(rx_one(&q, "{\"command_id\":9}") == MQTT_ENQ_OK, "REQ-WMQ-03_rx_ok_with_weight_pending");
    test_check(mqtt_link_queue_next(&q, &it, 0U, 5001U, 1000U) && it.kind == MQTT_ITEM_RX,
               "REQ-WMQ-03_order_1_rx");
    test_check(mqtt_link_queue_next(&q, &it, 0U, 5001U, 1000U) && it.kind == MQTT_ITEM_PUB &&
                   strcmp(it.data, "{\"p\":1}") == 0,
               "REQ-WMQ-03_order_2_pub");
    test_check(mqtt_link_queue_next(&q, &it, 0U, 5001U, 1000U) && strcmp(it.data, "{\"c\":1}") == 0,
               "REQ-WMQ-03_order_3_weight_ctl_slot0");
    test_check(mqtt_link_queue_next(&q, &it, 0U, 5001U, 1000U) && strcmp(it.data, "{\"t\":1}") == 0,
               "REQ-WMQ-03_order_4_weight_tel_slot1");

    static char big[MQTT_LINK_PAYLOAD_MAX + 64U];
    memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    test_check(mqtt_link_queue_put_weight(&q, 1U, "t", big, 1U) == MQTT_ENQ_TOO_BIG,
               "REQ-WMQ-01_put_weight_oversize_too_big");

    uint32_t before = q.weight_overwritten;
    for (int i = 0; i < 1000; i++) put_str(&q, 1U, "{\"w\":8}", 6000U);
    test_check(q.weight_overwritten - before >= 999U, "REQ-WMQ-01_1000_weight_puts_never_block_and_overwrite");
    queues_free(&q);
}

static mqtt_link_weight_t sample_w(uint8_t uart, uint32_t uptime, uint32_t age)
{
    mqtt_link_weight_t w = {
        .uptime_ms = uptime, .scale_id = "SCALE1", .src_uart = uart, .weight_g = 2500, .stable = true,
        .age_ms = age, .cas_seq = 77U, .source = "CAS_RS485",
    };
    return w;
}

static void test_heartbeat(QueueHandle_t manager)
{
    test_check(MQTT_LINK_WEIGHT_HB_PERIOD_MS == 1000U, "REQ-WMQ-06_hb_period_is_1000ms");

    mqtt_link_wgate_t g;
    mqtt_link_wgate_init(&g);
    test_check(mqtt_link_wgate_hb_due(&g, 5000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS), "REQ-WMQ-06_hb_first_call_due");
    test_check(!mqtt_link_wgate_hb_due(&g, 5999U, MQTT_LINK_WEIGHT_HB_PERIOD_MS), "REQ-WMQ-06_hb_not_due_at_999ms");
    test_check(mqtt_link_wgate_hb_due(&g, 6000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS), "REQ-WMQ-06_hb_due_at_1000ms");
    test_check(!mqtt_link_wgate_hb_due(&g, 6500U, MQTT_LINK_WEIGHT_HB_PERIOD_MS), "REQ-WMQ-06_hb_timer_reset_after_due");
    test_check(g.last_tel_ms == 6000U, "REQ-WMQ-06_hb_timer_stamp_is_last_publish");
    test_check(mqtt_link_wgate_hb_due(&g, 7000U, MQTT_LINK_WEIGHT_HB_PERIOD_MS), "REQ-WMQ-06_hb_second_period_due");

    mqtt_link_wgate_t gw;
    mqtt_link_wgate_init(&gw);
    (void)mqtt_link_wgate_hb_due(&gw, 0xFFFFFE00U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    test_check(!mqtt_link_wgate_hb_due(&gw, 0x000001E7U, MQTT_LINK_WEIGHT_HB_PERIOD_MS) &&
                   mqtt_link_wgate_hb_due(&gw, 0x000001E8U, MQTT_LINK_WEIGHT_HB_PERIOD_MS),
               "REQ-WMQ-06_hb_wrap_safe");

    mqtt_link_wgate_t gd;
    mqtt_link_wgate_init(&gd);
    test_check(mqtt_link_wgate_new_frame(&gd, 1U, 77U), "REQ-WMQ-06_dedupe_first_frame_new");
    (void)mqtt_link_wgate_hb_due(&gd, 100U, MQTT_LINK_WEIGHT_HB_PERIOD_MS);
    test_check(!mqtt_link_wgate_new_frame(&gd, 1U, 77U), "REQ-WMQ-06_hb_does_not_reopen_dedupe");

    char a[MQTT_LINK_ENV_MAX + 1U];
    char b[MQTT_LINK_ENV_MAX + 1U];
    mqtt_link_weight_t w1 = sample_w(1U, 5000U, 10U);
    mqtt_link_weight_t w2 = sample_w(1U, 6000U, 1010U);
    size_t n1 = mqtt_link_weight_json(a, sizeof(a), BOOT, 1U, &w1, true);
    size_t n2 = mqtt_link_weight_json(b, sizeof(b), BOOT, 2U, &w2, true);
    test_check(n1 > 0U && n2 > 0U && strstr(a, "\"uptime_ms\":5000") && strstr(b, "\"uptime_ms\":6000"),
               "REQ-WMQ-07_heartbeats_differ_in_uptime_ms");
    test_check(strstr(a, "\"age_ms\":10,") && strstr(b, "\"age_ms\":1010,") && strstr(a, "\"cas_seq\":77") &&
                   strstr(b, "\"cas_seq\":77") && strstr(a, "\"weight_g\":2500") && strstr(b, "\"weight_g\":2500"),
               "REQ-WMQ-07_age_grows_by_period_weight_and_cas_seq_unchanged");

    weight_sample_t s;
    weight_source_start();
    weight_source_select_channel(0U);
    scale_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.channel_id = 0U;
    ev.type = SCALE_EVENT_READING;
    ev.reading = (scale_reading_t){
        .valid = true, .has_display = true, .display = 2.5, .display_is_physical = true,
        .has_gross = true, .gross = 2.5, .gross_is_physical = true, .has_unit = true, .has_status = true,
        .stable = true, .timestamp_ms = weight_source_now_ms() - 2800U, .channel_id = 0U,
    };
    strcpy(ev.reading.unit, "kg");
    (void)xQueueSend(manager, &ev, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_REAL && s.age_ms >= 2800U && s.age_ms < 3000U,
               "REQ-WMQ-06_source_real_with_true_age_below_3000ms");
    ev.reading.timestamp_ms = weight_source_now_ms() - 3300U;
    (void)xQueueSend(manager, &ev, 0);
    vTaskDelay(pdMS_TO_TICKS(40));
    test_check(weight_source_get(&s) == WEIGHT_SOURCE_RESULT_NONE, "REQ-WMQ-06_source_none_past_3000ms_no_weight");
    test_check(WEIGHT_CAS_STALE_MS == 3000, "REQ-WMQ-06_stale_threshold_is_3000ms");
}

static void test_payload(void)
{
    char out[MQTT_LINK_PAYLOAD_MAX + 1U];
    mqtt_link_weight_t w1 = sample_w(1U, 123456U, 12U);
    mqtt_link_weight_t w2 = sample_w(2U, 123456U, 12U);
    size_t n = mqtt_link_weight_json(out, sizeof(out), BOOT, 41U, &w1, true);
    test_check(n > 0U && n <= MQTT_LINK_PAYLOAD_MAX && strstr(out, "\"channel\":\"CH1\",\"src_uart\":\"UART1\""),
               "REQ-WMQ-12_channel_ch1_next_to_src_uart");
    n = mqtt_link_weight_json(out, sizeof(out), BOOT, 41U, &w2, true);
    test_check(n > 0U && strstr(out, "\"channel\":\"CH2\",\"src_uart\":\"UART2\""),
               "REQ-WMQ-12_channel_ch2_next_to_src_uart");
    test_check(strstr(out, "\"schema_version\":1,") != NULL, "REQ-WMQ-12_schema_version_stays_1");
    n = mqtt_link_weight_json(out, sizeof(out), BOOT, 41U, &w1, false);
    test_check(n > 0U && strstr(out, "\"channel\":\"CH1\"") && strstr(out, "\"src_uart\":\"UART1\""),
               "REQ-WMQ-12_channel_also_on_ctl_envelope");
    mqtt_link_weight_t bad = sample_w(3U, 1U, 1U);
    test_check(mqtt_link_weight_json(out, sizeof(out), BOOT, 1U, &bad, true) == 0U,
               "REQ-WMQ-12_invalid_src_uart_rejected");
    mqtt_link_weight_t worst = sample_w(2U, 4294967295U, 4294967295U);
    worst.weight_g = -2147483647 - 1;
    worst.cas_seq = 4294967295U;
    n = mqtt_link_weight_json(out, sizeof(out), BOOT, 4294967295U, &worst, true);
    test_check(n > 0U && n <= 512U, "REQ-WMQ-12_worst_case_with_channel_within_512_bytes");
    test_check(strstr(out, "\"message_id\":\"" BOOT "-4294967295\"") != NULL,
               "REQ-WMQ-12_message_id_boot_seq_kept");
}

static uint32_t crc32_ref(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
    }
    return ~c;
}

static void reseal(broker_cfg_rec_t *r)
{
    r->crc32 = crc32_ref((const uint8_t *)r, offsetof(broker_cfg_rec_t, crc32));
}

static bool is_fallback(broker_cfg_src_t s)
{
    return s != BROKER_CFG_SRC_NVS && s != BROKER_CFG_SRC_KCONFIG;
}

static void test_broker_cfg(void)
{
    test_check(BROKER_CFG_VERSION == 1U && BROKER_CFG_URI_MAX == 95U && BROKER_CFG_USER_MAX == 31U &&
                   BROKER_CFG_PASS_MAX == 63U,
               "REQ-WMQ-22_limits_match_spec");

    const char *good[] = {"mqtt://broker.local", "mqtts://a.b-c.example.com:8883", "mqtt://10.0.0.5:1883",
                          "mqtt://h:1", "mqtt://h:65535", "mqtts://BROKER-1.lan"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "REQ-WMQ-22_uri_good_%u", (unsigned)i);
        test_check(broker_cfg_uri_valid(good[i]), nm);
    }
    const char *bad[] = {"", "http://x.com", "ws://x.com", "mqtt://", "mqtt://:1883", "mqtt://u:p@host",
                         "mqtt://u@host", "mqtt://host/path", "mqtt://host/", "mqtt://host:0",
                         "mqtt://host:65536", "mqtt://host:abc", "mqtt://host:", "mqtt://ho st",
                         "mqtt://ho_st", "mqtt://host?x=1", "mqtt:/host", "host:1883", "mqtt//host",
                         "mqtt://host:-1", "mqtt://host:99999999999"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char nm[64];
        snprintf(nm, sizeof(nm), "REQ-WMQ-22_uri_bad_%u", (unsigned)i);
        test_check(!broker_cfg_uri_valid(bad[i]), nm);
    }
    test_check(!broker_cfg_uri_valid(NULL), "REQ-WMQ-22_uri_null_rejected");

    char longuri[260];
    memcpy(longuri, "mqtt://", 7U);
    memset(longuri + 7, 'a', 200U);
    longuri[207] = '\0';
    test_check(!broker_cfg_uri_valid(longuri), "REQ-WMQ-22_uri_200_char_host_rejected");
    memset(longuri + 7, 'a', 64U);
    longuri[71] = '\0';
    test_check(broker_cfg_uri_valid(longuri), "REQ-WMQ-22_uri_64_char_host_accepted");
    memset(longuri + 7, 'a', 65U);
    longuri[72] = '\0';
    test_check(!broker_cfg_uri_valid(longuri), "REQ-WMQ-22_uri_65_char_host_rejected");

    broker_cfg_rec_t r;
    broker_cfg_rec_t out;
    memset(&r, 0xA5, sizeof(r));
    test_check(broker_cfg_encode(&r, "mqtts://broker.lan:8883", "scale", "s3cret"),
               "REQ-WMQ-21_encode_valid_ok");
    test_check(r.magic == 0x42524B31U && r.version == BROKER_CFG_VERSION,
               "REQ-WMQ-23_encode_sets_magic_and_version");
    test_check(r.crc32 == crc32_ref((const uint8_t *)&r, offsetof(broker_cfg_rec_t, crc32)),
               "REQ-WMQ-23_encode_crc32_over_preceding_bytes");
    memset(&out, 0, sizeof(out));
    test_check(broker_cfg_decode(&r, sizeof(r), &out) == BROKER_CFG_SRC_NVS &&
                   strcmp(out.uri, "mqtts://broker.lan:8883") == 0 && strcmp(out.user, "scale") == 0 &&
                   strcmp(out.pass, "s3cret") == 0,
               "REQ-WMQ-21_decode_present_roundtrip_is_nvs");

    broker_cfg_rec_t e;
    test_check(broker_cfg_encode(&e, "", "", ""), "REQ-WMQ-21_encode_empty_uri_allowed");
    test_check(broker_cfg_decode(&e, sizeof(e), &out) == BROKER_CFG_SRC_KCONFIG,
               "REQ-WMQ-21_empty_uri_in_valid_record_means_kconfig");

    broker_cfg_rec_t t;
    test_check(!broker_cfg_encode(&t, "http://x.com", "u", "p"), "REQ-WMQ-22_encode_rejects_invalid_uri");
    test_check(!broker_cfg_encode(&t, "mqtt://u:p@host", "u", "p"), "REQ-WMQ-22_encode_rejects_userinfo");
    char u32[40];
    char p64[80];
    memset(u32, 'u', 32U);
    u32[32] = '\0';
    memset(p64, 'p', 64U);
    p64[64] = '\0';
    test_check(!broker_cfg_encode(&t, "mqtt://h", u32, "p"), "REQ-WMQ-22_encode_rejects_user_32_chars");
    test_check(!broker_cfg_encode(&t, "mqtt://h", "u", p64), "REQ-WMQ-22_encode_rejects_pass_64_chars");
    u32[31] = '\0';
    p64[63] = '\0';
    test_check(broker_cfg_encode(&t, "mqtt://h", u32, p64) &&
                   broker_cfg_decode(&t, sizeof(t), &out) == BROKER_CFG_SRC_NVS && strlen(out.user) == 31U &&
                   strlen(out.pass) == 63U,
               "REQ-WMQ-22_encode_accepts_max_length_user_and_pass");
    test_check(!broker_cfg_encode(NULL, "mqtt://h", "u", "p") && !broker_cfg_encode(&t, NULL, "u", "p"),
               "REQ-WMQ-22_encode_null_args_rejected");

    test_check(broker_cfg_decode(NULL, 0U, &out) == BROKER_CFG_SRC_FALLBACK_ABSENT,
               "REQ-WMQ-21_decode_absent_null");
    test_check(broker_cfg_decode(&r, 0U, &out) == BROKER_CFG_SRC_FALLBACK_ABSENT,
               "REQ-WMQ-21_decode_absent_len_zero");

    int torn_ok = 1;
    for (size_t len = 1U; len < sizeof(r); len++) {
        if (!is_fallback(broker_cfg_decode(&r, len, &out))) torn_ok = 0;
    }
    test_check(torn_ok == 1, "REQ-WMQ-23_every_truncated_blob_falls_back");

    int flip_ok = 1;
    uint8_t blob[sizeof(r)];
    for (size_t i = 0; i < sizeof(r); i++) {
        memcpy(blob, &r, sizeof(r));
        blob[i] ^= 0x01U;
        if (!is_fallback(broker_cfg_decode(blob, sizeof(blob), &out))) flip_ok = 0;
    }
    test_check(flip_ok == 1, "REQ-WMQ-23_every_single_bit_flip_falls_back");

    memcpy(blob, &r, sizeof(r));
    blob[offsetof(broker_cfg_rec_t, uri) + 3U] ^= 0x20U;
    test_check(broker_cfg_decode(blob, sizeof(blob), &out) == BROKER_CFG_SRC_FALLBACK_CRC,
               "REQ-WMQ-23_corrupt_payload_reports_fallback_crc");

    broker_cfg_rec_t v = r;
    v.version = 2U;
    reseal(&v);
    test_check(broker_cfg_decode(&v, sizeof(v), &out) == BROKER_CFG_SRC_FALLBACK_VERSION,
               "REQ-WMQ-23_version_2_reports_fallback_version");
    v = r;
    v.version = 0U;
    reseal(&v);
    test_check(broker_cfg_decode(&v, sizeof(v), &out) == BROKER_CFG_SRC_FALLBACK_VERSION,
               "REQ-WMQ-23_version_0_reports_fallback_version");

    v = r;
    v.magic = 0x12345678U;
    reseal(&v);
    test_check(is_fallback(broker_cfg_decode(&v, sizeof(v), &out)), "REQ-WMQ-23_wrong_magic_falls_back");

    v = r;
    memset(v.uri, 0, sizeof(v.uri));
    strcpy(v.uri, "http://evil.example");
    reseal(&v);
    test_check(broker_cfg_decode(&v, sizeof(v), &out) == BROKER_CFG_SRC_FALLBACK_INVALID,
               "REQ-WMQ-22_valid_crc_invalid_uri_reports_fallback_invalid");
    v = r;
    memset(v.uri, 'a', sizeof(v.uri));
    reseal(&v);
    test_check(broker_cfg_decode(&v, sizeof(v), &out) == BROKER_CFG_SRC_FALLBACK_INVALID,
               "REQ-WMQ-22_unterminated_uri_reports_fallback_invalid");
    v = r;
    memset(v.pass, 'p', sizeof(v.pass));
    reseal(&v);
    test_check(is_fallback(broker_cfg_decode(&v, sizeof(v), &out)), "REQ-WMQ-22_unterminated_pass_falls_back");

    test_check(BROKER_CFG_SRC_KCONFIG == 0, "REQ-WMQ-21_src_kconfig_is_zero");
}

static uint32_t g_now;

static bool m_link(const char **n)
{
    *n = "ONLINE";
    return true;
}

static uint32_t m_now(void)
{
    return g_now;
}

static bool m_sample(uint8_t ch, uint32_t *seq, int32_t *g, bool *st)
{
    (void)ch;
    *seq = 1U;
    *g = 1000;
    *st = true;
    return true;
}

static void m_delay(uint32_t ms)
{
    g_now += ms;
}

static scale_cmd_t s_cmd;
static bool s_cmd_live;

static void cmd_fresh(uint32_t now)
{
    if (s_cmd_live) {
        vSemaphoreDelete(s_cmd.lock);
        vSemaphoreDelete(s_cmd.wake);
    }
    g_now = now;
    scale_cmd_cfg_t cfg = {
        .now_ms = m_now, .link_online = m_link, .post_sample = m_sample, .send_frame = NULL,
        .delay_ms = m_delay, .encode = NULL, .active_channel = 2U, .settle_timeout_ms = 200U,
    };
    strcpy(cfg.device_id, DEV);
    (void)scale_cmd_init(&s_cmd, &cfg);
    s_cmd_live = true;
}

static scale_submit_t cmd_submit(unsigned id, const char *type, unsigned ttl, scale_cmd_ack_t *ack)
{
    char p[160];
    snprintf(p, sizeof(p), "{\"command_id\":%u,\"type\":\"%s\",\"channel_id\":\"CH2\",\"ttl_ms\":%u}", id, type, ttl);
    memset(ack, 0, sizeof(*ack));
    return scale_cmd_submit(&s_cmd, p, strlen(p), false, g_now, ack);
}

static void test_scale_cmd(void)
{
    scale_cmd_ack_t a;
    test_check(SCALE_CMD_ZT_TTL_MAX_MS == 10000U, "REQ-WMQ-18_ttl_cap_constant_is_10000");

    cmd_fresh(100000U);
    test_check(cmd_submit(1U, "ZERO", 10000U, &a) == SCALE_SUBMIT_QUEUED, "REQ-WMQ-18_ttl_10000_accepted");
    test_check(cmd_submit(2U, "TARE", 10001U, &a) == SCALE_SUBMIT_ACK && strstr(a.reason, "ttl too long") &&
                   a.result == SCALE_RESULT_REJECTED && !a.applied,
               "REQ-WMQ-18_ttl_10001_rejected_ttl_too_long");
    test_check(cmd_submit(3U, "ZERO", 60000U, &a) == SCALE_SUBMIT_ACK && strstr(a.reason, "ttl too long"),
               "REQ-WMQ-18_ttl_60000_rejected_ttl_too_long");
    test_check(cmd_submit(4U, "ZERO", 4294967295U, &a) == SCALE_SUBMIT_ACK && !a.applied,
               "REQ-WMQ-18_ttl_uint32_max_rejected");
    test_check(cmd_submit(5U, "ZERO", 0U, &a) == SCALE_SUBMIT_ACK && !a.applied,
               "REQ-WMQ-18_ttl_zero_still_rejected");

    cmd_fresh(100000U);
    test_check(cmd_submit(10U, "ZERO", 3000U, &a) == SCALE_SUBMIT_QUEUED, "REQ-WMQ-30_first_command_accepted");
    g_now += 1999U;
    test_check(cmd_submit(11U, "ZERO", 3000U, &a) == SCALE_SUBMIT_ACK && strstr(a.reason, "rate limited") &&
                   !a.applied,
               "REQ-WMQ-30_second_within_1999ms_rate_limited");
    g_now += 1U;
    test_check(cmd_submit(12U, "TARE", 3000U, &a) == SCALE_SUBMIT_QUEUED,
               "REQ-WMQ-30_accepted_at_2000ms_rejected_does_not_reset_window");
    g_now += 500U;
    test_check(cmd_submit(13U, "ZERO", 3000U, &a) == SCALE_SUBMIT_ACK && strstr(a.reason, "rate limited"),
               "REQ-WMQ-30_tare_and_zero_share_the_channel_window");
    test_check(s_cmd.last_accept_ms[1] == g_now - 500U, "REQ-WMQ-30_last_accept_ms_tracks_channel_index");
    test_check(cmd_submit(10U, "ZERO", 3000U, &a) != SCALE_SUBMIT_ACK || !strstr(a.reason, "rate limited"),
               "REQ-WMQ-30_duplicate_id_dedupe_precedes_rate_limit");

    cmd_fresh(500U);
    test_check(cmd_submit(20U, "ZERO", 3000U, &a) == SCALE_SUBMIT_QUEUED,
               "REQ-WMQ-30_first_command_after_boot_not_limited_at_small_uptime");

    cmd_fresh(0xFFFFFF00U);
    test_check(cmd_submit(30U, "ZERO", 3000U, &a) == SCALE_SUBMIT_QUEUED, "REQ-WMQ-30_wrap_first_accepted");
    g_now += 1999U;
    test_check(cmd_submit(31U, "ZERO", 3000U, &a) == SCALE_SUBMIT_ACK && strstr(a.reason, "rate limited"),
               "REQ-WMQ-30_wrap_safe_limited");
    g_now += 1U;
    test_check(cmd_submit(32U, "ZERO", 3000U, &a) == SCALE_SUBMIT_QUEUED, "REQ-WMQ-30_wrap_safe_accepted_at_2000");
}

static void test_task_hb(void)
{
    uint32_t mask = 0xFFFFFFFFU;
    test_check(TASK_HB_COUNT == 3, "REQ-WMQ-51_three_heartbeats_defined");
    task_hb_bump(TASK_HB_WEIGHT_TX);
    task_hb_bump(TASK_HB_MQTT_PUB);
    task_hb_bump(TASK_HB_SCALE_CMD);
    test_check(task_hb_check(1000U, 10000U, &mask) == 0U && mask == 0U, "REQ-WMQ-51_baseline_nothing_frozen");

    for (uint32_t t = 2000U; t <= 9000U; t += 1000U) {
        task_hb_bump(TASK_HB_WEIGHT_TX);
        task_hb_bump(TASK_HB_MQTT_PUB);
        task_hb_bump(TASK_HB_SCALE_CMD);
        (void)task_hb_check(t, 10000U, &mask);
    }
    test_check(task_hb_check(9500U, 10000U, &mask) == 0U && mask == 0U, "REQ-WMQ-51_all_alive_none_frozen");

    for (uint32_t t = 10000U; t <= 15000U; t += 1000U) {
        task_hb_bump(TASK_HB_WEIGHT_TX);
        task_hb_bump(TASK_HB_SCALE_CMD);
        (void)task_hb_check(t, 10000U, &mask);
    }
    test_check(task_hb_check(15500U, 10000U, &mask) == 0U && mask == 0U,
               "REQ-WMQ-51_frozen_for_under_threshold_not_reported");

    uint32_t n = 0U;
    for (uint32_t t = 16000U; t <= 20000U; t += 1000U) {
        task_hb_bump(TASK_HB_WEIGHT_TX);
        task_hb_bump(TASK_HB_SCALE_CMD);
        n = task_hb_check(t, 10000U, &mask);
    }
    test_check(n == 1U && mask == (1U << TASK_HB_MQTT_PUB), "REQ-WMQ-51_mqtt_pub_frozen_10s_reported_by_mask");

    task_hb_bump(TASK_HB_MQTT_PUB);
    task_hb_bump(TASK_HB_WEIGHT_TX);
    task_hb_bump(TASK_HB_SCALE_CMD);
    test_check(task_hb_check(21000U, 10000U, &mask) == 0U && mask == 0U, "REQ-WMQ-51_recovery_clears_frozen");

    task_hb_bump(TASK_HB_COUNT);
    task_hb_bump((task_hb_id_t)99);
    test_check(task_hb_check(22000U, 10000U, &mask) == 0U, "REQ-WMQ-51_invalid_id_bump_is_harmless");

    n = task_hb_check(40000U, 10000U, &mask);
    test_check(n == 3U && mask == ((1U << TASK_HB_COUNT) - 1U), "REQ-WMQ-51_all_three_frozen_counted");
    test_check(task_hb_check(40000U, 10000U, NULL) == 3U, "REQ-WMQ-51_null_mask_accepted");
}

void test_weight_mqtt_run(QueueHandle_t manager)
{
    test_queues();
    test_heartbeat(manager);
    test_payload();
    test_broker_cfg();
    test_scale_cmd();
    test_task_hb();
}
