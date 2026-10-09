/* Single-scale operation (CONTRACT 9.11): TWO pumps, ONE scale moved by the operator.
 * Software only. The relay seam is the TEST-ONLY fake (BITS_QEMU_TEST_HOOKS); no
 * hardware is ever actuated. All thresholds used here are the UNVALIDATED defaults
 * unless a test says otherwise. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dual_dispense_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "job_queue.h"
#include "mqtt_link_core.h"
#include "relay_driver.h"
#include "safety_manager.h"
#include "telemetry_client.h"
#include "weight_mqtt.h"
#include "weight_receiver.h"

extern void audit_check(bool condition, const char *name);
extern void dual_dispense_controller_test_set_relay_hook(esp_err_t (*set_fn)(uint8_t, bool),
                                                         bool (*get_fn)(uint8_t));
#define CK(cond, name) audit_check((cond), (name))

static const char *const A = "aabbccdd";
static const char *const B = "55667788";

/* ---- fake relays ----------------------------------------------------------- */
static bool s_fake[RELAY_COUNT];
static bool s_ever_on[2];
static bool s_both_on_seen;
static int s_on_edges;

static esp_err_t fake_set(uint8_t i, bool on)
{
    if (i >= RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    if (on && !s_fake[i]) s_on_edges++;
    s_fake[i] = on;
    if (on && i < 2U) s_ever_on[i] = true;
    if (s_fake[0] && s_fake[1]) s_both_on_seen = true;
    return ESP_OK;
}
static bool fake_get(uint8_t i) { return i < RELAY_COUNT && s_fake[i]; }

static void fake_reset(void)
{
    memset(s_fake, 0, sizeof(s_fake));
    s_ever_on[0] = s_ever_on[1] = false;
    s_both_on_seen = false;
    s_on_edges = 0;
}

/* ---- fixture --------------------------------------------------------------- */
static uint32_t s_seq, s_cmd = 7000;
static const char *s_boot = A;
static const char *s_sid = "SCALE1";
static dual_scale_cfg_t s_cfg;

static void cfg_apply(void) { dual_dispense_controller_set_scale_cfg(&s_cfg); }

static void cfg_baseline(void)
{
    dual_scale_cfg_t c;
    dual_dispense_controller_scale_cfg_default(&c);
    c.start_max_g = 0;   /* the pre-9.11 suites expect this baseline (see test_main.c) */
    dual_dispense_controller_set_scale_cfg(&c);
}

/* Fresh controller; 5000 g slots on both pumps (kp 0.99, tolerance 20, overshoot 100,
 * inflight 20 so the known in-band stall cannot interfere); production defaults for
 * every single-scale threshold, except a 1 s NO_PROGRESS learn window. */
static void fx_init(void)
{
    fake_reset();
    dual_dispense_controller_test_set_relay_hook(fake_set, fake_get);
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    for (uint8_t ch = 1; ch <= 2U; ch++) {
        (void)dual_dispense_controller_set_pending_profile_staged(ch, ch == 1U ? "ss-slot1" : "ss-slot2",
              ch, 5000, 0.99f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15, 200, 20);
    }
    dual_dispense_controller_scale_cfg_default(&s_cfg);
    s_cfg.np_learn_ms = 1000U;
    cfg_apply();
    s_seq = 0U;
    s_boot = A;
    s_sid = "SCALE1";
    telemetry_client_test_reset_commands();
    telemetry_client_test_reset_ready();
}

static void fx_end(void)
{
    dual_dispense_controller_test_set_relay_hook(NULL, NULL);
    fake_reset();
    (void)safety_manager_init();
    (void)dual_dispense_controller_init();
    cfg_baseline();
}

static void feed_id(uint32_t now, int32_t w, bool st, const char *sid, const char *boot)
{
    weight_msg_t m;
    memset(&m, 0, sizeof(m));
    m.weight_g = w;
    m.stable = st;
    m.has_stable = true;
    m.has_scale_id = true;
    snprintf(m.scale_id, sizeof(m.scale_id), "%s", sid);
    m.has_boot_id = true;
    snprintf(m.boot_id, sizeof(m.boot_id), "%s", boot);
    m.has_sequence = true;
    m.sequence = ++s_seq;
    dual_dispense_controller_on_weight(&m, now);
}
static void feed(uint32_t now, int32_t w, bool st) { feed_id(now, w, st, s_sid, s_boot); }
static void tk(uint32_t now) { dual_dispense_controller_tick(now); }

/* One production-cadence step (100 ms): sample, then the supervisor tick. */
static void step(uint32_t *t, int32_t w, bool st)
{
    *t += 100U;
    feed(*t, w, st);
    tk(*t);
}
static void hold(uint32_t *t, uint32_t ms, int32_t w, bool st)
{
    uint32_t end = *t + ms;
    while (*t < end) step(t, w, st);
}
/* The pump adds per_on grams per tick while its relay was ON at the previous tick. */
static void pstep(uint32_t *t, int32_t *w, uint8_t ch, int32_t per_on)
{
    *t += 100U;
    if (fake_get((uint8_t)(ch - 1U))) *w += per_on;
    feed(*t, *w, false);
    tk(*t);
}

static dual_channel_snapshot_t snap(uint8_t ch, uint32_t now)
{
    dual_channel_snapshot_t s;
    memset(&s, 0, sizeof(s));
    (void)dual_dispense_controller_snapshot(ch, now, &s);
    return s;
}
static bool job_is(uint32_t id, job_state_t st, const char *err)
{
    dispense_job_t j;
    if (!job_queue_get(id, &j) || j.state != st) return false;
    return err == NULL || strcmp(j.error, err) == 0;
}
static bool job_running(uint32_t id)
{
    dispense_job_t j;
    return job_queue_get(id, &j) && j.state == JOB_RUNNING;
}
static uint32_t add_job(uint8_t ch)
{
    uint32_t id = 0U;
    (void)dual_dispense_controller_add_material_server_job_pinned(ch, 5000, 0, s_cmd++, NULL, &id);
    return id;
}
static dual_ready_result_t rdy(uint8_t ch, uint32_t job, const char *boot, uint32_t age, uint32_t now)
{
    dual_ready_req_t r;
    memset(&r, 0, sizeof(r));
    r.has_job_id = true;
    r.has_boot = true;
    r.job_id = job;
    r.age_ms = age;
    snprintf(r.scale_boot_id, sizeof(r.scale_boot_id), "%s", boot);
    return dual_dispense_controller_scale_ready(ch, &r, now);
}
static bool waiting(uint8_t ch, uint32_t job, uint32_t now)
{
    dual_channel_snapshot_t s = snap(ch, now);
    return s.state == DCH_WAITING_FOR_SCALE_MOVE && s.awaiting_operator_ready &&
           s.active_job_id == job && !s.relay_on && !s.owns_scale;
}
/* Queue a job on `ch`, give the scale a 3.4 s stable run at w0 and confirm READY. */
static uint32_t ready_job(uint8_t ch, uint32_t *t, int32_t w0)
{
    uint32_t id = add_job(ch);
    hold(t, 3400U, w0, true);
    dual_ready_result_t r = rdy(ch, id, s_boot, 0U, *t);
    return r == READY_OK ? id : 0U;
}
/* Pump `ch` (10 g per ON tick) until the job leaves RUNNING. Returns ticks used. */
static int run_to_end(uint8_t ch, uint32_t *t, int32_t *w, uint32_t job, int max_ticks)
{
    int n = 0;
    while (n < max_ticks && job_running(job)) {
        pstep(t, w, ch, 10);
        n++;
    }
    return n;
}

/* ---- the CLEAR the operator performs after a latched safety fault ---------- */
static void operator_clear(void)
{
    (void)safety_manager_clear();
    dual_dispense_controller_clear_emergency_stop();
}

/* ---------------------------------------------------------------------------
 * 1. Each pump gets weight from the ONE scale whatever the sender's UART tag
 * ------------------------------------------------------------------------ */
static void ctl_handler(const weight_msg_t *m, uint32_t ms) { dual_dispense_controller_on_weight(m, ms); }
static uint32_t s_mq_seq;
static weight_mqtt_result_t mq(uint32_t now, long w, bool st, const char *uart)
{
    char pl[256];
    int n = snprintf(pl, sizeof(pl),
        "{\"schema_version\":1,\"boot_id\":\"%s\",\"seq\":%lu,\"uptime_ms\":%lu,\"scale_id\":\"SCALE1\","
        "\"src_uart\":\"%s\",\"weight_g\":%ld,\"stable\":%s,\"age_ms\":0,\"cas_seq\":9,\"source\":\"CAS_RS232\"}",
        s_boot, (unsigned long)++s_mq_seq, (unsigned long)(now - 500U), uart, w, st ? "true" : "false");
    return weight_mqtt_on_payload(pl, (size_t)n, now);
}

static void mq_pump_job(uint8_t ch, const char *uart, uint32_t *t, int32_t *w, const char *tag)
{
    char n[96];
    uint32_t id = add_job(ch);
    for (int i = 0; i < 34; i++) { *t += 100U; (void)mq(*t, *w, true, uart); tk(*t); }
    snprintf(n, sizeof(n), "ss_%s_waits_for_scale_move_fed_by_%s", tag, uart);
    CK(waiting(ch, id, *t), n);
    snprintf(n, sizeof(n), "ss_%s_ready_with_job_and_boot_ok", tag);
    CK(rdy(ch, id, A, 0U, *t) == READY_OK, n);
    int guard = 0;
    while (job_running(id) && guard++ < 1500) {
        *t += 100U;
        if (fake_get((uint8_t)(ch - 1U))) *w += 10;
        (void)mq(*t, *w, false, uart);
        tk(*t);
    }
    dual_channel_snapshot_t s = snap(ch, *t);
    snprintf(n, sizeof(n), "ss_%s_job_completes_on_scale_tagged_%s", tag, uart);
    CK(job_is(id, JOB_COMPLETE, NULL) && s.current_weight_g >= 4980 && s.current_weight_g <= 5100, n);
    snprintf(n, sizeof(n), "ss_%s_only_its_own_relay_ran", tag);
    CK(s_ever_on[ch - 1U] && !s_ever_on[2U - ch] && !s_both_on_seen, n);
}

static void test_pump_gets_weight_whatever_the_tag(void)
{
    fx_init();
    (void)weight_receiver_init();
    weight_receiver_set_actuation_handler(ctl_handler);
    weight_mqtt_init();
    weight_mqtt_set_enabled(true);
    s_mq_seq = 100U;
    uint32_t t = 1000U;
    int32_t w = 0;
    (void)mq(t, 0, true, "UART2");   /* first message of a boot is pending, not usable */
    mq_pump_job(1, "UART2", &t, &w, "pump1");
    CK(!snap(2, t).have_weight, "ss_pump2_never_saw_a_sample_while_pump1_owned_the_scale");

    /* The vessel is emptied and moved to Pump 2; the tag is UART1 now. Same scale. */
    w = 0;
    s_ever_on[0] = s_ever_on[1] = false;
    s_both_on_seen = false;
    mq_pump_job(2, "UART1", &t, &w, "pump2");

    weight_receiver_set_actuation_handler(NULL);
    (void)weight_receiver_init();
    weight_mqtt_init();
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 2. No owner: the pumps stay stale, no relay ever moves
 * ------------------------------------------------------------------------ */
static void test_no_owner_pumps_stale(void)
{
    fx_init();
    uint32_t t = 1000U;
    hold(&t, 2000U, 100, true);
    dual_scale_info_t si;
    dual_dispense_controller_scale_info(t, &si);
    CK(si.online && si.stable && si.weight_g == 100 && strcmp(si.boot_id, A) == 0 &&
       strcmp(si.scale_id, "SCALE1") == 0, "ss_scale_view_tracks_the_scale_with_no_owner");
    CK(!snap(1, t).have_weight && !snap(2, t).have_weight &&
       snap(1, t).weight_age_ms == UINT32_MAX && snap(2, t).weight_age_ms == UINT32_MAX,
       "ss_no_owner_neither_pump_has_weight");
    CK(!fake_get(0) && !fake_get(1) && !s_ever_on[0] && !s_ever_on[1], "ss_no_owner_relays_off");

    /* A queued job waits for READY: fresh stable weight alone never starts it. */
    uint32_t id = add_job(1);
    hold(&t, 4000U, 100, true);
    CK(waiting(1, id, t) && !snap(1, t).have_weight, "ss_waiting_job_pump_still_has_no_weight");
    CK(snap(1, t).weight_age_ms == UINT32_MAX && !snap(2, t).have_weight, "ss_waiting_pumps_stay_stale");
    CK(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE && !s_ever_on[0] && !s_ever_on[1],
       "ss_waiting_job_does_not_take_the_scale");

    /* Nothing starts it for ~590 s of perfect weight: only a READY does. */
    for (int i = 0; i < 590; i++) { t += 1000U; feed(t, 100, true); tk(t); }
    CK(waiting(1, id, t) && !s_ever_on[0] && !s_ever_on[1] && job_running(id),
       "ss_perfect_weight_for_590s_never_auto_starts_the_job");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 3. READY refusals (each refused READY changes nothing)
 * ------------------------------------------------------------------------ */
static void test_ready_refusals(void)
{
    fx_init();
    uint32_t t = 1000U;
    uint32_t id = add_job(1);
    hold(&t, 1000U, 100, true);
    CK(waiting(1, id, t), "ss_ready_setup_job_waits");
    CK(rdy(1, id, A, 0U, t) == READY_SCALE_UNSTABLE, "ss_ready_refused_unstable_only_1s");
    hold(&t, 2500U, 100, true);
    CK(rdy(1, id + 1U, A, 0U, t) == READY_JOB_MISMATCH, "ss_ready_refused_wrong_job_id");
    CK(rdy(1, 0U, A, 0U, t) == READY_JOB_MISMATCH, "ss_ready_refused_job_id_zero");
    CK(rdy(1, id, B, 0U, t) == READY_SCALE_BOOT_MISMATCH, "ss_ready_refused_wrong_scale_boot_id");
    CK(rdy(1, id, "aabbccd", 0U, t) == READY_SCALE_BOOT_MISMATCH, "ss_ready_refused_truncated_boot_id");
    CK(rdy(1, id, "", 0U, t) == READY_SCALE_BOOT_MISMATCH, "ss_ready_refused_empty_boot_id");
    CK(rdy(1, id, A, 120001U, t) == READY_STALE, "ss_ready_refused_older_than_ready_max_age");
    CK(rdy(2, id, A, 0U, t) == READY_NOT_WAITING, "ss_ready_refused_pump_without_waiting_job");
    CK(rdy(3, id, A, 0U, t) == READY_BAD_CHANNEL && rdy(0, id, A, 0U, t) == READY_BAD_CHANNEL,
       "ss_ready_refused_bad_channel");
    CK(dual_dispense_controller_scale_ready(1, NULL, t) == READY_MALFORMED, "ss_ready_refused_null_request");

    step(&t, 100, false);   /* one unstable sample restarts the stable run */
    CK(rdy(1, id, A, 0U, t) == READY_SCALE_UNSTABLE, "ss_ready_refused_after_one_unstable_sample");
    hold(&t, 3300U, 100, true);
    CK(rdy(1, id, A, 0U, t + 6000U) == READY_SCALE_OFFLINE, "ss_ready_refused_scale_offline");
    safety_manager_raise(SAFETY_WEIGHT_STALE, t);
    CK(rdy(1, id, A, 0U, t) == READY_BLOCKED, "ss_ready_refused_while_a_safety_fault_is_active");
    (void)safety_manager_clear();

    dual_channel_snapshot_t s = snap(1, t);
    CK(waiting(1, id, t) && job_running(id) && dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE &&
       !s_ever_on[0] && s.start_weight_g == 0, "ss_refused_ready_changed_nothing");

    CK(rdy(1, id, A, 120000U, t) == READY_OK, "ss_ready_at_exactly_ready_max_age_accepted");
    s = snap(1, t);
    dispense_job_t j;
    CK(s.state == DCH_WAIT_WEIGHT && s.owns_scale && dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1 &&
       s.start_weight_g == 100 && job_queue_get(id, &j) && j.start_g == 100,
       "ss_ready_accepted_pump_owns_scale_start_weight_recorded");
    CK(rdy(1, id, A, 0U, t) == READY_NOT_WAITING, "ss_second_ready_refused_not_waiting");
    CK(!s_ever_on[0] && !s_ever_on[1], "ss_ready_itself_never_turns_a_relay_on");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 4. Start weight plausibility (UNVALIDATED start_max_g)
 * ------------------------------------------------------------------------ */
static void test_start_weight(void)
{
    fx_init();
    uint32_t t = 1000U;
    uint32_t id = add_job(1);
    hold(&t, 3400U, 800, true);
    CK(rdy(1, id, A, 0U, t) == READY_START_WEIGHT, "ss_ready_refused_start_weight_above_start_max_g");
    CK(waiting(1, id, t) && !s_ever_on[0], "ss_start_weight_refusal_leaves_the_job_waiting");
    hold(&t, 500U, 500, true);
    CK(rdy(1, id, A, 0U, t) == READY_OK, "ss_start_weight_equal_to_limit_accepted");
    CK(snap(1, t).start_weight_g == 500, "ss_start_weight_recorded_in_snapshot");

    fx_init();
    s_cfg.start_max_g = 0;
    cfg_apply();
    t = 1000U;
    id = add_job(1);
    hold(&t, 3400U, 4000, true);
    CK(rdy(1, id, A, 0U, t) == READY_OK, "ss_start_max_g_zero_disables_the_check");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 5. Alternating pumps always prompt; reconfirm_same_pump
 * ------------------------------------------------------------------------ */
static void test_alternating_and_reconfirm(void)
{
    fx_init();
    uint32_t t = 1000U;
    int32_t w = 0;
    uint32_t j1 = add_job(1), j2 = add_job(2), j3 = add_job(1);
    hold(&t, 3400U, 0, true);
    CK(waiting(1, j1, t) && waiting(2, j2, t), "ss_both_pumps_prompt_for_their_first_jobs");
    CK(rdy(1, j1, A, 0U, t) == READY_OK, "ss_alt_ready_pump1");
    CK(rdy(2, j2, A, 0U, t) == READY_SCALE_BUSY, "ss_alt_pump2_ready_refused_while_pump1_owns_the_scale");
    (void)run_to_end(1, &t, &w, j1, 1500);
    CK(job_is(j1, JOB_COMPLETE, NULL), "ss_alt_job1_complete");
    w = 0;
    hold(&t, 3400U, 0, true);
    CK(waiting(1, j3, t) && waiting(2, j2, t), "ss_alt_next_jobs_both_prompt_even_for_the_same_pump");
    CK(!s_ever_on[1], "ss_alt_pump2_never_started_without_its_ready");
    CK(rdy(2, j2, A, 0U, t) == READY_OK, "ss_alt_ready_pump2");
    (void)run_to_end(2, &t, &w, j2, 1500);
    CK(job_is(j2, JOB_COMPLETE, NULL) && s_ever_on[1] && !s_both_on_seen, "ss_alt_job2_complete_on_pump2");
    w = 0;
    hold(&t, 3400U, 0, true);
    CK(waiting(1, j3, t), "ss_alt_pump1_job_prompts_again_after_pump2_ran");
    CK(rdy(1, j3, A, 0U, t) == READY_OK, "ss_alt_ready_pump1_again");
    (void)run_to_end(1, &t, &w, j3, 1500);
    CK(job_is(j3, JOB_COMPLETE, NULL), "ss_alt_job3_complete");

    /* reconfirm_same_pump = 1 (default): two consecutive jobs on ONE pump still prompt. */
    fx_init();
    t = 1000U; w = 0;
    uint32_t a1 = ready_job(1, &t, 0);
    (void)run_to_end(1, &t, &w, a1, 1500);
    uint32_t a2 = add_job(1);
    w = 0;
    hold(&t, 3400U, 0, true);
    CK(job_is(a1, JOB_COMPLETE, NULL) && waiting(1, a2, t) && !s_ever_on[1],
       "ss_reconfirm_default_second_job_on_same_pump_prompts");

    /* reconfirm_same_pump = 0: the SAME pump continues; the other pump and any change prompt. */
    fx_init();
    s_cfg.reconfirm_same_pump = false;
    cfg_apply();
    t = 1000U; w = 0;
    uint32_t other = add_job(2);
    uint32_t b1 = ready_job(1, &t, 0);
    CK(b1 != 0U && waiting(2, other, t), "ss_noreconfirm_setup");
    (void)run_to_end(1, &t, &w, b1, 1500);
    CK(job_is(b1, JOB_COMPLETE, NULL), "ss_noreconfirm_first_job_complete");
    w = 0;
    feed(t + 100U, 0, true);
    t += 100U;
    uint32_t b2 = add_job(1);
    tk(t);
    dual_channel_snapshot_t s = snap(1, t);
    CK(s.active_job_id == b2 && !s.awaiting_operator_ready && s.owns_scale &&
       (s.state == DCH_WAIT_WEIGHT || s.state == DCH_DISPENSING),
       "ss_noreconfirm_same_pump_next_job_starts_without_a_new_ready");
    CK(waiting(2, other, t) || snap(2, t).awaiting_operator_ready, "ss_noreconfirm_other_pump_still_prompts");
    CK(rdy(2, other, A, 0U, t) == READY_SCALE_BUSY, "ss_noreconfirm_other_pump_never_takes_the_owned_scale");

    /* ...but a sender reboot between the jobs voids the confirmation. */
    fx_init();
    s_cfg.reconfirm_same_pump = false;
    cfg_apply();
    t = 1000U; w = 0;
    b1 = ready_job(1, &t, 0);
    (void)run_to_end(1, &t, &w, b1, 1500);
    w = 0;
    s_boot = B;
    hold(&t, 400U, 0, true);
    b2 = add_job(1);
    tk(t + 100U);
    CK(waiting(1, b2, t + 100U), "ss_noreconfirm_sender_reboot_between_jobs_prompts");

    /* ...and a start weight above the limit (vessel not emptied) prompts too. */
    fx_init();
    s_cfg.reconfirm_same_pump = false;
    cfg_apply();
    t = 1000U; w = 0;
    b1 = ready_job(1, &t, 0);
    (void)run_to_end(1, &t, &w, b1, 1500);
    b2 = add_job(1);
    hold(&t, 400U, w, true);
    CK(waiting(1, b2, t), "ss_noreconfirm_full_vessel_between_jobs_prompts");

    /* ...and a fault between the jobs voids it. */
    fx_init();
    s_cfg.reconfirm_same_pump = false;
    cfg_apply();
    t = 1000U; w = 0;
    b1 = ready_job(1, &t, 0);
    (void)run_to_end(1, &t, &w, b1, 1500);
    dual_dispense_controller_emergency_stop(t + 10U);
    (void)safety_manager_release_estop();
    dual_dispense_controller_clear_emergency_stop();
    t += 20U;
    hold(&t, 400U, 0, true);
    b2 = add_job(1);
    hold(&t, 200U, 0, true);
    CK(waiting(1, b2, t), "ss_noreconfirm_fault_between_jobs_prompts");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 6. scale_move_timeout
 * ------------------------------------------------------------------------ */
static void test_move_timeout(void)
{
    fx_init();
    s_cfg.scale_move_timeout_ms = 5000U;
    cfg_apply();
    uint32_t t = 1000U;
    uint32_t id = add_job(1);
    uint32_t id2 = add_job(2);
    hold(&t, 4000U, 0, true);
    CK(waiting(1, id, t), "ss_timeout_job_waits");
    hold(&t, 1500U, 0, true);   /* 5.5 s since the job started waiting */
    dispense_job_t j;
    CK(job_queue_get(id, &j) && j.state == JOB_QUEUED && j.held, "ss_timeout_job_returned_to_queue_parked");
    CK(snap(1, t).active_job_id == 0U && !snap(1, t).awaiting_operator_ready && snap(1, t).state == DCH_IDLE,
       "ss_timeout_pump_idle_again");
    CK(!s_ever_on[0] && !s_ever_on[1], "ss_timeout_no_relay_was_ever_on");
    CK(job_queue_get(id, &j) && j.id == id && j.channel_id == 1U && j.target_g == 5000,
       "ss_timeout_job_keeps_identity");
    hold(&t, 3000U, 0, true);
    CK(job_queue_get(id, &j) && j.state == JOB_QUEUED && snap(1, t).active_job_id == 0U,
       "ss_timeout_parked_job_not_reclaimed_by_the_automatic_line");
    /* The other pump's own job has its own wait clock and also timed out. */
    CK(job_queue_get(id2, &j) && j.state == JOB_QUEUED, "ss_timeout_other_pump_job_also_returned");
    CK(job_queue_release(id), "ss_timeout_operator_releases_the_job");
    hold(&t, 300U, 0, true);
    CK(waiting(1, id, t), "ss_timeout_released_job_prompts_again");

    /* No safe un-start (the queue record is no longer RUNNING): cancelled with the reason. */
    fx_init();
    s_cfg.scale_move_timeout_ms = 5000U;
    cfg_apply();
    t = 1000U;
    id = add_job(1);
    hold(&t, 1000U, 0, true);
    CK(job_queue_finish(id, JOB_FAILED, 0, "x", t), "ss_timeout_fallback_setup");
    hold(&t, 5500U, 0, true);
    CK(job_is(id, JOB_CANCELLED, "SCALE_MOVE_TIMEOUT") && snap(1, t).state == DCH_IDLE &&
       snap(1, t).active_job_id == 0U && !s_ever_on[0], "ss_timeout_without_safe_unstart_cancels_with_reason");

    /* job_queue_unstart contract */
    fx_init();
    id = add_job(1);
    CK(!job_queue_unstart(id, true), "ss_unstart_refuses_a_queued_job");
    CK(!job_queue_unstart(9999U, true) && !job_queue_unstart(0U, true), "ss_unstart_refuses_unknown_ids");
    dispense_job_t run;
    CK(job_queue_start_next_available(1, 10U, &run) && run.id == id, "ss_unstart_setup_running");
    CK(job_queue_unstart(id, false) && job_queue_get(id, &run) && run.state == JOB_QUEUED && !run.held &&
       run.start_ms == 0U, "ss_unstart_returns_running_to_queued");
    CK(job_queue_start_next_available(1, 20U, &run) && run.id == id, "ss_unstart_job_can_start_again");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 7. Sender reboot (new boot_id)
 * ------------------------------------------------------------------------ */
static void test_sender_reboot(void)
{
    /* while waiting: the old confirmation quote is useless, a fresh stable run is needed */
    fx_init();
    uint32_t t = 1000U;
    uint32_t id = add_job(1);
    hold(&t, 3400U, 100, true);
    s_boot = B;                      /* the sender rebooted */
    step(&t, 100, true);
    CK(rdy(1, id, A, 0U, t) == READY_SCALE_BOOT_MISMATCH, "ss_reboot_waiting_old_boot_quote_refused");
    CK(rdy(1, id, B, 0U, t) == READY_SCALE_UNSTABLE, "ss_reboot_waiting_new_boot_needs_a_new_stable_run");
    hold(&t, 3300U, 100, true);
    dual_scale_info_t si;
    dual_dispense_controller_scale_info(t, &si);
    CK(strcmp(si.boot_id, B) == 0, "ss_reboot_status_reports_the_new_boot_id");
    CK(rdy(1, id, B, 0U, t) == READY_OK, "ss_reboot_waiting_new_boot_ready_accepted");

    /* while running: SCALE_MOVED, relay OFF in the same tick, latched, CLEAR required */
    fx_init();
    t = 1000U;
    int32_t w = 100;
    id = ready_job(1, &t, w);
    CK(id != 0U, "ss_reboot_running_setup_ready");
    pstep(&t, &w, 1, 10);
    pstep(&t, &w, 1, 10);
    CK(snap(1, t).relay_on && fake_get(0), "ss_reboot_running_relay_on_before");
    s_boot = B;
    t += 100U;
    feed(t, w, false);
    tk(t);
    CK(!fake_get(0) && !snap(1, t).relay_on, "ss_reboot_running_relay_off_same_tick");
    CK(job_is(id, JOB_FAILED, "SCALE_MOVED") && safety_manager_fault() == SAFETY_SCALE_MOVED,
       "ss_reboot_running_job_failed_scale_moved_fault_latched");
    CK(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE, "ss_reboot_running_scale_released");
    for (int i = 0; i < 20; i++) step(&t, w, true);
    safety_manager_on_valid_weight(t);
    CK(safety_manager_fault() == SAFETY_SCALE_MOVED && !fake_get(0) && !dual_dispense_controller_clear_fault(1, t),
       "ss_reboot_running_fresh_weight_does_not_clear");
    operator_clear();
    CK(safety_manager_fault() == SAFETY_NONE && snap(1, t).state == DCH_IDLE, "ss_reboot_running_explicit_clear_works");
    uint32_t id2 = add_job(1);
    hold(&t, 3400U, w, true);
    CK(waiting(1, id2, t), "ss_reboot_next_job_needs_a_fresh_ready");
    CK(rdy(1, id2, B, 0U, t) == READY_OK, "ss_reboot_next_job_ready_against_the_new_boot_accepted");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 8. NO_PROGRESS
 * ------------------------------------------------------------------------ */
static void test_no_progress(void)
{
    /* flat weight with the relay ON */
    fx_init();
    uint32_t t = 1000U;
    int32_t w = 0;
    uint32_t id = ready_job(1, &t, 0);
    int n = 0;
    while (job_running(id) && n < 100) { pstep(&t, &w, 1, 0); n++; }
    CK(job_is(id, JOB_FAILED, "NO_PROGRESS"), "ss_np_flat_weight_fails_no_progress");
    CK(!fake_get(0) && !snap(1, t).relay_on, "ss_np_relay_off_in_the_tripping_tick");
    CK(n >= 10 && n <= 14, "ss_np_trips_after_one_learn_window_of_relay_on_time");
    CK(safety_manager_fault() == SAFETY_NO_PROGRESS, "ss_np_fault_latched");
    CK(!job_is(id, JOB_FAILED, "CHANNEL_TIMEOUT"), "ss_np_independent_of_channel_timeout");
    tk(t + 100U);
    CK(snap(1, t + 100U).state == DCH_FAULT && snap(1, t + 100U).fault != NULL &&
       strcmp(snap(1, t + 100U).fault, "NO_PROGRESS") == 0, "ss_np_pump_in_fault_names_no_progress");
    t += 100U;
    for (int i = 0; i < 30; i++) step(&t, 0, true);
    safety_manager_on_valid_weight(t);
    CK(safety_manager_fault() == SAFETY_NO_PROGRESS && !fake_get(0) && !dual_dispense_controller_clear_fault(1, t),
       "ss_np_needs_explicit_clear");
    operator_clear();
    CK(safety_manager_fault() == SAFETY_NONE && snap(1, t).state == DCH_IDLE, "ss_np_clear_returns_to_idle");
    hold(&t, 5000U, 0, true);
    CK(job_is(id, JOB_FAILED, "NO_PROGRESS") && !fake_get(0) && snap(1, t).active_job_id == 0U,
       "ss_np_no_automatic_retry");

    /* healthy ramp (100 g/s): never trips, completes */
    fx_init();
    t = 1000U; w = 0;
    id = ready_job(1, &t, 0);
    n = run_to_end(1, &t, &w, id, 1500);
    CK(job_is(id, JOB_COMPLETE, NULL) && safety_manager_fault() == SAFETY_NONE, "ss_np_healthy_ramp_completes_without_trip");
    CK(n > 400, "ss_np_healthy_ramp_ran_the_whole_job");

    /* learned bound: the rate collapses to 10 % of what the first window measured */
    fx_init();
    t = 1000U; w = 0;
    id = ready_job(1, &t, 0);
    n = 0;
    while (job_running(id) && n < 80) { pstep(&t, &w, 1, n < 12 ? 10 : 1); n++; }
    CK(job_is(id, JOB_FAILED, "NO_PROGRESS") && !fake_get(0), "ss_np_learned_bound_trips_on_a_collapsed_rate");
    CK(n >= 18 && n <= 30, "ss_np_learned_bound_trips_at_the_second_window");

    /* a slower rate that still clears 25 % of the learned rise does not trip */
    fx_init();
    t = 1000U; w = 0;
    id = ready_job(1, &t, 0);
    n = 0;
    while (job_running(id) && n < 70) { pstep(&t, &w, 1, n < 12 ? 10 : 4); n++; }
    CK(job_running(id) && safety_manager_fault() == SAFETY_NONE, "ss_np_rate_above_25_percent_of_learned_does_not_trip");

    /* first window below np_abs_floor_g fails at once; at the floor it passes */
    fx_init();
    s_cfg.np_abs_floor_g = 30;
    cfg_apply();
    t = 1000U; w = 0;
    id = ready_job(1, &t, 0);
    n = 0;
    while (job_running(id) && n < 40) { pstep(&t, &w, 1, 2); n++; }
    CK(job_is(id, JOB_FAILED, "NO_PROGRESS") && n <= 14, "ss_np_first_window_below_abs_floor_fails_at_once");
    fx_init();
    s_cfg.np_abs_floor_g = 30;
    cfg_apply();
    t = 1000U; w = 0;
    id = ready_job(1, &t, 0);
    n = 0;
    while (job_running(id) && n < 40) { pstep(&t, &w, 1, 4); n++; }
    CK(job_running(id) && safety_manager_fault() == SAFETY_NONE, "ss_np_first_window_above_abs_floor_passes");

    /* relay-ON time, not wall time: 30 s of PAUSE with a flat weight never counts */
    fx_init();
    t = 1000U; w = 0;
    id = ready_job(1, &t, 0);
    pstep(&t, &w, 1, 0);
    pstep(&t, &w, 1, 0);
    CK(snap(1, t).state == DCH_DISPENSING && fake_get(0), "ss_np_pause_setup_relay_on");
    CK(dual_dispense_controller_pause(1, t) && !fake_get(0), "ss_np_pause_turns_the_relay_off");
    for (int i = 0; i < 300; i++) step(&t, 0, true);
    CK(dual_dispense_controller_resume(1, t), "ss_np_resume");
    for (int i = 0; i < 4; i++) pstep(&t, &w, 1, 0);
    CK(job_running(id) && safety_manager_fault() == SAFETY_NONE, "ss_np_counts_relay_on_time_not_wall_time");

    /* pulsed MICRO with the profile's own thresholds (np_micro_window_ms/np_micro_min_rise_g) */
    for (int healthy = 0; healthy < 2; healthy++) {
        fx_init();
        s_cfg.start_max_g = 0;
        cfg_apply();
        job_profile_t pin;
        memset(&pin, 0, sizeof(pin));
        pin.valid = true;
        snprintf(pin.profile_id, sizeof(pin.profile_id), "np-pin");
        pin.version = 1U;
        pin.kp = 0.99f;
        pin.tolerance_g = 20; pin.max_overshoot_g = 100; pin.max_duration_ms = 120000U;
        pin.window_ms = 500U; pin.min_on_ms = 40U; pin.min_off_ms = 40U;
        pin.coarse_threshold_g = 1000; pin.fine_threshold_g = 600; pin.micro_threshold_g = 300;
        pin.coarse_min_on_ms = 80U; pin.fine_min_on_ms = 30U; pin.micro_min_on_ms = 15U;
        pin.settle_time_ms = 200U; pin.inflight_comp_g = 20;
        pin.np_window_ms[2] = 400U;
        pin.np_min_rise_g[2] = 3;
        uint32_t pid = 0U;
        (void)dual_dispense_controller_add_material_server_job_pinned(1, 5000, 0, s_cmd++, &pin, &pid);
        t = 1000U; w = 4800;
        hold(&t, 3400U, w, true);
        CK(rdy(1, pid, A, 0U, t) == READY_OK, healthy ? "ss_np_pulse_healthy_ready" : "ss_np_pulse_flat_ready");
        int edges0 = s_on_edges;
        n = 0;
        while (job_running(pid) && n < (healthy ? 90 : 60)) { pstep(&t, &w, 1, healthy ? 2 : 0); n++; }
        if (healthy) {
            CK(job_running(pid) && safety_manager_fault() == SAFETY_NONE && snap(1, t).stage == DCH_STAGE_MICRO &&
               s_on_edges - edges0 >= 12, "ss_np_pulsed_micro_healthy_rise_does_not_trip");
        } else {
            CK(job_is(pid, JOB_FAILED, "NO_PROGRESS") && !fake_get(0) && s_on_edges - edges0 >= 3 &&
               s_on_edges - edges0 <= 5, "ss_np_pulsed_micro_trips_after_the_profile_window_of_on_time");
            CK(safety_manager_fault() == SAFETY_NO_PROGRESS, "ss_np_pulsed_micro_fault_latched");
        }
    }
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 9. SCALE_MOVED
 * ------------------------------------------------------------------------ */
/* Pump 1 DISPENSING with the scale reading `w0` at the start. */
static uint32_t dispensing(uint32_t *t, int32_t w0)
{
    uint32_t id = ready_job(1, t, w0);
    step(t, w0, true);
    return id;
}

static void test_scale_moved(void)
{
    /* drop below the previous STABLE weight by drop_trip_g */
    fx_init();
    uint32_t t = 1000U;
    uint32_t id = dispensing(&t, 100);
    CK(id != 0U && snap(1, t).state == DCH_DISPENSING && fake_get(0), "ss_moved_setup_dispensing");
    step(&t, 2000, false);
    step(&t, 2000, true);                   /* previous stable weight = 2000 */
    step(&t, 1801, false);                  /* a 199 g dip is not a move */
    CK(job_running(id) && safety_manager_fault() == SAFETY_NONE, "ss_moved_drop_199_g_does_not_trip");
    t += 100U;
    feed(t, 1799, false);                   /* 201 g below the stable weight */
    tk(t);
    CK(!fake_get(0) && !snap(1, t).relay_on, "ss_moved_drop_relay_off_same_tick");
    CK(job_is(id, JOB_FAILED, "SCALE_MOVED") && safety_manager_fault() == SAFETY_SCALE_MOVED,
       "ss_moved_drop_job_failed_and_fault_latched");
    tk(t + 100U);
    CK(snap(1, t + 100U).state == DCH_FAULT && !fake_get(0), "ss_moved_pump_faults_next_tick");
    for (int i = 0; i < 20; i++) step(&t, 100, true);
    safety_manager_on_valid_weight(t);
    CK(safety_manager_fault() == SAFETY_SCALE_MOVED && !dual_dispense_controller_clear_fault(1, t) && !fake_get(0),
       "ss_moved_requires_explicit_clear");
    operator_clear();
    CK(safety_manager_fault() == SAFETY_NONE && snap(1, t).state == DCH_IDLE &&
       dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE, "ss_moved_clear_releases_everything");
    uint32_t again = add_job(1);
    hold(&t, 3400U, 100, true);
    CK(waiting(1, again, t) && !fake_get(0), "ss_moved_after_clear_next_job_needs_a_fresh_ready");

    /* drop below start_weight_g - drop_trip_g (the stable weight alone would not catch it) */
    fx_init();
    t = 1000U;
    id = dispensing(&t, 400);
    CK(id != 0U && snap(1, t).start_weight_g == 400, "ss_moved_start_rule_setup");
    step(&t, 350, true);
    step(&t, 201, false);
    CK(job_running(id), "ss_moved_start_rule_201_is_inside_the_limit");
    t += 100U;
    feed(t, 199, false);
    tk(t);
    CK(!fake_get(0) && job_is(id, JOB_FAILED, "SCALE_MOVED"), "ss_moved_below_start_weight_minus_trip_trips");

    /* while SETTLING */
    fx_init();
    t = 1000U;
    id = dispensing(&t, 100);
    step(&t, 5000, true);
    CK(snap(1, t).state == DCH_SETTLING && !fake_get(0), "ss_moved_settling_setup");
    t += 100U;
    feed(t, 4700, false);
    tk(t);
    CK(job_is(id, JOB_FAILED, "SCALE_MOVED") && safety_manager_fault() == SAFETY_SCALE_MOVED,
       "ss_moved_while_settling_trips_instead_of_completing");

    /* while WAIT_WEIGHT */
    fx_init();
    t = 1000U;
    id = ready_job(1, &t, 400);
    CK(snap(1, t).state == DCH_WAIT_WEIGHT, "ss_moved_wait_weight_setup");
    t += 100U;
    feed(t, 100, false);
    tk(t);
    CK(job_is(id, JOB_FAILED, "SCALE_MOVED") && !s_ever_on[0], "ss_moved_wait_weight_trips_before_any_pulse");

    /* scale offline mid-job: SCALE_MOVED (not a plain stale), relay OFF the same tick */
    fx_init();
    t = 1000U;
    id = dispensing(&t, 100);
    CK(fake_get(0), "ss_moved_offline_setup_relay_on");
    tk(t + 5000U);
    CK(job_running(id) && fake_get(0), "ss_moved_offline_exactly_at_the_limit_still_running");
    tk(t + 5001U);
    CK(!fake_get(0) && job_is(id, JOB_FAILED, "SCALE_MOVED") && safety_manager_fault() == SAFETY_SCALE_MOVED,
       "ss_moved_offline_relay_off_same_tick_job_scale_moved");

    /* scale_id change mid-job */
    fx_init();
    t = 1000U;
    id = dispensing(&t, 100);
    t += 100U;
    feed_id(t, 100, false, "SCALE2", A);
    tk(t);
    CK(!fake_get(0) && job_is(id, JOB_FAILED, "SCALE_MOVED"), "ss_moved_scale_id_change_mid_job_trips");

    /* a plain stale reading BEFORE the first dispense tick keeps the old reason */
    fx_init();
    t = 1000U;
    id = ready_job(1, &t, 100);
    tk(t + 6000U);
    CK(job_is(id, JOB_FAILED, "CHANNEL_WEIGHT_STALE") && !s_ever_on[0] && safety_manager_fault() == SAFETY_NONE,
       "ss_stale_weight_in_wait_weight_fails_safe_relay_never_on");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 10. Mutual exclusion
 * ------------------------------------------------------------------------ */
static void test_mutual_exclusion(void)
{
    fx_init();
    uint32_t t = 1000U;
    int32_t w = 0;
    uint32_t j2 = add_job(2);
    uint32_t j1 = ready_job(1, &t, 0);
    CK(j1 != 0U && waiting(2, j2, t), "ss_mutex_setup");
    CK(rdy(2, j2, A, 0U, t) == READY_SCALE_BUSY, "ss_mutex_other_pump_ready_refused_while_owned");
    CK(dual_dispense_controller_manual_relay(2, true, t) != ESP_OK && !fake_get(1), "ss_mutex_manual_start_refused");
    for (int i = 0; i < 200 && job_running(j1); i++) {
        pstep(&t, &w, 1, 10);
        if (fake_get(0) && fake_get(1)) break;
    }
    CK(!s_both_on_seen && !s_ever_on[1], "ss_mutex_never_both_relays_on_and_pump2_never_started");
    CK(snap(1, t).owns_scale && !snap(2, t).owns_scale && dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1,
       "ss_mutex_single_owner");
    (void)run_to_end(1, &t, &w, j1, 1500);
    CK(job_is(j1, JOB_COMPLETE, NULL), "ss_mutex_job1_completes");
    CK(!s_ever_on[1] && waiting(2, j2, t), "ss_mutex_pump2_job_still_waits_after_pump1_done");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 11. Legacy WS adapter: ONE sample type into the controller
 * ------------------------------------------------------------------------ */
static void test_legacy_ws_adapter(void)
{
    fx_init();
    (void)weight_receiver_init();
    weight_receiver_set_actuation_handler(ctl_handler);
    uint32_t t = 1000U, wseq = 0U;
    char f[256];
    uint32_t id = add_job(1);
    for (int i = 0; i < 6; i++) {
        t += 100U;
        int n = snprintf(f, sizeof(f),
            "{\"type\":\"weight\",\"weight_g\":120,\"stable\":true,\"weight1_g\":null,\"weight2_g\":888,"
            "\"stable2\":true,\"sequence\":%lu}", (unsigned long)++wseq);
        weight_receiver_on_message(f, (size_t)n, t);
        tk(t);
    }
    dual_scale_info_t si;
    dual_dispense_controller_scale_info(t, &si);
    CK(si.online && si.weight_g == 120 && si.scale_id[0] == '\0' && si.boot_id[0] == '\0',
       "ss_ws_legacy_frame_adapted_to_one_sample_weight_g");
    CK(rdy(1, id, A, 0U, t) == READY_SCALE_BOOT_MISMATCH, "ss_ws_legacy_scale_has_no_boot_id_new_form_refused");
    s_cfg.legacy_ready = false;
    cfg_apply();
    CK(dual_dispense_controller_legacy_ready(1, t) == READY_LEGACY_DISABLED && !dual_dispense_controller_operator_ready(1, t) &&
       waiting(1, id, t), "ss_ws_legacy_ready_refused_when_legacy_ready_is_off");
    s_cfg.legacy_ready = true;
    cfg_apply();
    CK(dual_dispense_controller_operator_ready(1, t), "ss_ws_legacy_ready_accepted_when_legacy_ready_is_on");
    t += 100U;
    {
        int n = snprintf(f, sizeof(f),
            "{\"type\":\"weight\",\"weight_g\":130,\"stable\":false,\"weight1_g\":777,\"weight2_g\":null,"
            "\"sequence\":%lu}", (unsigned long)++wseq);
        weight_receiver_on_message(f, (size_t)n, t);
        tk(t);
    }
    CK(snap(1, t).current_weight_g == 130 && snap(1, t).owns_scale, "ss_ws_owner_uses_weight_g_not_weight1_g_or_weight2_g");
    CK(snap(2, t).current_weight_g != 130 && !snap(2, t).have_weight, "ss_ws_other_pump_not_renewed");

    /* a replayed or older sequence is dropped without moving freshness or the weight */
    t += 100U;
    {
        int n = snprintf(f, sizeof(f), "{\"type\":\"weight\",\"weight_g\":999,\"stable\":true,\"sequence\":%lu}",
                         (unsigned long)(wseq - 1U));
        weight_receiver_on_message(f, (size_t)n, t);
    }
    CK(snap(1, t).current_weight_g == 130, "ss_ws_older_sequence_dropped");
    weight_receiver_set_actuation_handler(NULL);
    (void)weight_receiver_init();
    fx_end();
}

/* Controller-level duplicate/out-of-order protection per scale */
static void test_controller_sequence_protection(void)
{
    fx_init();
    uint32_t t = 1000U;
    int32_t w = 100;
    uint32_t id = ready_job(1, &t, w);
    CK(id != 0U, "ss_seq_setup");
    uint32_t last_seq = s_seq;
    weight_msg_t m;
    memset(&m, 0, sizeof(m));
    m.weight_g = 4242;
    m.has_scale_id = m.has_boot_id = true;
    snprintf(m.scale_id, sizeof(m.scale_id), "SCALE1");
    snprintf(m.boot_id, sizeof(m.boot_id), "%s", A);
    m.has_sequence = true;
    m.sequence = last_seq;                           /* duplicate */
    dual_dispense_controller_on_weight(&m, t + 50U);
    CK(snap(1, t + 50U).current_weight_g == 100, "ss_seq_duplicate_sample_dropped");
    m.sequence = last_seq - 3U;                      /* reordered */
    dual_dispense_controller_on_weight(&m, t + 60U);
    CK(snap(1, t + 60U).current_weight_g == 100, "ss_seq_older_sample_dropped");
    m.sequence = last_seq + 1U;
    dual_dispense_controller_on_weight(&m, t + 70U);
    CK(snap(1, t + 70U).current_weight_g == 4242, "ss_seq_newer_sample_accepted");
    m.simulated = true;
    m.weight_g = 1;
    m.sequence = last_seq + 2U;
    dual_dispense_controller_on_weight(&m, t + 80U);
    CK(snap(1, t + 80U).current_weight_g == 4242, "ss_seq_simulated_sample_rejected");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 12. READY on the command wire (HTTP/MQTT dispatch) and the status
 * ------------------------------------------------------------------------ */
typedef struct { uint32_t id; char state[12]; uint8_t ch; char error[120]; } ack_t;
static ack_t s_acks[16];
static int s_ack_n;
static void ack_sink(void *ctx, uint32_t id, const char *state, uint32_t lj, uint8_t ch, const char *error)
{
    (void)ctx; (void)lj;
    if (s_ack_n >= 16) return;
    s_acks[s_ack_n].id = id;
    snprintf(s_acks[s_ack_n].state, sizeof(s_acks[s_ack_n].state), "%s", state);
    s_acks[s_ack_n].ch = ch;
    snprintf(s_acks[s_ack_n].error, sizeof(s_acks[s_ack_n].error), "%s", error);
    s_ack_n++;
}
static char s_js[400];
static void send_ready(uint32_t cmd, const char *ch, const char *extra, uint32_t recv, uint32_t now)
{
    snprintf(s_js, sizeof(s_js),
        "{\"command_id\": %lu, \"type\": \"READY\", \"command_type\": \"READY\", \"channel_id\": \"%s\", "
        "\"ttl_ms\": 0%s}", (unsigned long)cmd, ch, extra);
    telemetry_client_dispatch_command(s_js, TELEMETRY_CMD_MQTT, recv, now, ack_sink, NULL);
}
static bool last_ack_is(const char *state, const char *err_prefix)
{
    if (s_ack_n == 0) return false;
    const ack_t *a = &s_acks[s_ack_n - 1];
    return strcmp(a->state, state) == 0 && strncmp(a->error, err_prefix, strlen(err_prefix)) == 0;
}

static void test_ready_wire(void)
{
    fx_init();
    s_ack_n = 0;
    uint32_t t = 1000U;
    uint32_t id = add_job(1);
    hold(&t, 3400U, 100, true);
    char extra[120];

    snprintf(extra, sizeof(extra), ", \"job_id\": %lu, \"scale_boot_id\": \"%s\"", (unsigned long)(id + 7U), A);
    send_ready(5001, "CH1", extra, t, t + 5U);
    CK(last_ack_is("FAILED", "READY_JOB_MISMATCH") && waiting(1, id, t), "ss_wire_wrong_job_id_acked_failed_with_code");
    snprintf(extra, sizeof(extra), ", \"job_id\": %lu, \"scale_boot_id\": \"%s\"", (unsigned long)id, B);
    send_ready(5002, "CH1", extra, t, t + 5U);
    CK(last_ack_is("FAILED", "READY_SCALE_BOOT_MISMATCH") && waiting(1, id, t), "ss_wire_wrong_boot_acked_failed_with_code");
    snprintf(extra, sizeof(extra), ", \"job_id\": %lu", (unsigned long)id);
    send_ready(5003, "CH1", extra, t, t + 5U);
    CK(last_ack_is("FAILED", "READY_MALFORMED") && waiting(1, id, t), "ss_wire_job_id_without_boot_malformed");
    snprintf(extra, sizeof(extra), ", \"scale_boot_id\": \"%s\"", A);
    send_ready(5004, "CH1", extra, t, t + 5U);
    CK(last_ack_is("FAILED", "READY_MALFORMED") && waiting(1, id, t), "ss_wire_boot_without_job_id_malformed");
    send_ready(5005, "CH1", ", \"job_id\": \"abc\", \"scale_boot_id\": \"aabbccdd\"", t, t + 5U);
    CK(last_ack_is("FAILED", "READY_MALFORMED") && waiting(1, id, t), "ss_wire_non_numeric_job_id_malformed");
    send_ready(5006, "CH1", ", \"job_id\": 1, \"scale_boot_id\": \"aabbcc\"", t, t + 5U);
    CK(last_ack_is("FAILED", "READY_MALFORMED") && waiting(1, id, t), "ss_wire_short_boot_id_malformed");
    send_ready(5007, "CH2", ", \"job_id\": 1, \"scale_boot_id\": \"aabbccdd\"", t, t + 5U);
    CK(last_ack_is("FAILED", "READY_NOT_WAITING"), "ss_wire_ready_for_pump_without_job_not_waiting");
    snprintf(extra, sizeof(extra), ", \"job_id\": %lu, \"scale_boot_id\": \"%s\"", (unsigned long)id, A);
    send_ready(5008, "CH1", extra, t, t + 200000U);
    CK(last_ack_is("FAILED", "expired") || last_ack_is("FAILED", "READY_STALE"), "ss_wire_old_ready_not_applied");
    CK(waiting(1, id, t), "ss_wire_old_ready_changed_nothing");
    send_ready(5009, "CH1", extra, t, t + 10U);
    CK(last_ack_is("APPLIED", "") && snap(1, t).owns_scale && snap(1, t).state == DCH_WAIT_WEIGHT,
       "ss_wire_good_ready_applied_pump_owns_scale");
    int n = s_ack_n;
    send_ready(5009, "CH1", extra, t, t + 20U);
    CK(s_ack_n == n + 1 && !strcmp(s_acks[n].state, "APPLIED") && snap(1, t).owns_scale,
       "ss_wire_redelivered_ready_reacked_not_reapplied");
    send_ready(5000, "CH1", extra, t, t + 30U);
    CK(!strcmp(s_acks[s_ack_n - 1].state, "FAILED"), "ss_wire_older_command_id_never_applied");

    /* legacy form {channel} only, honoured only while legacy_ready is on */
    fx_init();
    s_ack_n = 0;
    t = 1000U;
    id = add_job(2);
    hold(&t, 400U, 100, false);
    s_cfg.legacy_ready = false;
    cfg_apply();
    send_ready(5101, "CH2", "", t, t + 5U);
    CK(last_ack_is("FAILED", "READY_LEGACY_DISABLED") && waiting(2, id, t), "ss_wire_legacy_ready_refused_when_off");
    s_cfg.legacy_ready = true;
    cfg_apply();
    send_ready(5102, "CH2", "", t, t + 10U);
    CK(last_ack_is("APPLIED", "") && snap(2, t).owns_scale, "ss_wire_legacy_ready_accepted_when_on_migration");
    fx_end();
}

static void test_status_and_scale_info(void)
{
    fx_init();
    uint32_t t = 1000U;
    char js[3600];
    uint32_t id = add_job(2);
    hold(&t, 3400U, 100, true);
    dual_scale_info_t si;
    dual_dispense_controller_scale_info(t, &si);
    CK(si.await_job_id == id && si.await_channel == 2U && si.needs_station == 2U && si.in_transit &&
       si.online && si.stable && strcmp(si.boot_id, A) == 0, "ss_info_waiting_job_needs_station");
    int n = telemetry_client_status_json(js, sizeof(js), t);
    char want[96];
    snprintf(want, sizeof(want), "\"awaiting_scale_move\":{\"job_id\":%lu,\"channel\":\"CH2\"}", (unsigned long)id);
    CK(n > 0 && strstr(js, want) != NULL, "ss_status_awaiting_scale_move_object");
    CK(n > 0 && strstr(js, "\"needs_station\":\"CH2\"") && strstr(js, "\"scale_id\":\"SCALE1\"") &&
       strstr(js, "\"scale_boot_id\":\"aabbccdd\"") && strstr(js, "\"scale_in_transit\":true") &&
       strstr(js, "\"state\":\"WAITING_FOR_SCALE_MOVE\""), "ss_status_station_scale_and_transit_fields");
    CK(n > 0 && strstr(js, "\"scale_weight_g\":100") && strstr(js, "\"scale_online\":true"), "ss_status_scale_reading");
    CK(rdy(2, id, A, 0U, t) == READY_OK, "ss_info_ready");
    dual_dispense_controller_scale_info(t, &si);
    CK(si.await_job_id == 0U && si.needs_station == 0U && !si.in_transit, "ss_info_confirmed_station_not_in_transit");
    n = telemetry_client_status_json(js, sizeof(js), t);
    CK(n > 0 && strstr(js, "\"awaiting_scale_move\":null") && strstr(js, "\"needs_station\":null") &&
       strstr(js, "\"scale_in_transit\":false"), "ss_status_after_ready");
    /* the scale vanishes: offline, nothing confirmed any more */
    tk(t + 6000U);
    dual_dispense_controller_scale_info(t + 6000U, &si);
    n = telemetry_client_status_json(js, sizeof(js), t + 6000U);
    CK(!si.online && n > 0 && strstr(js, "\"scale_online\":false") && strstr(js, "\"scale_weight_g\":null"),
       "ss_status_offline_scale_has_no_weight");
    fx_end();

    /* nothing ever received: ids are null */
    fx_init();
    int m = telemetry_client_status_json(js, sizeof(js), 100U);
    CK(m > 0 && strstr(js, "\"scale_id\":null") && strstr(js, "\"scale_boot_id\":null") &&
       strstr(js, "\"scale_in_transit\":true"), "ss_status_no_scale_yet");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 13. Retained commands never reach dispatch
 * ------------------------------------------------------------------------ */
static void test_retained_commands(void)
{
    fx_init();
    telemetry_cmd_stats_t st0, st;
    const char *start = "{\"command_id\": 901, \"type\": \"START\", \"command_type\": \"START\", \"channel_id\": \"CH1\", \"ttl_ms\": 0}";
    const char *pump = "{\"command_id\": 902, \"type\": \"PUMP_START\", \"command_type\": \"PUMP_START\", \"channel_id\": \"CH1\", \"ttl_ms\": 0}";
    const char *job = "{\"command_id\": 903, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": \"CH1\", "
                      "\"material_id\": \"M1\", \"target_g\": 5000, \"priority\": 0, \"ttl_ms\": 0}";
    const char *ready = "{\"command_id\": 904, \"type\": \"READY\", \"command_type\": \"READY\", \"channel_id\": \"CH1\", \"ttl_ms\": 0}";
    const char *msgs[4] = { start, pump, job, ready };
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) telemetry_client_test_reset_commands();   /* "reboot": an empty ledger */
        telemetry_client_cmd_stats(&st0);
        for (int i = 0; i < 4; i++) {
            CK(telemetry_client_mqtt_intake(msgs[i], strlen(msgs[i]), true, 5000U, 0) == TELEMETRY_INTAKE_RETAINED,
               pass ? "ss_retained_after_reboot_dropped" : "ss_retained_command_dropped_at_intake");
        }
        telemetry_client_cmd_stats(&st);
        CK(telemetry_client_mqtt_staged() == 0U && !telemetry_client_mqtt_step(5000U), "ss_retained_nothing_staged_nothing_runs");
        CK(st.drop_retained - st0.drop_retained == 4U, pass ? "ss_retained_after_reboot_counted" : "ss_retained_counted");
        CK(job_queue_depth() == 0U && !fake_get(0) && !fake_get(1) && safety_manager_fault() == SAFETY_NONE &&
           snap(1, 5000U).state == DCH_IDLE, "ss_retained_no_job_no_relay_no_state_change");
    }
    CK(telemetry_client_mqtt_intake(job, strlen(job), false, 5000U, 0) == TELEMETRY_INTAKE_NORMAL,
       "ss_retained_control_same_job_not_retained_is_staged");
    telemetry_client_test_reset_commands();   /* do not leave the staged control message behind */

    /* the callback path: the link refuses retain=1 on commands, weight and run/ack */
    static mqtt_link_queue_t q;
    CK(mqtt_link_queue_init(&q, MQTT_LINK_RX_DEPTH, MQTT_LINK_TX_DEPTH), "ss_retained_queue_init");
    CK(mqtt_link_queue_weight_init(&q, MQTT_LINK_WRX_DEPTH) && mqtt_link_queue_ack_init(&q, MQTT_LINK_ARX_DEPTH),
       "ss_retained_queue_aux_init");
    const char *ct = "cas/dev/commands", *wt = "cas/snd/weight/ctl", *at = "cas/dev/run/ack";
    CK(mqtt_link_queue_push_event(&q, wt, at, ct, strlen(ct), pump, strlen(pump), strlen(pump), true, 1U, 1) == MQTT_ENQ_RETAINED,
       "ss_retained_pump_start_refused_by_callback_queue");
    CK(mqtt_link_queue_push_event(&q, wt, at, ct, strlen(ct), job, strlen(job), strlen(job), true, 1U, 1) == MQTT_ENQ_RETAINED,
       "ss_retained_job_refused_by_callback_queue");
    CK(mqtt_link_queue_push_event(&q, wt, at, wt, strlen(wt), "{}", 2U, 2U, true, 1U, 1) == MQTT_ENQ_RETAINED,
       "ss_retained_weight_refused_by_callback_queue");
    CK(mqtt_link_queue_push_event(&q, wt, at, at, strlen(at), "{}", 2U, 2U, true, 1U, 1) == MQTT_ENQ_RETAINED,
       "ss_retained_run_ack_refused_by_callback_queue");
    CK(q.dropped_retained == 4U && mqtt_link_queue_pending_rx(&q) == 0U && mqtt_link_queue_pending_weight(&q) == 0U,
       "ss_retained_counted_in_the_link_and_nothing_queued");
    CK(mqtt_link_queue_push_event(&q, wt, at, ct, strlen(ct), pump, strlen(pump), strlen(pump), false, 1U, 1) == MQTT_ENQ_OK &&
       mqtt_link_queue_pending_rx(&q) == 1U, "ss_retained_control_live_message_still_queued");
    mqtt_link_queue_deinit(&q);
    fx_end();
}

/* ---------------------------------------------------------------------------
 * 14. Optional NO_PROGRESS profile fields on a pinned JOB
 * ------------------------------------------------------------------------ */
static void test_pin_np_fields(void)
{
    char obj[900], err[100];
    job_profile_t pin;
    const char *tail = "\"tolerance_g\": 20, \"max_overshoot_g\": 100, \"max_duration_ms\": 120000, "
                       "\"window_ms\": 500, \"min_on_ms\": 40, \"min_off_ms\": 40";
    snprintf(obj, sizeof(obj),
        "{\"command_id\": 1, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": \"CH1\", \"material_id\": \"M1\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"np-a\", \"profile_version\": 1, \"profile\": "
        "{\"profile_id\": \"np-a\", \"version\": 1, \"kp\": 0.1, \"ki\": 0.0, \"kd\": 0.0, %s, "
        "\"np_micro_window_ms\": 400, \"np_micro_min_rise_g\": 3, \"np_coarse_min_rise_g\": 50}}", tail);
    CK(telemetry_client_parse_job_pin(obj, 1, 5000, &pin, err, sizeof(err)) == JOB_PIN_PARSED &&
       pin.np_window_ms[2] == 400U && pin.np_min_rise_g[2] == 3 && pin.np_min_rise_g[0] == 50 &&
       pin.np_window_ms[0] == 0U && pin.np_window_ms[1] == 0U && pin.np_min_rise_g[1] == 0,
       "ss_pin_np_fields_parsed_per_stage");
    snprintf(obj, sizeof(obj),
        "{\"command_id\": 1, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": \"CH1\", \"material_id\": \"M1\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"np-a\", \"profile_version\": 1, \"profile\": "
        "{\"profile_id\": \"np-a\", \"version\": 1, \"kp\": 0.1, \"ki\": 0.0, \"kd\": 0.0, %s}}", tail);
    CK(telemetry_client_parse_job_pin(obj, 1, 5000, &pin, err, sizeof(err)) == JOB_PIN_PARSED &&
       pin.np_window_ms[2] == 0U && pin.np_min_rise_g[2] == 0, "ss_pin_np_fields_absent_means_learned");
    snprintf(obj, sizeof(obj),
        "{\"command_id\": 1, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": \"CH1\", \"material_id\": \"M1\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"np-a\", \"profile_version\": 1, \"profile\": "
        "{\"profile_id\": \"np-a\", \"version\": 1, \"kp\": 0.1, \"ki\": 0.0, \"kd\": 0.0, %s, \"np_fine_min_rise_g\": 2.5}}", tail);
    CK(telemetry_client_parse_job_pin(obj, 1, 5000, &pin, err, sizeof(err)) == JOB_PIN_REFUSED, "ss_pin_np_fraction_refused");
    snprintf(obj, sizeof(obj),
        "{\"command_id\": 1, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": \"CH1\", \"material_id\": \"M1\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"np-a\", \"profile_version\": 1, \"profile\": "
        "{\"profile_id\": \"np-a\", \"version\": 1, \"kp\": 0.1, \"ki\": 0.0, \"kd\": 0.0, %s, \"np_fine_window_ms\": 0}}", tail);
    CK(telemetry_client_parse_job_pin(obj, 1, 5000, &pin, err, sizeof(err)) == JOB_PIN_REFUSED, "ss_pin_np_zero_window_refused");
    snprintf(obj, sizeof(obj),
        "{\"command_id\": 1, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": \"CH1\", \"material_id\": \"M1\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"np-a\", \"profile_version\": 1, \"profile\": "
        "{\"profile_id\": \"np-a\", \"version\": 1, \"kp\": 0.1, \"ki\": 0.0, \"kd\": 0.0, %s, \"np_fine_min_rise_g\": \"x\"}}", tail);
    CK(telemetry_client_parse_job_pin(obj, 1, 5000, &pin, err, sizeof(err)) == JOB_PIN_REFUSED, "ss_pin_np_string_refused");
}

static void test_names(void)
{
    CK(strcmp(dual_dispense_state_name(DCH_WAITING_FOR_SCALE_MOVE), "WAITING_FOR_SCALE_MOVE") == 0, "ss_state_name");
    CK(strcmp(dual_ready_result_name(READY_JOB_MISMATCH), "READY_JOB_MISMATCH") == 0 &&
       strcmp(dual_ready_result_name(READY_SCALE_BOOT_MISMATCH), "READY_SCALE_BOOT_MISMATCH") == 0 &&
       strcmp(dual_ready_result_name(READY_STALE), "READY_STALE") == 0 &&
       strcmp(dual_ready_result_name(READY_SCALE_UNSTABLE), "READY_SCALE_UNSTABLE") == 0 &&
       strcmp(dual_ready_result_name(READY_NOT_WAITING), "READY_NOT_WAITING") == 0, "ss_ready_result_names");
    CK(strcmp(safety_fault_name(SAFETY_NO_PROGRESS), "NO_PROGRESS") == 0 &&
       strcmp(safety_fault_name(SAFETY_SCALE_MOVED), "SCALE_MOVED") == 0, "ss_safety_fault_names");
    CK(!safety_fault_is_transient(SAFETY_NO_PROGRESS) && !safety_fault_is_transient(SAFETY_SCALE_MOVED),
       "ss_new_faults_are_latched_not_transient");
    dual_scale_cfg_t c;
    dual_dispense_controller_scale_cfg_default(&c);
    CK(c.ready_stable_ms == 3000U && c.ready_max_age_ms == 120000U && c.reconfirm_same_pump && c.legacy_ready &&
       c.scale_move_timeout_ms == 600000U && c.start_max_g == 500 && c.drop_trip_g == 200 &&
       c.np_learn_ms == 10000U, "ss_default_thresholds_documented_in_contract");
}

/* ---------------------------------------------------------------------------
 * 15. Stack margin of the paths the 4096-byte supervisor task runs
 * ------------------------------------------------------------------------ */
static volatile bool s_stk_done, s_stk_np, s_stk_moved, s_stk_timeout;
static volatile uint32_t s_stk_hwm;

static void stack_task(void *arg)
{
    (void)arg;
    fx_init();
    uint32_t t = 1000U;
    int32_t w = 0;
    /* NO_PROGRESS trip, emergency stop, CLEAR */
    uint32_t id = ready_job(1, &t, 0);
    for (int n = 0; n < 100 && job_running(id); n++) pstep(&t, &w, 1, 0);
    s_stk_np = job_is(id, JOB_FAILED, "NO_PROGRESS");
    tk(t + 100U);
    operator_clear();
    /* SCALE_MOVED by a sender reboot */
    id = dispensing(&t, 100);
    s_boot = B;
    t += 100U;
    feed(t, 100, false);
    tk(t);
    s_stk_moved = job_is(id, JOB_FAILED, "SCALE_MOVED");
    tk(t + 100U);
    operator_clear();
    /* waiting job that times out */
    s_cfg.scale_move_timeout_ms = 3000U;
    cfg_apply();
    id = add_job(2);
    hold(&t, 4500U, 100, true);
    dispense_job_t j;
    s_stk_timeout = job_queue_get(id, &j) && j.state == JOB_QUEUED;
    s_stk_hwm = uxTaskGetStackHighWaterMark(NULL);
    fx_end();
    s_stk_done = true;
    vTaskDelete(NULL);
}

static void test_supervisor_stack_margin(void)
{
    s_stk_done = false;
    CK(xTaskCreate(stack_task, "ss_stack", 4096, NULL, uxTaskPriorityGet(NULL) + 1, NULL) == pdPASS,
       "ss_stack_task_created");
    while (!s_stk_done) vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI("SS", "supervisor-sized (4096 B) task: watchdog trips + READY + timeout, free stack high-water = %u B",
             (unsigned)s_stk_hwm);
    CK(s_stk_np && s_stk_moved && s_stk_timeout, "ss_stack_scenarios_ran_inside_the_4096_byte_task");
    CK(s_stk_hwm >= 700U, "ss_stack_supervisor_paths_leave_at_least_700_bytes_free");
}
void test_single_scale_run(void)
{
    ESP_LOGI("SS", "single-scale tests (CONTRACT 9.11)");
    test_names();
    test_pump_gets_weight_whatever_the_tag();
    test_no_owner_pumps_stale();
    test_ready_refusals();
    test_start_weight();
    test_alternating_and_reconfirm();
    test_move_timeout();
    test_sender_reboot();
    test_no_progress();
    test_scale_moved();
    test_mutual_exclusion();
    test_legacy_ws_adapter();
    test_controller_sequence_protection();
    test_ready_wire();
    test_status_and_scale_info();
    test_retained_commands();
    test_pin_np_fields();
    test_supervisor_stack_margin();
    cfg_baseline();
}
