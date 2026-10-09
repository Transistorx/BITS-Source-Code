/* MQTT link + command dispatch tests (pure logic, no broker, no network).
 *
 * mqtt_link_core is exercised directly. The command path is driven the way the
 * firmware drives it: core RX queue -> telemetry_client_mqtt_intake ->
 * telemetry_client_mqtt_step -> ACK captured by a mock publish hook. The HTTP
 * transport is driven through telemetry_client_dispatch_command with a capture
 * ACK sink (the real HTTP ACK needs a server).
 *
 * QEMU has no relay expander, so a manual START is refused by the relay layer;
 * START tests therefore assert on the execution counters and the stored ACK,
 * not on the relay turning on. STOP, E-Stop, READY and the queue/profile
 * logic are fully observable.
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dual_dispense_controller.h"
#include "job_queue.h"
#include "mqtt_link_core.h"
#include "relay_driver.h"
#include "safety_manager.h"
#include "telemetry_client.h"
#include "weight_receiver.h"

extern void audit_check(bool condition, const char *name);
#define CK(cond, name) audit_check((cond), (name))

/* ---- mocks ---------------------------------------------------------------- */
typedef struct { char suffix[40]; char body[400]; int qos; bool retain; } pub_rec_t;
static pub_rec_t s_pubs[24];
static int s_pub_n;
static bool s_mqtt_connected;
static bool s_publish_accepts = true;

static bool mock_connected(void) { return s_mqtt_connected; }
static bool mock_publish(const char *suffix, const char *json, int qos, bool retain)
{
    if (!s_publish_accepts || s_pub_n >= 24) return false;
    pub_rec_t *p = &s_pubs[s_pub_n++];
    snprintf(p->suffix, sizeof(p->suffix), "%s", suffix);
    snprintf(p->body, sizeof(p->body), "%s", json);
    p->qos = qos;
    p->retain = retain;
    return true;
}

typedef struct { uint32_t id; char state[10]; uint32_t local_job; uint8_t ch; char error[100]; } ack_rec_t;
static ack_rec_t s_acks[24];
static int s_ack_n;
static void ack_sink(void *ctx, uint32_t id, const char *state, uint32_t lj, uint8_t ch,
                     const char *error)
{
    (void)ctx;
    if (s_ack_n >= 24) return;
    ack_rec_t *a = &s_acks[s_ack_n++];
    a->id = id;
    snprintf(a->state, sizeof(a->state), "%s", state);
    a->local_job = lj;
    a->ch = ch;
    snprintf(a->error, sizeof(a->error), "%s", error);
}

static const telemetry_mqtt_ops_t s_ops = {
    .connected = mock_connected, .publish = mock_publish, .rx_pop = NULL,
};

/* ---- builders (json.dumps-style spacing, as the MQTT bridge emits) --------- */
static char s_js[1100];
static mqtt_link_queue_t s_q;
static mqtt_rx_item_t s_item;

static const char *job_json(uint32_t id, const char *material, const char *chfield,
                            const char *extra, const char *pid, uint32_t pver,
                            const char *nid, uint32_t nver, double kp)
{
    snprintf(s_js, sizeof(s_js),
        "{\"command_id\": %lu, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": %s, "
        "\"issued_at\": \"2026-10-07T08:15:30.123Z\", \"ttl_ms\": 0, %s\"material_id\": \"%s\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"%s\", \"profile_version\": %lu, "
        "\"profile\": {\"profile_id\": \"%s\", \"version\": %lu, \"kp\": %.4f, \"ki\": 0.0, "
        "\"kd\": 0.0, \"tolerance_g\": 20, \"max_overshoot_g\": 100, \"max_duration_ms\": 120000, "
        "\"window_ms\": 500, \"min_on_ms\": 40, \"min_off_ms\": 40}}",
        (unsigned long)id, chfield, extra, material, pid, (unsigned long)pver, nid,
        (unsigned long)nver, kp);
    return s_js;
}

static const char *cmd_json(uint32_t id, const char *type, const char *chfield, unsigned long ttl,
                            const char *extra)
{
    snprintf(s_js, sizeof(s_js),
        "{\"command_id\": %lu, \"type\": \"%s\", \"command_type\": \"%s\", \"channel_id\": %s, "
        "\"issued_at\": \"2026-10-07T08:15:30.123Z\", \"ttl_ms\": %lu%s}",
        (unsigned long)id, type, type, chfield, ttl, extra);
    return s_js;
}

static const char *profile_json(uint32_t id, const char *ch, const char *pid, uint32_t ver,
                                const char *material, const char *extra, double kp)
{
    snprintf(s_js, sizeof(s_js),
        "{\"command_id\": %lu, \"type\": \"PROFILE\", \"command_type\": \"PROFILE\", "
        "\"channel_id\": \"%s\", \"ttl_ms\": 0, \"profile_id\": \"%s\", \"version\": %lu, "
        "\"material_id\": \"%s\", \"target_g\": 5000, %s\"kp\": %.4f, \"ki\": 0.0, \"kd\": 0.0, "
        "\"tolerance_g\": 20, \"max_overshoot_g\": 100, \"max_duration_ms\": 120000, "
        "\"window_ms\": 500, \"min_on_ms\": 40, \"min_off_ms\": 40, \"settle_time_ms\": 200, "
        "\"inflight_comp_g\": 0}",
        (unsigned long)id, ch, pid, (unsigned long)ver, material, extra, kp);
    return s_js;
}

/* Deliver one message the way the firmware does: esp-mqtt callback push into
 * the core RX queue, then the command task pops it into intake. */
static telemetry_intake_result_t mq_send(const char *json, uint32_t recv_ms)
{
    static const char topic[] = "cas/relay-test/commands";
    size_t n = strlen(json);
    if (mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), json, n, n, false, recv_ms,
                                esp_timer_get_time()) != MQTT_ENQ_OK) {
        return TELEMETRY_INTAKE_INVALID;
    }
    if (!mqtt_link_queue_pop_rx(&s_q, &s_item, 0U)) return TELEMETRY_INTAKE_INVALID;
    return telemetry_client_mqtt_intake(s_item.data, s_item.len, false, s_item.recv_ms,
                                        s_item.recv_us);
}

static bool pub_has(int i, const char *needle)
{
    return i >= 0 && i < s_pub_n && strstr(s_pubs[i].body, needle) != NULL;
}

static void mq_reset_io(void)
{
    s_pub_n = 0;
    s_ack_n = 0;
    s_publish_accepts = true;
    memset(s_pubs, 0, sizeof(s_pubs));
    memset(s_acks, 0, sizeof(s_acks));
}

/* Fresh controller + the same 5000 g slots the READY-gate tests use. */
static void mq_setup(void)
{
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    (void)dual_dispense_controller_set_pending_profile_staged(1, "or-slot1", 31, 5000,
          0.99f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15, 200, 0);
    (void)dual_dispense_controller_set_pending_profile_staged(2, "or-slot2", 32, 5000,
          0.98f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15, 200, 0);
    telemetry_client_test_reset_commands();
    telemetry_client_test_reset_ready();
    mq_reset_io();
}

static void mq_feed(uint32_t now, int32_t w)
{
    weight_msg_t m;
    memset(&m, 0, sizeof(m));
    m.has_weight1 = true;
    m.weight1_g = w;
    m.has_stable1 = true;
    m.stable1 = true;
    m.weight_g = w;
    dual_dispense_controller_on_weight(&m, now);
}

static uint32_t jobs_queued(void)
{
    static dispense_job_t jobs[JOB_QUEUE_MAX];
    return job_queue_snapshot(jobs, JOB_QUEUE_MAX);
}

static bool relays_off(void)
{
    return !relay_get_state(RELAY_INDEX_COARSE) && !relay_get_state(RELAY_INDEX_FINE);
}

static int s_flushed;
static void flush_count_cb(const mqtt_tx_item_t *it, void *c) { (void)it; (void)c; s_flushed++; }

/* ---- core: topics, presence, backoff, queues, redaction -------------------- */
static void test_live_body(void)
{
    dual_channel_snapshot_t s[2];
    memset(s, 0, sizeof(s));
    char b[300];
    s[0].have_weight = true; s[0].current_weight_g = 1234; s[0].weight_age_ms = 40; s[0].stable = true;
    s[1].have_weight = true; s[1].current_weight_g = 99; s[1].weight_age_ms = 40;
    int n = telemetry_client_live_body(b, sizeof(b), 5000, s);
    CK(n > 0 && (size_t)n == strlen(b) && strstr(b, "\"channel_id\":\"CH1\",\"weight_g\":1234,\"weight_valid\":true") &&
       strstr(b, "\"channel_id\":\"CH2\",\"weight_g\":99,\"weight_valid\":true"), "live_body_valid_weight_integer");
    s[1].have_weight = false; s[1].current_weight_g = 777; /* stale last-known value must not leak */
    n = telemetry_client_live_body(b, sizeof(b), 5000, s);
    CK(n > 0 && strstr(b, "\"channel_id\":\"CH2\",\"weight_g\":null,\"weight_valid\":false") && !strstr(b, "777"),
       "live_body_no_weight_is_null");
    s[1].have_weight = true; s[1].weight_age_ms = WEIGHT_MESSAGE_TIMEOUT_MS + 1U;
    n = telemetry_client_live_body(b, sizeof(b), 5000, s);
    CK(n > 0 && strstr(b, "\"channel_id\":\"CH2\",\"weight_g\":null,\"weight_valid\":false") && !strstr(b, "777"),
       "live_body_stale_weight_is_null");
    s[1].weight_age_ms = UINT32_MAX;
    n = telemetry_client_live_body(b, sizeof(b), 5000, s);
    CK(n > 0 && strstr(b, "\"weight_g\":null,\"weight_valid\":false,\"weight_age_ms\":null"), "live_body_unknown_age_is_null");
    s[1].weight_age_ms = 40; s[1].current_weight_g = -5;
    n = telemetry_client_live_body(b, sizeof(b), 5000, s);
    CK(n > 0 && strstr(b, "\"weight_g\":-5,"), "live_body_negative_weight");
    /* worst case: max-width numbers on both channels */
    for (int i = 0; i < 2; i++) {
        s[i].have_weight = true; s[i].current_weight_g = INT32_MIN; s[i].weight_age_ms = WEIGHT_MESSAGE_TIMEOUT_MS;
        s[i].stable = true;
    }
    n = telemetry_client_live_body(b, sizeof(b), UINT32_MAX, s);
    CK(n > 0 && n < 300 && b[n - 1] == '}' && (size_t)n == strlen(b), "live_body_worst_case_under_300");
    /* age == timeout is still valid (<=); changing the builder to < must fail this */
    CK(strstr(b, "\"weight_g\":-2147483648,\"weight_valid\":true") != NULL, "live_body_age_at_timeout_is_valid");
    /* exact snapshot: valid CH1, invalid CH2 (stable flag set but must print false) */
    memset(s, 0, sizeof(s));
    s[0].have_weight = true; s[0].current_weight_g = 1234; s[0].weight_age_ms = 40; s[0].stable = true;
    s[1].have_weight = false; s[1].current_weight_g = 777; s[1].weight_age_ms = 40; s[1].stable = true;
    n = telemetry_client_live_body(b, sizeof(b), 5000, s);
    CK(n > 0 && !strcmp(b, "{\"uptime_ms\":5000,\"channels\":["
       "{\"channel_id\":\"CH1\",\"weight_g\":1234,\"weight_valid\":true,\"weight_age_ms\":40,\"stable\":true},"
       "{\"channel_id\":\"CH2\",\"weight_g\":null,\"weight_valid\":false,\"weight_age_ms\":40,\"stable\":false}]}"),
       "live_body_exact_snapshot");
    CK(n > 0 && strstr(b, "\"weight_valid\":false,\"weight_age_ms\":40,\"stable\":false") != NULL, "live_body_invalid_not_stable");
    for (int i = 0; i < 2; i++) {
        s[i].have_weight = true; s[i].current_weight_g = INT32_MIN; s[i].weight_age_ms = WEIGHT_MESSAGE_TIMEOUT_MS;
        s[i].stable = true;
    }
    n = telemetry_client_live_body(b, sizeof(b), UINT32_MAX, s);
    char tiny[100];
    memset(tiny, 'x', sizeof(tiny));
    CK(telemetry_client_live_body(tiny, sizeof(tiny), 5000, s) == -1, "live_body_truncation_returns_error");
    int fit = n;
    int r_short = telemetry_client_live_body(b, (size_t)fit, UINT32_MAX, s);
    int r_fit = telemetry_client_live_body(b, (size_t)fit + 1U, UINT32_MAX, s);
    CK(r_short == -1 && r_fit == fit, "live_body_exact_fit_boundary");
    /* the suffix telemetry_client publishes on is the one mqtt_link routes to the live queue */
    CK(!strcmp(MQTT_SUFFIX_LIVE, "telemetry/live"), "live_suffix_macro_value");
}

static void test_core(void)
{
    char t[MQTT_LINK_TOPIC_MAX];
    CK(mqtt_link_topic(t, sizeof(t), "relay-1", MQTT_SUFFIX_SAMPLES) &&
       !strcmp(t, "cas/relay-1/telemetry/samples"), "mqtt_topic_samples");
    CK(mqtt_link_topic(t, sizeof(t), "relay-1", MQTT_SUFFIX_STATUS) &&
       !strcmp(t, "cas/relay-1/telemetry/status"), "mqtt_topic_status");
    CK(mqtt_link_topic(t, sizeof(t), "relay-1", MQTT_SUFFIX_EVENTS) &&
       !strcmp(t, "cas/relay-1/telemetry/events"), "mqtt_topic_events");
    CK(mqtt_link_topic(t, sizeof(t), "relay-1", MQTT_SUFFIX_LIVE) &&
       !strcmp(t, "cas/relay-1/telemetry/live"), "mqtt_topic_live");
    CK(mqtt_link_topic(t, sizeof(t), "relay-1", MQTT_SUFFIX_COMMANDS) &&
       !strcmp(t, "cas/relay-1/commands"), "mqtt_topic_commands");
    CK(mqtt_link_topic(t, sizeof(t), "relay-1", MQTT_SUFFIX_ACK) &&
       !strcmp(t, "cas/relay-1/commands/ack"), "mqtt_topic_ack");
    CK(!mqtt_link_topic(t, sizeof(t), "a/b", "x") && !mqtt_link_topic(t, sizeof(t), "", "x") &&
       !mqtt_link_topic(t, sizeof(t), "has space", "x") && !mqtt_link_topic(t, sizeof(t), "a#", "x") &&
       !mqtt_link_topic(t, sizeof(t), "a+b", "x") && !mqtt_link_topic(t, sizeof(t), NULL, "x"),
       "mqtt_topic_rejects_unsafe_device_ids");
    char long_id[80];
    memset(long_id, 'a', 65);
    long_id[65] = '\0';
    CK(!mqtt_link_device_id_valid(long_id), "mqtt_device_id_65_chars_rejected");
    long_id[64] = '\0';
    CK(mqtt_link_device_id_valid(long_id), "mqtt_device_id_64_chars_ok");

    mqtt_link_presence_t p;
    CK(mqtt_link_presence_plan(&p, "relay-1"), "mqtt_presence_plan_ok");
    CK(!strcmp(p.lwt_topic, "cas/relay-1/status") && !strcmp(p.birth_topic, p.lwt_topic),
       "mqtt_lwt_and_birth_share_retained_presence_topic");
    CK(!strcmp(p.lwt_payload, "{\"online\":false}") &&
       !strcmp(p.birth_payload, "{\"online\":true}"), "mqtt_lwt_offline_birth_online_payloads");
    CK(!strcmp(p.graceful_payload, "{\"online\":false,\"graceful\":true}"),
       "mqtt_graceful_payload_is_offline_not_online");
    CK(p.qos == 1 && p.retain, "mqtt_presence_is_qos1_retained");

    /* Reconnect: the broker forgets a clean session, so EVERY connect plan
     * subscribes again to the commands topic at QoS 1, then sends birth. */
    mqtt_link_connect_plan_t c1, c2;
    CK(mqtt_link_connect_plan(&c1, "relay-1") && mqtt_link_connect_plan(&c2, "relay-1"),
       "mqtt_connect_plan_ok");
    CK(!strcmp(c1.subscribe_topic, "cas/relay-1/commands") && c1.subscribe_qos == 1 &&
       !strcmp(c1.subscribe_topic, c2.subscribe_topic) && c1.subscribe_qos == c2.subscribe_qos,
       "mqtt_every_connect_resubscribes_commands_qos1");
    CK(!strcmp(c1.presence.birth_payload, "{\"online\":true}"),
       "mqtt_birth_follows_each_connect");

    CK(mqtt_link_backoff_ms(0) == 1000 && mqtt_link_backoff_ms(1) == 2000 &&
       mqtt_link_backoff_ms(2) == 4000 && mqtt_link_backoff_ms(4) == 16000 &&
       mqtt_link_backoff_ms(5) == 30000 && mqtt_link_backoff_ms(40) == 30000,
       "mqtt_backoff_doubles_to_30s");
    mqtt_link_reconnect_t rc;
    mqtt_link_reconnect_init(&rc);
    CK(mqtt_link_reconnect_on_disconnect(&rc, 1000000) == 1000, "mqtt_reconnect_first_wait_1s");
    CK(!mqtt_link_reconnect_due(&rc, 1500000), "mqtt_reconnect_not_due_early");
    CK(mqtt_link_reconnect_due(&rc, 2000000) && !mqtt_link_reconnect_due(&rc, 2100000),
       "mqtt_reconnect_due_exactly_once");
    CK(mqtt_link_reconnect_on_disconnect(&rc, 3000000) == 2000 &&
       mqtt_link_reconnect_on_disconnect(&rc, 4000000) == 4000, "mqtt_reconnect_backoff_grows");
    mqtt_link_reconnect_on_connected(&rc);
    CK(mqtt_link_reconnect_on_disconnect(&rc, 5000000) == 1000,
       "mqtt_reconnect_backoff_resets_after_connect");
    for (int i = 0; i < 100; i++) (void)mqtt_link_reconnect_on_disconnect(&rc, 6000000);
    CK(mqtt_link_reconnect_on_disconnect(&rc, 6000000) == 30000,
       "mqtt_reconnect_attempt_counter_saturates");

    /* RX queue: copy, order, retained/fragment/oversize refused, never blocks. */
    CK(mqtt_link_queue_init(&s_q, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH), "mqtt_queue_init");
    static const char topic[] = "cas/relay-1/commands";
    const char *m = "{\"command_id\": 1}";
    CK(mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), m, strlen(m), strlen(m), false, 1234,
                               5678) == MQTT_ENQ_OK, "mqtt_rx_push_ok");
    CK(mqtt_link_queue_pop_rx(&s_q, &s_item, 0) && !strcmp(s_item.data, m) &&
       !strcmp(s_item.topic, topic) && s_item.len == strlen(m) && s_item.recv_ms == 1234 &&
       s_item.recv_us == 5678, "mqtt_rx_pop_roundtrip_keeps_receipt_times");
    CK(mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), m, strlen(m), strlen(m), true, 1, 1) ==
       MQTT_ENQ_RETAINED && mqtt_link_queue_pending_rx(&s_q) == 0, "mqtt_rx_retained_ignored");
    CK(mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), m, 5, strlen(m), false, 1, 1) ==
       MQTT_ENQ_FRAGMENT && mqtt_link_queue_pending_rx(&s_q) == 0, "mqtt_rx_fragment_ignored");
    static char big[MQTT_LINK_RX_PAYLOAD_MAX + 8U];
    memset(big, 'x', sizeof(big));
    CK(mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), big, MQTT_LINK_RX_PAYLOAD_MAX + 1U,
                               MQTT_LINK_RX_PAYLOAD_MAX + 1U, false, 1, 1) == MQTT_ENQ_TOO_BIG &&
       mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), big, MQTT_LINK_RX_PAYLOAD_MAX,
                               MQTT_LINK_RX_PAYLOAD_MAX, false, 1, 1) == MQTT_ENQ_OK,
       "mqtt_rx_oversize_dropped_max_accepted");
    (void)mqtt_link_queue_pop_rx(&s_q, &s_item, 0);
    CK(mqtt_link_queue_push_rx(&s_q, topic, MQTT_LINK_TOPIC_MAX, m, strlen(m), strlen(m), false, 1,
                               1) == MQTT_ENQ_TOO_BIG, "mqtt_rx_long_topic_dropped");
    CK(mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), NULL, 0, 0, false, 1, 1) ==
       MQTT_ENQ_INVALID, "mqtt_rx_null_payload_invalid");
    char one[40];
    int ok_pushes = 0;
    int64_t t0 = esp_timer_get_time();
    for (unsigned i = 0; i < MQTT_LINK_RX_DEPTH + 4U; i++) {
        snprintf(one, sizeof(one), "{\"command_id\": %u}", i + 1U);
        if (mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), one, strlen(one), strlen(one),
                                    false, i, i) == MQTT_ENQ_OK) ok_pushes++;
    }
    int64_t spent = esp_timer_get_time() - t0;
    CK(ok_pushes == (int)MQTT_LINK_RX_DEPTH && s_q.rx_dropped_full == 4U,
       "mqtt_rx_saturation_drops_excess_counted");
    CK(spent < 500000, "mqtt_rx_saturation_never_blocks_callback");
    bool fifo = true;
    for (unsigned i = 0; i < MQTT_LINK_RX_DEPTH; i++) {
        snprintf(one, sizeof(one), "{\"command_id\": %u}", i + 1U);
        if (!mqtt_link_queue_pop_rx(&s_q, &s_item, 0) || strcmp(s_item.data, one) != 0) fifo = false;
    }
    CK(fifo && mqtt_link_queue_pending_rx(&s_q) == 0, "mqtt_rx_fifo_order_kept_under_saturation");

    /* TX queue: heap bodies, QoS0 capped so ACK slots stay free. */
    CK(mqtt_link_queue_push_pub(&s_q, "cas/relay-1/commands/ack", "{\"command_id\":1}", 1, false) ==
       MQTT_ENQ_OK, "mqtt_tx_ack_qos1_ok");
    mqtt_tx_item_t tx;
    CK(mqtt_link_queue_pop_tx(&s_q, &tx, 0) && tx.qos == 1 && !tx.retain && tx.data != NULL &&
       !strcmp(tx.data, "{\"command_id\":1}") && !strcmp(tx.topic, "cas/relay-1/commands/ack"),
       "mqtt_tx_roundtrip");
    mqtt_link_tx_item_free(&tx);
    CK(tx.data == NULL, "mqtt_tx_item_free_clears_pointer");
    int q0 = 0;
    for (unsigned i = 0; i < MQTT_LINK_TX_DEPTH; i++) {
        if (mqtt_link_queue_push_pub(&s_q, "cas/relay-1/telemetry/status", "{}", 0, false) ==
            MQTT_ENQ_OK) q0++;
    }
    CK(q0 == (int)MQTT_LINK_TX_QOS0_LIMIT, "mqtt_tx_qos0_telemetry_capped");
    CK(mqtt_link_queue_push_pub(&s_q, "cas/relay-1/commands/ack", "{\"command_id\":2}", 1, false) ==
       MQTT_ENQ_OK, "mqtt_tx_ack_still_accepted_when_telemetry_saturated");
    mqtt_link_queue_reset(&s_q);
    uint32_t full0 = s_q.tx_dropped_full, live0 = s_q.tx_dropped_live;
    int lv = 0;
    for (unsigned i = 0; i < MQTT_LINK_TX_DEPTH; i++) {
        if (mqtt_link_queue_push_live(&s_q, "cas/relay-1/telemetry/live", "{}") == MQTT_ENQ_OK) lv++;
    }
    CK(lv == (int)MQTT_LINK_TX_LIVE_LIMIT, "mqtt_tx_live_capped_below_qos0_limit");
    CK(s_q.tx_dropped_live - live0 == MQTT_LINK_TX_DEPTH - MQTT_LINK_TX_LIVE_LIMIT &&
       s_q.tx_dropped_full == full0, "mqtt_tx_live_drops_counted_separately");
    CK(mqtt_link_queue_push_pub(&s_q, "cas/relay-1/telemetry/events", "{}", 0, false) == MQTT_ENQ_OK,
       "mqtt_tx_events_still_accepted_when_live_saturated");
    CK(mqtt_link_queue_pop_tx(&s_q, &tx, 0) && tx.qos == 0 && !tx.retain &&
       !strcmp(tx.topic, "cas/relay-1/telemetry/live"), "mqtt_tx_live_qos0_not_retained");
    mqtt_link_tx_item_free(&tx);
    static char tbig[MQTT_LINK_TX_PAYLOAD_MAX + 4U];
    memset(tbig, 'y', sizeof(tbig));
    tbig[MQTT_LINK_TX_PAYLOAD_MAX + 1U] = '\0';
    mqtt_link_queue_reset(&s_q);
    CK(mqtt_link_queue_push_pub(&s_q, "t", tbig, 1, false) == MQTT_ENQ_TOO_BIG, "mqtt_tx_oversize_refused");
    tbig[MQTT_LINK_TX_PAYLOAD_MAX] = '\0';
    CK(mqtt_link_queue_push_pub(&s_q, "t", tbig, 1, false) == MQTT_ENQ_OK, "mqtt_tx_max_body_accepted");
    mqtt_link_queue_reset(&s_q);
    CK(mqtt_link_queue_pending_tx(&s_q) == 0 && mqtt_link_queue_pending_rx(&s_q) == 0,
       "mqtt_queue_reset_drops_pending");
    for (int i = 0; i < 3; i++) (void)mqtt_link_queue_push_pub(&s_q, "t", "{}", 1, false);
    s_flushed = 0;
    CK(mqtt_link_queue_flush_tx(&s_q, 2, flush_count_cb, NULL) == 2 &&
       s_flushed == 2 && mqtt_link_queue_pending_tx(&s_q) == 1, "mqtt_flush_is_bounded");
    mqtt_link_queue_deinit(&s_q);
    CK(mqtt_link_queue_push_pub(&s_q, "t", "{}", 1, false) == MQTT_ENQ_INVALID &&
       mqtt_link_queue_push_rx(&s_q, topic, strlen(topic), m, strlen(m), strlen(m), false, 1, 1) ==
       MQTT_ENQ_INVALID, "mqtt_queue_refuses_after_deinit");
    mqtt_link_queue_deinit(&s_q);

    mqtt_link_lc_t lc;
    mqtt_link_lc_init(&lc);
    CK(!mqtt_link_lc_begin_stop(&lc) && mqtt_link_lc_start(&lc) && !mqtt_link_lc_start(&lc) &&
       mqtt_link_lc_running(&lc), "mqtt_lifecycle_start_once");
    CK(mqtt_link_lc_begin_stop(&lc) && !mqtt_link_lc_begin_stop(&lc) && !mqtt_link_lc_start(&lc),
       "mqtt_lifecycle_stop_idempotent_and_blocks_restart");
    mqtt_link_lc_end_stop(&lc);
    CK(mqtt_link_lc_start(&lc), "mqtt_lifecycle_restartable");

    /* Secrets: a broker URI with credentials is never shown with them. */
    char shown[96];
    mqtt_link_uri_redact(shown, sizeof(shown), "mqtt://relay:s3cr3t@192.168.137.1:1883");
    CK(!strcmp(shown, "mqtt://192.168.137.1:1883") && !strstr(shown, "s3cr3t") && !strstr(shown, "relay:"),
       "mqtt_redact_strips_user_and_password");
    mqtt_link_uri_redact(shown, sizeof(shown), "mqtt://u:p@ss@host:1883");
    CK(!strcmp(shown, "mqtt://host:1883") && !strstr(shown, "ss"), "mqtt_redact_password_containing_at");
    mqtt_link_uri_redact(shown, sizeof(shown), "mqtt://192.168.137.1:1883");
    CK(!strcmp(shown, "mqtt://192.168.137.1:1883"), "mqtt_redact_leaves_plain_uri");
    mqtt_link_uri_redact(shown, sizeof(shown), "mqtt://host/path@x");
    CK(!strcmp(shown, "mqtt://host/path@x"), "mqtt_redact_ignores_at_in_path");
    mqtt_link_uri_redact(shown, 12, "mqtt://user:topsecret@host:1883");
    CK(strlen(shown) < 12 && !strstr(shown, "topsecret"), "mqtt_redact_truncation_never_leaks");
}

/* ---- command path over MQTT ------------------------------------------------- */
static void test_commands(void)
{
    telemetry_client_set_mqtt_ops(&s_ops);
    CK(mqtt_link_queue_init(&s_q, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH), "mqttcmd_queue_init");
    telemetry_cmd_stats_t st;
    uint32_t t = 900000U;

    /* 1. A JOB over MQTT is queued exactly like the HTTP route, ACK on MQTT. */
    mq_setup();
    CK(mq_send(job_json(501, "M1", "null", "", "tune-a", 3, "tune-a", 3, 0.0025), t) ==
       TELEMETRY_INTAKE_NORMAL, "mqttcmd_job_staged_normal");
    CK(telemetry_client_mqtt_staged() == 1U && jobs_queued() == 0U,
       "mqttcmd_intake_does_not_execute");
    CK(telemetry_client_mqtt_step(t + 5U) && jobs_queued() == 1U, "mqttcmd_job_dispatched_on_step");
    CK(s_pub_n == 1 && !strcmp(s_pubs[0].suffix, "commands/ack") && s_pubs[0].qos == 1 &&
       !s_pubs[0].retain, "mqttcmd_ack_goes_to_mqtt_ack_topic_qos1_not_retained");
    CK(pub_has(0, "\"command_id\":501") && pub_has(0, "\"state\":\"QUEUED\"") &&
       !pub_has(0, "\"local_job_id\":null") && pub_has(0, "\"channel_id\":null"),
       "mqttcmd_ack_carries_id_state_and_local_job");
    {
        static dispense_job_t jobs[JOB_QUEUE_MAX];
        uint32_t n = job_queue_snapshot(jobs, JOB_QUEUE_MAX);
        char want[40];
        snprintf(want, sizeof(want), "\"local_job_id\":%lu", (unsigned long)jobs[0].id);
        CK(n == 1U && jobs[0].material_id == 1U && jobs[0].remote_command_id == 501U &&
           jobs[0].pin.valid && jobs[0].pin.version == 3U && !strcmp(jobs[0].pin.profile_id, "tune-a") &&
           pub_has(0, want), "mqttcmd_job_record_matches_ack_correlation");
    }
    CK(s_ack_n == 0, "mqttcmd_mqtt_command_never_acks_over_http");

    /* 2. ACK correlation across several commands: each ack names its own id. */
    mq_setup();
    (void)mq_send(job_json(511, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11), t);
    (void)mq_send(job_json(512, "M2", "null", "", "tune-b", 2, "tune-b", 2, 0.22), t);
    (void)mq_send(cmd_json(513, "HOLD", "null", 0, ", \"remote_command_id\": 512"), t);
    (void)mq_send(cmd_json(514, "BOGUS", "null", 0, ""), t);
    for (int i = 0; i < 4; i++) (void)telemetry_client_mqtt_step(t + 10U + (uint32_t)i);
    CK(s_pub_n == 4 && pub_has(0, "\"command_id\":511") && pub_has(1, "\"command_id\":512") &&
       pub_has(2, "\"command_id\":513") && pub_has(3, "\"command_id\":514"),
       "mqttcmd_acks_in_order_each_with_own_id");
    CK(pub_has(2, "\"state\":\"APPLIED\"") && pub_has(3, "\"state\":\"FAILED\"") &&
       pub_has(3, "unknown command") && pub_has(3, "\"reason\":\"unknown command\""),
       "mqttcmd_ack_states_applied_and_failed_with_reason");
    CK(telemetry_client_mqtt_intake("{\"type\":\"JOB\"}", 14, false, t, 0) == TELEMETRY_INTAKE_INVALID &&
       telemetry_client_mqtt_intake("", 0, false, t, 0) == TELEMETRY_INTAKE_INVALID &&
       telemetry_client_mqtt_intake("{\"command_id\": 0}", 17, false, t, 0) == TELEMETRY_INTAKE_INVALID,
       "mqttcmd_missing_or_zero_command_id_dropped");

    /* 3. Same id over HTTP and MQTT runs once; the second transport gets the
     *    stored outcome back on its own transport. */
    mq_setup();
    const char *j601 = job_json(601, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11);
    telemetry_client_dispatch_command(j601, TELEMETRY_CMD_HTTP, t, t + 1U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED") && jobs_queued() == 1U,
       "dup_http_first_run_queues_job");
    uint32_t first_local = s_acks[0].local_job;
    static char j601_copy[1100];
    snprintf(j601_copy, sizeof(j601_copy), "%s", j601);
    (void)mq_send(j601_copy, t + 2U);
    (void)telemetry_client_mqtt_step(t + 3U);
    telemetry_client_cmd_stats(&st);
    CK(jobs_queued() == 1U && st.dup_reacked == 1U, "dup_mqtt_after_http_not_run_again");
    CK(s_pub_n == 1 && pub_has(0, "\"command_id\":601") && pub_has(0, "\"state\":\"QUEUED\""),
       "dup_mqtt_reacks_original_outcome_on_mqtt");
    {
        char want[40];
        snprintf(want, sizeof(want), "\"local_job_id\":%lu", (unsigned long)first_local);
        CK(pub_has(0, want), "dup_reack_carries_original_local_job_id");
    }
    CK(s_ack_n == 1, "dup_reack_stays_on_arrival_transport");
    telemetry_client_dispatch_command(j601_copy, TELEMETRY_CMD_HTTP, t, t + 9U, ack_sink, NULL);
    CK(jobs_queued() == 1U && s_ack_n == 2 && !strcmp(s_acks[1].state, "QUEUED") &&
       s_acks[1].local_job == first_local, "dup_http_redelivery_reacks_without_rerun");
    /* reverse order: MQTT first, then HTTP */
    mq_setup();
    (void)mq_send(job_json(602, "M2", "null", "", "tune-b", 2, "tune-b", 2, 0.22), t);
    (void)telemetry_client_mqtt_step(t + 1U);
    telemetry_client_dispatch_command(s_js, TELEMETRY_CMD_HTTP, t, t + 2U, ack_sink, NULL);
    CK(jobs_queued() == 1U && s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED"),
       "dup_http_after_mqtt_not_run_again");

    /* 4. ttl_ms: no wall clock, so expire when not applied within ttl of receipt. */
    mq_setup();
    (void)relay_init_all();
    telemetry_client_dispatch_command(cmd_json(701, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, 1000U, 20000U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED") && strstr(s_acks[0].error, "expired") &&
       st.manual_start_exec == 0U && st.expired == 1U, "ttl_expired_pump_start_never_executes");
    telemetry_client_dispatch_command(cmd_json(702, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, 1000U, 5000U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 1U, "ttl_fresh_pump_start_is_attempted");
    telemetry_client_dispatch_command(cmd_json(703, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, 1000U, 11000U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 2U && st.expired == 1U, "ttl_exactly_at_ttl_is_still_valid");
    telemetry_client_dispatch_command(cmd_json(704, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, 1000U, 11001U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.expired == 2U && st.manual_start_exec == 2U, "ttl_one_ms_over_expires");
    telemetry_client_dispatch_command(job_json(705, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_MQTT, 1000U, 9000000U, ack_sink, NULL);
    CK(jobs_queued() == 1U, "ttl_zero_means_no_expiry");
    telemetry_client_dispatch_command(cmd_json(706, "ESTOP", "null", 1, ""), TELEMETRY_CMD_MQTT,
                                      1000U, 9000000U, ack_sink, NULL);
    CK(safety_manager_fault() == SAFETY_EMERGENCY_STOP, "ttl_never_expires_a_stop");
    {
        /* An expired id stays failed: a later redelivery must not run it. */
        int before = s_ack_n;
        telemetry_client_dispatch_command(cmd_json(701, "PUMP_START", "\"CH1\"", 10000, ""),
                                          TELEMETRY_CMD_HTTP, 99000U, 99001U, ack_sink, NULL);
        telemetry_client_cmd_stats(&st);
        CK(s_ack_n == before + 1 && !strcmp(s_acks[before].state, "FAILED") && st.manual_start_exec == 2U,
           "ttl_expired_id_not_revived_by_redelivery");
    }
    /* Expired READY does not release the gate; a fresh one does. */
    mq_setup();
    (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &(uint32_t){0});
    mq_feed(t, 0);
    dual_dispense_controller_tick(t);
    dual_channel_snapshot_t ch1, ch2;
    telemetry_client_dispatch_command(cmd_json(711, "READY", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, 1000U, 30000U, ack_sink, NULL);
    CK(dual_dispense_controller_snapshot(1, t + 1U, &ch1) && ch1.awaiting_operator_ready &&
       !strcmp(s_acks[0].state, "FAILED"), "ttl_expired_ready_leaves_gate_closed");
    telemetry_client_dispatch_command(cmd_json(712, "READY", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 20U, ack_sink, NULL);
    CK(dual_dispense_controller_snapshot(1, t + 30U, &ch1) && !ch1.awaiting_operator_ready &&
       !strcmp(s_acks[1].state, "APPLIED"), "ttl_fresh_ready_releases_gate");

    /* 5. Retained messages are never acted on. */
    mq_setup();
    telemetry_client_cmd_stats(&st);
    const char *js721 = cmd_json(721, "ESTOP", "null", 0, "");
    CK(telemetry_client_mqtt_intake(js721, strlen(js721), true, t, 0) ==
       TELEMETRY_INTAKE_RETAINED && telemetry_client_mqtt_staged() == 0U, "retained_estop_not_staged");
    telemetry_client_cmd_stats(&st);
    CK(!telemetry_client_mqtt_step(t) && safety_manager_fault() == SAFETY_NONE && st.drop_retained == 1U,
       "retained_estop_does_not_latch");
    {
        const char *js = cmd_json(722, "PUMP_START", "\"CH1\"", 0, "");
        size_t n = strlen(js);
        CK(mqtt_link_queue_push_rx(&s_q, "cas/relay-test/commands", 23, js, n, n, true, t, 0) ==
           MQTT_ENQ_RETAINED && mqtt_link_queue_pending_rx(&s_q) == 0, "retained_rejected_by_callback_queue");
    }

    /* 6. Staging saturation: drops are counted, nothing blocks, STOP still gets in. */
    mq_setup();
    int staged = 0, full = 0;
    for (uint32_t i = 0; i < 9U; i++) {
        telemetry_intake_result_t r = mq_send(job_json(800U + i, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11), t);
        if (r == TELEMETRY_INTAKE_NORMAL) staged++;
        if (r == TELEMETRY_INTAKE_FULL) full++;
    }
    telemetry_client_cmd_stats(&st);
    CK(staged == 6 && full == 3 && st.drop_full_normal == 3U, "saturation_normal_staging_drops_excess");
    CK(mq_send(cmd_json(820, "ESTOP", "null", 0, ""), t) == TELEMETRY_INTAKE_URGENT,
       "saturation_stop_accepted_when_normal_full");
    int urgent_ok = 1, urgent_full = 0;
    for (uint32_t i = 0; i < 6U; i++) {
        telemetry_intake_result_t r = mq_send(cmd_json(830U + i, "PUMP_STOP", "\"CH1\"", 0, ""), t);
        if (r == TELEMETRY_INTAKE_URGENT) urgent_ok++;
        if (r == TELEMETRY_INTAKE_FULL) urgent_full++;
    }
    CK(urgent_ok == 4 && urgent_full == 3, "saturation_urgent_queue_bounded");

    /* 7. Urgent bypass under load: a STOP behind a full normal queue runs FIRST,
     *    then exactly one normal command per step. */
    mq_setup();
    for (uint32_t i = 0; i < 6U; i++) {
        (void)mq_send(job_json(900U + i, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11), t);
    }
    (void)mq_send(cmd_json(950, "ESTOP", "null", 0, ""), t);
    CK(telemetry_client_mqtt_staged() == 7U && safety_manager_fault() == SAFETY_NONE,
       "urgent_load_seven_staged_nothing_run");
    CK(telemetry_client_mqtt_step(t + 1U), "urgent_load_step_runs");
    CK(safety_manager_fault() == SAFETY_EMERGENCY_STOP && relays_off(),
       "urgent_estop_latched_in_first_step_before_jobs");
    CK(s_pub_n == 2 && pub_has(0, "\"command_id\":950") && pub_has(1, "\"command_id\":900"),
       "urgent_ack_precedes_first_normal_ack");
    telemetry_client_cmd_stats(&st);
    CK(st.urgent_executed == 1U && st.normal_executed == 1U && telemetry_client_mqtt_staged() == 5U,
       "urgent_step_runs_one_normal_only");
    CK(st.lat_urgent_last_us < 1000000U, "urgent_latency_counter_recorded_and_small");
    /* a STOP arriving mid-backlog jumps ahead of the 5 still staged */
    (void)mq_send(cmd_json(951, "PUMP_STOP", "\"CH2\"", 0, ""), t);
    mq_reset_io();
    (void)telemetry_client_mqtt_step(t + 2U);
    CK(s_pub_n == 2 && pub_has(0, "\"command_id\":951") && pub_has(1, "\"command_id\":901"),
       "urgent_late_stop_overtakes_backlog");
    /* E-STOP priority: after it, a manual START is refused and nothing is energised */
    telemetry_client_dispatch_command(cmd_json(952, "PUMP_START", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 5U, ack_sink, NULL);
    CK(!strcmp(s_acks[s_ack_n - 1].state, "FAILED") && relays_off() &&
       dual_dispense_controller_snapshot(1, t + 6U, &ch1) && !ch1.manual && !ch1.relay_on,
       "estop_priority_start_refused_after_latch");
    /* a slow publisher never delays a STOP: ACK enqueue refused, STOP still ran */
    mq_setup();
    s_publish_accepts = false;
    (void)mq_send(cmd_json(960, "ESTOP", "null", 0, ""), t);
    (void)telemetry_client_mqtt_step(t + 1U);
    CK(safety_manager_fault() == SAFETY_EMERGENCY_STOP && s_pub_n == 0,
       "urgent_runs_even_when_ack_publish_is_refused");

    /* 8. STOP when already OFF is idempotent and always applied. */
    mq_setup();
    CK(relays_off(), "stop_off_precondition_relays_off");
    telemetry_client_dispatch_command(cmd_json(970, "PUMP_STOP", "\"CH1\"", 0, ""), TELEMETRY_CMD_MQTT,
                                      t, t + 1U, ack_sink, NULL);
    telemetry_client_dispatch_command(cmd_json(971, "PUMP_STOP", "\"CH1\"", 0, ""), TELEMETRY_CMD_MQTT,
                                      t, t + 2U, ack_sink, NULL);
    telemetry_client_dispatch_command(cmd_json(970, "PUMP_STOP", "\"CH1\"", 0, ""), TELEMETRY_CMD_HTTP,
                                      t, t + 3U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(s_ack_n == 3 && !strcmp(s_acks[0].state, "APPLIED") && !strcmp(s_acks[1].state, "APPLIED") &&
       !strcmp(s_acks[2].state, "APPLIED"), "stop_when_already_off_acks_applied_every_time");
    CK(st.manual_stop_exec == 3U && relays_off() && dual_dispense_controller_snapshot(1, t + 4U, &ch1) &&
       !ch1.relay_on && !ch1.manual, "stop_duplicate_id_is_reapplied_idempotently");
    telemetry_client_dispatch_command(cmd_json(972, "PUMP_STOP", "null", 0, ""), TELEMETRY_CMD_MQTT,
                                      t, t + 5U, ack_sink, NULL);
    CK(!strcmp(s_acks[3].state, "FAILED") && strstr(s_acks[3].error, "channel"),
       "stop_without_channel_is_refused_not_guessed");
    telemetry_client_dispatch_command(cmd_json(973, "ESTOP", "null", 0, ""), TELEMETRY_CMD_MQTT, t,
                                      t + 6U, ack_sink, NULL);
    telemetry_client_dispatch_command(cmd_json(974, "ESTOP", "null", 0, ""), TELEMETRY_CMD_MQTT, t,
                                      t + 7U, ack_sink, NULL);
    CK(!strcmp(s_acks[4].state, "APPLIED") && !strcmp(s_acks[5].state, "APPLIED") &&
       safety_manager_fault() == SAFETY_EMERGENCY_STOP && relays_off(),
       "estop_when_already_latched_is_idempotent");

    /* 9. Manual START/STOP: correlated by id, never replayed. */
    mq_setup();
    telemetry_client_dispatch_command(cmd_json(980, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 1U, ack_sink, NULL);
    telemetry_client_dispatch_command(cmd_json(980, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_HTTP, t, t + 2U, ack_sink, NULL);
    telemetry_client_dispatch_command(cmd_json(980, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 3U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 1U && s_ack_n == 3 && s_acks[0].id == 980 && s_acks[1].id == 980 &&
       !strcmp(s_acks[1].state, s_acks[0].state), "manual_start_same_id_executes_once_reacks_stored");
    telemetry_client_dispatch_command(cmd_json(985, "PUMP_STOP", "\"CH1\"", 0, ""), TELEMETRY_CMD_MQTT,
                                      t, t + 4U, ack_sink, NULL);
    telemetry_client_dispatch_command(cmd_json(983, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 5U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 1U && !strcmp(s_acks[s_ack_n - 1].state, "APPLIED"),
       "manual_start_older_than_a_stop_never_energises");
    /* reconnect: the broker redelivers the same QoS 1 START, ids survive */
    mqtt_link_reconnect_t rc;
    mqtt_link_reconnect_init(&rc);
    (void)mqtt_link_reconnect_on_disconnect(&rc, 1000);
    mqtt_link_reconnect_on_connected(&rc);
    mq_reset_io();
    (void)mq_send(cmd_json(980, "PUMP_START", "\"CH1\"", 10000, ""), t + 6U);
    (void)telemetry_client_mqtt_step(t + 7U);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 1U && s_pub_n == 1, "manual_start_not_replayed_after_reconnect");
    telemetry_client_dispatch_command(cmd_json(990, "PUMP_START", "null", 10000, ""), TELEMETRY_CMD_MQTT,
                                      t, t + 8U, ack_sink, NULL);
    CK(!strcmp(s_acks[s_ack_n - 1].state, "FAILED") && strstr(s_acks[s_ack_n - 1].error, "channel"),
       "manual_start_without_channel_refused");
    /* auto job ownership wins: a manual START never runs over an active job */
    mq_setup();
    (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &(uint32_t){0});
    mq_feed(t, 0);
    dual_dispense_controller_tick(t);
    telemetry_client_dispatch_command(cmd_json(995, "PUMP_START", "\"CH2\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 10U, ack_sink, NULL);
    CK(!strcmp(s_acks[0].state, "FAILED") && strstr(s_acks[0].error, "refused") &&
       dual_dispense_controller_snapshot(2, t + 20U, &ch2) && !ch2.manual && !ch2.relay_on,
       "manual_start_refused_while_a_job_owns_the_line");

    /* 10. Sequential-relay invariant driven entirely through MQTT. */
    mq_setup();
    (void)mq_send(job_json(1001, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11), t);
    (void)mq_send(job_json(1002, "M2", "null", "", "tune-b", 2, "tune-b", 2, 0.22), t);
    (void)telemetry_client_mqtt_step(t + 1U);
    (void)telemetry_client_mqtt_step(t + 2U);
    CK(jobs_queued() == 2U, "seq_two_jobs_queued_via_mqtt");
    mq_feed(t + 100U, 0);
    dual_dispense_controller_tick(t + 100U);
    mq_reset_io();
    (void)mq_send(cmd_json(1003, "READY", "\"CH1\"", 10000, ""), t + 110U);
    (void)mq_send(cmd_json(1004, "READY", "\"CH2\"", 10000, ""), t + 110U);
    (void)telemetry_client_mqtt_step(t + 120U);
    (void)telemetry_client_mqtt_step(t + 121U);
    CK(pub_has(0, "\"command_id\":1003") && pub_has(0, "\"state\":\"APPLIED\"") &&
       pub_has(1, "\"command_id\":1004") && pub_has(1, "\"state\":\"FAILED\""),
       "seq_ready_ch2_refused_while_ch1_owns_scale");
    bool never_both = true;
    for (uint32_t k = 0; k < 20U; k++) {
        mq_feed(t + 200U + k * 10U, 0);
        dual_dispense_controller_tick(t + 200U + k * 10U);
        (void)dual_dispense_controller_snapshot(1, t + 200U + k * 10U, &ch1);
        (void)dual_dispense_controller_snapshot(2, t + 200U + k * 10U, &ch2);
        if (ch1.relay_on && ch2.relay_on) never_both = false;
        if (ch2.owns_scale || ch2.relay_on) never_both = false;
    }
    CK(never_both && dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1 &&
       ch2.state == DCH_WAITING_SCALE, "seq_only_one_channel_owns_scale_and_relay");

    /* 11. Config/PROFILE validation: fixed mapping, exact id/version, independence. */
    mq_setup();
    (void)mq_send(profile_json(1101, "CH1", "mat-a", 1, "M1", "", 0.11), t);
    (void)mq_send(profile_json(1102, "CH2", "mat-b", 2, "M2", "", 0.22), t);
    (void)telemetry_client_mqtt_step(t + 1U);
    (void)telemetry_client_mqtt_step(t + 2U);
    CK(pub_has(0, "\"state\":\"APPLIED\"") && pub_has(1, "\"state\":\"APPLIED\""),
       "profile_material_a_pump1_and_material_b_pump2_accepted");
    uint32_t ja = 0, jb = 0;
    (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
    (void)dual_dispense_controller_add_job(2, 5000, JOB_PRIORITY_NORMAL, &jb);
    mq_feed(t + 100U, 0);
    dual_dispense_controller_tick(t + 100U);
    telemetry_client_dispatch_command(cmd_json(1103, "READY", "\"CH1\"", 10000, ""), TELEMETRY_CMD_MQTT,
                                      t + 100U, t + 110U, ack_sink, NULL);
    CK(dual_dispense_controller_snapshot(1, t + 110U, &ch1) && !strcmp(ch1.profile_id, "mat-a") &&
       ch1.profile_version == 1U && ch1.kp > 0.10f && ch1.kp < 0.12f,
       "profile_material_a_pump1_runs_its_own_gains");
    mq_feed(t + 500U, 0);
    dual_dispense_controller_tick(t + 500U);
    mq_feed(t + 550U, 5000);
    dual_dispense_controller_tick(t + 550U);
    mq_feed(t + 850U, 5000);
    dual_dispense_controller_tick(t + 850U);
    mq_feed(t + 900U, 5000);
    dual_dispense_controller_tick(t + 900U);
    dispense_job_t jr;
    CK(job_queue_get(ja, &jr) && jr.state == JOB_COMPLETE, "profile_material_a_job_completes");
    dual_dispense_controller_tick(t + 1500U);
    dual_dispense_controller_tick(t + 1600U);
    telemetry_client_dispatch_command(cmd_json(1104, "READY", "\"CH2\"", 10000, ""), TELEMETRY_CMD_MQTT,
                                      t + 1600U, t + 1700U, ack_sink, NULL);
    CK(dual_dispense_controller_snapshot(2, t + 1700U, &ch2) && !strcmp(ch2.profile_id, "mat-b") &&
       ch2.profile_version == 2U && ch2.kp > 0.21f && ch2.kp < 0.23f,
       "profile_material_b_pump2_runs_independent_gains");

    mq_setup();
    static char bad_copy[8][1100];
    /* profile_json reuses one buffer, so each case is copied out first */
    snprintf(bad_copy[0], 1100, "%s", profile_json(1110, "CH1", "mat-b", 2, "M2", "", 0.22));
    snprintf(bad_copy[1], 1100, "%s", profile_json(1111, "CH2", "mat-a", 1, "M1", "", 0.11));
    snprintf(bad_copy[2], 1100, "%s", profile_json(1112, "CH1", "mat-a", 1, "M1", "\"pump_id\": \"Pump 2\", ", 0.11));
    snprintf(bad_copy[3], 1100, "%s", profile_json(1113, "CH2", "mat-b", 2, "M2", "\"relay_id\": \"Relay 1\", ", 0.22));
    snprintf(bad_copy[4], 1100, "%s", profile_json(1114, "CH1", "mat-a", 1, "M1", "\"scale_id\": \"Scale 2\", ", 0.11));
    snprintf(bad_copy[5], 1100, "%s", profile_json(1115, "CH1", "mat-a", 0, "M1", "", 0.11));
    snprintf(bad_copy[6], 1100, "%s", profile_json(1116, "CH1", "", 1, "M1", "", 0.11));
    snprintf(bad_copy[7], 1100, "%s", profile_json(1117, "CH3", "mat-a", 1, "M1", "", 0.11));
    int rejected = 0;
    for (int i = 0; i < 8; i++) {
        s_ack_n = 0;
        telemetry_client_dispatch_command(bad_copy[i], TELEMETRY_CMD_MQTT, t, t + 1U, ack_sink, NULL);
        if (s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED")) rejected++;
    }
    CK(rejected == 8, "profile_incompatible_material_pump_channel_version_all_rejected");
    (void)dual_dispense_controller_add_job(1, 5000, JOB_PRIORITY_NORMAL, &ja);
    mq_feed(t + 100U, 0);
    dual_dispense_controller_tick(t + 100U);
    (void)dual_dispense_controller_operator_ready(1, t + 110U);
    CK(dual_dispense_controller_snapshot(1, t + 110U, &ch1) && !strcmp(ch1.profile_id, "or-slot1"),
       "profile_rejected_commands_changed_nothing");

    /* JOB mapping and pin identity. */
    mq_setup();

    static char jb_copy[7][1100];
    snprintf(jb_copy[0], 1100, "%s", job_json(1201, "M1", "\"CH2\"", "", "tune-a", 1, "tune-a", 1, 0.11));
    snprintf(jb_copy[1], 1100, "%s", job_json(1202, "M2", "null", "\"pump_id\": \"Pump 1\", ", "tune-b", 2, "tune-b", 2, 0.22));
    snprintf(jb_copy[2], 1100, "%s", job_json(1203, "M1", "null", "\"relay_id\": \"Relay 2\", ", "tune-a", 1, "tune-a", 1, 0.11));
    snprintf(jb_copy[3], 1100, "%s", job_json(1204, "M1", "null", "", "tune-a", 3, "tune-a", 4, 0.11));   /* version mismatch */
    snprintf(jb_copy[4], 1100, "%s", job_json(1205, "M1", "null", "", "tune-a", 3, "tune-z", 3, 0.11));   /* id mismatch */
    snprintf(jb_copy[5], 1100, "%s", job_json(1206, "M3", "null", "", "tune-a", 1, "tune-a", 1, 0.11));   /* no such material */
    snprintf(jb_copy[6], 1100, "%s", job_json(1207, "M2", "\"CH1\"", "", "tune-b", 2, "tune-b", 2, 0.22));

    rejected = 0;
    for (int i = 0; i < 7; i++) {
        s_ack_n = 0;
        telemetry_client_dispatch_command(jb_copy[i], TELEMETRY_CMD_MQTT, t, t + 1U, ack_sink, NULL);
        if (s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED")) rejected++;
    }
    CK(rejected == 7 && jobs_queued() == 0U, "job_material_pump_channel_pin_mismatch_all_rejected_none_queued");
    /* Pinned v3 is honoured even though a NEWER v9 sits in the target slot. */
    (void)mq_send(profile_json(1210, "CH1", "tune-a", 9, "M1", "", 0.99), t);
    (void)telemetry_client_mqtt_step(t + 1U);
    (void)mq_send(job_json(1211, "M1", "\"CH1\"", "\"pump_id\": \"Pump 1\", ", "tune-a", 3, "tune-a", 3, 0.0025), t);
    (void)telemetry_client_mqtt_step(t + 2U);
    {
        static dispense_job_t jobs[JOB_QUEUE_MAX];
        uint32_t n = job_queue_snapshot(jobs, JOB_QUEUE_MAX);
        CK(n == 1U && jobs[0].pin.valid && jobs[0].pin.version == 3U && jobs[0].pin.kp < 0.01f,
           "job_exact_pinned_version_never_replaced_by_newer_profile");
    }
    /* Legacy (no pin) JOB over MQTT would need a blocking HTTP fetch: deferred to
     * the HTTP poll, never ACKed and never recorded as processed. */
    mq_setup();
    snprintf(s_js, sizeof(s_js),
        "{\"command_id\": 1301, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": null, "
        "\"ttl_ms\": 0, \"material_id\": \"M1\", \"target_g\": 5000, \"priority\": 0}");
    (void)mq_send(s_js, t);
    (void)telemetry_client_mqtt_step(t + 1U);
    (void)mq_send(s_js, t);
    (void)telemetry_client_mqtt_step(t + 2U);
    telemetry_client_cmd_stats(&st);
    CK(s_pub_n == 0 && jobs_queued() == 0U && st.deferred_to_http == 2U && st.dup_reacked == 0U,
       "legacy_job_over_mqtt_deferred_to_http_not_acked_not_recorded");

    /* 12. Safe state at restart: nothing from a previous life survives. */
    mq_setup();
    (void)mq_send(job_json(1401, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11), t);
    (void)telemetry_client_mqtt_step(t + 1U);
    telemetry_client_dispatch_command(cmd_json(1402, "PUMP_START", "\"CH1\"", 10000, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 2U, ack_sink, NULL);
    mqtt_link_queue_reset(&s_q);
    mq_setup(); /* what boot does: relay_init_all, safety, queue, controller */
    CK(relays_off() && jobs_queued() == 0U && safety_manager_fault() == SAFETY_NONE &&
       dual_dispense_controller_snapshot(1, t + 10U, &ch1) && ch1.state == DCH_IDLE &&
       ch1.active_job_id == 0U && !ch1.relay_on && !ch1.manual &&
       dual_dispense_controller_snapshot(2, t + 10U, &ch2) && ch2.state == DCH_IDLE && !ch2.relay_on,
       "restart_boots_to_safe_state_relays_off_no_jobs");
    CK(telemetry_client_mqtt_staged() == 0U && mqtt_link_queue_pending_rx(&s_q) == 0U &&
       mqtt_link_queue_pending_tx(&s_q) == 0U, "restart_has_no_staged_or_queued_commands");
    CK(mq_send(cmd_json(1403, "PUMP_START", "\"CH1\"", 10000, ""), t) == TELEMETRY_INTAKE_NORMAL &&
       telemetry_client_mqtt_staged() == 1U && relays_off(), "restart_start_only_runs_when_freshly_delivered");

    mqtt_link_queue_deinit(&s_q);
    telemetry_client_set_mqtt_ops(NULL);
    mq_setup();
}

/* ---- safety-review hardening: ledger floor, non-blocking send, stop order, HTTP size ---- */
static int s_enq_n, s_enq_rc;
static size_t s_outbox;
static int s_enq_qos;
static int mock_enqueue(void *ctx, const char *topic, const char *data, int len, int qos, bool retain)
{
    (void)ctx; (void)topic; (void)data; (void)len; (void)retain;
    s_enq_n++;
    s_enq_qos = qos;
    return s_enq_rc;
}
static size_t mock_outbox(void *ctx) { (void)ctx; return s_outbox; }

static int s_st_goodbye, s_st_stop, s_st_destroy, s_st_delays, s_st_delays_at_stop;
static int s_st_exit_after;
static bool s_st_exited_at_destroy, s_st_stop_before_destroy;
static void st_goodbye(void *c) { (void)c; s_st_goodbye++; }
static void st_stop(void *c) { (void)c; s_st_stop++; s_st_delays_at_stop = s_st_delays; }
static bool st_exited(void *c) { (void)c; return s_st_delays >= s_st_exit_after; }
static void st_delay(void *c, uint32_t ms) { (void)c; (void)ms; s_st_delays++; }
static void st_destroy(void *c)
{
    (void)c;
    s_st_destroy++;
    s_st_exited_at_destroy = s_st_delays >= s_st_exit_after;
    s_st_stop_before_destroy = s_st_stop > 0;
}

static void test_mqtt_tx_and_stop(void)
{
    static char body[MQTT_LINK_TX_PAYLOAD_MAX + 1U];
    memset(body, 'x', sizeof(body) - 1U);
    body[sizeof(body) - 1U] = '\0';
    mqtt_link_tx_ops_t ops = { .ctx = NULL, .enqueue = mock_enqueue, .outbox_size = mock_outbox };
    mqtt_link_queue_t q;
    CK(mqtt_link_queue_init(&q, 2U, 2U), "m1_queue_init");
    mqtt_tx_item_t tel = { .qos = 0, .retain = false, .len = 4800U, .data = body };
    mqtt_tx_item_t ack = { .qos = 1, .retain = false, .len = 120U, .data = body };
    snprintf(tel.topic, sizeof(tel.topic), "cas/d/telemetry/samples");
    snprintf(ack.topic, sizeof(ack.topic), "cas/d/commands/ack");

    s_enq_n = 0; s_enq_rc = 1; s_outbox = 0U;
    CK(mqtt_link_tx_dispatch(&ops, &q, &tel, true, 8192U) == MQTT_TX_ENQUEUED && s_enq_n == 1 &&
       q.tx_dropped_net == 0U, "m1_telemetry_enqueued_when_outbox_empty");
    s_enq_n = 0; s_outbox = 4000U;
    CK(mqtt_link_tx_dispatch(&ops, &q, &tel, true, 8192U) == MQTT_TX_DROPPED_TELEMETRY &&
       s_enq_n == 0 && q.tx_dropped_net == 1U, "m1_telemetry_dropped_and_counted_under_outbox_pressure");
    s_enq_n = 0; s_outbox = 8000U;
    CK(mqtt_link_tx_dispatch(&ops, &q, &ack, true, 8192U) == MQTT_TX_ENQUEUED && s_enq_n == 1 &&
       s_enq_qos == 1, "m1_ack_still_enqueued_when_telemetry_is_shed");
    s_enq_n = 0; s_outbox = 0U; s_enq_rc = -2;
    CK(mqtt_link_tx_dispatch(&ops, &q, &tel, true, 8192U) == MQTT_TX_DROPPED_TELEMETRY &&
       q.tx_dropped_net == 2U, "m1_telemetry_refused_by_client_is_dropped_and_counted");
    CK(mqtt_link_tx_dispatch(&ops, &q, &ack, true, 8192U) == MQTT_TX_ACK_REJECTED,
       "m1_rejected_ack_is_reported_not_hidden");
    s_enq_n = 0; s_enq_rc = 1;
    CK(mqtt_link_tx_dispatch(&ops, &q, &tel, false, 8192U) == MQTT_TX_DROPPED_OFFLINE &&
       s_enq_n == 0 && q.tx_dropped_net == 2U, "m1_offline_telemetry_dropped_not_enqueued");
    CK(mqtt_link_tx_dispatch(&ops, &q, &ack, false, 8192U) == MQTT_TX_ENQUEUED && s_enq_n == 1,
       "m1_offline_ack_waits_in_outbox");
    mqtt_link_queue_deinit(&q);

    mqtt_link_stop_ops_t so = { .ctx = NULL, .goodbye = st_goodbye, .client_stop = st_stop,
        .task_exited = st_exited, .delay_ms = st_delay, .client_destroy = st_destroy };
    /* publish in flight: the pub task needs 500 ms to get out */
    s_st_goodbye = s_st_stop = s_st_destroy = s_st_delays = s_st_delays_at_stop = 0;
    s_st_exit_after = 50;
    CK(mqtt_link_stop_sequence(&so, 2000U) == MQTT_STOP_DESTROYED, "m2_stop_with_publish_in_flight_completes");
    CK(s_st_destroy == 1 && s_st_exited_at_destroy, "m2_client_not_destroyed_while_pub_task_alive");
    CK(s_st_stop == 1 && s_st_stop_before_destroy && s_st_delays_at_stop == 0 && s_st_goodbye == 1,
       "m2_client_stopped_before_waiting_for_pub_task");
    /* task never exits: leak, never destroy under it, wait is bounded */
    s_st_goodbye = s_st_stop = s_st_destroy = s_st_delays = s_st_delays_at_stop = 0;
    s_st_exit_after = 1000000;
    CK(mqtt_link_stop_sequence(&so, 2000U) == MQTT_STOP_LEAKED && s_st_destroy == 0 &&
       s_st_stop == 1 && s_st_delays > 0 && s_st_delays <= 200, "m2_stuck_pub_task_leaks_client_never_destroys");
    /* idle: immediate */
    s_st_goodbye = s_st_stop = s_st_destroy = s_st_delays = s_st_delays_at_stop = 0;
    s_st_exit_after = 0;
    CK(mqtt_link_stop_sequence(&so, 2000U) == MQTT_STOP_DESTROYED && s_st_destroy == 1 &&
       s_st_delays == 0, "m2_idle_stop_is_immediate");
}

static void test_ledger_floor(void)
{
    telemetry_cmd_stats_t st;
    uint32_t t = 900000U;

    /* H1a. 40 JOBs (first half HTTP, second half MQTT) push the first 8 out of the
     *      32-slot ledger. Replaying the first must not queue it again. */
    mq_setup();
    for (uint32_t i = 0; i < 40U; i++) {
        const char *j = job_json(2001U + i, (i & 1U) ? "M2" : "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11);
        telemetry_client_dispatch_command(j, i < 20U ? TELEMETRY_CMD_HTTP : TELEMETRY_CMD_MQTT,
                                          t, t + 1U, ack_sink, NULL);
    }
    uint32_t queued = jobs_queued();
    s_ack_n = 0;
    telemetry_client_dispatch_command(job_json(2001, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_MQTT, t, t + 2U, ack_sink, NULL);
    /* 2001 is still in the queue: its lost ACK is repaired with the real state
     * (QUEUED), never FAILED "stale", and it is never queued a second time. */
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED") && !strstr(s_acks[0].error, "stale") &&
       jobs_queued() == queued, "floor_evicted_job_replayed_over_mqtt_reacked_not_requeued");
    s_ack_n = 0;
    telemetry_client_dispatch_command(job_json(2002, "M2", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED") && !strstr(s_acks[0].error, "stale") &&
       jobs_queued() == queued, "floor_evicted_job_replayed_over_http_reacked_not_requeued");
    s_ack_n = 0;
    telemetry_client_dispatch_command(job_json(2040, "M2", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 4U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(s_ack_n == 1 && !strstr(s_acks[0].error, "stale") && jobs_queued() == queued,
       "floor_id_still_in_ledger_gets_original_outcome");

    /* H1b. A refused PUMP_START falls out of the ledger; its replay must never run. */
    mq_setup();
    telemetry_client_dispatch_command(cmd_json(3000, "ESTOP", "null", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 1U, ack_sink, NULL);
    s_ack_n = 0;
    telemetry_client_dispatch_command(cmd_json(3001, "PUMP_START", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 1U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 1U && s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED"),
       "floor_setup_start_was_refused_by_estop");
    for (uint32_t i = 0; i < 40U; i++) {
        telemetry_client_dispatch_command(cmd_json(3002U + i, "BOGUS", "null", 0, ""),
                                          TELEMETRY_CMD_HTTP, t, t + 2U, ack_sink, NULL);
    }
    /* the operator clears the E-Stop; a late replay of the refused START must not run now */
    (void)safety_manager_release_estop();
    dual_dispense_controller_clear_emergency_stop();
    CK(safety_manager_fault() == SAFETY_NONE, "floor_estop_cleared_before_replay");
    s_ack_n = 0;
    telemetry_client_dispatch_command(cmd_json(3001, "PUMP_START", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 3U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED") && strstr(s_acks[0].error, "stale") &&
       st.manual_start_exec == 1U && relays_off(), "floor_evicted_refused_pump_start_replay_not_run_relay_off");
    s_ack_n = 0;
    telemetry_client_dispatch_command(cmd_json(3001, "PUMP_START", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_HTTP, t, t + 4U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(s_ack_n == 1 && strstr(s_acks[0].error, "stale") && st.manual_start_exec == 1U && relays_off(),
       "floor_evicted_pump_start_replay_over_http_not_run");

    /* H1c. STOP-class is exempt and always runs, even with an id below the floor. */
    s_ack_n = 0;
    telemetry_client_dispatch_command(cmd_json(5, "PUMP_STOP", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 5U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_stop_exec == 1U && s_ack_n == 1 && !strstr(s_acks[0].error, "stale"),
       "floor_pump_stop_below_floor_still_runs");
    telemetry_client_dispatch_command(cmd_json(6, "ESTOP", "null", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 6U, ack_sink, NULL);
    CK(safety_manager_fault() == SAFETY_EMERGENCY_STOP, "floor_estop_below_floor_still_runs");

    /* H1d. A fresh, higher id is accepted. */
    mq_setup();
    for (uint32_t i = 0; i < 40U; i++) {
        telemetry_client_dispatch_command(cmd_json(100U + i, "BOGUS", "null", 0, ""),
                                          TELEMETRY_CMD_HTTP, t, t + 1U, ack_sink, NULL);
    }
    s_ack_n = 0;
    telemetry_client_dispatch_command(job_json(200, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_MQTT, t, t + 2U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED") && jobs_queued() == 1U,
       "floor_fresh_higher_job_accepted");
    telemetry_client_dispatch_command(cmd_json(201, "PUMP_START", "\"CH2\"", 0, ""),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    telemetry_client_cmd_stats(&st);
    CK(st.manual_start_exec == 1U, "floor_fresh_higher_pump_start_attempted");
    mq_setup();
}

static char s_resp[4096];
static void test_http_object_size(void)
{
    uint32_t t = 900000U;
    char longid[65];
    memset(longid, 'p', 64);
    longid[64] = '\0';

    /* L1a. A pinned JOB with 64-char profile ids (~780 B on the wire) followed by a
     *      normal one: both must run over the HTTP fallback. */
    mq_setup();
    /* A real pinned JOB is ~560-700 B (64-char ids, long float reprs); pad past
     * 768 so the old per-object cap would have dropped it. */
    static char pad[330];
    snprintf(pad, sizeof(pad), "\"note\": \"");
    size_t pl = strlen(pad);
    memset(pad + pl, 'n', 300U);
    snprintf(pad + pl + 300U, sizeof(pad) - pl - 300U, "\", ");
    const char *big = job_json(4001, "M1", "null", pad, longid, 7, longid, 7, 0.11);
    size_t biglen = strlen(big);
    snprintf(s_resp, sizeof(s_resp), "[%s,", big);
    const char *small = job_json(4002, "M2", "null", "", "tune-a", 1, "tune-a", 1, 0.11);
    size_t o = strlen(s_resp);
    snprintf(s_resp + o, sizeof(s_resp) - o, "%s]", small);
    telemetry_client_http_process_response(s_resp, t, t + 1U, ack_sink, NULL);
    CK(biglen > 768U && biglen < 1024U && s_ack_n == 2 && jobs_queued() == 2U,
       "http_pinned_job_over_768_bytes_not_lost");

    /* L1b. An object beyond any real size is refused alone; the rest still run. */
    mq_setup();
    snprintf(s_resp, sizeof(s_resp), "[{\"command_id\": 4003, \"command_type\": \"JOB\", \"note\": \"");
    o = strlen(s_resp);
    memset(s_resp + o, 'n', 1300U);
    o += 1300U;
    snprintf(s_resp + o, sizeof(s_resp) - o, "\"},");
    o = strlen(s_resp);
    small = job_json(4004, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11);
    snprintf(s_resp + o, sizeof(s_resp) - o, "%s]", small);
    telemetry_client_http_process_response(s_resp, t, t + 1U, ack_sink, NULL);
    CK(s_ack_n == 2 && s_acks[0].id == 4003U && !strcmp(s_acks[0].state, "FAILED") &&
       strstr(s_acks[0].error, "too large") && s_acks[1].id == 4004U &&
       !strcmp(s_acks[1].state, "QUEUED") && jobs_queued() == 1U,
       "http_oversize_object_skipped_acked_failed_rest_continues");
    mq_setup();
}

/* ---- truncated HTTP response / spliced JOB / pin identity / lost-ACK re-ACK ---- */
static char s_stream[12000];
static char s_wire[8100];
static const dispense_job_t *find_remote(uint32_t remote)
{
    static dispense_job_t jobs[JOB_QUEUE_MAX];
    uint32_t n = job_queue_snapshot(jobs, JOB_QUEUE_MAX);
    for (uint32_t i = 0; i < n; i++) if (jobs[i].remote_command_id == remote) return &jobs[i];
    return NULL;
}
static const ack_rec_t *ack_for(uint32_t id)
{
    for (int i = 0; i < s_ack_n; i++) if (s_acks[i].id == id) return &s_acks[i];
    return NULL;
}

static void test_http_truncation(void)
{
    uint32_t t = 900000U;
    const size_t cap = sizeof(s_wire) - 100U;
    char longid[65];
    memset(longid, 'p', 64);
    longid[64] = '\0';
    static char pad[330];
    snprintf(pad, sizeof(pad), "\"note\": \"");
    size_t pl = strlen(pad);
    memset(pad + pl, 'n', 300U);
    snprintf(pad + pl + 300U, sizeof(pad) - pl - 300U, "\", ");

    /* Wire stream as the server sends it: STOP first, then 10 large pinned JOBs
     * (ids 6001..6010, each with its own kp so a splice is detectable). */
    mq_setup();
    snprintf(s_stream, sizeof(s_stream), "[%s", cmd_json(6000, "PUMP_STOP", "\"CH1\"", 0, ""));
    for (uint32_t i = 0; i < 10U; i++) {
        const char *j = job_json(6001U + i, (i & 1U) ? "M2" : "M1", "null", pad, longid, 7, longid, 7,
                                 0.10 + 0.01 * (double)i);
        size_t o = strlen(s_stream);
        snprintf(s_stream + o, sizeof(s_stream) - o, ",%s", j);
    }
    strcat(s_stream, "]");
    const char *kp7 = strstr(strstr(s_stream, "\"command_id\": 6007"), "\"kp\": ");
    const char *kp10 = strstr(strstr(s_stream, "\"command_id\": 6010"), "\"kp\": ");
    size_t a_len = (size_t)(kp7 - s_stream), b_len = (size_t)(kp10 - kp7);
    size_t c_len = strlen(kp10);
    CK(a_len < cap && a_len + b_len >= cap && a_len + c_len < cap, "trunc_setup_dropped_middle_small_tail");

    /* Chunks: A fits, B (middle of JOB 6007 .. head of 6010) overflows, C is small. */
    char *chunk = s_stream + a_len;
    s_wire[0] = '\0';
    bool truncated = false;
    (void)telemetry_client_test_response_append(s_wire, cap, &truncated, s_stream, a_len);
    (void)telemetry_client_test_response_append(s_wire, cap, &truncated, chunk, b_len);
    (void)telemetry_client_test_response_append(s_wire, cap, &truncated, kp10, c_len);
    CK(truncated, "trunc_overflow_latches_flag");
    CK(strlen(s_wire) == a_len, "trunc_nothing_appended_after_first_overflow");

    telemetry_client_http_process_response(s_wire, t, t + 1U, ack_sink, NULL);
    telemetry_cmd_stats_t st;
    telemetry_client_cmd_stats(&st);
    CK(st.manual_stop_exec == 1U && ack_for(6000U) != NULL, "trunc_stop_in_response_still_executes");
    CK(ack_for(6001U) != NULL && !strcmp(ack_for(6001U)->state, "QUEUED") &&
       ack_for(6006U) != NULL && !strcmp(ack_for(6006U)->state, "QUEUED"),
       "trunc_complete_jobs_before_cut_execute");
    CK(ack_for(6007U) == NULL && ack_for(6008U) == NULL && ack_for(6010U) == NULL &&
       find_remote(6007U) == NULL && find_remote(6010U) == NULL,
       "trunc_partial_object_discarded_no_ack_no_job");
    bool own_gains = true;
    for (uint32_t id = 6001U; id <= 6010U; id++) {
        const dispense_job_t *jb = find_remote(id);
        double want = 0.10 + 0.01 * (double)(id - 6001U);
        if (jb && !(jb->pin.valid && jb->pin.kp > want - 0.001 && jb->pin.kp < want + 0.001)) own_gains = false;
    }
    CK(own_gains, "trunc_no_job_carries_another_jobs_gains");

    /* Next poll re-serves the unACKed tail; it now executes with its own gains. */
    s_ack_n = 0;
    telemetry_client_http_process_response(s_stream, t, t + 2U, ack_sink, NULL);
    const dispense_job_t *j7 = find_remote(6007U);
    CK(ack_for(6007U) != NULL && !strcmp(ack_for(6007U)->state, "QUEUED") && j7 != NULL &&
       j7->pin.valid && j7->pin.kp > 0.159 && j7->pin.kp < 0.161, "trunc_tail_reserved_next_poll_runs_with_own_gains");
    mq_setup();
}

static void test_pin_identity(void)
{
    job_profile_t out;
    char err[80];
    /* head names the pin but the nested object carries no identity: gains cannot
     * be proven to belong to this JOB (spliced tail) -> refuse. */
    const char *no_nested_id =
        "{\"command_id\":9,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
        "\"profile_id\":\"A\",\"profile_version\":1,\"profile\":{\"kp\":0.5,\"ki\":0,\"kd\":0,"
        "\"tolerance_g\":20,\"max_overshoot_g\":100,\"max_duration_ms\":120000,"
        "\"window_ms\":500,\"min_on_ms\":40,\"min_off_ms\":40}}";
    CK(telemetry_client_parse_job_pin(no_nested_id, 1, 5000, &out, err, sizeof(err)) == JOB_PIN_REFUSED,
       "pin_nested_without_identity_refused");
    /* gains found only outside the nested profile object are not this pin's gains */
    const char *loose_gains =
        "{\"command_id\":9,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
        "\"profile_id\":\"A\",\"profile_version\":1,\"profile\":null,\"kp\":0.5,\"ki\":0,\"kd\":0,"
        "\"tolerance_g\":20,\"max_overshoot_g\":100,\"max_duration_ms\":120000,"
        "\"window_ms\":500,\"min_on_ms\":40,\"min_off_ms\":40}";
    CK(telemetry_client_parse_job_pin(loose_gains, 1, 5000, &out, err, sizeof(err)) == JOB_PIN_REFUSED,
       "pin_gains_outside_nested_profile_refused");
}

static bool stale_unknown_ack(void);

static void test_lost_ack_reack(void)
{
    uint32_t t = 900000U;
    telemetry_cmd_stats_t st;
    mq_setup();
    telemetry_client_dispatch_command(job_json(7001, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 1U, ack_sink, NULL);
    const dispense_job_t *jb = find_remote(7001U);
    uint32_t local = jb ? jb->id : 0U;
    CK(local != 0U && s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED"), "reack_setup_job_queued");
    for (uint32_t i = 0; i < 40U; i++) {
        telemetry_client_dispatch_command(cmd_json(7100U + i, "BOGUS", "null", 0, ""),
                                          TELEMETRY_CMD_HTTP, t, t + 2U, ack_sink, NULL);
    }
    uint32_t queued = jobs_queued();
    s_ack_n = 0;
    telemetry_client_dispatch_command(job_json(7001, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED") && s_acks[0].local_job == local &&
       jobs_queued() == queued, "reack_evicted_queued_job_reacked_queued_not_stale");
    telemetry_client_cmd_stats(&st);
    CK(st.stale_refused == 0U, "reack_not_counted_as_stale_refusal");
    /* an id unknown to the queue stays refused */
    s_ack_n = 0;
    telemetry_client_dispatch_command(job_json(7000, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 4U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED") && stale_unknown_ack() &&
       find_remote(7000U) == NULL, "reack_unknown_job_still_refused_stale");
    mq_setup();
}

/* Third safety review: a stale JOB the queue cannot vouch for is reported
 * STALE_UNKNOWN (it may already have run), and nothing is ever re-queued. */
#define STALE_UNKNOWN_PREFIX "STALE_UNKNOWN:"
static bool stale_unknown_ack(void)
{
    return s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED") &&
           !strncmp(s_acks[0].error, STALE_UNKNOWN_PREFIX, strlen(STALE_UNKNOWN_PREFIX)) &&
           strlen(s_acks[0].error) <= 95U;
}

/* mode: 0 queued, 1 cancelled, 2 failed, 3 complete, 4 absent (record gone). */
static void stale_job_replay(uint32_t id, int mode, uint32_t t)
{
    mq_setup();
    telemetry_client_dispatch_command(job_json(id, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 1U, ack_sink, NULL);
    uint32_t local = s_ack_n ? s_acks[0].local_job : 0U;
    dispense_job_t run;
    if (mode == 4) job_queue_reset();   /* the queue no longer holds it at all */
    if (mode == 1) (void)job_queue_cancel(local);
    if (mode == 2 || mode == 3) {
        memset(&run, 0, sizeof(run));
        (void)job_queue_start_next_available(1U, t, &run);
        (void)job_queue_finish(run.id, mode == 2 ? JOB_FAILED : JOB_COMPLETE, 5000, "x", t + 2U);
    }
    for (uint32_t i = 0; i < 40U; i++) {
        telemetry_client_dispatch_command(cmd_json(id + 100U + i, "BOGUS", "null", 0, ""),
                                          TELEMETRY_CMD_HTTP, t, t + 2U, ack_sink, NULL);
    }
    s_ack_n = 0;
}

static void test_stale_job_unknown(void)
{
    uint32_t t = 900000U, queued;
    telemetry_cmd_stats_t st;

    stale_job_replay(7500U, 4, t);
    queued = jobs_queued();
    telemetry_client_cmd_stats(&st);
    uint32_t refused0 = st.stale_refused;
    telemetry_client_dispatch_command(job_json(7500, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    CK(stale_unknown_ack() && jobs_queued() == queued && queued == 0U,
       "stale_job_absent_failed_stale_unknown_not_enqueued");
    telemetry_client_cmd_stats(&st);
    CK(st.stale_refused == refused0 + 1U, "stale_job_absent_counted_stale_refused");

    stale_job_replay(7510U, 1, t);
    queued = jobs_queued();
    telemetry_client_dispatch_command(job_json(7510, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    CK(stale_unknown_ack() && jobs_queued() == queued, "stale_job_cancelled_failed_stale_unknown");

    stale_job_replay(7520U, 2, t);
    queued = jobs_queued();
    telemetry_client_dispatch_command(job_json(7520, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    CK(stale_unknown_ack() && jobs_queued() == queued, "stale_job_failed_state_failed_stale_unknown");

    stale_job_replay(7530U, 3, t);
    queued = jobs_queued();
    telemetry_client_dispatch_command(job_json(7530, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_HTTP, t, t + 3U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "APPLIED") && s_acks[0].local_job != 0U &&
       jobs_queued() == queued, "stale_job_complete_reacked_applied");

    stale_job_replay(7540U, 0, t);
    queued = jobs_queued();
    telemetry_client_dispatch_command(job_json(7540, "M1", "null", "", "tune-a", 1, "tune-a", 1, 0.11),
                                      TELEMETRY_CMD_MQTT, t, t + 3U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "QUEUED") && jobs_queued() == queued && queued == 1U,
       "stale_job_queued_reacked_queued");

    /* job_queue_find_by_remote: small locked lookup, no behaviour change */
    {
        job_view_t v;
        dispense_job_t full;
        memset(&v, 0, sizeof(v));
        CK(job_queue_find_by_remote(7540U, &v) && v.state == JOB_QUEUED && v.id != 0U &&
           job_queue_get(v.id, &full) && full.remote_command_id == 7540U &&
           full.channel_id == v.channel_id, "find_by_remote_returns_state_and_local_id");
        CK(!job_queue_find_by_remote(7541U, &v) && !job_queue_find_by_remote(0U, &v) &&
           !job_queue_find_by_remote(7540U, NULL), "find_by_remote_unknown_zero_null_false");
        (void)job_queue_cancel(v.id);
        CK(job_queue_find_by_remote(7540U, &v) && v.state == JOB_CANCELLED,
           "find_by_remote_reports_cancelled_record");
    }

    /* PUMP_START keeps its own refusal text */
    mq_setup();
    telemetry_client_dispatch_command(cmd_json(7600, "PUMP_START", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 1U, ack_sink, NULL);
    for (uint32_t i = 0; i < 40U; i++)
        telemetry_client_dispatch_command(cmd_json(7700U + i, "BOGUS", "null", 0, ""),
                                          TELEMETRY_CMD_HTTP, t, t + 2U, ack_sink, NULL);
    s_ack_n = 0;
    telemetry_client_dispatch_command(cmd_json(7600, "PUMP_START", "\"CH1\"", 0, ""),
                                      TELEMETRY_CMD_MQTT, t, t + 3U, ack_sink, NULL);
    CK(s_ack_n == 1 && !strcmp(s_acks[0].state, "FAILED") && strstr(s_acks[0].error, "stale") &&
       strncmp(s_acks[0].error, STALE_UNKNOWN_PREFIX, strlen(STALE_UNKNOWN_PREFIX)) != 0,
       "stale_pump_start_text_unchanged");
    mq_setup();
}

static volatile bool s_mqtt_tests_done;

/* The suite's main task has a 3.5 KiB stack; these tests keep a few KiB of
 * locals, so they run in a task of their own. */
static void mqtt_tests_task(void *arg)
{
    (void)arg;
    test_core();
    test_live_body();
    test_commands();
    test_mqtt_tx_and_stop();
    test_ledger_floor();
    test_http_object_size();
    test_http_truncation();
    test_pin_identity();
    test_lost_ack_reack();
    test_stale_job_unknown();
    s_mqtt_tests_done = true;
    vTaskDelete(NULL);
}

void test_mqtt_run(void)
{
    ESP_LOGI("TEST", "mqtt link + command path tests");
    s_mqtt_tests_done = false;
    if (xTaskCreate(mqtt_tests_task, "mqtt_tests", 12288, NULL, 2, NULL) != pdPASS) {
        audit_check(false, "mqtt_tests_task_created");
        return;
    }
    while (!s_mqtt_tests_done) vTaskDelay(pdMS_TO_TICKS(20));
}
