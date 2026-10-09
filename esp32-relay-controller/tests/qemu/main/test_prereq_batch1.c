/* Phase 5 prerequisite batch 1 (PRE-02..PRE-06). Software only: the relay seam
 * below is a TEST-ONLY fake (compiled in only because tests/qemu/CMakeLists.txt
 * defines BITS_QEMU_TEST_HOOKS); no hardware is ever actuated. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dual_dispense_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "job_queue.h"
#include "relay_driver.h"
#include "safety_manager.h"
#include "telemetry_client.h"
#include "test_prereq_batch1.h"
#include "test_prereq_fix1.h"

extern void audit_check(bool condition, const char *name);
extern void dual_dispense_controller_test_set_relay_hook(esp_err_t (*set_fn)(uint8_t, bool),
                                                         bool (*get_fn)(uint8_t));
extern void dual_dispense_controller_test_set_gate_hook(bool (*allowed_fn)(void));
static bool gate_closed(void) { return false; } /* fix1 (M1): the gate itself says "refused" */

/* ---------------------------------------------------------------------------
 * Fake relay seam
 * ------------------------------------------------------------------------ */
static bool s_fake[RELAY_COUNT];
static esp_err_t s_on_err, s_off_err;
static int s_off_writes;

static esp_err_t fake_set(uint8_t index, bool on)
{
    if (index >= RELAY_COUNT) return ESP_ERR_INVALID_ARG;
    if (on) {
        if (s_on_err != ESP_OK) return s_on_err;
    } else {
        s_off_writes++;
        if (s_off_err != ESP_OK) return s_off_err;
    }
    s_fake[index] = on;
    return ESP_OK;
}
static bool fake_get(uint8_t index) { return index < RELAY_COUNT && s_fake[index]; }

static void fake_reset(void)
{
    memset(s_fake, 0, sizeof(s_fake));
    s_on_err = s_off_err = ESP_OK;
    s_off_writes = 0;
}

/* ---------------------------------------------------------------------------
 * Controller fixture
 * ------------------------------------------------------------------------ */
static uint32_t s_cmd = 9000;
static uint32_t s_job;

static void feed_s(uint32_t now, int32_t w, bool has_seq, uint32_t seq)
{
    weight_msg_t m;
    memset(&m, 0, sizeof(m));
    m.has_weight1 = true;
    m.weight1_g = w;
    m.has_stable1 = true;
    m.stable1 = true;
    m.weight_g = w;
    m.has_sequence = has_seq;
    m.sequence = seq;
    dual_dispense_controller_on_weight(&m, now);
}
static void feed(uint32_t now, int32_t w) { feed_s(now, w, false, 0U); }

static dual_channel_snapshot_t snap(uint32_t now)
{
    dual_channel_snapshot_t s;
    memset(&s, 0, sizeof(s));
    (void)dual_dispense_controller_snapshot(1, now, &s);
    return s;
}

static bool job_is(uint32_t id, job_state_t st, const char *err)
{
    dispense_job_t j;
    if (!job_queue_get(id, &j) || j.state != st) return false;
    return err == NULL || strcmp(j.error, err) == 0;
}

/* Fresh controller, 5000 g slot on CH1 (kp 0.99 => full duty at any real error),
 * tolerance 20, max_overshoot 100, optionally one queued job. */
static void fx_init(uint32_t inflight, uint32_t max_dur, uint32_t settle, bool add_job)
{
    fake_reset();
    dual_dispense_controller_test_set_relay_hook(fake_set, fake_get);
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    (void)dual_dispense_controller_set_pending_profile_staged(1, "pre-slot", 1, 5000,
          0.99f, 0.0f, 0.0f, 20, 100, max_dur, 500, 40, 40, 500, 100, 30, 80, 30, 15,
          settle, (int32_t)inflight);
    s_job = 0;
    if (add_job)
        (void)dual_dispense_controller_add_material_server_job_pinned(1, 5000, 0, s_cmd++, NULL, &s_job);
}

/* Takes the job through the READY gate to its first DISPENSING tick (t+20). */
static void fx_start(uint32_t t, int32_t w)
{
    feed(t, w);
    dual_dispense_controller_tick(t);
    (void)dual_dispense_controller_operator_ready(1, t + 10U);
    feed(t + 20U, w);
    dual_dispense_controller_tick(t + 20U);
}

static void fx_end(void)
{
    dual_dispense_controller_test_set_relay_hook(NULL, NULL);
    dual_dispense_controller_test_set_gate_hook(NULL);
    fake_reset();
    (void)safety_manager_init();
    (void)dual_dispense_controller_init();
}

/* ---------------------------------------------------------------------------
 * PRE-03: overweight cut-off in every state
 * ------------------------------------------------------------------------ */
static void overweight_case(const char *tag, int32_t w0, int32_t over_w)
{
    char n[96];
    fx_init(0, 120000, 200, true);
    fx_start(1000, w0);
    dual_channel_snapshot_t s = snap(1020);
    snprintf(n, sizeof(n), "pre03_%s_relay_on_before_step", tag);
    audit_check(s.state == DCH_DISPENSING && s.relay_on && fake_get(0), n);

    int off_before = s_off_writes;
    feed(1100, over_w);
    dual_dispense_controller_tick(1100);
    s = snap(1100);
    snprintf(n, sizeof(n), "pre03_%s_relay_off_same_tick", tag);
    audit_check(!s.relay_on && !fake_get(0) && s_off_writes > off_before, n);
    snprintf(n, sizeof(n), "pre03_%s_job_failed_overweight", tag);
    audit_check(job_is(s_job, JOB_FAILED, "OVERWEIGHT") && s.active_job_id == 0, n);
    snprintf(n, sizeof(n), "pre03_%s_scale_released_state_fault", tag);
    audit_check(s.state == DCH_FAULT && dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE, n);
    fx_end();
}

static void test_pre03(void)
{
    overweight_case("coarse", 0, 5101);
    overweight_case("fine", 4900, 5101);
    overweight_case("micro", 4975, 5101);
    overweight_case("coarse_huge_step", 0, 20000);

    /* Boundary: exactly target + max_overshoot is NOT cut off (COMPLETE band is
     * [target - tol, target + ovs], one-sided and unchanged); +1 is. */
    fx_init(0, 120000, 200, true);
    fx_start(1000, 0);
    feed(1100, 5100);
    dual_dispense_controller_tick(1100);
    dual_channel_snapshot_t s = snap(1100);
    audit_check(s.state == DCH_SETTLING && s.active_job_id != 0 && !s.relay_on,
                "pre03_boundary_plus_ovs_not_cut_off");
    feed(1400, 5100);
    dual_dispense_controller_tick(1400);
    audit_check(job_is(s_job, JOB_COMPLETE, NULL), "pre03_boundary_plus_ovs_still_completes_one_sided_band");
    fx_end();

    /* SETTLING: +ovs+1 arrives while settling. */
    fx_init(0, 120000, 200, true);
    fx_start(1000, 0);
    feed(1100, 5000);
    dual_dispense_controller_tick(1100);
    s = snap(1100);
    audit_check(s.state == DCH_SETTLING, "pre03_settling_reached");
    feed(1120, 5101);
    dual_dispense_controller_tick(1120);
    s = snap(1120);
    audit_check(job_is(s_job, JOB_FAILED, "OVERWEIGHT") && !s.relay_on && s.active_job_id == 0,
                "pre03_settling_plus_ovs_plus_1_fails_in_same_tick");

    /* WAIT_WEIGHT: the job starts already overweight. */
    fx_init(0, 120000, 200, true);
    feed(1000, 5101);
    dual_dispense_controller_tick(1000);
    (void)dual_dispense_controller_operator_ready(1, 1010);
    dual_dispense_controller_tick(1020);
    s = snap(1020);
    audit_check(job_is(s_job, JOB_FAILED, "OVERWEIGHT") && !s.relay_on,
                "pre03_wait_weight_start_overweight_fails");

    /* Stale weight still fails safe by the existing path, not by this one.
     * ADJUSTED (CONTRACT 9.11): a scale that goes offline while the relay can be ON
     * is now named SCALE_MOVED and latches a safety fault (relay OFF the same tick). */
    fx_init(0, 120000, 200, true);
    fx_start(1000, 0);
    dual_dispense_controller_tick(1000 + 6000);
    audit_check(job_is(s_job, JOB_FAILED, "SCALE_MOVED") && !fake_get(0) &&
                safety_manager_fault() == SAFETY_SCALE_MOVED, "pre03_stale_weight_still_fails_safe");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * PRE-04: relay write failure
 * ------------------------------------------------------------------------ */
static void test_pre04(void)
{
    dual_channel_snapshot_t s;

    /* ON write fails with a real bus error. */
    fx_init(0, 120000, 200, true);
    s_on_err = ESP_FAIL;
    fx_start(1000, 0);
    s = snap(1020);
    audit_check(job_is(s_job, JOB_FAILED, "RELAY_WRITE_FAILED"), "pre04_on_fail_job_failed");
    audit_check(s.state == DCH_FAULT && s.active_job_id == 0 && !s.relay_on && !fake_get(0),
                "pre04_on_fail_channel_fault_no_stale_job_relay_off");
    audit_check(safety_manager_fault_active() && safety_manager_fault() == SAFETY_CONTROL_FAILURE,
                "pre04_on_fail_latched_safety_fault");
    audit_check(!s.owns_scale && dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
                "pre04_on_fail_scale_released");
    s_on_err = ESP_OK;
    /* clear_fault before the supervisor's next tick: IDLE, never with a stale id. */
    feed(1030, 0);
    audit_check(dual_dispense_controller_clear_fault(1, 1030), "pre04_clear_fault_accepted");
    s = snap(1030);
    audit_check(s.state == DCH_IDLE && s.active_job_id == 0 && s.fault == NULL,
                "pre04_clear_fault_goes_idle_with_no_active_job");
    dual_dispense_controller_tick(1040); /* latched safety fault -> emergency stop */
    s = snap(1040);
    audit_check(s.state == DCH_FAULT && !fake_get(0) && !fake_get(1),
                "pre04_latched_fault_forces_fault_and_all_off_next_tick");
    /* A latched fault does not self-clear on fresh weight. */
    feed(1060, 0);
    safety_manager_on_valid_weight(1060);
    audit_check(safety_manager_fault_active(), "pre04_latched_fault_survives_fresh_weight");
    audit_check(!dual_dispense_controller_clear_fault(1, 1070), "pre04_clear_fault_refused_while_estopped");
    dual_dispense_controller_clear_emergency_stop();
    audit_check(safety_manager_clear(), "pre04_operator_clears_latched_fault");
    s = snap(1090);
    audit_check(s.state == DCH_IDLE && s.active_job_id == 0, "pre04_idle_after_operator_clears");
    fx_end();

    /* ON merely refused by the actuation gate: nothing was written. */
    fx_init(0, 120000, 200, true);
    s_on_err = ESP_ERR_INVALID_STATE;
    dual_dispense_controller_test_set_gate_hook(gate_closed); /* fix1 (M1): refusal is decided by the gate, not the code */
    fx_start(1000, 0);
    s = snap(1020);
    audit_check(job_is(s_job, JOB_FAILED, "RELAY_WRITE_FAILED") && s.state == DCH_FAULT &&
                s.active_job_id == 0 && !s.relay_on,
                "pre04_gated_on_refusal_fails_job_channel_fault");
    audit_check(!safety_manager_fault_active(), "pre04_gated_on_refusal_does_not_latch_safety_fault");
    fx_end();

    /* OFF write fails while energized: must not look like a clean COMPLETE. */
    fx_init(0, 120000, 200, true);
    fx_start(1000, 0);
    s = snap(1020);
    audit_check(s.relay_on && fake_get(0), "pre04_off_fail_setup_relay_on");
    s_off_err = ESP_FAIL;
    feed(1100, 5000);
    dual_dispense_controller_tick(1100);
    s = snap(1100);
    audit_check(job_is(s_job, JOB_FAILED, "RELAY_WRITE_FAILED") && s.state == DCH_FAULT &&
                s.active_job_id == 0, "pre04_off_fail_job_failed_not_settling");
    audit_check(s.relay_on, "pre04_off_fail_reported_visibly_energized");
    audit_check(safety_manager_fault() == SAFETY_CONTROL_FAILURE, "pre04_off_fail_latched");
    /* fix1 (M6): a relay still energized/unknown keeps the scale; it is released
     * only once an OFF write is confirmed (see fix1_m6_*). */
    audit_check(dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1, "pre04_off_fail_scale_released");
    s_off_err = ESP_OK;
    dual_dispense_controller_tick(1200);
    s = snap(1200);
    audit_check(!s.relay_on && !fake_get(0), "pre04_off_retried_by_emergency_stop_when_bus_recovers");
    fx_end();

    /* Manual start (no job): a real write failure latches, a refusal does not. */
    fx_init(0, 120000, 200, false);
    s_on_err = ESP_FAIL;
    audit_check(dual_dispense_controller_manual_relay(1, true, 500) == ESP_ERR_INVALID_STATE &&
                !snap(500).manual, "pre04_manual_on_fail_refused");
    audit_check(safety_manager_fault() == SAFETY_CONTROL_FAILURE, "pre04_manual_on_fail_latched");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * PRE-05a: settle freshness
 * ------------------------------------------------------------------------ */
static void test_pre05a(void)
{
    dual_channel_snapshot_t s;

    /* A sample at/before the cut does not satisfy settle, however young. */
    fx_init(0, 120000, 200, true);
    fx_start(1000, 0);
    feed(1100, 5000);
    dual_dispense_controller_tick(1100); /* cut at 1100 */
    s = snap(1100);
    audit_check(s.state == DCH_SETTLING, "pre05a_cut_enters_settling");
    dual_dispense_controller_tick(1400);
    dual_dispense_controller_tick(3000);
    dual_dispense_controller_tick(4900); /* age 3800 ms: old rule (<5000) would complete */
    s = snap(4900);
    audit_check(s.state == DCH_SETTLING && s.active_job_id != 0,
                "pre05a_stale_but_under_5s_sample_does_not_satisfy_settle");
    feed(1000, 5000); /* acquired BEFORE the cut, delivered late */
    dual_dispense_controller_tick(4910);
    audit_check(snap(4910).state == DCH_SETTLING, "pre05a_sample_acquired_before_cut_does_not_refresh");
    feed(1100, 5000); /* acquired exactly at the cut: not strictly newer */
    dual_dispense_controller_tick(4920);
    audit_check(snap(4920).state == DCH_SETTLING, "pre05a_sample_acquired_at_cut_does_not_refresh");
    feed(4930, 5000);
    dual_dispense_controller_tick(4930);
    audit_check(job_is(s_job, JOB_COMPLETE, NULL), "pre05a_fresh_post_cut_sample_completes");
    fx_end();

    /* Re-entering DISPENSING also needs a post-cut sample. */
    fx_init(0, 120000, 200, true);
    fx_start(1000, 0);
    feed(1100, 5000);
    dual_dispense_controller_tick(1100);
    feed(1050, 4000); /* pre-cut sample reading low */
    dual_dispense_controller_tick(1500);
    audit_check(snap(1500).state == DCH_SETTLING, "pre05a_pre_cut_sample_cannot_reenter_dispensing");
    feed(1600, 4000);
    dual_dispense_controller_tick(1600);
    audit_check(snap(1600).state == DCH_DISPENSING, "pre05a_post_cut_sample_reenters_dispensing");
    fx_end();

    /* Sequenced feed: duplicate / reordered / unsequenced never refresh. */
    fx_init(0, 120000, 200, true);
    feed_s(1000, 0, true, 10);
    dual_dispense_controller_tick(1000);
    (void)dual_dispense_controller_operator_ready(1, 1010);
    feed_s(1020, 0, true, 11);
    dual_dispense_controller_tick(1020);
    feed_s(1100, 5000, true, 12);
    dual_dispense_controller_tick(1100); /* cut at seq 12 */
    audit_check(snap(1100).state == DCH_SETTLING, "pre05a_seq_cut");
    feed_s(1400, 4000, true, 12); /* duplicate, later timestamp */
    dual_dispense_controller_tick(1400);
    s = snap(1400);
    audit_check(s.state == DCH_SETTLING && s.current_weight_g == 5000,
                "pre05a_duplicate_sequence_ignored_weight_unchanged");
    feed_s(1450, 4000, true, 9); /* reordered (older) */
    dual_dispense_controller_tick(1450);
    s = snap(1450);
    audit_check(s.state == DCH_SETTLING && s.current_weight_g == 5000,
                "pre05a_reordered_sequence_ignored");
    feed(1500, 5000); /* no sequence after a sequenced cut */
    dual_dispense_controller_tick(1500);
    audit_check(snap(1500).state == DCH_SETTLING, "pre05a_unsequenced_sample_after_sequenced_cut_not_fresh");
    feed_s(1600, 5000, true, 13);
    dual_dispense_controller_tick(1600);
    audit_check(job_is(s_job, JOB_COMPLETE, NULL), "pre05a_strictly_newer_sequence_completes");
    fx_end();

    /* Known near-target stall is unchanged: error 17 g with inflight 15 g and
     * tolerance 20 g never reaches SETTLING (inflight < |error| <= tolerance),
     * so this change is not on its path. It still ends in CHANNEL_TIMEOUT. */
    fx_init(15, 3000, 200, true);
    fx_start(1000, 4983);
    for (uint32_t t = 1100; t <= 3500; t += 100) {
        feed(t, 4983);
        dual_dispense_controller_tick(t);
    }
    s = snap(3500);
    audit_check(s.state == DCH_DISPENSING && !s.relay_on && s.active_job_id != 0 &&
                s.stage == DCH_STAGE_SETTLING,
                "pre05a_known_stall_still_present_and_visible");
    feed(4100, 4983);
    dual_dispense_controller_tick(4100);
    audit_check(job_is(s_job, JOB_FAILED, "CHANNEL_TIMEOUT"), "pre05a_known_stall_still_ends_in_timeout");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * Cross-channel weight isolation (review F1/F2). Controller level: a CH2 job
 * must never consume Scale1, and other-channel traffic must not renew freshness
 * or satisfy a post-cut settle sample.
 * ------------------------------------------------------------------------ */
/* mode 0: per-channel frame, only CH1 present (weight2_g null) - what weight_mqtt
 *         emits for a CH1 trigger; weight_g carries CH1's reading
 * mode 1: per-channel frame, only CH2 present (weight1_g null)
 * mode 2: both channels present
 * mode 3: legacy frame, weight_g only, no per-channel field
 * mode 4: per-channel frame with weight1_g only and NO null marker (hand-built) */
static void feed_x(uint32_t now, int mode, int32_t w1, int32_t w2)
{
    weight_msg_t m;
    memset(&m, 0, sizeof(m));
    m.weight_g = (mode == 1) ? w2 : w1;
    if (mode != 3) m.has_channel_fields = (mode != 4);
    if (mode == 0 || mode == 2 || mode == 4) {
        m.has_weight1 = true; m.weight1_g = w1; m.has_stable1 = true; m.stable1 = true;
    }
    if (mode == 1 || mode == 2) {
        m.has_weight2 = true; m.weight2_g = w2; m.has_stable2 = true; m.stable2 = true;
    }
    dual_dispense_controller_on_weight(&m, now);
}

static dual_channel_snapshot_t snap2(uint32_t now)
{
    dual_channel_snapshot_t s;
    memset(&s, 0, sizeof(s));
    (void)dual_dispense_controller_snapshot(2, now, &s);
    return s;
}

/* CH2 owns the scale and sits in its first DISPENSING tick (t+20). */
static void fx2_start(int mode, int32_t w2)
{
    fake_reset();
    dual_dispense_controller_test_set_relay_hook(fake_set, fake_get);
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    (void)dual_dispense_controller_set_pending_profile_staged(2, "pre-slot", 1, 5000,
          0.99f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15,
          200, 0);
    s_job = 0;
    (void)dual_dispense_controller_add_material_server_job_pinned(2, 5000, 0, s_cmd++, NULL, &s_job);
    feed_x(1000, mode, 0, w2);
    dual_dispense_controller_tick(1000);
    (void)dual_dispense_controller_operator_ready(2, 1010);
    feed_x(1020, mode, 0, w2);
    dual_dispense_controller_tick(1020);
}

static void test_cross_channel(void)
{
    dual_channel_snapshot_t s;

    /* ADJUSTED (CONTRACT 9.11): the per-channel model is intentionally replaced.
     * There is ONE scale; the sample goes to the scale OWNER whatever legacy
     * per-channel fields it carries, so the former F1/F2 isolation checks (a CH2 job
     * must not read a "CH1" frame) no longer describe the machine. What matters now:
     *   - the owner consumes weight_g, never weight1_g/weight2_g;
     *   - a scale that stops sending fails the owner safe (SCALE_MOVED, relay OFF). */
    fx2_start(2, 0);
    s = snap2(1020);
    audit_check(s.state == DCH_DISPENSING && s.relay_on && s.owns_scale, "xch_f1_ch2_dispensing_on_own_scale");
    uint32_t t = 1100;
    for (; t <= 1020 + WEIGHT_MESSAGE_TIMEOUT_MS; t += 100) {
        feed_x(t, 0, 2000, 0);          /* per-channel fields say "CH1"; weight_g = 2000 */
        dual_dispense_controller_tick(t);
    }
    s = snap2(t);
    audit_check(s.current_weight_g == 2000 && s.active_job_id != 0 && s.relay_on,
                "xch_single_scale_owner_consumes_weight_g_whatever_the_channel_fields");
    t += 100;
    dual_dispense_controller_tick(t + WEIGHT_MESSAGE_TIMEOUT_MS);   /* the scale falls silent */
    s = snap2(t + WEIGHT_MESSAGE_TIMEOUT_MS);
    audit_check(job_is(s_job, JOB_FAILED, "SCALE_MOVED") && !s.relay_on && !fake_get(1),
                "xch_silent_scale_fails_owner_safe_relay_off");
    fx_end();

    /* After the owner's cut, ANY later sample of the one scale (no sequence either
     * side) is a post-cut sample: there is no "other channel" traffic to exclude. */
    fx2_start(2, 0);
    feed_x(1100, 1, 0, 5000);
    dual_dispense_controller_tick(1100);
    audit_check(snap2(1100).state == DCH_SETTLING, "xch_f2_cut_enters_settling");
    feed_x(1150, 0, 5000, 0);
    dual_dispense_controller_tick(1150);
    audit_check(snap2(1150).state == DCH_SETTLING && snap2(1150).active_job_id != 0,
                "xch_settle_time_still_required");
    feed_x(1400, 0, 5000, 0);
    dual_dispense_controller_tick(1400);
    audit_check(job_is(s_job, JOB_COMPLETE, NULL), "xch_post_cut_sample_of_the_one_scale_completes");
    fx_end();
    /* Legacy frame (neither per-channel field) still drives the owner. */
    fx2_start(3, 0);
    s = snap2(1020);
    audit_check(s.state == DCH_DISPENSING && s.relay_on, "xch_legacy_frame_still_drives_owner");
    feed_x(1100, 3, 5000, 0);
    dual_dispense_controller_tick(1100);
    s = snap2(1100);
    audit_check(s.current_weight_g == 5000 && s.state == DCH_SETTLING && !s.relay_on,
                "xch_legacy_frame_weight_g_fallback_cuts_at_target");
    fx_end();
}

/* ---------------------------------------------------------------------------
 * PRE-06: watchdog task priority
 * ------------------------------------------------------------------------ */
static volatile uint32_t s_wd_count;
static volatile bool s_spin_done;
static volatile uint32_t s_wd_during;

static void wd_standin(void *arg)
{
    (void)arg;
    for (;;) {
        s_wd_count++;
        vTaskDelay(2); /* 20 ms at 100 Hz */
    }
}

static void spinner(void *arg)
{
    /* A supervisor that holds the core without ever blocking (250 ms). */
    TickType_t until = xTaskGetTickCount() + 25;
    (void)arg;
    while ((int32_t)(xTaskGetTickCount() - until) < 0) { }
    s_wd_during = s_wd_count;
    s_spin_done = true;
    vTaskDelete(NULL);
}

static uint32_t run_starvation(UBaseType_t wd_prio)
{
    TaskHandle_t wd = NULL;
    s_wd_count = 0;
    s_spin_done = false;
    xTaskCreatePinnedToCore(wd_standin, "wd_standin", 2048, NULL, wd_prio, &wd, 0);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let it start; count is reset after */
    s_wd_count = 0;
    xTaskCreatePinnedToCore(spinner, "spin_sup", 2048, NULL, SAFETY_SUPERVISOR_TASK_PRIORITY, NULL, 0);
    while (!s_spin_done) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelete(wd);
    vTaskDelay(pdMS_TO_TICKS(50));
    return s_wd_during;
}

static void test_pre06(void)
{
    audit_check(SAFETY_WATCHDOG_TASK_PRIORITY > SAFETY_SUPERVISOR_TASK_PRIORITY,
                "pre06_watchdog_priority_above_supervisor");
    uint32_t hi = run_starvation(SAFETY_WATCHDOG_TASK_PRIORITY);
    audit_check(hi >= 5U, "pre06_watchdog_runs_during_busy_supervisor_section");
    /* Negative control: the old priority (4) is starved by the same section. */
    uint32_t lo = run_starvation(SAFETY_SUPERVISOR_TASK_PRIORITY - 1);
    audit_check(lo <= 2U && lo < hi, "pre06_negative_control_old_priority_starved");
}

/* ---------------------------------------------------------------------------
 * PRE-02: pin validation
 * ------------------------------------------------------------------------ */
typedef struct { const char *key; const char *val; } kv_t;
static const kv_t k_base[] = {
    {"profile_id", "\"tune-a\""}, {"version", "3"},
    {"kp", "0.0025"}, {"ki", "0.0003"}, {"kd", "0.0001"},
    {"tolerance_g", "20"}, {"max_overshoot_g", "100"}, {"max_duration_ms", "120000"},
    {"window_ms", "500"}, {"min_on_ms", "40"}, {"min_off_ms", "40"},
    {"coarse_threshold_g", "500"}, {"fine_threshold_g", "100"}, {"micro_threshold_g", "30"},
    {"coarse_min_on_ms", "40"}, {"fine_min_on_ms", "20"}, {"micro_min_on_ms", "7"},
    {"settle_time_ms", "1500"}, {"inflight_comp_g", "15"},
};
#define BASE_N (sizeof(k_base) / sizeof(k_base[0]))

typedef struct { char key[24]; char val[48]; char op; } ov_t;

static int parse_ovs(const char *ovs, ov_t *o, int max)
{
    int n = 0;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", ovs ? ovs : "");
    for (char *tok = strtok(tmp, ";"); tok && n < max; tok = strtok(NULL, ";")) {
        ov_t *e = &o[n];
        memset(e, 0, sizeof(*e));
        char *eq = strchr(tok, '=');
        size_t kl;
        if (eq) {
            e->op = '=';
            kl = (size_t)(eq - tok);
            if (kl && (tok[kl - 1] == '+' || tok[kl - 1] == '^')) { e->op = tok[kl - 1]; kl--; }
            snprintf(e->val, sizeof(e->val), "%s", eq + 1);
        } else {
            kl = strlen(tok);
            if (kl && tok[kl - 1] == '-') { e->op = '-'; kl--; }
            else continue;
        }
        if (kl >= sizeof(e->key)) kl = sizeof(e->key) - 1;
        memcpy(e->key, tok, kl);
        n++;
    }
    return n;
}

static char s_js[1600];

/* ovs: ';'-separated  key=val (replace) | key+=val (add duplicate member after
 * the original) | key- (remove from nested) | key^=val (move outside nested). */
static const char *pin_json(const char *ovs)
{
    ov_t ov[16];
    int n = parse_ovs(ovs, ov, 16);
    char outside[160] = "";
    char nested[1200] = "";
    for (size_t i = 0; i < BASE_N; i++) {
        const char *val = k_base[i].val;
        bool emit = true, dup = false;
        const char *dupval = NULL;
        for (int j = 0; j < n; j++) {
            if (strcmp(ov[j].key, k_base[i].key) != 0) continue;
            if (ov[j].op == '=') val = ov[j].val;
            else if (ov[j].op == '-') emit = false;
            else if (ov[j].op == '+') { dup = true; dupval = ov[j].val; }
            else if (ov[j].op == '^') {
                emit = false;
                size_t l = strlen(outside);
                snprintf(outside + l, sizeof(outside) - l, "\"%s\":%s,", k_base[i].key, ov[j].val);
            }
        }
        size_t l = strlen(nested);
        if (emit) {
            snprintf(nested + l, sizeof(nested) - l, "%s\"%s\": %s", l ? "," : "", k_base[i].key, val);
            l = strlen(nested);
            if (dup) snprintf(nested + l, sizeof(nested) - l, ",\"%s\": %s", k_base[i].key, dupval);
        }
    }
    snprintf(s_js, sizeof(s_js),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"priority\":0,\"profile_id\":\"tune-a\",\"profile_version\":3,%s"
             "\"profile\":{%s}}", outside, nested);
    return s_js;
}

static double pf(const job_profile_t *p, const char *k)
{
    if (!strcmp(k, "kp")) return p->kp;
    if (!strcmp(k, "ki")) return p->ki;
    if (!strcmp(k, "kd")) return p->kd;
    if (!strcmp(k, "tolerance_g")) return p->tolerance_g;
    if (!strcmp(k, "max_overshoot_g")) return p->max_overshoot_g;
    if (!strcmp(k, "max_duration_ms")) return p->max_duration_ms;
    if (!strcmp(k, "window_ms")) return p->window_ms;
    if (!strcmp(k, "min_on_ms")) return p->min_on_ms;
    if (!strcmp(k, "min_off_ms")) return p->min_off_ms;
    if (!strcmp(k, "coarse_threshold_g")) return p->coarse_threshold_g;
    if (!strcmp(k, "fine_threshold_g")) return p->fine_threshold_g;
    if (!strcmp(k, "micro_threshold_g")) return p->micro_threshold_g;
    if (!strcmp(k, "coarse_min_on_ms")) return p->coarse_min_on_ms;
    if (!strcmp(k, "fine_min_on_ms")) return p->fine_min_on_ms;
    if (!strcmp(k, "micro_min_on_ms")) return p->micro_min_on_ms;
    if (!strcmp(k, "settle_time_ms")) return p->settle_time_ms;
    if (!strcmp(k, "inflight_comp_g")) return p->inflight_comp_g;
    return NAN;
}

static job_pin_result_t run_pin(const char *ovs, job_profile_t *out, char *err, size_t cap)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';
    return telemetry_client_parse_job_pin(pin_json(ovs), 1, 5000, out, err, cap);
}

static bool err_has(const char *err, const char *code, const char *key)
{
    return strstr(err, code) != NULL && (key == NULL || strstr(err, key) != NULL);
}

typedef struct {
    const char *key, *below, *lo, *hi, *above;
    const char *lo_ov, *hi_ov;
    bool hi_ok, integral, gain;
} row_t;

static const row_t k_rows[] = {
    {"kp", "-0.0001", "0", "1", "1.0000001", NULL, NULL, true, false, true},
    {"ki", "-0.0001", "0", "1", "1.0000001", NULL, NULL, true, false, true},
    {"kd", "-0.0001", "0", "1", "1.0000001", NULL, NULL, true, false, true},
    {"tolerance_g", "-1", "0", "4999", "100001", NULL, NULL, true, true, true}, /* fix1: max is now target-1 (M2) */
    {"max_overshoot_g", "-1", "0", "5000", "100001", NULL, NULL, true, true, true}, /* fix1: max is now min(target, cap) (M2) */
    {"max_duration_ms", "0", "1", "3600000", "3600001", NULL, NULL, true, true, true},
    {"window_ms", "49", "50", "10000", "10001", "min_off_ms=20", NULL, true, true, true}, /* fix1: min_off must fit the fine gap (M3) */
    {"min_on_ms", "0", "1", "10000", "10001", NULL, "window_ms=10000", true, true, true},
    {"min_off_ms", "0", "1", "5000", "10001", NULL, "window_ms=10000", true, true, true}, /* fix1: max is window - fine ceiling (M3) */
    {"coarse_threshold_g", "-1", "0", "100000", "100001", "fine_threshold_g=0;micro_threshold_g=0", NULL, true, true, false},
    {"fine_threshold_g", "-1", "0", "100000", "100001", "micro_threshold_g=0", "coarse_threshold_g=100000", true, true, false},
    {"micro_threshold_g", "-1", "0", "100000", "100001", NULL, "fine_threshold_g=100000;coarse_threshold_g=100000", true, true, false},
    {"coarse_min_on_ms", "0", "1", "10000", "10001", NULL, "window_ms=10000", true, true, false},
    {"fine_min_on_ms", "0", "1", "10000", "10001", NULL, "window_ms=10000", false, true, false},
    {"micro_min_on_ms", "0", "1", "10000", "10001", NULL, "window_ms=10000", false, true, false},
    {"settle_time_ms", "0", "1", "60000", "60001", NULL, NULL, true, true, false},
    {"inflight_comp_g", "-1", "0", "10000", "10001", NULL, NULL, true, true, false},
};

/* Value a staged field takes when the nested object does not carry it: the
 * Kconfig default (tests/qemu/CMakeLists.txt), NOT the table's base value. */
static const char *base_val(const char *key)
{
    if (!strcmp(key, "coarse_min_on_ms")) return "80";
    if (!strcmp(key, "fine_min_on_ms")) return "30";
    if (!strcmp(key, "micro_min_on_ms")) return "15";
    for (size_t i = 0; i < BASE_N; i++) if (!strcmp(k_base[i].key, key)) return k_base[i].val;
    return "0";
}

static void ovs_for(char *dst, size_t cap, const char *key, const char *val, const char *extra)
{
    snprintf(dst, cap, "%s=%s%s%s", key, val, extra ? ";" : "", extra ? extra : "");
}

static void rejects(const row_t *r, const char *tag, const char *val, const char *code)
{
    char ov[160], n[96], err[80];
    job_profile_t out;
    ovs_for(ov, sizeof(ov), r->key, val, NULL);
    job_pin_result_t res = run_pin(ov, &out, err, sizeof(err));
    snprintf(n, sizeof(n), "pre02_%s_%s", r->key, tag);
    audit_check(res == JOB_PIN_REFUSED && !out.valid && err_has(err, code, r->key), n);
}

static void test_pre02_table(void)
{
    char ov[260], n[96], err[80];
    job_profile_t out;

    /* The unmodified base must itself parse (guards the table). */
    audit_check(run_pin("", &out, err, sizeof(err)) == JOB_PIN_PARSED && out.valid, "pre02_base_pin_parses");

    for (size_t i = 0; i < sizeof(k_rows) / sizeof(k_rows[0]); i++) {
        const row_t *r = &k_rows[i];
        rejects(r, "below_min", r->below, "PIN_OUT_OF_RANGE");
        rejects(r, "negative", "-5", "PIN_OUT_OF_RANGE");
        rejects(r, "above_max", r->above, "PIN_OUT_OF_RANGE");
        rejects(r, "1e20", "1e20", "PIN_OUT_OF_RANGE");
        rejects(r, "1e999_inf", "1e999", "PIN_NOT_A_NUMBER");
        rejects(r, "string", "\"7\"", "PIN_NOT_A_NUMBER");
        rejects(r, "null", "null", "PIN_NOT_A_NUMBER");
        rejects(r, "bool", "true", "PIN_NOT_A_NUMBER");
        rejects(r, "nan_literal", "NaN", "PIN_NOT_A_NUMBER");
        if (r->integral) rejects(r, "fraction", "60.5", "PIN_NOT_INTEGER");

        ovs_for(ov, sizeof(ov), r->key, r->lo, r->lo_ov);
        job_pin_result_t res = run_pin(ov, &out, err, sizeof(err));
        snprintf(n, sizeof(n), "pre02_%s_min_accepted", r->key);
        audit_check(res == JOB_PIN_PARSED && out.valid && fabs(pf(&out, r->key) - atof(r->lo)) < 1e-6, n);

        ovs_for(ov, sizeof(ov), r->key, r->hi, r->hi_ov);
        res = run_pin(ov, &out, err, sizeof(err));
        snprintf(n, sizeof(n), "pre02_%s_max_%s", r->key, r->hi_ok ? "accepted" : "refused_cannot_pulse");
        if (r->hi_ok)
            audit_check(res == JOB_PIN_PARSED && out.valid && fabs(pf(&out, r->key) - atof(r->hi)) < 1e-3, n);
        else
            audit_check(res == JOB_PIN_REFUSED && !out.valid && err_has(err, "PIN_STAGE_CANNOT_PULSE", NULL), n);

        snprintf(ov, sizeof(ov), "%s+=%s", r->key, r->lo);
        res = run_pin(ov, &out, err, sizeof(err));
        snprintf(n, sizeof(n), "pre02_%s_duplicate_key_refused", r->key);
        audit_check(res == JOB_PIN_REFUSED && !out.valid && err_has(err, "PIN_DUPLICATE_KEY", r->key), n);

        /* BLK-20: the key exists only OUTSIDE the nested profile object. */
        snprintf(ov, sizeof(ov), "%s^=%s", r->key, r->lo);
        res = run_pin(ov, &out, err, sizeof(err));
        snprintf(n, sizeof(n), "pre02_%s_outside_nested_not_used", r->key);
        if (r->gain)
            audit_check(res == JOB_PIN_REFUSED && !out.valid, n);
        else
            audit_check(res == JOB_PIN_PARSED && fabs(pf(&out, r->key) - atof(base_val(r->key))) < 1e-6, n);

        if (!r->gain) {
            snprintf(ov, sizeof(ov), "%s-", r->key);
            res = run_pin(ov, &out, err, sizeof(err));
            snprintf(n, sizeof(n), "pre02_%s_absent_uses_default", r->key);
            audit_check(res == JOB_PIN_PARSED, n);
        }
    }
}

static void test_pre02_rules(void)
{
    char err[80];
    job_profile_t out;
    job_pin_result_t res;

    /* Strict number grammar. */
    static const char *bad[] = {"+0.5", ".5", "5.", "00.5", "0x1", "Infinity", "-", "1e", "0.5x", "[0.5]", "{}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char ov[64], n[64];
        snprintf(ov, sizeof(ov), "kp=%s", bad[i]);
        res = run_pin(ov, &out, err, sizeof(err));
        snprintf(n, sizeof(n), "pre02_grammar_rejects_%u", (unsigned)i);
        audit_check(res == JOB_PIN_REFUSED && !out.valid, n);
    }
    res = run_pin("kp=1E-1", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED && fabs(out.kp - 0.1) < 1e-6, "pre02_grammar_accepts_exponent_form");
    res = run_pin("tolerance_g=60.0", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED && out.tolerance_g == 60, "pre02_integral_float_form_accepted");

    /* Identity members and a second "profile" member. */
    res = run_pin("profile_id+=\"tune-a\"", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_DUPLICATE_KEY", "profile_id"), "pre02_duplicate_nested_profile_id");
    res = run_pin("version+=3", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_DUPLICATE_KEY", "version"), "pre02_duplicate_nested_version");
    {
        static char dup[1700];
        snprintf(dup, sizeof(dup), "%s", pin_json(""));
        size_t l = strlen(dup);
        dup[l - 1] = '\0'; /* drop final '}' */
        snprintf(dup + l - 1, sizeof(dup) - l + 1, ",\"profile\":{\"kp\":0.9}}");
        memset(&out, 0, sizeof(out));
        res = telemetry_client_parse_job_pin(dup, 1, 5000, &out, err, sizeof(err));
        audit_check(res == JOB_PIN_REFUSED && !out.valid && err_has(err, "PIN_DUPLICATE_KEY", "profile"),
                    "pre02_second_profile_member_refused");
    }

    /* Ordering / consistency. */
    res = run_pin("min_on_ms=600", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_INCONSISTENT", NULL), "pre02_min_on_gt_window_refused");
    res = run_pin("min_off_ms=501", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_INCONSISTENT", NULL), "pre02_min_off_gt_window_refused");
    res = run_pin("min_on_ms=500;min_off_ms=250", &out, err, sizeof(err)); /* fix1 (M3): min_off 500 now saturates the fine ceiling */
    audit_check(res == JOB_PIN_PARSED, "pre02_min_on_off_equal_window_accepted");
    res = run_pin("micro_threshold_g=101", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_INCONSISTENT", "thresholds"), "pre02_micro_gt_fine_refused");
    res = run_pin("fine_threshold_g=501", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_INCONSISTENT", "thresholds"), "pre02_fine_gt_coarse_refused");
    res = run_pin("micro_threshold_g=100;fine_threshold_g=100;coarse_threshold_g=100", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED, "pre02_equal_thresholds_accepted");
    /* Chosen rule: max_overshoot_g only has to be >= 0 (same as store path and
     * server schema); a pin with overshoot < tolerance is accepted. */
    res = run_pin("tolerance_g=50;max_overshoot_g=10", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED && out.max_overshoot_g == 10, "pre02_overshoot_below_tolerance_accepted_by_rule");

    /* Stage pulse rule at window 500: coarse <= 500, fine <= 250, micro <= 75. */
    res = run_pin("coarse_min_on_ms=500;min_on_ms=40", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED, "pre02_pulse_coarse_500_ok");
    res = run_pin("coarse_min_on_ms=501", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_STAGE_CANNOT_PULSE", "coarse"), "pre02_pulse_coarse_501_refused");
    res = run_pin("fine_min_on_ms=250", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED, "pre02_pulse_fine_250_ok");
    res = run_pin("fine_min_on_ms=251", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_STAGE_CANNOT_PULSE", "fine"), "pre02_pulse_fine_251_refused");
    res = run_pin("micro_min_on_ms=75", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED, "pre02_pulse_micro_75_ok");
    res = run_pin("micro_min_on_ms=76", &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_STAGE_CANNOT_PULSE", "micro"), "pre02_pulse_micro_76_refused");

    /* Staged defaults (80/30/15) apply when the nested object omits them. */
    static const char *no_staged =
        "coarse_threshold_g-;fine_threshold_g-;micro_threshold_g-;coarse_min_on_ms-;"
        "fine_min_on_ms-;micro_min_on_ms-;settle_time_ms-;inflight_comp_g-";
    char ov[300];
    snprintf(ov, sizeof(ov), "%s;window_ms=500", no_staged);
    res = run_pin(ov, &out, err, sizeof(err));
    audit_check(res == JOB_PIN_PARSED && out.coarse_min_on_ms == 80 && out.micro_min_on_ms == 15 &&
                out.settle_time_ms == 1500, "pre02_server_shaped_pin_without_staged_fields_parses_with_defaults");
    snprintf(ov, sizeof(ov), "%s;window_ms=100", no_staged);
    audit_check(run_pin(ov, &out, err, sizeof(err)) == JOB_PIN_PARSED, "pre02_default_stages_pulse_at_window_100");
    snprintf(ov, sizeof(ov), "%s;window_ms=90", no_staged);
    res = run_pin(ov, &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_STAGE_CANNOT_PULSE", "micro"),
                "pre02_phase4_finding_micro_never_pulses_is_refused");
    snprintf(ov, sizeof(ov), "%s;window_ms=50;min_on_ms=10;min_off_ms=10", no_staged);
    res = run_pin(ov, &out, err, sizeof(err));
    audit_check(res == JOB_PIN_REFUSED && err_has(err, "PIN_STAGE_CANNOT_PULSE", NULL), "pre02_window_50_with_default_stages_refused");

    /* Legacy command (no pin at all) is unchanged. */
    memset(&out, 0, sizeof(out));
    res = telemetry_client_parse_job_pin(
        "{\"command_id\":1,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\",\"priority\":0}",
        1, 5000, &out, err, sizeof(err));
    audit_check(res == JOB_PIN_ABSENT, "pre02_legacy_command_still_absent");
}

typedef struct { int n; char state[12]; char error[121]; uint32_t job; } ack1_t;
static ack1_t s_ack;
static void ack1(void *ctx, uint32_t id, const char *state, uint32_t lj, uint8_t ch, const char *error)
{
    (void)ctx; (void)id; (void)ch;
    s_ack.n++;
    snprintf(s_ack.state, sizeof(s_ack.state), "%s", state);
    snprintf(s_ack.error, sizeof(s_ack.error), "%s", error);
    s_ack.job = lj;
}

static uint32_t queued_jobs(void)
{
    static dispense_job_t jobs[JOB_QUEUE_MAX];
    return job_queue_snapshot(jobs, JOB_QUEUE_MAX);
}

static void test_pre02_dispatch(void)
{
    static char cmd[1000];
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    telemetry_client_test_reset_commands();
    telemetry_client_test_reset_ready();

    const char *fmt =
        "{\"command_id\": %u, \"type\": \"JOB\", \"command_type\": \"JOB\", \"channel_id\": null, "
        "\"issued_at\": \"2026-10-07T08:15:30.123Z\", \"ttl_ms\": 0, \"material_id\": \"M1\", "
        "\"target_g\": 5000, \"priority\": 0, \"profile_id\": \"tune-a\", \"profile_version\": 3, "
        "\"profile\": {\"profile_id\": \"tune-a\", \"version\": 3, \"kp\": %s, \"ki\": 0.0, "
        "\"kd\": 0.0, \"tolerance_g\": 20, \"max_overshoot_g\": 100, \"max_duration_ms\": 120000, "
        "\"window_ms\": 500, \"min_on_ms\": 40, \"min_off_ms\": 40}}";

    memset(&s_ack, 0, sizeof(s_ack));
    snprintf(cmd, sizeof(cmd), fmt, 8101U, "5.0");
    telemetry_client_dispatch_command(cmd, TELEMETRY_CMD_HTTP, 1000, 1001, ack1, NULL);
    audit_check(s_ack.n == 1 && !strcmp(s_ack.state, "FAILED") &&
                err_has(s_ack.error, "PIN_OUT_OF_RANGE", "kp") && s_ack.job == 0,
                "pre02_dispatch_bad_gain_fails_job_with_specific_text");
    audit_check(queued_jobs() == 0, "pre02_dispatch_bad_gain_enqueues_nothing");

    memset(&s_ack, 0, sizeof(s_ack));
    snprintf(cmd, sizeof(cmd), fmt, 8102U, "1e20");
    telemetry_client_dispatch_command(cmd, TELEMETRY_CMD_HTTP, 1000, 1002, ack1, NULL);
    audit_check(!strcmp(s_ack.state, "FAILED") && queued_jobs() == 0, "pre02_dispatch_1e20_enqueues_nothing");

    memset(&s_ack, 0, sizeof(s_ack));
    snprintf(cmd, sizeof(cmd), fmt, 8103U, "0.25");
    telemetry_client_dispatch_command(cmd, TELEMETRY_CMD_HTTP, 1000, 1003, ack1, NULL);
    audit_check(!strcmp(s_ack.state, "QUEUED") && queued_jobs() == 1, "pre02_dispatch_valid_pin_still_queued");
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */
static void batch1_body(void)
{
    ESP_LOGI("PRE", "prereq batch 1 tests");
    test_pre02_table();
    test_pre02_rules();
    test_pre02_dispatch();
    test_pre03();
    test_pre04();
    test_pre05a();
    test_cross_channel();
    test_pre06();
    fx_end();
    (void)relay_init_all();
    (void)safety_manager_init();
}

/* fix1: the main task stack in this QEMU project is only 3584 bytes, which the
 * deeper pin-parse and failure-path frames overran (heap corruption seen in
 * pre06). The batch runs in a task with a normal-sized stack instead; no
 * assertion changed. */
void test_prereq_batch1_run(void)
{
    test_run_on_big_stack(batch1_body);
}
