/* Weight-over-MQTT intake (CONTRACT 8.2/8.3/9.11), SINGLE-SCALE: one cache keyed by
 * (scale_id, boot_id) in front of the UNCHANGED weight_receiver acceptance path, the
 * one-slot hand-off, the separate bounded weight queue, and the default-OFF
 * guarantee. Pure logic only: no broker, no esp-mqtt. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dual_dispense_controller.h"
#include "job_queue.h"
#include "mqtt_link_core.h"
#include "relay_driver.h"
#include "safety_manager.h"
#include "weight_mqtt.h"
#include "weight_receiver.h"
#include "weight_slots.h"

extern void audit_check(bool condition, const char *name);
#define CK(cond, name) audit_check((cond), (name))

static weight_msg_t s_cap;
static int s_cap_n;
static uint32_t s_cap_ms;
static void cap_handler(const weight_msg_t *m, uint32_t ms)
{
    s_cap = *m;
    s_cap_ms = ms;
    s_cap_n++;
}

static void wm_setup(void)
{
    (void)relay_all_off();
    (void)weight_receiver_init();
    weight_receiver_set_actuation_handler(cap_handler);
    weight_mqtt_init();
    weight_mqtt_set_enabled(true);
    s_cap_n = 0;
    memset(&s_cap, 0, sizeof(s_cap));
}

static char s_pl[400];
/* One sender frame (single-scale wire). Sender uptime is whatever the caller passes. */
static weight_mqtt_result_t wmu(const char *boot, uint32_t seq, uint32_t up, const char *sid,
                                const char *uart, long w, bool st, unsigned age, uint32_t recv)
{
    int n = snprintf(s_pl, sizeof(s_pl), "{\"schema_version\":1,\"boot_id\":\"%s\",", boot);
    n += snprintf(s_pl + n, sizeof(s_pl) - (size_t)n,
                  "\"seq\":%lu,\"uptime_ms\":%lu,\"scale_id\":\"%s\",\"src_uart\":\"%s\",\"weight_g\":%ld,"
                  "\"stable\":%s,\"age_ms\":%u,\"cas_seq\":7,\"source\":\"CAS_RS232\"}",
                  (unsigned long)seq, (unsigned long)up, sid, uart, w, st ? "true" : "false", age);
    return weight_mqtt_on_payload(s_pl, (size_t)n, recv);
}
static weight_mqtt_result_t wm(const char *boot, uint32_t seq, uint32_t up, long w, bool st,
                               unsigned age, uint32_t recv)
{
    return wmu(boot, seq, up, "SCALE1", "UART2", w, st, age, recv);
}
static weight_mqtt_result_t raw(const char *json, uint32_t recv)
{
    return weight_mqtt_on_payload(json, strlen(json), recv);
}

/* Two consecutive messages bring a boot_id into use. Returns the 2nd result. */
static weight_mqtt_result_t est(const char *boot, uint32_t seq, long w, uint32_t recv)
{
    (void)wm(boot, seq, recv - 500U, w, true, 0U, recv);
    return wm(boot, seq + 1U, recv + 10U - 500U, w, true, 0U, recv + 10U);
}

static bool all_off(void)
{
    for (uint8_t i = 0; i < RELAY_COUNT; i++) if (relay_get_state(i)) return false;
    return true;
}
static bool all_on(void)
{
    for (uint8_t i = 0; i < RELAY_COUNT; i++) if (!relay_get_state(i)) return false;
    return true;
}

static void test_select_and_default_off(void)
{
    CK(weight_mqtt_select(NULL, NULL) == WEIGHT_SEL_WS, "wmq_select_null_is_ws");
    CK(weight_mqtt_select("ws", "bits-s1") == WEIGHT_SEL_WS, "wmq_select_ws_default");
    CK(weight_mqtt_select("MQTT", "bits-s1") == WEIGHT_SEL_WS, "wmq_select_exact_lowercase_only");
    CK(weight_mqtt_select("mqtt", "bits-s1") == WEIGHT_SEL_MQTT, "wmq_select_mqtt_valid_peer");
    CK(weight_mqtt_select("mqtt", NULL) == WEIGHT_SEL_MQTT_BLOCKED, "wmq_invalid_peer_null_blocked");
    CK(weight_mqtt_select("mqtt", "") == WEIGHT_SEL_MQTT_BLOCKED, "wmq_invalid_peer_empty_blocked");
    CK(weight_mqtt_select("mqtt", "a/b") == WEIGHT_SEL_MQTT_BLOCKED, "wmq_invalid_peer_slash_blocked");
    CK(weight_mqtt_select("mqtt", "+") == WEIGHT_SEL_MQTT_BLOCKED, "wmq_invalid_peer_wildcard_blocked");
    CK(weight_mqtt_select("mqtt", "#") == WEIGHT_SEL_MQTT_BLOCKED, "wmq_invalid_peer_hash_blocked");

    char t[MQTT_LINK_TOPIC_MAX];
    CK(mqtt_link_topic(t, sizeof(t), "bits-s1", MQTT_SUFFIX_WEIGHT_CTL) &&
       strcmp(t, "cas/bits-s1/weight/ctl") == 0, "wmq_topic_is_exact_peer_weight_ctl");
    CK(!mqtt_link_topic(t, sizeof(t), "x/#", MQTT_SUFFIX_WEIGHT_CTL), "wmq_topic_rejects_wildcard_peer");
    CK(mqtt_link_topic_equals("cas/bits-s1/weight/ctl", 22U, "cas/bits-s1/weight/ctl"), "wmq_topic_equals_exact");
    CK(!mqtt_link_topic_equals("cas/bits-s1/weight/ctlx", 23U, "cas/bits-s1/weight/ctl"), "wmq_topic_equals_no_prefix_match");
    CK(!mqtt_link_topic_equals("cas/+/weight/ctl", 16U, "cas/bits-s1/weight/ctl"), "wmq_topic_equals_no_wildcard");

    /* Default (ws): a perfectly valid MQTT weight is dropped and reaches nothing. */
    wm_setup();
    weight_mqtt_set_enabled(false);
    CK(est("aabbccdd", 1U, 12000, 1000U) == WMQ_DROP_DISABLED, "wmq_default_ws_drops_mqtt_weight");
    CK(s_cap_n == 0 && !weight_receiver_have_valid(), "wmq_default_ws_feeds_nothing");
    weight_mqtt_stats_t st;
    weight_mqtt_stats(&st);
    CK(st.dropped_disabled == 2U && st.fed == 0U, "wmq_default_ws_counted_as_disabled");
}

static void test_order(void)
{
    wm_setup();
    const char *A = "aabbccdd";
    CK(wm(A, 10U, 500U, 5000, true, 0U, 1000U) == WMQ_PENDING_BOOT, "wmq_first_message_is_pending");
    CK(s_cap_n == 0 && !weight_receiver_have_valid(), "wmq_pending_not_usable_not_fed");
    CK(wm(A, 11U, 510U, 5000, true, 0U, 1010U) == WMQ_FED, "wmq_second_consecutive_accepted");
    CK(s_cap_n == 1 && s_cap.weight_g == 5000, "wmq_fed_through_receiver_as_one_sample");
    CK(s_cap.has_scale_id && strcmp(s_cap.scale_id, "SCALE1") == 0 &&
       s_cap.has_boot_id && strcmp(s_cap.boot_id, A) == 0, "wmq_sample_carries_scale_id_and_boot_id");
    CK(!s_cap.has_weight1 && !s_cap.has_weight2 && !s_cap.has_channel_fields,
       "wmq_sample_has_no_per_pump_fields");
    CK(s_cap.has_stable && !s_cap.stable, "wmq_first_after_boot_switch_forced_unstable");
    CK(weight_receiver_have_valid() && weight_receiver_last_rx_ms() == 1010U, "wmq_receiver_timestamp_set");
    CK(wm(A, 12U, 520U, 5001, true, 0U, 1020U) == WMQ_FED && s_cap.stable, "wmq_next_keeps_sender_stable");

    uint32_t last = weight_receiver_last_rx_ms();
    int n = s_cap_n;
    CK(wm(A, 12U, 521U, 9999, true, 0U, 1300U) == WMQ_DROP_OLD, "wmq_duplicate_seq_dropped");
    CK(wm(A, 11U, 530U, 9999, true, 0U, 1310U) == WMQ_DROP_OLD, "wmq_older_seq_dropped");
    CK(s_cap_n == n && weight_receiver_last_rx_ms() == last, "wmq_dropped_do_not_move_timestamp");
    CK(s_cap.weight_g == 5001, "wmq_dropped_do_not_replace_weight");

    /* out of order: 14 then 13 */
    CK(wm(A, 14U, 540U, 5002, true, 0U, 1040U) == WMQ_FED, "wmq_gap_forward_accepted");
    CK(wm(A, 13U, 545U, 7777, true, 0U, 1045U) == WMQ_DROP_OLD, "wmq_out_of_order_dropped");
    CK(s_cap.weight_g == 5002, "wmq_out_of_order_keeps_newer_weight");

    /* sender uptime running backwards with a newer seq is inconsistent */
    CK(wm(A, 15U, 100U, 7777, true, 0U, 1050U) == WMQ_DROP_OLD, "wmq_uptime_regression_dropped");

    /* u32 wrap of seq */
    wm_setup();
    const char *B = "11223344";
    CK(wm(B, 0xFFFFFFFEU, 500U, 3000, true, 0U, 2000U) == WMQ_PENDING_BOOT, "wmq_wrap_first_pending");
    CK(wm(B, 0xFFFFFFFFU, 510U, 3000, true, 0U, 2010U) == WMQ_FED, "wmq_wrap_pair_consecutive_accepted");
    CK(wm(B, 0U, 520U, 3001, true, 0U, 2020U) == WMQ_FED, "wmq_wrap_zero_is_newer");
    CK(wm(B, 0xFFFFFFFFU, 530U, 9000, true, 0U, 2030U) == WMQ_DROP_OLD, "wmq_wrap_pre_wrap_seq_now_older");
    CK(wm(B, 1U, 540U, 3002, true, 0U, 2040U) == WMQ_FED && s_cap.weight_g == 3002, "wmq_wrap_continues");
}

static void test_boot_debounce(void)
{
    wm_setup();
    const char *A = "aabbccdd", *B = "55667788";
    CK(est(A, 100U, 4000, 1000U) == WMQ_FED, "wmq_boot_established");
    int n = s_cap_n;
    uint32_t last = weight_receiver_last_rx_ms();

    CK(wm(B, 5U, 100U, 6000, true, 0U, 1100U) == WMQ_PENDING_BOOT, "wmq_new_boot_first_pending");
    CK(wm(B, 7U, 110U, 6000, true, 0U, 1110U) == WMQ_PENDING_BOOT, "wmq_new_boot_nonconsecutive_stays_pending");
    CK(wm(B, 7U, 111U, 6000, true, 0U, 1111U) == WMQ_PENDING_BOOT, "wmq_new_boot_duplicate_does_not_confirm");
    CK(s_cap_n == n && weight_receiver_last_rx_ms() == last, "wmq_pending_boot_never_fed_or_renews");
    CK(wm(A, 102U, 600U, 4000, true, 0U, 1120U) == WMQ_PENDING_BOOT, "wmq_old_boot_after_switch_also_debounced");
    CK(wm(B, 20U, 100U, 6000, true, 0U, 1130U) == WMQ_PENDING_BOOT, "wmq_new_boot_restart_pending");
    CK(wm(B, 21U, 110U, 6100, true, 0U, 1140U) == WMQ_FED, "wmq_new_boot_accepted_after_two_consecutive");
    CK(s_cap.weight_g == 6100 && !s_cap.stable && strcmp(s_cap.boot_id, B) == 0, "wmq_new_boot_forced_unstable_on_first");
    CK(wm(B, 22U, 120U, 6100, true, 0U, 1150U) == WMQ_FED && s_cap.stable, "wmq_new_boot_then_stable_allowed");

    /* guard was re-based for the new sender clock: a small uptime does not trip it */
    CK(wm(B, 23U, 130U, 6100, true, 0U, 1160U) == WMQ_FED, "wmq_new_boot_transit_rebased");

    /* a new boot needs a MONOTONIC sender uptime between its two messages */
    const char *C = "0a0b0c0d";
    CK(wm(C, 200U, 700U, 5000, true, 0U, 1200U) == WMQ_PENDING_BOOT, "wmq_new_boot_clock_case_first");
    CK(wm(C, 201U, 650U, 5000, true, 0U, 1210U) == WMQ_PENDING_BOOT, "wmq_new_boot_uptime_regression_no_pair");

    /* the pair is on the sender's ONE counter: the consecutive message completes it */
    wm_setup();
    CK(wm(A, 10U, 500U, 4000, true, 0U, 1000U) == WMQ_PENDING_BOOT, "wmq_one_counter_first_pending");
    CK(wm(A, 11U, 510U, 4001, true, 0U, 1010U) == WMQ_FED, "wmq_one_counter_pair_accepted");
    CK(wm(A, 12U, 520U, 4002, true, 0U, 1020U) == WMQ_FED && s_cap.stable, "wmq_one_counter_established_keeps_stable");
    /* a different boot_id never completes another boot's pair */
    wm_setup();
    CK(wm(A, 20U, 500U, 4000, true, 0U, 1000U) == WMQ_PENDING_BOOT &&
       wm(B, 21U, 510U, 4000, true, 0U, 1010U) == WMQ_PENDING_BOOT, "wmq_other_boot_does_not_pair");
    /* a gap or duplicate never forms a pair */
    wm_setup();
    CK(wm(B, 100U, 600U, 5000, true, 0U, 1100U) == WMQ_PENDING_BOOT, "wmq_gap_case_first");
    CK(wm(B, 102U, 610U, 5000, true, 0U, 1110U) == WMQ_PENDING_BOOT, "wmq_gap_stays_pending");
    CK(wm(B, 102U, 611U, 5000, true, 0U, 1111U) == WMQ_PENDING_BOOT, "wmq_duplicate_does_not_confirm");
    CK(wm(B, 103U, 620U, 5000, true, 0U, 1120U) == WMQ_FED, "wmq_consecutive_after_gap_accepted");
}

/* ---- scale_id: only the configured expected scale is accepted ------------- */
static void test_scale_id(void)
{
    wm_setup();
    const char *A = "aabbccdd";
    weight_mqtt_stats_t st;
    CK(strcmp(weight_mqtt_expected_scale_id(), "SCALE1") == 0, "wmq_expected_scale_default_SCALE1");
    CK(wmu(A, 1U, 500U, "SCALE2", "UART2", 4000, true, 0U, 1000U) == WMQ_DROP_SCALE, "wmq_scale_mismatch_rejected");
    CK(wmu(A, 2U, 510U, "SCALE2", "UART2", 4000, true, 0U, 1010U) == WMQ_DROP_SCALE, "wmq_scale_mismatch_never_pairs");
    CK(s_cap_n == 0 && !weight_receiver_have_valid(), "wmq_scale_mismatch_feeds_nothing");
    weight_mqtt_stats(&st);
    CK(st.dropped_scale == 2U && st.fed == 0U && st.dropped_invalid == 0U, "wmq_scale_mismatch_counted");
    CK(wmu(A, 3U, 520U, "scale1", "UART2", 4000, true, 0U, 1020U) == WMQ_DROP_SCALE, "wmq_scale_id_case_sensitive");

    /* the expected scale is accepted; a mismatching frame in between changes no state */
    CK(wmu(A, 10U, 500U, "SCALE1", "UART1", 4000, true, 0U, 1100U) == WMQ_PENDING_BOOT, "wmq_expected_scale_first_pending");
    CK(wmu(A, 11U, 510U, "SCALE9", "UART1", 9999, true, 0U, 1110U) == WMQ_DROP_SCALE, "wmq_foreign_scale_between_pair_ignored");
    CK(wmu(A, 11U, 510U, "SCALE1", "UART1", 4001, true, 0U, 1111U) == WMQ_FED, "wmq_pair_completes_despite_foreign_frame");
    CK(s_cap.weight_g == 4001 && strcmp(s_cap.scale_id, "SCALE1") == 0, "wmq_foreign_frame_did_not_replace_weight");

    /* the sender's UART tag is NOT a pump: both tags yield the same single sample */
    CK(wmu(A, 12U, 520U, "SCALE1", "UART2", 4100, true, 0U, 1120U) == WMQ_FED && s_cap.weight_g == 4100, "wmq_uart2_tag_accepted");
    CK(wmu(A, 13U, 530U, "SCALE1", "UART1", 4200, true, 0U, 1130U) == WMQ_FED && s_cap.weight_g == 4200, "wmq_uart1_tag_accepted_same_stream");

    /* NVS-configured expected scale */
    wm_setup();
    CK(weight_mqtt_set_expected_scale_id("LAB-2_b"), "wmq_set_expected_scale_valid");
    CK(strcmp(weight_mqtt_expected_scale_id(), "LAB-2_b") == 0, "wmq_expected_scale_changed");
    CK(est(A, 1U, 4000, 1000U) == WMQ_DROP_SCALE, "wmq_default_scale_now_rejected");
    CK(wmu(A, 20U, 500U, "LAB-2_b", "UART2", 4000, true, 0U, 1100U) == WMQ_PENDING_BOOT &&
       wmu(A, 21U, 510U, "LAB-2_b", "UART2", 4000, true, 0U, 1110U) == WMQ_FED &&
       strcmp(s_cap.scale_id, "LAB-2_b") == 0, "wmq_configured_scale_accepted");

    /* invalid expectation: NO weight is accepted at all (fail safe) */
    wm_setup();
    CK(!weight_mqtt_set_expected_scale_id("bad id!") && !weight_mqtt_set_expected_scale_id("") &&
       !weight_mqtt_set_expected_scale_id("12345678901234567") && !weight_mqtt_set_expected_scale_id(NULL),
       "wmq_invalid_expected_scale_refused");
    CK(est(A, 1U, 4000, 1000U) == WMQ_DROP_SCALE && s_cap_n == 0 && !weight_receiver_have_valid(),
       "wmq_invalid_expected_scale_accepts_no_weight");

    /* changing the expectation drops the cache: the old scale's data cannot ride along */
    wm_setup();
    CK(est(A, 1U, 4000, 1000U) == WMQ_FED, "wmq_scale_change_setup");
    CK(weight_mqtt_set_expected_scale_id("SCALE7"), "wmq_scale_change_set");
    CK(wmu(A, 3U, 600U, "SCALE7", "UART2", 5000, true, 0U, 1100U) == WMQ_PENDING_BOOT,
       "wmq_scale_change_needs_fresh_pair");
}

/* One hand-off slot: there is one scale, the newest sample wins. */
static void test_weight_slots(void)
{
    weight_slot_sample_t s;
    CK(weight_slots_init(), "slots_init");
    CK(!weight_slots_take(&s), "slots_empty_returns_false");

    weight_msg_t a, b;
    memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
    a.weight_g = 1000; b.weight_g = 1500;
    weight_slots_post(&a, 100U);
    weight_slots_post(&b, 101U);
    CK(weight_slots_take(&s) && s.msg.weight_g == 1500 && s.acquired_ms == 101U && !weight_slots_take(&s),
       "slots_single_slot_keeps_only_newest");
    weight_slots_post(NULL, 1U);
    CK(!weight_slots_take(&s), "slots_null_post_ignored");
    weight_slots_post(&a, 0xFFFFFFFFU);
    weight_slots_post(&b, 3U);
    CK(weight_slots_take(&s) && s.acquired_ms == 3U && !weight_slots_take(&s), "slots_wrap_keeps_freshest");
    CK(weight_slots_init() && !weight_slots_take(&s), "slots_reinit_clears");
}

static void test_validation(void)
{
    wm_setup();
    const char *A = "aabbccdd";
    CK(est(A, 1U, 1000, 1000U) == WMQ_FED, "wmq_val_setup");
    uint32_t last = weight_receiver_last_rx_ms();
    int n = s_cap_n;
    uint32_t t = 1100U;

    /* age */
    CK(wm(A, 10U, t - 500U, 1000, true, 5001U, t) == WMQ_DROP_INVALID, "wmq_stale_age_5001_rejected");
    CK(wm(A, 11U, t - 500U, 1000, true, 5000U, t) == WMQ_FED, "wmq_age_5000_accepted");
    CK(weight_receiver_last_rx_ms() == t - 5000U, "wmq_age_backdates_freshness_like_ws");
    last = weight_receiver_last_rx_ms();
    n = s_cap_n;

    /* scale_id shape */
    CK(wmu(A, 12U, t - 500U, "", "UART2", 1000, true, 0U, t) == WMQ_DROP_INVALID, "wmq_empty_scale_id_rejected");
    CK(wmu(A, 12U, t - 500U, "SCALE 1", "UART2", 1000, true, 0U, t) == WMQ_DROP_INVALID, "wmq_scale_id_space_rejected");
    CK(wmu(A, 12U, t - 500U, "SCALE1234567890123", "UART2", 1000, true, 0U, t) == WMQ_DROP_INVALID, "wmq_scale_id_too_long_rejected");
    CK(wmu(A, 12U, t - 500U, "SCALE1", "UART3", 1000, true, 0U, t) == WMQ_DROP_INVALID, "wmq_bad_src_uart_rejected");
    CK(wmu(A, 12U, t - 500U, "SCALE1", "uart1", 1000, true, 0U, t) == WMQ_DROP_INVALID, "wmq_src_uart_case_sensitive");

    /* the old per-channel frame is never attributable to a pump */
    CK(raw("{\"schema_version\":1,\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"channel\":\"CH1\",\"scale_id\":\"SCALE1\","
           "\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_channel_field_rejected");

    /* required fields */
    CK(raw("{\"schema_version\":1,\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_missing_scale_id_rejected");
    CK(raw("{\"schema_version\":1,\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_missing_boot_id_rejected");
    CK(raw("{\"schema_version\":1,\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_missing_stable_rejected");
    CK(raw("{\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_missing_weight_rejected");
    CK(raw("{\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true}", t)
       == WMQ_DROP_INVALID, "wmq_missing_age_rejected");
    CK(raw("{\"boot_id\":\"aabbccdd\",\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_missing_seq_rejected");
    CK(raw("{\"boot_id\":\"aabbccdd\",\"seq\":12,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_missing_uptime_rejected");

    /* weight_g */
    CK(wm(A, 12U, t - 500U, -1, true, 0U, t) == WMQ_DROP_INVALID, "wmq_negative_weight_rejected");
    CK(wm(A, 12U, t - 500U, 100001, true, 0U, t) == WMQ_DROP_INVALID, "wmq_weight_over_100000_rejected");
#define PFX "{\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\","
    CK(raw(PFX "\"weight_g\":9500.5,\"stable\":true,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_fractional_weight_rejected");
    CK(raw(PFX "\"weight_g\":1e3,\"stable\":true,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_exponent_weight_rejected");
    CK(raw(PFX "\"weight_g\":NaN,\"stable\":true,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_nan_weight_rejected");
    CK(raw(PFX "\"weight_g\":\"1000\",\"stable\":true,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_string_weight_rejected");
    CK(raw(PFX "\"weight_g\":null,\"stable\":true,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_null_weight_rejected");
    CK(raw(PFX "\"weight_g\":1000,\"stable\":1,\"age_ms\":0}", t) == WMQ_DROP_INVALID, "wmq_non_boolean_stable_rejected");

    /* envelope */
    CK(raw("{\"schema_version\":2,\"boot_id\":\"aabbccdd\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_unknown_schema_version_rejected");
    CK(raw("{\"boot_id\":\"aabbcc\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_short_boot_id_rejected");
    CK(raw("{\"boot_id\":\"aabbccdz\",\"seq\":12,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_nonhex_boot_id_rejected");
    CK(raw("{\"boot_id\":\"aabbccdd\",\"seq\":12,\"seq\":13,\"uptime_ms\":600,\"scale_id\":\"SCALE1\",\"weight_g\":1000,\"stable\":true,\"age_ms\":0}", t)
       == WMQ_DROP_INVALID, "wmq_duplicate_key_rejected");
    CK(raw(PFX "\"weight_g\":1000,\"stable\":true,\"age_ms\":0,\"x\":{\"a\":1}}", t)
       == WMQ_DROP_INVALID, "wmq_nested_object_rejected");
    CK(raw("not json", t) == WMQ_DROP_INVALID, "wmq_garbage_rejected");
    CK(raw("{}", t) == WMQ_DROP_INVALID, "wmq_empty_object_rejected");
    CK(weight_mqtt_on_payload(NULL, 0U, t) == WMQ_DROP_INVALID, "wmq_null_payload_rejected");

    /* oversize: a valid message padded past 256 B */
    char big[400];
    int bn = snprintf(big, sizeof(big), PFX "\"weight_g\":1000,\"stable\":true,\"age_ms\":0,\"pad\":\"");
    while (bn < 290) big[bn++] = 'x';
    bn += snprintf(big + bn, sizeof(big) - (size_t)bn, "\"}");
    CK(bn > (int)WEIGHT_MQTT_PAYLOAD_MAX && weight_mqtt_on_payload(big, (size_t)bn, t) == WMQ_DROP_INVALID,
       "wmq_oversized_payload_rejected");
#undef PFX

    CK(s_cap_n == n && weight_receiver_last_rx_ms() == last, "wmq_every_reject_leaves_receiver_untouched");

    /* boundary values that ARE valid */
    CK(wm(A, 12U, t - 500U, 100000, true, 0U, t) == WMQ_FED && s_cap.weight_g == 100000, "wmq_weight_100000_accepted");
    CK(wm(A, 13U, t - 490U, 0, true, 0U, t + 10U) == WMQ_FED && s_cap.weight_g == 0, "wmq_weight_zero_accepted");
}

static void test_transit_guard(void)
{
    wm_setup();
    const char *A = "aabbccdd";
    /* offset = recv - uptime = 500 */
    CK(est(A, 1U, 1000, 1000U) == WMQ_FED, "wmq_transit_setup");
    CK(wm(A, 3U, 600U, 1000, true, 0U, 1100U) == WMQ_FED, "wmq_transit_same_offset_ok");
    CK(wm(A, 4U, 650U, 1000, true, 0U, 1200U) == WMQ_FED, "wmq_transit_plus_50_ok");
    CK(wm(A, 5U, 700U, 1000, true, 0U, 1500U) == WMQ_FED, "wmq_transit_plus_300_boundary_ok");
    uint32_t last = weight_receiver_last_rx_ms();
    int n = s_cap_n;
    CK(wm(A, 6U, 800U, 2222, true, 0U, 1701U) == WMQ_DROP_TRANSIT, "wmq_transit_over_300ms_rejected");
    CK(s_cap_n == n && weight_receiver_last_rx_ms() == last, "wmq_transit_reject_does_not_move_timestamp");
    CK(wm(A, 6U, 800U, 1000, true, 0U, 1300U) == WMQ_FED, "wmq_transit_recovers_when_delay_clears");
    /* a lower offset (faster path) is accepted and lowers the minimum */
    CK(wm(A, 7U, 900U, 1000, true, 0U, 1350U) == WMQ_FED, "wmq_transit_lower_offset_accepted");
    CK(wm(A, 8U, 1000U, 1000, true, 0U, 1760U) == WMQ_DROP_TRANSIT, "wmq_transit_new_minimum_applies");
    /* the window slides: after >40 s of nothing the old minimum is forgotten */
    CK(wm(A, 9U, 1000U, 1000, true, 0U, 60000U) == WMQ_FED, "wmq_transit_window_expires");
}

static void test_failsafe_on_silence(void)
{
    /* Legacy group policy (no actuation handler): the relay decision comes from the
     * unchanged weight_receiver, so silence under MQTT must still force OFF. */
    wm_setup();
    weight_receiver_set_actuation_handler(NULL);
    const char *A = "aabbccdd";
    CK(est(A, 1U, 12000, 1000U) == WMQ_FED, "wmq_failsafe_setup_fed");
    CK(all_on(), "wmq_failsafe_weight_above_trigger_energizes_via_receiver");
    weight_receiver_tick(1010U + WEIGHT_MESSAGE_TIMEOUT_MS);
    CK(all_on(), "wmq_failsafe_not_off_at_exactly_5000ms");
    weight_receiver_tick(1010U + WEIGHT_MESSAGE_TIMEOUT_MS + 1U);
    CK(all_off(), "wmq_silence_forces_relays_off_under_mqtt");
    CK(weight_receiver_link_state() == WEIGHT_LINK_TIMEOUT && !weight_receiver_have_valid(),
       "wmq_silence_marks_link_timeout_without_broker_input");

    /* a duplicate flood must not keep the link alive */
    wm_setup();
    weight_receiver_set_actuation_handler(NULL);
    CK(est(A, 1U, 12000, 1000U) == WMQ_FED, "wmq_dupflood_setup");
    for (uint32_t t = 2000U; t < 8000U; t += 100U) {
        (void)wm(A, 2U, 510U, 12000, true, 0U, t);   /* same seq every time */
    }
    weight_receiver_tick(1010U + WEIGHT_MESSAGE_TIMEOUT_MS + 1U);
    CK(all_off(), "wmq_duplicate_flood_cannot_renew_freshness");

    /* a flood of the WRONG scale cannot keep the link alive either */
    wm_setup();
    weight_receiver_set_actuation_handler(NULL);
    CK(est(A, 1U, 12000, 1000U) == WMQ_FED, "wmq_foreign_flood_setup");
    for (uint32_t t = 2000U; t < 8000U; t += 100U) {
        (void)wmu(A, 100U + t, t - 500U, "SCALE9", "UART2", 12000, true, 0U, t);
    }
    weight_receiver_tick(1010U + WEIGHT_MESSAGE_TIMEOUT_MS + 1U);
    CK(all_off(), "wmq_wrong_scale_flood_cannot_renew_freshness");

    /* MQTT selected but peer invalid: nothing is ever enabled, receiver stays stale */
    wm_setup();
    weight_receiver_set_actuation_handler(NULL);
    weight_mqtt_set_enabled(weight_mqtt_select("mqtt", "bad/peer") == WEIGHT_SEL_MQTT);
    CK(!weight_mqtt_enabled(), "wmq_invalid_peer_leaves_mqtt_weight_disabled");
    CK(est(A, 1U, 12000, 1000U) == WMQ_DROP_DISABLED && !weight_receiver_have_valid() && all_off(),
       "wmq_invalid_peer_weight_stays_stale_relays_off");
    weight_receiver_tick(1000U);
    CK(all_off(), "wmq_invalid_peer_tick_keeps_relays_off");
}

static void test_queue(void)
{
    static mqtt_link_queue_t q;
    CK(mqtt_link_queue_init(&q, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH), "wmq_q_init");
    const char *msg = "{\"seq\":1}";
    CK(mqtt_link_queue_push_weight(&q, msg, strlen(msg), strlen(msg), false, 1U) == MQTT_ENQ_INVALID,
       "wmq_q_weight_off_by_default_invalid");
    CK(mqtt_link_queue_weight_init(&q, MQTT_LINK_WRX_DEPTH), "wmq_q_weight_init");
    CK(mqtt_link_queue_push_weight(&q, msg, strlen(msg), strlen(msg), true, 1U) == MQTT_ENQ_RETAINED,
       "wmq_q_retained_ignored");
    CK(mqtt_link_queue_pending_weight(&q) == 0U, "wmq_q_retained_not_queued");
    CK(q.dropped_retained == 1U, "wmq_q_retained_counted");
    CK(mqtt_link_queue_push_weight(&q, msg, 3U, strlen(msg), false, 1U) == MQTT_ENQ_FRAGMENT,
       "wmq_q_fragment_refused");
    char big[MQTT_LINK_WRX_PAYLOAD_MAX + 8U];
    memset(big, 'x', sizeof(big));
    CK(mqtt_link_queue_push_weight(&q, big, MQTT_LINK_WRX_PAYLOAD_MAX + 1U, MQTT_LINK_WRX_PAYLOAD_MAX + 1U, false, 1U)
       == MQTT_ENQ_TOO_BIG, "wmq_q_oversized_refused");
    CK(mqtt_link_queue_push_weight(&q, big, MQTT_LINK_WRX_PAYLOAD_MAX, MQTT_LINK_WRX_PAYLOAD_MAX, false, 1U)
       == MQTT_ENQ_OK, "wmq_q_256_bytes_accepted");
    mqtt_link_queue_reset(&q);

    /* Fill the weight queue: further pushes drop (zero timeout) and are counted. */
    for (uint32_t i = 0U; i < MQTT_LINK_WRX_DEPTH; i++) {
        CK(mqtt_link_queue_push_weight(&q, msg, strlen(msg), strlen(msg), false, 10U + i) == MQTT_ENQ_OK,
           "wmq_q_fill_slot");
    }
    CK(mqtt_link_queue_push_weight(&q, msg, strlen(msg), strlen(msg), false, 99U) == MQTT_ENQ_FULL,
       "wmq_q_full_drops");
    CK(q.wrx_dropped_full == 1U, "wmq_q_full_counted");

    /* ...and the command queue is completely independent of that backlog. */
    CK(mqtt_link_queue_pending_rx(&q) == 0U, "wmq_q_cmd_queue_untouched_by_weight_flood");
    CK(mqtt_link_queue_push_rx(&q, "cas/dev/commands", 16U, "{}", 2U, 2U, false, 1U, 1) == MQTT_ENQ_OK,
       "wmq_q_command_still_accepted_while_weight_full");
    CK(q.rx_dropped_full == 0U, "wmq_q_command_not_dropped_by_weight_flood");

    mqtt_wrx_item_t it;
    CK(mqtt_link_queue_pop_weight(&q, &it, 0U) && it.recv_ms == 10U && strcmp(it.data, msg) == 0,
       "wmq_q_pop_fifo");
    mqtt_link_queue_deinit(&q);
    CK(mqtt_link_queue_push_weight(&q, msg, strlen(msg), strlen(msg), false, 1U) == MQTT_ENQ_INVALID,
       "wmq_q_deinit_refuses");
}

static void test_identity(void)
{
    const char *b = mqtt_link_boot_id();
    bool hex = b != NULL && strlen(b) == MQTT_LINK_BOOT_ID_LEN;
    for (size_t i = 0U; hex && i < MQTT_LINK_BOOT_ID_LEN; i++) {
        hex = (b[i] >= '0' && b[i] <= '9') || (b[i] >= 'a' && b[i] <= 'f');
    }
    CK(hex, "wmq_boot_id_is_8_lower_hex");
    CK(mqtt_link_boot_id() == b && strcmp(mqtt_link_boot_id(), b) == 0, "wmq_boot_id_stable_within_boot");
    char f[MQTT_LINK_BOOT_ID_LEN + 1U];
    mqtt_link_boot_id_format(f, 0x00ABCDEFU);
    CK(strcmp(f, "00abcdef") == 0, "wmq_boot_id_format_zero_padded");

    char birth[200];
    CK(mqtt_link_birth_build(birth, sizeof(birth), "00abcdef") && strlen(birth) <= 256U &&
       strstr(birth, "\"online\":true") && strstr(birth, "\"boot_id\":\"00abcdef\"") &&
       strstr(birth, "\"caps\":[\"cmd_mqtt\",\"weight_mqtt\"]"), "wmq_birth_has_boot_id_and_caps");
    CK(!mqtt_link_birth_build(birth, 20U, "00abcdef"), "wmq_birth_truncation_refused");
}

void test_weight_mqtt_run(void)
{
    test_select_and_default_off();
    test_order();
    test_boot_debounce();
    test_scale_id();
    test_weight_slots();
    test_validation();
    test_transit_guard();
    test_failsafe_on_silence();
    test_queue();
    test_identity();
    /* leave the receiver as a fresh, handler-less, stale instance */
    weight_receiver_set_actuation_handler(NULL);
    (void)weight_receiver_init();
    (void)relay_all_off();
    weight_mqtt_init();
}
