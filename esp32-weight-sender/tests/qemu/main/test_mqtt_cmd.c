#include "test_mqtt_cmd.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_link_core.h"
#include "scale_cmd.h"
#include "test_harness.h"

#define DEV "bits-a4cf12ab34cd"

/* ---- mock scale services ---------------------------------------------------- */

static uint32_t g_now;
static bool g_online;
static const char *g_state_name;
static bool g_have_sample;
static uint32_t g_seq;
static int32_t g_weight;
static bool g_stable;
static bool g_bump_on_delay;
static int g_encodes;
static int g_sends;
static bool g_send_ok;
static scale_cmd_t *g_reenter;  /* submit a second command from inside send */
static scale_submit_t g_reenter_result;
static scale_cmd_ack_t g_reenter_ack;

static uint32_t m_now(void) { return g_now; }

static bool m_link(const char **name)
{
    *name = g_state_name;
    return g_online;
}

static bool m_sample(uint8_t ch, uint32_t *seq, int32_t *g, bool *st)
{
    (void)ch;
    if (!g_have_sample) return false;
    *seq = g_seq;
    *g = g_weight;
    *st = g_stable;
    return true;
}

static void m_delay(uint32_t ms)
{
    g_now += ms;
    if (g_bump_on_delay) {
        g_seq++;
        g_weight = 0;
        g_stable = true;
    }
}

static bool m_send(uint8_t ch, const uint8_t *f, size_t n)
{
    (void)ch;
    (void)f;
    (void)n;
    g_sends++;
    if (g_reenter != NULL) {
        const char *p = "{\"command_id\":900,\"type\":\"TARE\",\"channel_id\":\"CH2\",\"ttl_ms\":5000}";
        g_reenter_result = scale_cmd_submit(g_reenter, p, strlen(p), false, g_now, &g_reenter_ack);
    }
    return g_send_ok;
}

/* Counts calls and reports NOT_VERIFIED like the production stub. */
static scale_enc_status_t enc_counting_unverified(scale_cmd_type_t t, uint8_t ch, uint8_t *f,
                                                  size_t cap, size_t *len)
{
    g_encodes++;
    return scale_command_encode(t, ch, f, cap, len);
}

/* TEST-ONLY stand-in so the post-encode path can be exercised. These are NOT
 * CAS bytes and exist nowhere outside this test. */
static scale_enc_status_t enc_fake_ok(scale_cmd_type_t t, uint8_t ch, uint8_t *f, size_t cap,
                                      size_t *len)
{
    (void)t;
    (void)ch;
    g_encodes++;
    if (cap < 2U) return SCALE_ENC_NOT_VERIFIED;
    f[0] = 0xEE;
    f[1] = 0xEE;
    *len = 2U;
    return SCALE_ENC_OK;
}

static scale_cmd_t s_cmd;
static scale_cmd_t *s_prev;

static void fresh(scale_cmd_encode_fn enc, bool with_send)
{
    if (s_prev != NULL) {
        vSemaphoreDelete(s_cmd.lock);
        vSemaphoreDelete(s_cmd.wake);
    }
    g_now = 100000U;
    g_online = true;
    g_state_name = "ONLINE";
    g_have_sample = true;
    g_seq = 10U;
    g_weight = 1500;
    g_stable = false;
    g_bump_on_delay = false;
    g_encodes = 0;
    g_sends = 0;
    g_send_ok = true;
    g_reenter = NULL;
    scale_cmd_test_allow_post_encode(false);
    scale_cmd_cfg_t cfg = {
        .now_ms = m_now, .link_online = m_link, .post_sample = m_sample,
        .send_frame = with_send ? m_send : NULL, .delay_ms = m_delay, .encode = enc,
        .active_channel = 2U, .settle_timeout_ms = 200U,
    };
    strcpy(cfg.device_id, DEV);
    scale_cmd_init(&s_cmd, &cfg);
    s_prev = &s_cmd;
}

static scale_submit_t submit(const char *payload, bool retain, scale_cmd_ack_t *ack)
{
    return scale_cmd_submit(&s_cmd, payload, strlen(payload), retain, g_now, ack);
}

static scale_submit_t submit_cmd(unsigned id, const char *type, const char *chan, const char *ttl,
                                 scale_cmd_ack_t *ack)
{
    char p[256];
    snprintf(p, sizeof(p),
             "{\"command_id\":%u,\"type\":\"%s\",\"channel_id\":%s,\"issued_at\":\"2026-10-07T08:15:30.123Z\"%s}",
             id, type, chan, ttl);
    return submit(p, false, ack);
}

#define OK_CH  "\"CH2\""
#define OK_TTL ",\"ttl_ms\":10000"

static bool rejected_with(scale_submit_t r, const scale_cmd_ack_t *a, const char *needle)
{
    return r == SCALE_SUBMIT_ACK && !a->applied && a->result == SCALE_RESULT_REJECTED &&
           strstr(a->reason, needle) != NULL;
}

/* ---- tests ------------------------------------------------------------------ */

static void test_core(void)
{
    char t[MQTT_LINK_TOPIC_MAX];
    test_check(mqtt_link_topic(t, sizeof(t), DEV, MQTT_SUFFIX_COMMANDS) &&
                   strcmp(t, "cas/" DEV "/commands") == 0, "mqtt_topic_commands");
    test_check(mqtt_link_topic(t, sizeof(t), DEV, MQTT_SUFFIX_ACK) &&
                   strcmp(t, "cas/" DEV "/commands/ack") == 0, "mqtt_topic_ack");
    test_check(mqtt_link_topic(t, sizeof(t), DEV, MQTT_SUFFIX_STATUS) &&
                   strcmp(t, "cas/" DEV "/telemetry/status") == 0, "mqtt_topic_telemetry_status");
    test_check(!mqtt_link_topic(t, sizeof(t), "a/b", "x") && !mqtt_link_topic(t, sizeof(t), "a+b", "x") &&
                   !mqtt_link_topic(t, sizeof(t), "a#", "x") && !mqtt_link_topic(t, sizeof(t), "", "x") &&
                   !mqtt_link_topic(t, sizeof(t), NULL, "x"), "mqtt_topic_rejects_bad_device_id");
    char longid[80];
    memset(longid, 'a', 65);
    longid[65] = '\0';
    test_check(!mqtt_link_device_id_valid(longid), "mqtt_device_id_over_64_rejected");
    longid[64] = '\0';
    test_check(mqtt_link_device_id_valid(longid), "mqtt_device_id_64_accepted");
    test_check(!mqtt_link_topic(t, 8, DEV, "commands"), "mqtt_topic_truncation_refused");

    mqtt_link_presence_t pr;
    test_check(mqtt_link_presence_plan(&pr, DEV), "mqtt_presence_plan_built");
    test_check(strcmp(pr.lwt_topic, "cas/" DEV "/status") == 0 &&
                   strcmp(pr.birth_topic, pr.lwt_topic) == 0, "mqtt_lwt_and_birth_topic");
    test_check(strcmp(pr.lwt_payload, "{\"online\":false}") == 0, "mqtt_lwt_payload_offline");
    test_check(strcmp(pr.birth_payload, "{\"online\":true}") == 0, "mqtt_birth_payload_online");
    test_check(pr.qos == 1 && pr.retain, "mqtt_presence_qos1_retained");
    test_check(strcmp(pr.birth_topic, t) != 0 && strstr(pr.birth_topic, "telemetry") == NULL,
               "mqtt_birth_not_on_telemetry_status");

    test_check(mqtt_link_backoff_ms(0) == 1000U && mqtt_link_backoff_ms(1) == 2000U &&
                   mqtt_link_backoff_ms(2) == 4000U && mqtt_link_backoff_ms(4) == 16000U,
               "mqtt_backoff_doubles_from_1s");
    test_check(mqtt_link_backoff_ms(5) == 30000U && mqtt_link_backoff_ms(6) == 30000U &&
                   mqtt_link_backoff_ms(0xFFFFFFFFU) == 30000U, "mqtt_backoff_capped_30s");

    char shown[64];
    mqtt_link_uri_redact(shown, sizeof(shown), "mqtt://user:s3cret@10.0.0.1:1883");
    test_check(strcmp(shown, "mqtt://10.0.0.1:1883") == 0 && strstr(shown, "s3cret") == NULL,
               "mqtt_uri_redacts_credentials");
    mqtt_link_uri_redact(shown, sizeof(shown), "mqtt://192.168.137.1:1883");
    test_check(strcmp(shown, "mqtt://192.168.137.1:1883") == 0, "mqtt_uri_without_userinfo_unchanged");

    /* Bounded queue: the event callback path. */
    mqtt_link_queues_t q;
    test_check(mqtt_link_queues_init(&q, 2, 2), "mqtt_queue_init");
    const char *tp = "cas/" DEV "/commands";
    const char *pl = "{\"command_id\":1}";
    test_check(mqtt_link_queue_push_rx(&q, tp, strlen(tp), pl, strlen(pl), strlen(pl), false, 77U) == MQTT_ENQ_OK,
               "mqtt_rx_enqueued");
    test_check(mqtt_link_queue_push_rx(&q, tp, strlen(tp), pl, strlen(pl), strlen(pl), true, 1U) == MQTT_ENQ_RETAINED,
               "mqtt_rx_retained_ignored");
    test_check(mqtt_link_queue_push_rx(&q, tp, strlen(tp), pl, 4, strlen(pl), false, 1U) == MQTT_ENQ_FRAGMENT,
               "mqtt_rx_fragment_refused");
    static char big[MQTT_LINK_PAYLOAD_MAX + 8];
    memset(big, 'x', sizeof(big));
    test_check(mqtt_link_queue_push_rx(&q, tp, strlen(tp), big, sizeof(big), sizeof(big), false, 1U) == MQTT_ENQ_TOO_BIG,
               "mqtt_rx_oversize_refused");
    mqtt_item_t *it = pvPortMalloc(sizeof(*it));
    test_check(mqtt_link_queue_next(&q, it, 0, 0U, 1000U) && it->kind == MQTT_ITEM_RX && it->recv_ms == 77U &&
                   strcmp(it->topic, tp) == 0 && strcmp(it->data, pl) == 0 && !it->retain,
               "mqtt_rx_item_roundtrip");
    test_check(!mqtt_link_queue_next(&q, it, 0, 0U, 1000U), "mqtt_queue_empty_after_pop");

    /* Saturate, then prove the callback path never waits. A blocking send would
     * cost at least one tick per rejected push. */
    (void)mqtt_link_queue_push_rx(&q, tp, strlen(tp), pl, strlen(pl), strlen(pl), false, 1U);
    (void)mqtt_link_queue_push_rx(&q, tp, strlen(tp), pl, strlen(pl), strlen(pl), false, 1U);
    (void)mqtt_link_queue_push_pub(&q, "cas/x/telemetry/status", "{}", 0, false);
    (void)mqtt_link_queue_push_pub(&q, "cas/x/telemetry/status", "{}", 0, false);
    int64_t t0 = esp_timer_get_time();
    int full = 0;
    for (int i = 0; i < 50; i++) {
        if (mqtt_link_queue_push_rx(&q, tp, strlen(tp), pl, strlen(pl), strlen(pl), false, 1U) == MQTT_ENQ_FULL) full++;
        if (mqtt_link_queue_push_pub(&q, "cas/x/telemetry/status", "{}", 0, false) == MQTT_ENQ_FULL) full++;
    }
    int64_t us = esp_timer_get_time() - t0;
    test_check(full == 100, "mqtt_full_queue_drops_every_push");
    test_check(us < 100000, "mqtt_publish_callback_never_blocks");
    test_check(q.rx_dropped_full + q.pub_dropped_full >= 100U, "mqtt_full_queue_counts_drops");
    vPortFree(it);
    mqtt_link_queues_deinit(&q);
}

static void test_commands(void)
{
    scale_cmd_ack_t a;
    char json[MQTT_LINK_PAYLOAD_MAX];

    /* Encoder stub: never invents bytes. */
    uint8_t f[SCALE_CMD_FRAME_MAX];
    size_t flen = 99U;
    test_check(scale_command_encode(SCALE_CMD_ZERO, 2U, f, sizeof(f), &flen) == SCALE_ENC_NOT_VERIFIED && flen == 0U,
               "scale_encode_zero_not_verified");
    test_check(scale_command_encode(SCALE_CMD_TARE, 2U, f, sizeof(f), &flen) == SCALE_ENC_NOT_VERIFIED && flen == 0U,
               "scale_encode_tare_not_verified");

    /* VERIFICATION REQUIRED path with the production encoder. */
    fresh(enc_counting_unverified, false);
    test_check(submit_cmd(42, "ZERO", OK_CH, OK_TTL, &a) == SCALE_SUBMIT_QUEUED, "cmd_valid_zero_queued");
    test_check(scale_cmd_run_one(&s_cmd, 0, &a), "cmd_run_one_executes");
    test_check(a.command_id == 42U && !a.applied && a.result == SCALE_RESULT_FAILED &&
                   strcmp(a.reason, SCALE_CMD_REASON_UNVERIFIED) == 0, "cmd_unverified_ack_failed");
    test_check(strlen(a.reason) <= SCALE_CMD_REASON_MAX, "cmd_reason_within_120");
    size_t n = scale_cmd_build_ack_json(&s_cmd, &a, json, sizeof(json));
    test_check(n > 0U && strstr(json, "\"command_id\":42") && strstr(json, "\"state\":\"FAILED\"") &&
                   strstr(json, "\"result\":\"failed\"") &&
                   strstr(json, "VERIFICATION REQUIRED: CAS CI-150A remote ZERO/TARE frame unverified") &&
                   strstr(json, "\"device_id\":\"" DEV "\"") && strstr(json, "\"channel_id\":\"CH2\"") &&
                   strstr(json, "weight_g") == NULL, "cmd_unverified_ack_json");
    test_check(g_encodes == 1 && !scale_cmd_busy(&s_cmd), "cmd_encoder_called_once_and_idle");

    /* Duplicate: re-ACK the stored outcome, never re-execute. */
    scale_cmd_ack_t d;
    test_check(submit_cmd(42, "ZERO", OK_CH, OK_TTL, &d) == SCALE_SUBMIT_ACK && d.command_id == 42U &&
                   d.result == SCALE_RESULT_FAILED && strcmp(d.reason, a.reason) == 0,
               "cmd_duplicate_reacked_same_outcome");
    test_check(!scale_cmd_run_one(&s_cmd, 0, &a) && g_encodes == 1, "cmd_duplicate_not_reexecuted");

    /* Retained and malformed messages are never acted on. */
    test_check(submit("{\"command_id\":43,\"type\":\"ZERO\",\"channel_id\":\"CH2\",\"ttl_ms\":10000}", true, &a) ==
                   SCALE_SUBMIT_IGNORED, "cmd_retained_ignored");
    test_check(!scale_cmd_run_one(&s_cmd, 0, &a), "cmd_retained_never_queued");
    test_check(submit_cmd(43, "ZERO", OK_CH, OK_TTL, &a) == SCALE_SUBMIT_QUEUED,
               "cmd_retained_id_still_unused");
    scale_cmd_run_one(&s_cmd, 0, &a);
    test_check(submit("not json", false, &a) == SCALE_SUBMIT_IGNORED &&
                   submit("{\"type\":\"ZERO\",\"ttl_ms\":1000}", false, &a) == SCALE_SUBMIT_IGNORED &&
                   submit("{\"command_id\":0,\"type\":\"ZERO\"}", false, &a) == SCALE_SUBMIT_IGNORED &&
                   submit("{\"command_id\":-5,\"type\":\"ZERO\"}", false, &a) == SCALE_SUBMIT_IGNORED &&
                   submit("{\"command_id\":4294967296,\"type\":\"ZERO\"}", false, &a) == SCALE_SUBMIT_IGNORED &&
                   submit("{\"command_id\":7,\"command_id\":8,\"type\":\"ZERO\"}", false, &a) == SCALE_SUBMIT_IGNORED &&
                   submit("{\"command_id\":7,\"type\":\"ZERO\"} trailing", false, &a) == SCALE_SUBMIT_IGNORED,
               "cmd_malformed_or_idless_ignored");

    /* Validation. */
    fresh(enc_counting_unverified, false);
    test_check(rejected_with(submit_cmd(1, "READY", OK_CH, OK_TTL, &a), &a, "unsupported"),
               "cmd_unsupported_type_rejected");
    test_check(rejected_with(submit_cmd(2, "zero", OK_CH, OK_TTL, &a), &a, "unsupported"),
               "cmd_type_is_case_sensitive");
    test_check(rejected_with(submit_cmd(3, "ZERO", "\"CH1\"", OK_TTL, &a), &a, "not served"),
               "cmd_inactive_channel_rejected");
    test_check(rejected_with(submit_cmd(4, "ZERO", "null", OK_TTL, &a), &a, "channel_id required"),
               "cmd_null_channel_rejected");
    test_check(rejected_with(submit_cmd(5, "ZERO", "\"CH9\"", OK_TTL, &a), &a, "invalid channel"),
               "cmd_invalid_channel_rejected");
    test_check(rejected_with(submit_cmd(6, "ZERO", OK_CH, "", &a), &a, "ttl_ms"), "cmd_missing_ttl_rejected");
    test_check(rejected_with(submit_cmd(7, "ZERO", OK_CH, ",\"ttl_ms\":0", &a), &a, "ttl_ms"),
               "cmd_zero_ttl_rejected");
    test_check(rejected_with(submit_cmd(8, "ZERO", OK_CH, ",\"ttl_ms\":60001", &a), &a, "ttl_ms"),
               "cmd_overlong_ttl_rejected");
    test_check(rejected_with(submit_cmd(9, "TARE", OK_CH, ",\"ttl_ms\":5000,\"device_id\":\"other-dev\"", &a), &a,
                             "device_id"), "cmd_wrong_device_rejected");
    test_check(rejected_with(submit_cmd(10, "TARE", OK_CH, ",\"ttl_ms\":5000,\"command_type\":\"ZERO\"", &a), &a,
                             "disagree"), "cmd_type_command_type_mismatch_rejected");
    test_check(submit_cmd(11, "TARE", OK_CH, ",\"ttl_ms\":5000,\"command_type\":\"TARE\",\"device_id\":\"" DEV "\","
                                             "\"profile\":{\"kp\":1,\"tags\":[1,2,{\"a\":\"}\"}]}", &a) ==
                   SCALE_SUBMIT_QUEUED, "cmd_tare_with_extra_fields_queued");
    scale_cmd_run_one(&s_cmd, 0, &a);

    /* Expiry: no wall clock, measured from receipt. */
    fresh(enc_counting_unverified, false);
    g_now = 100000U;
    char p[200];
    snprintf(p, sizeof(p), "{\"command_id\":20,\"type\":\"ZERO\",\"channel_id\":\"CH2\",\"ttl_ms\":10000}");
    test_check(scale_cmd_submit(&s_cmd, p, strlen(p), false, g_now - 10001U, &a) == SCALE_SUBMIT_ACK &&
                   a.result == SCALE_RESULT_TIMEOUT && !a.applied && strstr(a.reason, "expired"),
               "cmd_expired_at_receipt_rejected");
    /* Boundary: age == ttl is still valid, age == ttl+1 is expired. */
    snprintf(p, sizeof(p), "{\"command_id\":22,\"type\":\"ZERO\",\"channel_id\":\"CH2\",\"ttl_ms\":10000}");
    test_check(scale_cmd_submit(&s_cmd, p, strlen(p), false, g_now - 10000U, &a) == SCALE_SUBMIT_QUEUED,
               "cmd_age_equals_ttl_accepted");
    test_check(scale_cmd_run_one(&s_cmd, 0, &a) && a.command_id == 22U && a.result != SCALE_RESULT_TIMEOUT &&
                   g_encodes == 1, "cmd_age_equals_ttl_executes");
    g_encodes = 0;
    /* Execute side: accepted at age == ttl, then one tick later it is expired and must not run. */
    snprintf(p, sizeof(p), "{\"command_id\":23,\"type\":\"ZERO\",\"channel_id\":\"CH2\",\"ttl_ms\":10000}");
    test_check(scale_cmd_submit(&s_cmd, p, strlen(p), false, g_now - 10000U, &a) == SCALE_SUBMIT_QUEUED,
               "cmd_age_equals_ttl_accepted_2");
    g_now += 1U;
    test_check(scale_cmd_run_one(&s_cmd, 0, &a) && a.command_id == 23U && a.result == SCALE_RESULT_TIMEOUT &&
                   strstr(a.reason, "expired before start") && g_encodes == 0, "cmd_age_ttl_plus1_expires_at_execute");
    test_check(submit_cmd(21, "ZERO", OK_CH, ",\"ttl_ms\":1000", &a) == SCALE_SUBMIT_QUEUED, "cmd_fresh_queued");
    g_now += 1500U;
    test_check(scale_cmd_run_one(&s_cmd, 0, &a) && a.command_id == 21U && a.result == SCALE_RESULT_TIMEOUT &&
                   strstr(a.reason, "expired before start") && g_encodes == 0, "cmd_expired_before_start_not_executed");

    /* Offline / stale scale link. */
    fresh(enc_counting_unverified, false);
    g_online = false;
    g_state_name = "STALE";
    test_check(rejected_with(submit_cmd(30, "ZERO", OK_CH, OK_TTL, &a), &a, "not ONLINE (STALE)"),
               "cmd_stale_link_rejected");
    g_state_name = "OFFLINE";
    test_check(rejected_with(submit_cmd(31, "TARE", OK_CH, OK_TTL, &a), &a, "not ONLINE (OFFLINE)"),
               "cmd_offline_link_rejected");
    test_check(!scale_cmd_run_one(&s_cmd, 0, &a) && g_encodes == 0, "cmd_offline_never_queued");
    g_online = true;
    g_state_name = "ONLINE";
    test_check(submit_cmd(32, "ZERO", OK_CH, OK_TTL, &a) == SCALE_SUBMIT_QUEUED, "cmd_online_accepts");
    g_online = false;
    g_state_name = "STALE";
    test_check(scale_cmd_run_one(&s_cmd, 0, &a) && a.result == SCALE_RESULT_REJECTED &&
                   strstr(a.reason, "not ONLINE") && g_encodes == 0, "cmd_link_lost_before_start_rejected");

    /* Queue saturation. */
    fresh(enc_counting_unverified, false);
    for (unsigned i = 0; i < SCALE_CMD_QUEUE_DEPTH; i++) {
        char ok[8];
        snprintf(ok, sizeof(ok), "q%u", i);
        if (submit_cmd(100 + i, "ZERO", OK_CH, OK_TTL, &a) != SCALE_SUBMIT_QUEUED) test_check(false, ok);
    }
    test_check(rejected_with(submit_cmd(110, "ZERO", OK_CH, OK_TTL, &a), &a, "queue full"),
               "cmd_queue_saturation_rejects");
    /* Flooding rejects must not evict queued commands from the dedupe ring. */
    for (unsigned i = 0; i < 3U * SCALE_CMD_DEDUPE_SLOTS; i++) submit_cmd(200 + i, "READY", OK_CH, OK_TTL, &a);
    test_check(submit_cmd(100, "ZERO", OK_CH, OK_TTL, &a) == SCALE_SUBMIT_IGNORED,
               "cmd_inflight_dup_not_requeued_after_flood");
    unsigned ran = 0;
    while (scale_cmd_run_one(&s_cmd, 0, &a)) ran++;
    test_check(ran == SCALE_CMD_QUEUE_DEPTH && g_encodes == (int)SCALE_CMD_QUEUE_DEPTH,
               "cmd_queue_drains_in_order_once_each");
    test_check(submit_cmd(111, "ZERO", OK_CH, OK_TTL, &a) == SCALE_SUBMIT_QUEUED, "cmd_queue_accepts_after_drain");
    scale_cmd_run_one(&s_cmd, 0, &a);

    /* CONTRACT 8.4: an encoder that returns bytes must still never yield APPLIED
     * (or touch the scale) while SCALE_CMD_ENCODER_VERIFIED is off. */
    fresh(enc_fake_ok, true);
    submit_cmd(290, "ZERO", OK_CH, OK_TTL, &a);
    scale_cmd_run_one(&s_cmd, 0, &a);
    submit_cmd(291, "TARE", OK_CH, OK_TTL, &d);
    scale_cmd_ack_t t2;
    scale_cmd_run_one(&s_cmd, 0, &t2);
    n = scale_cmd_build_ack_json(&s_cmd, &t2, json, sizeof(json));
#if !SCALE_CMD_ENCODER_VERIFIED
    test_check(!a.applied && a.result == SCALE_RESULT_FAILED && strcmp(a.reason, SCALE_CMD_REASON_UNVERIFIED) == 0 &&
                   !t2.applied && t2.result == SCALE_RESULT_FAILED && g_sends == 0 &&
                   n > 0U && strstr(json, "APPLIED") == NULL && strstr(json, "\"state\":\"FAILED\""),
               "cmd_never_applied_while_unverified");
#endif

    /* Restored via the test-only seam (SCALE_CMD_TEST_HOOKS). APPLIED below exercises
     * INTERNAL LOGIC ONLY with a fake encoder; it says nothing about a real scale. */
    /* Busy: a second command while one is running. */
    fresh(enc_fake_ok, true);
    scale_cmd_test_allow_post_encode(true);
    g_bump_on_delay = true;
    g_reenter = &s_cmd;
    test_check(submit_cmd(300, "ZERO", OK_CH, OK_TTL, &a) == SCALE_SUBMIT_QUEUED, "cmd_first_queued_for_busy");
    test_check(scale_cmd_run_one(&s_cmd, 0, &a), "cmd_first_runs");
    test_check(rejected_with(g_reenter_result, &g_reenter_ack, "busy"), "cmd_busy_rejects_while_running");
    test_check(g_reenter_ack.command_id == 900U, "cmd_busy_ack_carries_its_id");

    /* Post-encode path with the test-only encoder: success carries a post sample. */
    test_check(a.applied && a.result == SCALE_RESULT_SUCCESS && a.has_weight && a.weight_g == 0 && a.stable &&
                   g_sends == 1, "cmd_success_carries_post_sample");
    n = scale_cmd_build_ack_json(&s_cmd, &a, json, sizeof(json));
    test_check(n > 0U && strstr(json, "\"state\":\"APPLIED\"") && strstr(json, "\"result\":\"success\"") &&
                   strstr(json, "\"weight_g\":0") && strstr(json, "\"stable\":true") &&
                   strstr(json, "\"error\"") == NULL, "cmd_success_ack_json");

    /* No fresh post-command sample: timeout, not a guess. */
    fresh(enc_fake_ok, true);
    scale_cmd_test_allow_post_encode(true);
    submit_cmd(310, "TARE", OK_CH, OK_TTL, &a);
    scale_cmd_run_one(&s_cmd, 0, &a);
    test_check(!a.applied && a.result == SCALE_RESULT_TIMEOUT && !a.has_weight, "cmd_no_post_sample_times_out");

    /* Transmit failure and missing transmit path fail safe. */
    fresh(enc_fake_ok, true);
    scale_cmd_test_allow_post_encode(true);
    g_send_ok = false;
    submit_cmd(320, "ZERO", OK_CH, OK_TTL, &a);
    scale_cmd_run_one(&s_cmd, 0, &a);
    test_check(!a.applied && a.result == SCALE_RESULT_FAILED && strstr(a.reason, "transmit failed"),
               "cmd_transmit_failure_fails");
    fresh(enc_fake_ok, false);
    scale_cmd_test_allow_post_encode(true);
    submit_cmd(321, "ZERO", OK_CH, OK_TTL, &a);
    scale_cmd_run_one(&s_cmd, 0, &a);
    test_check(!a.applied && a.result == SCALE_RESULT_FAILED && strstr(a.reason, "no scale transmit path"),
               "cmd_no_transmit_path_fails");
    scale_cmd_test_allow_post_encode(false);

    /* ACK JSON for a rejection with no valid channel, and buffer safety. */
    fresh(NULL, false);
    submit_cmd(330, "ZERO", "null", OK_TTL, &a);
    n = scale_cmd_build_ack_json(&s_cmd, &a, json, sizeof(json));
    test_check(n > 0U && strstr(json, "\"channel_id\":null") && strstr(json, "\"result\":\"rejected\""),
               "cmd_reject_ack_json_null_channel");
    char tiny[16];
    test_check(scale_cmd_build_ack_json(&s_cmd, &a, tiny, sizeof(tiny)) == 0U, "cmd_ack_json_tiny_buffer_refused");
}

void test_mqtt_cmd_run(void)
{
    test_core();
    test_commands();
}
