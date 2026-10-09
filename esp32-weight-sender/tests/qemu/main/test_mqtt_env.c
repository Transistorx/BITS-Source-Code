#include "test_mqtt_env.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_link_core.h"
#include "scale_cmd.h"
#include "test_harness.h"

#define DEV  "bits-a4cf12ab34cd"
#define BOOT "a1b2c3d4"

static uint32_t m_now(void) { return 1000U; }
static bool m_link(const char **name) { *name = "ONLINE"; return true; }
static bool m_sample(uint8_t ch, uint32_t *seq, int32_t *g, bool *st)
{
    (void)ch;
    *seq = 5U;
    *g = 1500;
    *st = true;
    return true;
}
static void m_delay(uint32_t ms) { (void)ms; }

static mqtt_link_weight_t sample_w(uint32_t cas_seq)
{
    mqtt_link_weight_t w = {
        .uptime_ms = 123456U, .scale_id = "SCALE1", .src_uart = 1U, .weight_g = 2500, .stable = true,
        .age_ms = 12U, .cas_seq = cas_seq, .source = "CAS_RS485",
    };
    return w;
}

/* Minimal publish model of mqtt_link_weight_frame: gate + build + zero-timeout
 * enqueue; the sequence advances only when the enqueue succeeded. */
static bool offer(mqtt_link_wgate_t *g, mqtt_link_queues_t *q, const mqtt_link_weight_t *w,
                  bool valid_real)
{
    char json[MQTT_LINK_ENV_MAX + 1U];
    if (!valid_real) return false; /* weight_tx never calls it for stale/invalid data */
    if (!mqtt_link_wgate_new_frame(g, w->src_uart, w->cas_seq)) return false;
    if (mqtt_link_weight_json(json, sizeof(json), BOOT, g->seq_ctl, w, false) == 0U) return false;
    if (mqtt_link_queue_push_pub(q, "cas/" DEV "/" MQTT_SUFFIX_WEIGHT_CTL, json, 0, false) !=
        MQTT_ENQ_OK) {
        return false;
    }
    g->seq_ctl++;
    return true;
}

void test_mqtt_env_run(void)
{
    /* boot_id */
    char b[MQTT_LINK_BOOT_ID_LEN + 1U];
    mqtt_link_boot_id_fmt(b, 0xA1B2C3D4U);
    test_check(strcmp(b, BOOT) == 0 && mqtt_link_boot_id_valid(b), "env_boot_id_8_lower_hex");
    mqtt_link_boot_id_fmt(b, 0x0000000FU);
    test_check(strcmp(b, "0000000f") == 0, "env_boot_id_zero_padded");
    test_check(!mqtt_link_boot_id_valid("A1B2C3D4") && !mqtt_link_boot_id_valid("a1b2c3d") &&
                   !mqtt_link_boot_id_valid("a1b2c3d45") && !mqtt_link_boot_id_valid(NULL),
               "env_boot_id_rejects_bad");

    /* birth */
    mqtt_link_presence_t pr;
    test_check(mqtt_link_presence_plan(&pr, DEV) &&
                   mqtt_link_presence_set_birth(&pr, BOOT, "[\"weight_mqtt\"]",
                                                "weight-sender 7.0", "weight_sender", "SCALE1"),
               "env_birth_built");
    test_check(strlen(pr.birth_payload) <= MQTT_LINK_ENV_MAX && strstr(pr.birth_payload, "\"online\":true") &&
                   strstr(pr.birth_payload, "\"boot_id\":\"" BOOT "\"") &&
                   strstr(pr.birth_payload, "\"caps\":[\"weight_mqtt\"]") &&
                   strstr(pr.birth_payload, "\"fw\":\"weight-sender 7.0\"") &&
                   strstr(pr.birth_payload, "\"role\":\"weight_sender\"") &&
                   strstr(pr.birth_payload, "\"scale_id\":\"SCALE1\""),
               "env_birth_fields_and_size");
    test_check(pr.qos == 1 && pr.retain && strcmp(pr.lwt_payload, "{\"online\":false}") == 0,
               "env_birth_qos1_retained_lwt_unchanged");
    char big_fw[300];
    memset(big_fw, 'f', sizeof(big_fw) - 1U);
    big_fw[sizeof(big_fw) - 1U] = '\0';
    test_check(!mqtt_link_presence_set_birth(&pr, BOOT, "[\"weight_mqtt\"]", big_fw, "r", "SCALE1") &&
                   !mqtt_link_presence_set_birth(&pr, BOOT, "[]", "a\"b", "r", "SCALE1") &&
                   !mqtt_link_presence_set_birth(&pr, BOOT, "[]", "fw", "r", "bad id"),
               "env_birth_over_256_or_unescaped_refused");

    /* weight/ctl envelope */
    char json[MQTT_LINK_ENV_MAX + 1U];
    mqtt_link_weight_t w = sample_w(77U);
    size_t n = mqtt_link_weight_json(json, sizeof(json), BOOT, 41U, &w, false);
    test_check(n > 0U && n <= MQTT_LINK_ENV_MAX && strlen(json) == n, "env_ctl_size_limit");
    test_check(strstr(json, "\"schema_version\":1") && strstr(json, "\"boot_id\":\"" BOOT "\"") &&
                   strstr(json, "\"seq\":41,") && strstr(json, "\"uptime_ms\":123456") &&
                   strstr(json, "\"scale_id\":\"SCALE1\"") && strstr(json, "\"src_uart\":\"UART1\"") && strstr(json, "\"weight_g\":2500") &&
                   strstr(json, "\"stable\":true") && strstr(json, "\"age_ms\":12") &&
                   strstr(json, "\"cas_seq\":77") && strstr(json, "\"source\":\"CAS_RS485\"") &&
                   strstr(json, "message_id") == NULL && strstr(json, "channel") == NULL &&
                   strstr(json, "weight1_g") == NULL && strstr(json, "weight2_g") == NULL &&
                   strstr(json, "pump") == NULL,
               "env_ctl_fields");
    test_check(strstr(json, "RS232") == NULL, "env_ctl_source_not_rs232_on_rs485");
    mqtt_link_weight_t worst = { .uptime_ms = 4294967295U, .scale_id = "AAAAAAAAAAAAAAAA", .src_uart = 2U, .weight_g = -2147483647,
                                 .stable = false, .age_ms = 4294967295U, .cas_seq = 4294967295U,
                                 .source = "CAS_RS485" };
    n = mqtt_link_weight_json(json, sizeof(json), BOOT, 4294967295U, &worst, false);
    test_check(n > 0U && n <= MQTT_LINK_ENV_MAX, "env_worst_case_fits_256");
    char tjson[MQTT_LINK_PAYLOAD_MAX + 1U];
    test_check(mqtt_link_weight_json(tjson, sizeof(tjson), BOOT, 4294967295U, &worst, true) > 0U &&
                   strstr(tjson, "\"message_id\":\"" BOOT "-4294967295\"") != NULL,
               "env_telemetry_message_id");
    mqtt_link_weight_t bad = sample_w(1U);
    bad.src_uart = 3U;
    mqtt_link_weight_t noid = sample_w(1U);
    noid.scale_id = "bad id";
    test_check(mqtt_link_weight_json(json, sizeof(json), BOOT, 1U, &bad, false) == 0U &&
                   mqtt_link_weight_json(json, sizeof(json), BOOT, 1U, &noid, false) == 0U &&
                   mqtt_link_weight_json(json, sizeof(json), "xyz", 1U, &w, false) == 0U,
               "env_ctl_bad_uart_scale_id_or_boot_refused");

    /* scale_id validation: [A-Za-z0-9_-]{1,16} */
    test_check(mqtt_link_scale_id_valid("SCALE1") && mqtt_link_scale_id_valid("a_B-9") &&
                   mqtt_link_scale_id_valid("AAAAAAAAAAAAAAAA") &&
                   !mqtt_link_scale_id_valid("AAAAAAAAAAAAAAAAA") && !mqtt_link_scale_id_valid("") &&
                   !mqtt_link_scale_id_valid(NULL) && !mqtt_link_scale_id_valid("a b") &&
                   !mqtt_link_scale_id_valid("a.b") && !mqtt_link_scale_id_valid("a\"b") &&
                   !mqtt_link_scale_id_valid("a/b"),
               "env_scale_id_validation");

    /* dedupe, retain=false, drop-when-full, sequence */
    mqtt_link_wgate_t g;
    mqtt_link_wgate_init(&g);
    mqtt_link_queues_t q;
    test_check(mqtt_link_queues_init(&q, 1, 2), "env_queue_init");
    mqtt_link_weight_t f1 = sample_w(10U);
    test_check(offer(&g, &q, &f1, true), "env_first_frame_published");
    test_check(!offer(&g, &q, &f1, true), "env_same_cas_seq_deduped");
    test_check(mqtt_link_queue_pending(&q) == 1U && g.seq_ctl == 1U, "env_dedupe_no_extra_enqueue");
    mqtt_link_weight_t f2 = sample_w(11U);
    test_check(offer(&g, &q, &f2, true) && g.seq_ctl == 2U, "env_new_cas_seq_published_seq_increments");
    mqtt_item_t *it = pvPortMalloc(sizeof(*it));
    test_check(it != NULL && mqtt_link_queue_next(&q, it, 0, 0U, 1000U) && it->kind == MQTT_ITEM_PUB && it->len <= MQTT_LINK_ENV_MAX &&
                   it->qos == 0 && !it->retain && strcmp(it->topic, "cas/" DEV "/weight/ctl") == 0 &&
                   strstr(it->data, "\"seq\":0,") != NULL,
               "env_ctl_qos0_retain_false_topic");

    mqtt_link_weight_t f3 = sample_w(12U);
    mqtt_link_weight_t f4 = sample_w(13U);
    mqtt_link_weight_t f5 = sample_w(14U);
    bool a3 = offer(&g, &q, &f3, true);   /* queue holds f2 + f3: full */
    bool a4 = offer(&g, &q, &f4, true);   /* dropped */
    bool a5 = offer(&g, &q, &f5, true);   /* dropped */
    test_check(a3 && !a4 && !a5 && q.pub_dropped_full >= 2U, "env_full_queue_drops_and_counts");
    test_check(g.seq_ctl == 3U, "env_seq_not_consumed_by_drop");

    /* No publish for stale/invalid data */
    mqtt_link_wgate_t g2;
    mqtt_link_wgate_init(&g2);
    mqtt_link_weight_t fz = sample_w(99U);
    fz.weight_g = 0;
    test_check(!offer(&g2, &q, &fz, false) && g2.seq_ctl == 0U && !g2.have_cas_seq[0],
               "env_no_publish_for_stale_or_invalid_frame");

    /* Single seq counter across both UARTs; dedupe by cas_seq stays per UART. */
    mqtt_link_wgate_t g5;
    mqtt_link_wgate_init(&g5);
    mqtt_link_queues_t q5;
    test_check(mqtt_link_queues_init(&q5, 1, 4), "env_queue5_init");
    mqtt_link_weight_t u1 = sample_w(1U);
    mqtt_link_weight_t u2 = sample_w(1U);
    u2.src_uart = 2U;
    mqtt_item_t *it5 = pvPortMalloc(sizeof(*it5));
    test_check(offer(&g5, &q5, &u1, true) && offer(&g5, &q5, &u2, true) && g5.seq_ctl == 2U &&
                   it5 != NULL && mqtt_link_queue_next(&q5, it5, 0, 0U, 1000U) && strstr(it5->data, "\"seq\":0,") &&
                   strstr(it5->data, "\"src_uart\":\"UART1\"") &&
                   mqtt_link_queue_next(&q5, it5, 0, 0U, 1000U) && strstr(it5->data, "\"seq\":1,") &&
                   strstr(it5->data, "\"src_uart\":\"UART2\""),
               "env_single_seq_counter_across_uarts");
    vPortFree(it5);
    mqtt_link_queues_deinit(&q5);

    /* Dedupe is per UART: equal cas_seq on UART1 and UART2 are both new. */
    mqtt_link_wgate_t g4;
    mqtt_link_wgate_init(&g4);
    test_check(mqtt_link_wgate_new_frame(&g4, 1U, 5U) && mqtt_link_wgate_new_frame(&g4, 2U, 5U) &&
                   !mqtt_link_wgate_new_frame(&g4, 1U, 5U) && !mqtt_link_wgate_new_frame(&g4, 2U, 5U) &&
                   !mqtt_link_wgate_new_frame(&g4, 3U, 6U),
               "env_dedupe_per_uart");
    vPortFree(it);
    mqtt_link_queues_deinit(&q);

    /* telemetry/weight reduced stream */
    mqtt_link_wgate_t g3;
    mqtt_link_wgate_init(&g3);
    test_check(mqtt_link_wgate_tel_due(&g3, 1000U, 500U) && !mqtt_link_wgate_tel_due(&g3, 1100U, 500U) &&
                   !mqtt_link_wgate_tel_due(&g3, 1499U, 500U) && mqtt_link_wgate_tel_due(&g3, 1500U, 500U),
               "env_telemetry_weight_rate_limited");
    test_check(mqtt_link_wgate_tel_due(&g3, 100U, 500U), "env_telemetry_rate_wrap_safe");

    /* backoff jitter: never below base, never above cap */
    bool ok = true;
    for (uint32_t a = 0U; a < 8U; a++) {
        uint32_t j = mqtt_link_backoff_jitter_ms(a, 0xFFFFFFFFU - a * 7U);
        if (j < mqtt_link_backoff_ms(a) || j > MQTT_LINK_BACKOFF_MAX_MS) ok = false;
    }
    test_check(ok && mqtt_link_backoff_jitter_ms(0U, 0U) == 1000U, "env_backoff_jitter_bounded");

    /* Guard: the test seam exists only because this project is the QEMU one
     * (scale_cmd_guard.h #errors otherwise); the encoder stays unverified. */
#if defined(SCALE_CMD_TEST_HOOKS) && defined(BITS_QEMU_TEST_PROJECT)
    test_check(SCALE_CMD_ENCODER_VERIFIED == 0, "env_guard_hooks_only_in_qemu_encoder_unverified");
#else
    test_check(false, "env_guard_hooks_only_in_qemu_encoder_unverified");
#endif

    /* ACK carries boot_id; ZERO/TARE still FAILED (not verified) */
    scale_cmd_cfg_t cfg = {
        .now_ms = m_now, .link_online = m_link, .post_sample = m_sample, .send_frame = NULL,
        .delay_ms = m_delay, .encode = NULL, .active_channel = 2U, .settle_timeout_ms = 100U,
    };
    strcpy(cfg.device_id, DEV);
    strcpy(cfg.boot_id, BOOT);
    static scale_cmd_t s;
    test_check(scale_cmd_init(&s, &cfg), "env_scale_cmd_init");
    const char *types[2] = { "ZERO", "TARE" };
    for (unsigned i = 0U; i < 2U; i++) {
        char p[200];
        scale_cmd_ack_t a;
        snprintf(p, sizeof(p),
                 "{\"command_id\":%u,\"type\":\"%s\",\"channel_id\":\"CH2\","
                 "\"issued_at\":\"2026-10-07T08:15:30.123Z\",\"ttl_ms\":10000}",
                 900U + i, types[i]);
        bool queued = scale_cmd_submit(&s, p, strlen(p), false, 1000U, &a) == SCALE_SUBMIT_QUEUED;
        bool ran = scale_cmd_run_one(&s, 0, &a);
        char ack[MQTT_LINK_PAYLOAD_MAX];
        size_t an = scale_cmd_build_ack_json(&s, &a, ack, sizeof(ack));
        char name[48];
        snprintf(name, sizeof(name), "env_%s_still_failed_with_boot_id", types[i]);
        test_check(queued && ran && !a.applied && a.result == SCALE_RESULT_FAILED && an > 0U &&
                       strstr(ack, "\"state\":\"FAILED\"") && strstr(ack, "\"boot_id\":\"" BOOT "\"") &&
                       strstr(ack, "\"state\":\"APPLIED\"") == NULL,
                   name);
    }
    vSemaphoreDelete(s.lock);
    vSemaphoreDelete(s.wake);
}
