/* Phase 5 batch 1, fix round 1. Every test name starts with fix1_.
 *
 * H1 tests run the REAL relay_expander + relay_driver + safety_manager +
 * dual_dispense_controller against a fake PCF8574 that models real shadow
 * semantics: a write can be latched by the device and still be reported failed
 * to the master. The fake is attached through BITS_QEMU_TEST_HOOKS seams that the
 * production project does not define. No hardware is ever actuated. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dual_dispense_controller.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "job_queue.h"
#include "relay_driver.h"
#include "relay_expander.h"
#include "safety_manager.h"
#include "telemetry_client.h"
#include "test_prereq_fix1.h"

extern void audit_check(bool condition, const char *name);
extern void dual_dispense_controller_test_set_relay_hook(esp_err_t (*set_fn)(uint8_t, bool),
                                                         bool (*get_fn)(uint8_t));
extern void dual_dispense_controller_test_set_gate_hook(bool (*allowed_fn)(void));
extern void relay_expander_test_set_bus(esp_err_t (*tx)(uint8_t), esp_err_t (*rx)(uint8_t *),
                                        uint8_t initial_shadow, int gate);

/* ---------------------------------------------------------------------------
 * Big-stack runner
 * ------------------------------------------------------------------------ */
static void (*s_run_fn)(void);
static volatile bool s_run_done;

static void runner_task(void *arg)
{
    (void)arg;
    s_run_fn();
    s_run_done = true;
    vTaskDelete(NULL);
}

void test_run_on_big_stack(void (*fn)(void))
{
    s_run_fn = fn;
    s_run_done = false;
    xTaskCreatePinnedToCore(runner_task, "fix1_runner", 16384, NULL,
                            uxTaskPriorityGet(NULL), NULL, xPortGetCoreID());
    while (!s_run_done) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---------------------------------------------------------------------------
 * Fake PCF8574 (ACTIVE-HIGH: relay bit 6 = Relay 1, bit 7 = Relay 2)
 * ------------------------------------------------------------------------ */
typedef enum {
    M_OK = 0,           /* device latches, master sees success */
    M_LATCH_FAIL,       /* device LATCHES the byte, master sees an error */
    M_REJECT,           /* device ignores the byte, master sees an error */
} wmode_t;

static uint8_t s_latch;      /* device output latch == pin level unless held */
static wmode_t s_on_mode, s_off_mode;
static esp_err_t s_fail_err;
static int s_tx_n, s_on_tx, s_off_tx, s_rx_n;
static bool s_rx_fail;
static uint8_t s_rx_or;      /* pins held high externally (read-back disagrees) */

static bool pattern_is_off(uint8_t v) { return (v & RELAY_EXPANDER_USED_MASK) == 0U; }

static esp_err_t fake_tx(uint8_t v)
{
    s_tx_n++;
    bool off = pattern_is_off(v);
    if (off) s_off_tx++; else s_on_tx++;
    wmode_t m = off ? s_off_mode : s_on_mode;
    if (m == M_OK) { s_latch = v; return ESP_OK; }
    if (m == M_LATCH_FAIL) { s_latch = v; return s_fail_err; }
    return s_fail_err;
}

static esp_err_t fake_rx(uint8_t *out)
{
    s_rx_n++;
    if (s_rx_fail) return ESP_FAIL;
    *out = (uint8_t)(s_latch | s_rx_or);
    return ESP_OK;
}

static bool latch_on(int ch) { return ((s_latch >> (6 + ch)) & 1U) != 0U; }

static void counters_zero(void)
{
    s_tx_n = s_on_tx = s_off_tx = s_rx_n = 0;
}

static void fb_up(int gate)
{
    s_latch = 0x3F;
    s_on_mode = s_off_mode = M_OK;
    s_fail_err = ESP_ERR_TIMEOUT;
    s_rx_fail = false;
    s_rx_or = 0;
    dual_dispense_controller_test_set_relay_hook(NULL, NULL);
    dual_dispense_controller_test_set_gate_hook(NULL);
    relay_expander_test_set_bus(fake_tx, fake_rx, 0x3FU, gate);
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    counters_zero();
}

static void fb_down(void)
{
    relay_expander_test_set_bus(NULL, NULL, 0U, -1);
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)dual_dispense_controller_init();
}

/* ---------------------------------------------------------------------------
 * Controller helpers
 * ------------------------------------------------------------------------ */
static uint32_t s_cmd = 31000;
static uint32_t s_job1, s_job2;

static void feed(uint32_t now, int32_t w)
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

static void store_slot(uint8_t ch)
{
    (void)dual_dispense_controller_set_pending_profile_staged(ch, "fix1-slot", 1, 5000,
          0.99f, 0.0f, 0.0f, 20, 100, 120000, 500, 40, 40, 500, 100, 30, 80, 30, 15, 200, 0);
}

/* CH1 job taken through the READY gate to its first DISPENSING tick (t+20). */
static void start_ch1(uint32_t t, int32_t w)
{
    store_slot(1);
    (void)dual_dispense_controller_add_material_server_job_pinned(1, 5000, 0, s_cmd++, NULL, &s_job1);
    feed(t, w);
    dual_dispense_controller_tick(t);
    (void)dual_dispense_controller_operator_ready(1, t + 10U);
    feed(t + 20U, w);
    dual_dispense_controller_tick(t + 20U);
}

static void reset_faults(void)
{
    dual_dispense_controller_clear_emergency_stop();
    (void)safety_manager_release_estop();
    (void)safety_manager_clear();
}

/* A latched-but-reported-failed ON on Relay 1 made directly through the driver. */
static void make_latched_failed_on(void)
{
    wmode_t keep = s_on_mode;
    s_on_mode = M_LATCH_FAIL;
    (void)relay_set(0, true);
    s_on_mode = keep;
}

/* ---------------------------------------------------------------------------
 * H1: failed ON that the device latched must still be turned OFF on the bus
 * ------------------------------------------------------------------------ */
static void test_h1_controller_failure_path(void)
{
    fb_up(1);
    s_on_mode = M_LATCH_FAIL;
    start_ch1(1000, 0);
    dual_channel_snapshot_t s = snap(1, 1020);
    audit_check(s_on_tx >= 1, "fix1_h1_failed_on_was_written_to_the_bus");
    audit_check(job_is(s_job1, JOB_FAILED, "RELAY_WRITE_FAILED") && s.state == DCH_FAULT,
                "fix1_h1_relay_write_failed_fails_job");
    audit_check(!latch_on(0) && (s_latch & RELAY_EXPANDER_USED_MASK) == 0U,
                "fix1_h1_relay_write_failed_turns_latched_on_off_on_the_bus");
    audit_check(s_off_tx >= 1, "fix1_h1_relay_write_failed_issued_an_actual_off_write");
    audit_check(safety_manager_fault() == SAFETY_CONTROL_FAILURE,
                "fix1_h1_relay_write_failed_latches_control_failure");
    audit_check(!relay_get_state(0) && !relay_expander_state_unknown(),
                "fix1_h1_state_known_off_after_confirmed_off");
    fb_down();
}

static void test_h1_every_off_path(void)
{
    fb_up(1);

    /* A failed ON is reported to the caller, the device latched it, and the
     * driver now reports the relay as ON (unknown is treated as energized). */
    make_latched_failed_on();
    audit_check(latch_on(0), "fix1_h1_fake_models_latched_but_reported_failed_on");
    audit_check(relay_expander_state_unknown() && relay_get_state(0),
                "fix1_h1_failed_on_marks_state_unknown_and_reports_on");
    audit_check(relay_pad_is_energized(0), "fix1_h1_pad_reports_energized_while_unknown");

    /* relay_set(false) is not short-circuited. */
    counters_zero();
    (void)relay_set(0, false);
    audit_check(s_off_tx == 1 && !latch_on(0), "fix1_h1_relay_set_off_writes_and_clears_latch");
    audit_check(!relay_expander_state_unknown() && !relay_get_state(0),
                "fix1_h1_relay_set_off_restores_known_state");

    /* relay_all_off. */
    make_latched_failed_on();
    counters_zero();
    (void)relay_all_off();
    audit_check(s_off_tx == 1 && !latch_on(0), "fix1_h1_relay_all_off_writes_and_clears_latch");

    /* relay_force_all_off. */
    make_latched_failed_on();
    counters_zero();
    audit_check(relay_force_all_off() == ESP_OK && s_off_tx == 1 && !latch_on(0),
                "fix1_h1_force_all_off_writes_and_clears_latch");

    /* safety_manager_raise (new fault edge). */
    make_latched_failed_on();
    counters_zero();
    safety_manager_raise(SAFETY_WEIGHT_STALE, 3000);
    audit_check(s_off_tx >= 1 && !latch_on(0), "fix1_h1_safety_raise_clears_latched_on");

    /* safety_manager_raise re-assert of an already held fault. */
    make_latched_failed_on();
    counters_zero();
    safety_manager_raise(SAFETY_WEIGHT_STALE, 3100);
    audit_check(s_off_tx >= 1 && !latch_on(0), "fix1_h1_safety_reassert_clears_latched_on");

    /* Estop entry and the per-tick estop retry (fault still held here). */
    dual_dispense_controller_tick(3200);       /* enters emergency stop */
    make_latched_failed_on();
    counters_zero();
    dual_dispense_controller_tick(3210);       /* per-tick retry, estop already held */
    audit_check(s_off_tx >= 1 && !latch_on(0), "fix1_h1_estop_per_tick_clears_latched_on");

    /* Emergency stop entry forces the write too. */
    reset_faults();
    (void)relay_all_off();
    make_latched_failed_on();
    counters_zero();
    dual_dispense_controller_emergency_stop(3300);
    audit_check(s_off_tx >= 1 && !latch_on(0), "fix1_h1_emergency_stop_entry_clears_latched_on");

    reset_faults();
    fb_down();
}

static void test_h1_failed_off_retries_and_known_short_circuits(void)
{
    fb_up(1);

    /* KNOWN state: no needless bus traffic. */
    counters_zero();
    (void)relay_set(0, false);
    (void)relay_set(1, false);
    (void)relay_all_off();
    audit_check(s_tx_n == 0, "fix1_h1_known_off_state_short_circuits_no_bus_traffic");
    (void)relay_set(0, true);
    audit_check(s_on_tx == 1 && latch_on(0), "fix1_h1_known_state_on_transition_is_written");
    counters_zero();
    (void)relay_set(0, true);
    audit_check(s_tx_n == 0, "fix1_h1_known_on_repeat_short_circuits");

    /* A failed OFF keeps retrying on every attempt. */
    s_off_mode = M_REJECT;
    counters_zero();
    audit_check(relay_set(0, false) != ESP_OK && latch_on(0), "fix1_h1_failed_off_reported_and_relay_still_on");
    audit_check(relay_get_state(0), "fix1_h1_failed_off_state_reads_on");
    for (int i = 0; i < 3; i++) (void)relay_all_off();
    audit_check(s_off_tx == 4 && latch_on(0), "fix1_h1_failed_off_retries_every_attempt");
    (void)relay_force_all_off();
    audit_check(s_off_tx == 5, "fix1_h1_force_retries_a_failed_off_too");

    /* Bus recovers: the next OFF lands, then the state is KNOWN and silent again. */
    s_off_mode = M_OK;
    audit_check(relay_all_off() == ESP_OK && !latch_on(0) && !relay_get_state(0),
                "fix1_h1_off_lands_when_bus_recovers");
    counters_zero();
    (void)relay_all_off();
    (void)relay_set(0, false);
    audit_check(s_tx_n == 0, "fix1_h1_silent_again_once_off_is_confirmed");
    fb_down();
}

static void test_h1_force_readback(void)
{
    fb_up(1);

    counters_zero();
    audit_check(relay_force_all_off() == ESP_OK, "fix1_h1_force_known_off_returns_ok");
    audit_check(s_off_tx == 1 && s_rx_n == 1, "fix1_h1_force_writes_even_when_state_known_and_reads_back");

    /* Read failure is fail-safe: not a confirmed OFF, state UNKNOWN, retried. */
    s_rx_fail = true;
    audit_check(relay_force_all_off() != ESP_OK, "fix1_h1_force_readback_failure_is_an_error");
    audit_check(relay_expander_state_unknown() && relay_get_state(0),
                "fix1_h1_readback_failure_leaves_state_unknown");
    s_rx_fail = false;
    counters_zero();
    (void)relay_all_off();
    audit_check(s_off_tx == 1 && !relay_expander_state_unknown(),
                "fix1_h1_all_off_writes_after_readback_failure");

    /* Read-back disagreeing on a relay bit (pin held high) is not a confirmed OFF. */
    s_rx_or = 0x40;
    audit_check(relay_force_all_off() == ESP_ERR_INVALID_RESPONSE, "fix1_h1_force_readback_mismatch_is_an_error");
    audit_check(relay_expander_state_unknown(), "fix1_h1_readback_mismatch_leaves_state_unknown");
    s_rx_or = 0;
    audit_check(relay_force_all_off() == ESP_OK && !relay_expander_state_unknown(),
                "fix1_h1_force_readback_clean_restores_known_state");

    /* Forced OFF that the bus rejects stays reported as failed and energized. */
    (void)relay_set(0, true);
    s_off_mode = M_REJECT;
    audit_check(relay_force_all_off() != ESP_OK && latch_on(0) && relay_get_state(0),
                "fix1_h1_force_rejected_off_stays_failed_and_energized");
    s_off_mode = M_OK;
    (void)relay_force_all_off();
    fb_down();
}

/* ---------------------------------------------------------------------------
 * M1: gated refusal is decided by the gate, not by the error code
 * ------------------------------------------------------------------------ */
static void test_m1(void)
{
    /* Real gate closed: the ON is refused with zero bus traffic, nothing latches. */
    fb_up(0);
    audit_check(!relay_actuation_allowed(), "fix1_m1_gate_closed_reported_by_driver");
    start_ch1(1000, 0);
    audit_check(s_on_tx == 0, "fix1_m1_gated_on_causes_zero_on_bus_traffic");
    audit_check(job_is(s_job1, JOB_FAILED, "RELAY_WRITE_FAILED") && snap(1, 1020).state == DCH_FAULT,
                "fix1_m1_gated_on_fails_job_and_faults_channel");
    audit_check(!safety_manager_fault_active(), "fix1_m1_gated_refusal_does_not_latch_safety_fault");
    fb_down();

    /* Gate open, bus reports ESP_ERR_INVALID_STATE (stuck bus) after the byte was
     * latched: this MUST latch like any write failure. */
    fb_up(1);
    audit_check(relay_actuation_allowed(), "fix1_m1_gate_open_reported_by_driver");
    s_fail_err = ESP_ERR_INVALID_STATE;
    s_on_mode = M_LATCH_FAIL;
    start_ch1(1000, 0);
    audit_check(safety_manager_fault() == SAFETY_CONTROL_FAILURE,
                "fix1_m1_invalid_state_from_stuck_bus_latches_control_failure");
    audit_check(!latch_on(0), "fix1_m1_invalid_state_latched_on_is_turned_off");
    fb_down();

    /* Same code, byte not latched: still a write failure, still latches. */
    fb_up(1);
    s_fail_err = ESP_ERR_INVALID_STATE;
    s_on_mode = M_REJECT;
    start_ch1(1000, 0);
    audit_check(safety_manager_fault() == SAFETY_CONTROL_FAILURE,
                "fix1_m1_invalid_state_rejected_write_latches_control_failure");
    fb_down();

    /* Manual start with the gate closed does not latch; with a bus failure it does. */
    fb_up(0);
    audit_check(dual_dispense_controller_manual_relay(1, true, 500) == ESP_ERR_INVALID_STATE &&
                !safety_manager_fault_active(), "fix1_m1_manual_on_gated_does_not_latch");
    fb_down();
    fb_up(1);
    s_on_mode = M_LATCH_FAIL;
    audit_check(dual_dispense_controller_manual_relay(1, true, 500) == ESP_ERR_INVALID_STATE &&
                safety_manager_fault() == SAFETY_CONTROL_FAILURE && !latch_on(0),
                "fix1_m1_manual_on_bus_failure_latches_and_turns_off");
    reset_faults();
    fb_down();
}

/* ---------------------------------------------------------------------------
 * M6: a failed OFF must not let the other channel start or release the scale
 * ------------------------------------------------------------------------ */
static void test_m6(void)
{
    fb_up(1);
    start_ch1(1000, 0);
    dual_channel_snapshot_t s = snap(1, 1020);
    audit_check(s.state == DCH_DISPENSING && s.relay_on && latch_on(0), "fix1_m6_setup_ch1_dispensing_relay_on");

    (void)dual_dispense_controller_add_material_server_job_pinned(2, 5000, 0, s_cmd++, NULL, &s_job2);
    feed(1030, 0);
    dual_dispense_controller_tick(1030);
    audit_check(job_is(s_job2, JOB_QUEUED, NULL), "fix1_m6_setup_ch2_job_waits_queued_while_ch1_owns_scale");

    /* CH1 reaches target; its OFF is rejected by the bus. */
    s_off_mode = M_REJECT;
    feed(1100, 5000);
    dual_dispense_controller_tick(1100);
    s = snap(1, 1100);
    audit_check(job_is(s_job1, JOB_FAILED, "RELAY_WRITE_FAILED") && s.relay_on && latch_on(0),
                "fix1_m6_failed_off_fails_ch1_job_and_stays_energized");
    audit_check(job_is(s_job2, JOB_QUEUED, NULL), "fix1_m6_ch2_job_still_queued_in_the_same_tick");
    audit_check(snap(2, 1100).state != DCH_WAIT_WEIGHT && snap(2, 1100).state != DCH_DISPENSING &&
                !snap(2, 1100).relay_on, "fix1_m6_ch2_did_not_start_in_the_same_tick");
    audit_check(dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1,
                "fix1_m6_scale_not_released_while_relay_on");
    audit_check(safety_manager_fault_active(), "fix1_m6_safety_fault_latched");

    for (uint32_t t = 1110; t <= 1300; t += 30) {
        feed(t, 5000);
        dual_dispense_controller_tick(t);
    }
    audit_check(job_is(s_job2, JOB_QUEUED, NULL) && latch_on(0) &&
                dual_dispense_controller_scale_owner() == SCALE_OWNER_CH1,
                "fix1_m6_ch2_stays_queued_and_scale_held_while_off_keeps_failing");
    audit_check(s_off_tx >= 3, "fix1_m6_off_retried_every_tick_while_failing");

    /* Bus recovers: estop retry lands, only then is the scale released. */
    s_off_mode = M_OK;
    feed(1330, 5000);
    dual_dispense_controller_tick(1330);
    audit_check(!latch_on(0) && !snap(1, 1330).relay_on, "fix1_m6_off_lands_after_recovery");
    audit_check(dual_dispense_controller_scale_owner() == SCALE_OWNER_NONE,
                "fix1_m6_scale_released_once_off_confirmed");
    audit_check(job_is(s_job2, JOB_QUEUED, NULL), "fix1_m6_ch2_job_still_queued_after_recovery_until_operator_clears");
    for (uint32_t t = 1360; t <= 1500; t += 30) {
        feed(t, 5000);
        dual_dispense_controller_tick(t);
    }
    audit_check(job_is(s_job2, JOB_QUEUED, NULL), "fix1_m6_ch2_never_left_queued");
    reset_faults();
    fb_down();
}

/* ---------------------------------------------------------------------------
 * M5: watchdog / panic reset latches CONTROL_FAILURE at boot
 * ------------------------------------------------------------------------ */
static void test_m5(void)
{
    static const struct { esp_reset_reason_t r; const char *n; } abnormal[] = {
        {ESP_RST_TASK_WDT, "task_wdt"}, {ESP_RST_INT_WDT, "int_wdt"},
        {ESP_RST_WDT, "wdt"}, {ESP_RST_PANIC, "panic"},
    };
    for (size_t i = 0; i < sizeof(abnormal) / sizeof(abnormal[0]); i++) {
        char n[80];
        fb_up(1);
        s_latch = 0x7F; /* pretend the pre-reset relay state is still on the bus */
        store_slot(1);
        (void)dual_dispense_controller_add_material_server_job_pinned(1, 5000, 0, s_cmd++, NULL, &s_job1);
        bool latched = safety_manager_note_boot_reset(abnormal[i].r, 100);
        snprintf(n, sizeof(n), "fix1_m5_%s_reset_latches_control_failure", abnormal[i].n);
        audit_check(latched && safety_manager_fault() == SAFETY_CONTROL_FAILURE, n);
        snprintf(n, sizeof(n), "fix1_m5_%s_reset_turns_relays_off_on_the_bus", abnormal[i].n);
        audit_check(!latch_on(0) && !latch_on(1), n);
        feed(200, 0);
        dual_dispense_controller_tick(200);
        (void)dual_dispense_controller_operator_ready(1, 210);
        dual_dispense_controller_tick(220);
        snprintf(n, sizeof(n), "fix1_m5_%s_reset_no_job_can_run", abnormal[i].n);
        audit_check(job_is(s_job1, JOB_QUEUED, NULL) && !latch_on(0), n);
        safety_manager_on_valid_weight(230);
        snprintf(n, sizeof(n), "fix1_m5_%s_reset_not_cleared_by_fresh_weight", abnormal[i].n);
        audit_check(safety_manager_fault() == SAFETY_CONTROL_FAILURE, n);
        dual_dispense_controller_clear_emergency_stop();
        snprintf(n, sizeof(n), "fix1_m5_%s_reset_cleared_only_by_operator", abnormal[i].n);
        audit_check(safety_manager_clear() && !safety_manager_fault_active(), n);
        fb_down();
    }

    static const struct { esp_reset_reason_t r; const char *n; } normal[] = {
        {ESP_RST_POWERON, "poweron"}, {ESP_RST_SW, "sw"}, {ESP_RST_EXT, "ext"},
        {ESP_RST_DEEPSLEEP, "deepsleep"}, {ESP_RST_BROWNOUT, "brownout"}, {ESP_RST_UNKNOWN, "unknown"},
    };
    for (size_t i = 0; i < sizeof(normal) / sizeof(normal[0]); i++) {
        char n[80];
        (void)safety_manager_init();
        bool latched = safety_manager_note_boot_reset(normal[i].r, 100);
        snprintf(n, sizeof(n), "fix1_m5_%s_reset_does_not_latch", normal[i].n);
        audit_check(!latched && !safety_manager_fault_active(), n);
    }
    (void)safety_manager_init();
}

/* ---------------------------------------------------------------------------
 * L3 / L8
 * ------------------------------------------------------------------------ */
static void test_l3_l8(void)
{
    /* L3: a failed OFF in pause leaves FAULT, never PAUSED. */
    fb_up(1);
    start_ch1(1000, 0);
    s_off_mode = M_REJECT;
    bool ok = dual_dispense_controller_pause(1, 1100);
    audit_check(!ok && snap(1, 1100).state == DCH_FAULT, "fix1_l3_pause_with_failed_off_stays_fault_not_paused");
    s_off_mode = M_OK;
    dual_dispense_controller_tick(1200);
    audit_check(!latch_on(0), "fix1_l3_failed_pause_off_cleared_by_estop_retry");
    reset_faults();
    fb_down();

    /* L8: overweight seen while PAUSED is handled before any ON after resume. */
    fb_up(1);
    start_ch1(1000, 0);
    audit_check(dual_dispense_controller_pause(1, 1100) && snap(1, 1100).state == DCH_PAUSED && !latch_on(0),
                "fix1_l8_paused_relay_off");
    int on_before = s_on_tx;
    feed(1500, 5101);
    dual_dispense_controller_tick(1500);
    audit_check(snap(1, 1500).state == DCH_PAUSED && s_on_tx == on_before, "fix1_l8_paused_stays_paused_no_on_write");
    audit_check(dual_dispense_controller_resume(1, 1600) && snap(1, 1600).state == DCH_WAIT_WEIGHT,
                "fix1_l8_resume_goes_to_wait_weight");
    feed(1610, 5101);
    dual_dispense_controller_tick(1610);
    audit_check(job_is(s_job1, JOB_FAILED, "OVERWEIGHT"), "fix1_l8_overweight_handled_on_first_tick_after_resume");
    audit_check(s_on_tx == on_before && !latch_on(0), "fix1_l8_relay_never_on_after_pause_before_overweight_handled");
    reset_faults();
    fb_down();
}

/* ---------------------------------------------------------------------------
 * L6: supervisor-like stack depth on the failure path (4096 B task)
 * ------------------------------------------------------------------------ */
static volatile bool s_l6_done;
static volatile uint32_t s_l6_free;

static void l6_task(void *arg)
{
    (void)arg;
    fb_up(1);
    s_on_mode = M_LATCH_FAIL;
    start_ch1(1000, 0);              /* ON fails -> relay_write_failed -> raise -> force */
    dual_dispense_controller_tick(1030); /* emergency stop path */
    s_on_mode = M_OK;
    s_off_mode = M_REJECT;
    reset_faults();
    start_ch1(2000, 0);
    (void)relay_set(0, true);
    feed(2100, 5000);
    dual_dispense_controller_tick(2100); /* failed OFF -> finish -> raise */
    dual_dispense_controller_tick(2110);
    s_l6_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_off_mode = M_OK;
    reset_faults();
    fb_down();
    s_l6_done = true;
    vTaskDelete(NULL);
}

static void test_l6(void)
{
    s_l6_done = false;
    xTaskCreatePinnedToCore(l6_task, "fix1_l6", 4096, NULL, uxTaskPriorityGet(NULL), NULL, xPortGetCoreID());
    while (!s_l6_done) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI("FIX1", "L6 failure-path min free stack in a 4096 B task: %u bytes (QEMU: fake bus, no real i2c driver frames)",
             (unsigned)s_l6_free);
    audit_check(s_l6_free >= 384U, "fix1_l6_failure_path_leaves_at_least_384_bytes_of_a_4096_stack");
}

/* ---------------------------------------------------------------------------
 * PRE-06 as far as QEMU allows: the TWDT subscribe/reset/delete pattern the
 * supervisor uses. Real expiry, panic reset and reset-reason are hardware-only.
 * ------------------------------------------------------------------------ */
static void test_twdt(void)
{
    esp_err_t add = esp_task_wdt_add(NULL);
    audit_check(add == ESP_OK || add == ESP_ERR_INVALID_ARG, "fix1_pre06_twdt_subscribe_api_usable_in_qemu");
    if (add == ESP_OK) {
        audit_check(esp_task_wdt_status(NULL) == ESP_OK, "fix1_pre06_twdt_task_reported_subscribed");
        audit_check(esp_task_wdt_reset() == ESP_OK, "fix1_pre06_twdt_reset_from_subscribed_task_ok");
        audit_check(esp_task_wdt_delete(NULL) == ESP_OK, "fix1_pre06_twdt_unsubscribe_ok");
        audit_check(esp_task_wdt_status(NULL) != ESP_OK, "fix1_pre06_twdt_task_reported_unsubscribed");
    }
}

/* ---------------------------------------------------------------------------
 * Pin parser: L1 / L2 / M2 / M3
 * ------------------------------------------------------------------------ */
static const char *k_pairs[][2] = {
    {"profile_id", "\"tune-a\""}, {"version", "3"},
    {"kp", "0.0025"}, {"ki", "0.0003"}, {"kd", "0.0001"},
    {"tolerance_g", "20"}, {"max_overshoot_g", "100"}, {"max_duration_ms", "120000"},
    {"window_ms", "500"}, {"min_on_ms", "40"}, {"min_off_ms", "40"},
    {"coarse_threshold_g", "500"}, {"fine_threshold_g", "100"}, {"micro_threshold_g", "30"},
    {"coarse_min_on_ms", "40"}, {"fine_min_on_ms", "20"}, {"micro_min_on_ms", "7"},
    {"settle_time_ms", "1500"}, {"inflight_comp_g", "15"},
};
#define PAIRS_N (sizeof(k_pairs) / sizeof(k_pairs[0]))

static char s_nested[1400];
static char s_cmdbuf[2200];

/* Nested body with up to two value overrides and a raw tail appended verbatim. */
static const char *nested_with(const char *k1, const char *v1, const char *k2, const char *v2,
                               const char *tail)
{
    s_nested[0] = '\0';
    for (size_t i = 0; i < PAIRS_N; i++) {
        const char *v = k_pairs[i][1];
        if (k1 && !strcmp(k1, k_pairs[i][0])) v = v1;
        if (k2 && !strcmp(k2, k_pairs[i][0])) v = v2;
        size_t l = strlen(s_nested);
        snprintf(s_nested + l, sizeof(s_nested) - l, "%s\"%s\":%s", i ? "," : "", k_pairs[i][0], v);
    }
    if (tail) {
        size_t l = strlen(s_nested);
        snprintf(s_nested + l, sizeof(s_nested) - l, "%s", tail);
    }
    return s_nested;
}

static job_pin_result_t pin_cmd(const char *pre, const char *nested, const char *post, int32_t target,
                                job_profile_t *out, char *err, size_t cap)
{
    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":%ld,\"material_id\":\"M1\","
             "\"priority\":0,%s\"profile_id\":\"tune-a\",\"profile_version\":3,\"profile\":{%s}%s}",
             (long)target, pre ? pre : "", nested, post ? post : "");
    memset(out, 0, sizeof(*out));
    err[0] = '\0';
    return telemetry_client_parse_job_pin(s_cmdbuf, 1, target, out, err, cap);
}

static bool refused_with(job_pin_result_t r, const job_profile_t *p, const char *err, const char *code)
{
    return r == JOB_PIN_REFUSED && !p->valid && strstr(err, code) != NULL;
}

static void test_pin_l1_l2(void)
{
    char err[80];
    job_profile_t p;
    job_pin_result_t r;

    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED && p.valid, "fix1_l1_base_pin_parses");

    /* Trailing junk after a number. */
    r = pin_cmd(NULL, nested_with("kp", "0.5 x", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_trailing_junk_after_number_refused");
    /* Missing comma between members. */
    r = pin_cmd(NULL, nested_with("kp", "0.1 \"kp\":0.9", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_missing_comma_duplicate_kp_refused");
    r = pin_cmd(NULL, nested_with("kp", "0.1 \"zzz\":1", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_missing_comma_other_member_refused");
    /* Trailing comma, comma-only, garbage after last member. */
    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, ","), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_trailing_comma_refused");
    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, ",,\"zzz\":1"), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_double_comma_refused");
    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, ",\"zzz\":"), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_member_without_value_refused");
    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, ",\"zzz\" 1"), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_member_without_colon_refused");
    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, ",zzz:1"), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MALFORMED"), "fix1_l1_unquoted_key_refused");
    /* Scalars in members the pin does not use are still held to the grammar. */
    static const char *bad_tails[] = {
        ",\"zzz\":NaN", ",\"zzz\":truex", ",\"zzz\":+1", ",\"zzz\":01", ",\"zzz\":1.", ",\"zzz\":.5",
        ",\"zzz\":[1 2]", ",\"zzz\":[1,]", ",\"zzz\":{\"a\":1,}", ",\"zzz\":\"a\\qb\"",
        ",\"zzz\":\"a\\u12G4\"", ",\"zzz\":\"unterminated", ",\"zzz\":[[[[[[[[[1]]]]]]]]]",
    };
    for (size_t i = 0; i < sizeof(bad_tails) / sizeof(bad_tails[0]); i++) {
        char n[64];
        r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL, bad_tails[i]), NULL, 5000, &p, err, sizeof(err));
        snprintf(n, sizeof(n), "fix1_l1_bad_unused_member_%u_refused", (unsigned)i);
        audit_check(r == JOB_PIN_REFUSED && !p.valid, n);
    }
    /* Well-formed extras (nested objects, arrays, escapes, literals) are fine. */
    r = pin_cmd(NULL, nested_with(NULL, NULL, NULL, NULL,
        ",\"meta\":{\"a\":[1,2.5e3,-0,\"x\\n\\u00e9\",null,true,false],\"b\":{}} , \"note\" : \"ok\""),
        NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED && p.valid, "fix1_l1_well_formed_extras_accepted");
    /* Whitespace everywhere. */
    r = pin_cmd(NULL, " \n\t \"profile_id\" : \"tune-a\" , \"version\" : 3 , \"kp\" : 0.0025 , \"ki\" : 0.0003 ,"
                " \"kd\" : 0.0001 , \"tolerance_g\" : 20 , \"max_overshoot_g\" : 100 , \"max_duration_ms\" : 120000 ,"
                " \"window_ms\" : 500 , \"min_on_ms\" : 40 , \"min_off_ms\" : 40 \n", NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED && p.valid && p.window_ms == 500, "fix1_l1_whitespace_between_tokens_accepted");

    /* Specific per-key codes are preserved (NaN as a pin value). */
    r = pin_cmd(NULL, nested_with("kp", "NaN", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_NOT_A_NUMBER"), "fix1_l1_per_key_code_kept_for_nan_value");

    /* L2: top-level locators use the walker. */
    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"meta\":{\"profile\":{\"kp\":0.9},\"profile_id\":\"other\"},\"profile_id\" : \"tune-a\","
             "\"profile_version\" : 3,\"profile\" : {%s}}", nested_with(NULL, NULL, NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED && p.valid && fabs(p.kp - 0.0025) < 1e-6 && !strcmp(p.profile_id, "tune-a"),
                "fix1_l2_sub_object_named_profile_and_spaces_before_colon_handled");

    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"profile_id\":\"tune-a\",\"profile_version\":3,\"profile\":{%s},\"profile\" : {\"kp\":0.9}}",
             nested_with(NULL, NULL, NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_DUPLICATE_KEY"), "fix1_l2_second_profile_member_with_space_before_colon_refused");

    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"profile_id\":\"tune-a\",\"profile_id\" : \"tune-b\",\"profile_version\":3,\"profile\":{%s}}",
             nested_with(NULL, NULL, NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_DUPLICATE_KEY"), "fix1_l2_duplicate_top_level_profile_id_refused");

    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"profile_id\":\"tune-a\",\"profile_version\":3,\"profile_version\" : 4,\"profile\":{%s}}",
             nested_with(NULL, NULL, NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_DUPLICATE_KEY"), "fix1_l2_duplicate_top_level_profile_version_refused");

    /* Non-integral version. */
    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"profile_id\":\"tune-a\",\"profile_version\":1.9,\"profile\":{%s}}",
             nested_with("version", "1", NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_NOT_INTEGER"), "fix1_l2_top_level_version_1_9_refused");
    r = pin_cmd(NULL, nested_with("version", "1.9", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_NOT_INTEGER"), "fix1_l2_nested_version_1_9_refused");
    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"profile_id\":\"tune-a\",\"profile_version\":3.0,\"profile\":{%s}}",
             nested_with("version", "3.0", NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED && p.version == 3U, "fix1_l2_integral_float_version_3_0_accepted");
    snprintf(s_cmdbuf, sizeof(s_cmdbuf),
             "{\"command_id\":2,\"command_type\":\"JOB\",\"target_g\":5000,\"material_id\":\"M1\","
             "\"profile_id\":\"tune-a\",\"profile_version\":\"3\",\"profile\":{%s}}",
             nested_with(NULL, NULL, NULL, NULL, NULL));
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin(s_cmdbuf, 1, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_NOT_A_NUMBER"), "fix1_l2_string_version_refused");

    /* A non-object command is refused, not guessed at. */
    memset(&p, 0, sizeof(p));
    r = telemetry_client_parse_job_pin("\"profile\":{\"kp\":1}", 1, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_REFUSED && !p.valid, "fix1_l2_non_object_command_refused");
}

static void test_pin_m2_m3(void)
{
    char err[80];
    job_profile_t p;
    job_pin_result_t r;
    char v[16];

    /* M2: tolerance / overshoot relative to the target. */
    r = pin_cmd(NULL, nested_with("tolerance_g", "5000", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "tolerance_g >= target_g"), "fix1_m2_tolerance_equal_target_refused");
    r = pin_cmd(NULL, nested_with("tolerance_g", "4999", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED, "fix1_m2_tolerance_target_minus_1_accepted");
    r = pin_cmd(NULL, nested_with("tolerance_g", "100000", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_INCONSISTENT"), "fix1_m2_tolerance_100000_on_5000_target_refused");
    r = pin_cmd(NULL, nested_with("max_overshoot_g", "100000", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "max_overshoot_g"), "fix1_m2_overshoot_100000_on_5000_target_refused");
    r = pin_cmd(NULL, nested_with("max_overshoot_g", "5000", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED && p.max_overshoot_g == 5000, "fix1_m2_overshoot_equal_target_accepted");
    r = pin_cmd(NULL, nested_with("max_overshoot_g", "5001", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "max_overshoot_g"), "fix1_m2_overshoot_target_plus_1_refused");
    r = pin_cmd(NULL, nested_with("max_overshoot_g", "5000", NULL, NULL, NULL), NULL, 20000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED, "fix1_m2_absolute_cap_5000_accepted_on_20000_target");
    r = pin_cmd(NULL, nested_with("max_overshoot_g", "5001", NULL, NULL, NULL), NULL, 20000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "max_overshoot_g"), "fix1_m2_absolute_cap_5001_refused_on_20000_target");
    r = pin_cmd(NULL, nested_with("tolerance_g", "50", "max_overshoot_g", "10", NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED, "fix1_m2_overshoot_below_tolerance_still_accepted");
    r = pin_cmd(NULL, nested_with("max_overshoot_g", "0", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(r == JOB_PIN_PARSED, "fix1_m2_zero_overshoot_still_accepted");

    /* M3: min_off must leave the stage ceiling a gap. window 500: fine on = 250. */
    static const struct { const char *w, *off; bool ok; } cases[] = {
        {"500", "40", true}, {"500", "250", true}, {"500", "251", false}, {"500", "450", false},
        {"500", "500", false}, {"100", "50", true}, {"100", "51", false}, {"333", "166", true},
        {"333", "167", false}, {"10000", "5000", true}, {"10000", "5001", false},
        {"50", "25", true}, {"50", "26", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char n[80];
        /* stage min_on values that pulse at every window used here */
        r = pin_cmd(NULL, nested_with("window_ms", cases[i].w, "min_off_ms", cases[i].off,
                                      ",\"zzz\":1"), NULL, 5000, &p, err, sizeof(err));
        /* min_on 40 > window/2 for tiny windows is fine: only fine/micro stage min_on matter. */
        snprintf(n, sizeof(n), "fix1_m3_window_%s_min_off_%s_%s", cases[i].w, cases[i].off,
                 cases[i].ok ? "accepted" : "refused");
        if (cases[i].ok)
            audit_check(r == JOB_PIN_PARSED, n);
        else
            audit_check(r == JOB_PIN_REFUSED && !p.valid &&
                        (strstr(err, "PIN_MIN_OFF_SATURATES") || strstr(err, "PIN_INCONSISTENT")), n);
    }
    r = pin_cmd(NULL, nested_with("min_off_ms", "251", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MIN_OFF_SATURATES"), "fix1_m3_window_500_min_off_251_text_is_pin_min_off_saturates");
    r = pin_cmd(NULL, nested_with("min_off_ms", "450", NULL, NULL, NULL), NULL, 5000, &p, err, sizeof(err));
    audit_check(refused_with(r, &p, err, "PIN_MIN_OFF_SATURATES"), "fix1_m3_window_500_min_off_450_text_is_pin_min_off_saturates");
    (void)v;
}

/* ---------------------------------------------------------------------------
 * Store path (legacy) parity: same rules as the pin path
 * ------------------------------------------------------------------------ */
static bool store(int32_t target, int32_t tol, int32_t ovs, uint32_t win, uint32_t on, uint32_t off,
                  int32_t ct, int32_t ft, int32_t mt, uint32_t con, uint32_t fon, uint32_t mon)
{
    return dual_dispense_controller_set_pending_profile_staged(1, "fix1-store", 1, target,
        0.01f, 0.0f, 0.0f, tol, ovs, 120000, win, on, off, ct, ft, mt, con, fon, mon, 1500, 15);
}

static void test_store_path(void)
{
    (void)dual_dispense_controller_init();
    audit_check(store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_baseline_accepted");
    audit_check(!store(5000, 5000, 100, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_tolerance_equal_target_refused");
    audit_check(store(5000, 4999, 100, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_tolerance_target_minus_1_accepted");
    audit_check(!store(5000, 20, 100000, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_overshoot_100000_on_5000_refused");
    audit_check(store(5000, 20, 5000, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_overshoot_equal_target_accepted");
    audit_check(!store(5000, 20, 5001, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_overshoot_target_plus_1_refused");
    audit_check(!store(20000, 20, 5001, 500, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_overshoot_over_absolute_cap_refused");
    audit_check(store(5000, 20, 100, 500, 40, 250, 500, 100, 30, 80, 30, 15), "fix1_store_min_off_250_accepted");
    audit_check(!store(5000, 20, 100, 500, 40, 251, 500, 100, 30, 80, 30, 15), "fix1_store_min_off_251_saturates_refused");
    audit_check(!store(5000, 20, 100, 500, 40, 450, 500, 100, 30, 80, 30, 15), "fix1_store_min_off_450_saturates_refused");
    audit_check(store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 80, 30, 75), "fix1_store_micro_min_on_75_pulses");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 80, 30, 76), "fix1_store_micro_min_on_76_never_pulses_refused");
    audit_check(store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 80, 250, 15), "fix1_store_fine_min_on_250_pulses");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 80, 251, 15), "fix1_store_fine_min_on_251_never_pulses_refused");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 501, 30, 15), "fix1_store_coarse_min_on_501_never_pulses_refused");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 500, 100, 101, 80, 30, 15), "fix1_store_micro_gt_fine_refused");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 99, 100, 30, 80, 30, 15), "fix1_store_fine_gt_coarse_refused");
    audit_check(store(5000, 20, 100, 500, 40, 40, 100000, 100000, 100000, 80, 30, 15), "fix1_store_thresholds_at_100000_accepted");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 100001, 100, 30, 80, 30, 15), "fix1_store_threshold_100001_refused");
    audit_check(!store(5000, 20, 100, 500, 40, 40, 500, 100, 30, 80, 30, 10001), "fix1_store_stage_min_on_10001_refused");
    audit_check(!store(5000, 20, 100, 90, 40, 40, 500, 100, 30, 80, 30, 15), "fix1_store_phase4_micro_never_pulses_window_90_refused");
    /* The non-staged setter derives thresholds; its default call must still be accepted. */
    audit_check(dual_dispense_controller_set_pending_default(1, 5000), "fix1_store_device_default_still_accepted");
    (void)dual_dispense_controller_init();
}

/* ---------------------------------------------------------------------------
 * M4: legacy PROFILE command, table driven
 * ------------------------------------------------------------------------ */
typedef struct { char state[12]; char error[121]; int n; uint32_t job; } ack_t;
static ack_t s_ack;
static void ack_cb(void *ctx, uint32_t id, const char *state, uint32_t lj, uint8_t ch, const char *error)
{
    (void)ctx; (void)id; (void)ch;
    s_ack.n++;
    snprintf(s_ack.state, sizeof(s_ack.state), "%s", state);
    snprintf(s_ack.error, sizeof(s_ack.error), "%s", error);
    s_ack.job = lj;
}

static const char *k_prof[][2] = {
    {"target_g", "5000"}, {"version", "7"},
    {"kp", "0.0025"}, {"ki", "0.0003"}, {"kd", "0.0001"},
    {"tolerance_g", "20"}, {"max_overshoot_g", "100"}, {"max_duration_ms", "120000"},
    {"window_ms", "500"}, {"min_on_ms", "40"}, {"min_off_ms", "40"},
    {"coarse_threshold_g", "500"}, {"fine_threshold_g", "100"}, {"micro_threshold_g", "30"},
    {"coarse_min_on_ms", "40"}, {"fine_min_on_ms", "20"}, {"micro_min_on_ms", "7"},
    {"settle_time_ms", "1500"}, {"inflight_comp_g", "15"},
};
#define PROF_N (sizeof(k_prof) / sizeof(k_prof[0]))

static char s_pcmd[1600];
static const char *profile_cmd(const char *key, const char *val, const char *key2, const char *val2)
{
    snprintf(s_pcmd, sizeof(s_pcmd),
             "{\"command_id\":%lu,\"type\":\"PROFILE\",\"command_type\":\"PROFILE\",\"channel_id\":\"CH1\","
             "\"material_id\":\"M1\",\"profile_id\":\"leg-a\"", (unsigned long)s_cmd++);
    for (size_t i = 0; i < PROF_N; i++) {
        const char *v = k_prof[i][1];
        if (key && !strcmp(key, k_prof[i][0])) v = val;
        if (key2 && !strcmp(key2, k_prof[i][0])) v = val2;
        size_t l = strlen(s_pcmd);
        snprintf(s_pcmd + l, sizeof(s_pcmd) - l, ",\"%s\":%s", k_prof[i][0], v);
    }
    size_t l = strlen(s_pcmd);
    snprintf(s_pcmd + l, sizeof(s_pcmd) - l, "}");
    return s_pcmd;
}

static bool dispatch_failed(const char *cmd)
{
    memset(&s_ack, 0, sizeof(s_ack));
    telemetry_client_dispatch_command(cmd, TELEMETRY_CMD_HTTP, 1000, 1001, ack_cb, NULL);
    return s_ack.n == 1 && !strcmp(s_ack.state, "FAILED");
}
static bool dispatch_applied(const char *cmd)
{
    memset(&s_ack, 0, sizeof(s_ack));
    telemetry_client_dispatch_command(cmd, TELEMETRY_CMD_HTTP, 1000, 1001, ack_cb, NULL);
    return s_ack.n == 1 && !strcmp(s_ack.state, "APPLIED");
}

static void test_m4(void)
{
    (void)relay_init_all();
    (void)safety_manager_init();
    (void)job_queue_init();
    (void)dual_dispense_controller_init();
    telemetry_client_test_reset_commands();

    audit_check(dispatch_applied(profile_cmd(NULL, NULL, NULL, NULL)), "fix1_m4_profile_command_baseline_applied");

    static const char *bad[] = {"1e20", "-5", "NaN", "1e999", "4294967346", "2147483648", "\"7\"", "null"};
    static const char *tags[] = {"1e20", "negative", "nan", "1e999", "2pow32_plus_50", "2pow31", "string", "null"};
    for (size_t k = 0; k < PROF_N; k++) {
        for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
            char n[96];
            /* 2^31 is a legal profile version (uint32); everything else is refused. */
            if (!strcmp(k_prof[k][0], "version") && !strcmp(tags[b], "2pow31")) continue;
            snprintf(n, sizeof(n), "fix1_m4_profile_%s_%s_refused", k_prof[k][0], tags[b]);
            audit_check(dispatch_failed(profile_cmd(k_prof[k][0], bad[b], NULL, NULL)), n);
        }
    }
    /* Rule parity with the pin path. */
    audit_check(dispatch_failed(profile_cmd("tolerance_g", "5000", NULL, NULL)), "fix1_m4_profile_tolerance_equal_target_refused");
    audit_check(dispatch_failed(profile_cmd("max_overshoot_g", "100000", NULL, NULL)), "fix1_m4_profile_overshoot_100000_on_5000_refused");
    audit_check(dispatch_applied(profile_cmd("max_overshoot_g", "5000", NULL, NULL)), "fix1_m4_profile_overshoot_equal_target_applied");
    audit_check(dispatch_failed(profile_cmd("min_off_ms", "251", NULL, NULL)), "fix1_m4_profile_min_off_251_saturates_refused");
    audit_check(dispatch_applied(profile_cmd("min_off_ms", "250", NULL, NULL)), "fix1_m4_profile_min_off_250_applied");
    audit_check(dispatch_applied(profile_cmd("micro_min_on_ms", "75", NULL, NULL)), "fix1_m4_profile_micro_min_on_75_applied");
    audit_check(dispatch_failed(profile_cmd("micro_min_on_ms", "76", NULL, NULL)), "fix1_m4_profile_micro_never_pulses_refused");
    audit_check(dispatch_failed(profile_cmd("window_ms", "90", "micro_min_on_ms", "15")), "fix1_m4_profile_window_90_micro_never_pulses_refused");
    audit_check(dispatch_failed(profile_cmd("micro_threshold_g", "101", NULL, NULL)), "fix1_m4_profile_micro_gt_fine_refused");
    audit_check(dispatch_failed(profile_cmd("coarse_threshold_g", "100001", NULL, NULL)), "fix1_m4_profile_threshold_over_100000_refused");
    /* Deliberate difference: a fraction is truncated, not refused (documented). */
    audit_check(dispatch_applied(profile_cmd("tolerance_g", "20.9", NULL, NULL)), "fix1_m4_profile_fraction_is_truncated_documented_difference");

    /* Non-canonical / hostile target. */
    audit_check(dispatch_failed(profile_cmd("target_g", "1e20", NULL, NULL)), "fix1_m4_profile_target_1e20_refused");
    audit_check(dispatch_failed(profile_cmd("target_g", "4999", NULL, NULL)), "fix1_m4_profile_non_canonical_target_refused");

    /* long_field: out-of-range ids/targets are not cast. */
    {
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "{\"command_id\":1e20,\"command_type\":\"JOB\",\"material_id\":\"M1\",\"target_g\":5000}");
        memset(&s_ack, 0, sizeof(s_ack));
        telemetry_client_dispatch_command(cmd, TELEMETRY_CMD_HTTP, 1000, 1001, ack_cb, NULL);
        audit_check(s_ack.n == 0, "fix1_m4_command_id_1e20_is_ignored_not_cast");
        snprintf(cmd, sizeof(cmd), "{\"command_id\":%lu,\"command_type\":\"JOB\",\"material_id\":\"M1\",\"target_g\":1e20}",
                 (unsigned long)s_cmd++);
        audit_check(dispatch_failed(cmd), "fix1_m4_job_target_1e20_fails_not_cast");
    }
    (void)dual_dispense_controller_init();
}

/* ---------------------------------------------------------------------------
 * Entry
 * ------------------------------------------------------------------------ */
static void fix1_body(void)
{
    ESP_LOGI("FIX1", "prereq fix round 1 tests");
    test_h1_controller_failure_path();
    test_h1_every_off_path();
    test_h1_failed_off_retries_and_known_short_circuits();
    test_h1_force_readback();
    test_m1();
    test_m6();
    test_m5();
    test_l3_l8();
    test_l6();
    test_twdt();
    test_pin_l1_l2();
    test_pin_m2_m3();
    test_store_path();
    test_m4();
    fb_down();
}

void test_prereq_fix1_run(void)
{
    test_run_on_big_stack(fix1_body);
    (void)relay_init_all();
    (void)safety_manager_init();
}
