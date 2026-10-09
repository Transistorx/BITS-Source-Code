#include "dual_dispense_controller.h"

#include "dispense_controller.h" /* tested pure PID and window math */
#include "esp_log.h"
#include "job_queue.h"
#include "relay_driver.h"
#include "safety_manager.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

#include <math.h>
#include <string.h>

#define TAG "DUAL_CTRL"
#define CHANNELS 2U

/* ---- Kconfig defaults (also the device-default profile) ---- */
#ifndef CONFIG_WEIGHT_DEMO_PID_KP_X10000
#define CONFIG_WEIGHT_DEMO_PID_KP_X10000 25
#endif
#ifndef CONFIG_WEIGHT_DEMO_PID_KI_X10000
#define CONFIG_WEIGHT_DEMO_PID_KI_X10000 3
#endif
#ifndef CONFIG_WEIGHT_DEMO_PID_KD_X10000
#define CONFIG_WEIGHT_DEMO_PID_KD_X10000 1
#endif
#ifndef CONFIG_WEIGHT_DEMO_TOLERANCE_G
#define CONFIG_WEIGHT_DEMO_TOLERANCE_G 20
#endif
#ifndef CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G
#define CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G 100
#endif
#ifndef CONFIG_WEIGHT_DEMO_WINDOW_MS
#define CONFIG_WEIGHT_DEMO_WINDOW_MS 500
#endif
#ifndef CONFIG_WEIGHT_DEMO_MIN_ON_MS
#define CONFIG_WEIGHT_DEMO_MIN_ON_MS 40
#endif
#ifndef CONFIG_WEIGHT_DEMO_MIN_OFF_MS
#define CONFIG_WEIGHT_DEMO_MIN_OFF_MS 40
#endif
#ifndef CONFIG_WEIGHT_DEMO_MAX_DURATION_MS
#define CONFIG_WEIGHT_DEMO_MAX_DURATION_MS 120000
#endif

/* Staged-dispense defaults. Coarse threshold is large (most of the job is
 * coarse fill); fine covers the mid-range; micro is the last few grams before
 * tolerance. In-flight compensation stops early by the amount still travelling to
 * the scale after the relay closes. */
#ifndef CONFIG_WEIGHT_DEMO_COARSE_THRESHOLD_G
#define CONFIG_WEIGHT_DEMO_COARSE_THRESHOLD_G 500
#endif
#ifndef CONFIG_WEIGHT_DEMO_FINE_THRESHOLD_G
#define CONFIG_WEIGHT_DEMO_FINE_THRESHOLD_G 100
#endif
#ifndef CONFIG_WEIGHT_DEMO_MICRO_THRESHOLD_G
#define CONFIG_WEIGHT_DEMO_MICRO_THRESHOLD_G 30
#endif
#ifndef CONFIG_WEIGHT_DEMO_COARSE_MIN_ON_MS
#define CONFIG_WEIGHT_DEMO_COARSE_MIN_ON_MS 80
#endif
#ifndef CONFIG_WEIGHT_DEMO_FINE_MIN_ON_MS
#define CONFIG_WEIGHT_DEMO_FINE_MIN_ON_MS 30
#endif
#ifndef CONFIG_WEIGHT_DEMO_MICRO_MIN_ON_MS
#define CONFIG_WEIGHT_DEMO_MICRO_MIN_ON_MS 15
#endif
#ifndef CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS
#define CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS 1500
#endif
#ifndef CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G
#define CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G 15
#endif

/* Canonical target table. Exactly these four targets are accepted; anything
 * else is refused at profile-store and at job-add time. */
#define PROFILE_SLOT_COUNT 4U
static const int32_t k_canonical_targets[PROFILE_SLOT_COUNT] = {
    5000, 10000, 15000, 20000
};

static bool target_is_canonical(int32_t target_g)
{
    for (uint8_t i = 0; i < PROFILE_SLOT_COUNT; ++i)
        if (k_canonical_targets[i] == target_g) return true;
    return false;
}

static int profile_slot_for(int32_t target_g)
{
    for (uint8_t i = 0; i < PROFILE_SLOT_COUNT; ++i)
        if (k_canonical_targets[i] == target_g) return (int)i;
    return -1;
}

/* Stage duty ceilings, single source for stage_duty_clamp() and the shared
 * profile consistency check below (the pin parser used to mirror them by hand). */
#define DCH_COARSE_DUTY_CEIL 1.0f
#define DCH_FINE_DUTY_CEIL   0.5f
#define DCH_MICRO_DUTY_CEIL  0.15f

/* On-time of a stage at its ceiling, exactly as dispense_window_output() rounds. */
static uint32_t stage_ceiling_on_ms(float ceiling, uint32_t window_ms)
{
    uint32_t on_ms = (uint32_t)(ceiling * (float)window_ms + 0.5f);
    return on_ms > window_ms ? window_ms : on_ms;
}

const char *dual_dispense_profile_consistency_error(const dual_profile_limits_t *p)
{
    if (p == NULL) return "PIN_INCONSISTENT no profile";
    if (p->min_on_ms > p->window_ms || p->min_off_ms > p->window_ms)
        return "PIN_INCONSISTENT min_on_ms/min_off_ms > window_ms";
    if (p->coarse_threshold_g < 0 || p->fine_threshold_g < 0 || p->micro_threshold_g < 0 ||
        p->coarse_threshold_g > 100000 || p->fine_threshold_g > 100000 ||
        p->micro_threshold_g > 100000)
        return "PIN_OUT_OF_RANGE thresholds (0..100000)";
    if (!(p->micro_threshold_g <= p->fine_threshold_g &&
          p->fine_threshold_g <= p->coarse_threshold_g))
        return "PIN_INCONSISTENT thresholds need micro<=fine<=coarse";
    if (p->coarse_min_on_ms == 0U || p->fine_min_on_ms == 0U || p->micro_min_on_ms == 0U ||
        p->coarse_min_on_ms > 10000U || p->fine_min_on_ms > 10000U ||
        p->micro_min_on_ms > 10000U)
        return "PIN_OUT_OF_RANGE stage min_on_ms (1..10000)";

    /* A stage pulses only if its ceiling on-time reaches min_on; otherwise every
     * burst is dropped and the stage never moves material. */
    if (stage_ceiling_on_ms(DCH_COARSE_DUTY_CEIL, p->window_ms) < p->coarse_min_on_ms)
        return "PIN_STAGE_CANNOT_PULSE coarse (min_on > window)";
    if (stage_ceiling_on_ms(DCH_FINE_DUTY_CEIL, p->window_ms) < p->fine_min_on_ms)
        return "PIN_STAGE_CANNOT_PULSE fine (min_on > 0.5*window)";
    if (stage_ceiling_on_ms(DCH_MICRO_DUTY_CEIL, p->window_ms) < p->micro_min_on_ms)
        return "PIN_STAGE_CANNOT_PULSE micro (min_on > 0.15*window)";

    /* M3: a gap shorter than min_off is removed by holding the valve ON for the
     * whole window (dispense_window_output), which defeats the stage ceiling.
     * The gap is smallest at the ceiling, so checking there is exact. COARSE is
     * exempt: its ceiling is already continuous ON. */
    if (stage_ceiling_on_ms(DCH_FINE_DUTY_CEIL, p->window_ms) + p->min_off_ms > p->window_ms)
        return "PIN_MIN_OFF_SATURATES fine (min_off > window - 0.5*window)";
    if (stage_ceiling_on_ms(DCH_MICRO_DUTY_CEIL, p->window_ms) + p->min_off_ms > p->window_ms)
        return "PIN_MIN_OFF_SATURATES micro (min_off > window - 0.15*window)";

    /* M2: with tolerance >= target every job starts inside the SETTLING dead
     * zone at weight 0; an overshoot allowance beyond the target (or the
     * absolute cap) is never a sane limit. */
    if (p->target_g > 0) {
        if (p->tolerance_g >= p->target_g)
            return "PIN_INCONSISTENT tolerance_g >= target_g";
        if (p->max_overshoot_g > p->target_g || p->max_overshoot_g > DCH_ABS_MAX_OVERSHOOT_G)
            return "PIN_INCONSISTENT max_overshoot_g > min(target_g, cap)";
    }
    return NULL;
}

/* ---- Profile: persistent, per-target, never consumed ---- */
typedef struct {
    bool valid;
    uint32_t version;
    int32_t target_g;
    float kp, ki, kd;
    int32_t tolerance_g, overshoot_g;
    uint32_t max_duration_ms, window_ms, min_on_ms, min_off_ms;
    /* Staged-dispense parameters. */
    int32_t coarse_threshold_g, fine_threshold_g, micro_threshold_g;
    uint32_t coarse_min_on_ms, fine_min_on_ms, micro_min_on_ms;
    uint32_t settle_time_ms;
    int32_t inflight_comp_g;
    /* Optional NO_PROGRESS thresholds per stage (COARSE/FINE/MICRO); 0 = learned. */
    uint32_t np_window_ms[3];
    int32_t np_min_rise_g[3];
    char profile_id[65];
} tuned_profile_t;

typedef struct {
    dual_channel_snapshot_t view;
    dispense_job_t job;
    uint32_t last_weight_ms, started_ms, state_since_ms, last_pid_ms;
    uint32_t window_since_ms, settled_since_ms;
    float integral, last_error;
    bool have_last_error;
    /* Persistent profile table, one slot per canonical target. */
    tuned_profile_t profiles[PROFILE_SLOT_COUNT];
    /* Staged-dispense runtime. */
    dispense_stage_t stage;
    bool awaiting_operator_ready;
    uint32_t last_cas_seq_seen;   /* to require a fresh post-pulse sample */
    bool have_cas_seq;
    const char *fault;
    /* PRE-05a: settle freshness. weight_gen counts accepted owner samples; the
     * cut_* fields are captured on the relay-off edge into SETTLING. */
    uint32_t weight_gen, cut_gen, cut_ms;
    bool have_seq, last_sample_has_seq, cut_have_seq;
    uint32_t last_seq, cut_seq;
    /* Single-scale watchdogs (CONTRACT 9.11). SCALE_MOVED: previous stable reading
     * and a flag set at sample time so no sample between two ticks can be missed.
     * NO_PROGRESS: relay-ON time in the current window and what it is measured from. */
    int32_t last_stable_g;
    bool have_last_stable, moved_pending;
    uint32_t wait_since_ms;          /* entered WAITING_FOR_SCALE_MOVE with a job */
    uint32_t np_last_ms, np_on_ms;
    int32_t np_base_g, np_learn_rise_g;
    bool np_active, np_learned;
    int np_stage;
} channel_t;

/* The ONE scale. Every accepted sample lands here; only the owner's pump also gets
 * it. With no owner this is all that is updated, so neither pump is ever renewed. */
typedef struct {
    bool have;
    char scale_id[17], boot_id[9];
    int32_t weight_g;
    bool stable;
    uint32_t rx_ms;
    bool have_seq;
    uint32_t last_seq;
    uint32_t stable_n, stable_since_ms;   /* consecutive stable samples and the first one's time */
} scale_view_t;

/* NO_PROGRESS: one tick never counts more than this much relay-ON time, so a
 * stalled supervisor cannot fabricate a long window out of one late tick. */
#define NP_DT_CAP_MS 500U
/* Few scale-resolution steps: no learned/derived rise bound goes below this many. */
#define NP_MIN_STEPS 3
/* Samples required in the stable run before READY (on top of the duration). */
#define READY_MIN_STABLE_SAMPLES 3U

static channel_t s_ch[CHANNELS];
static bool s_estopped;
static atomic_bool s_stop_requested;
/* All channel/ownership/profile APIs serialize across supervisor, commands,
 * httpd and live/history readers. No network work occurs under this mutex.
 * Recursive because several existing public APIs call other public APIs. */
static SemaphoreHandle_t s_state_mutex;
static scale_owner_t s_scale_owner = SCALE_OWNER_NONE;
static scale_view_t s_scale;
/* Station confirmation: the pump the scale was last confirmed at (by an accepted
 * READY) and the sender boot_id at that moment. Void on any fault, new boot_id,
 * scale offline or E-Stop. Not ownership: ownership is per running job. */
static bool s_station_valid;
static uint8_t s_station_ch;
static char s_station_boot[9];
/* UNVALIDATED conservative defaults; app_main installs the NVS values. NOT reset by
 * init so a configuration survives a controller re-init. */
static dual_scale_cfg_t s_cfg = {
    .ready_stable_ms = 3000U, .ready_max_age_ms = 120000U,
    .reconfirm_same_pump = true, .legacy_ready = true,
    .scale_move_timeout_ms = 600000U, .start_max_g = 500, .drop_trip_g = 200,
    .np_learn_ms = 10000U, .np_abs_floor_g = 10, .scale_res_g = 2,
};
static uint8_t s_manual_relay_channel; /* 0=none, 1=CH1, 2=CH2 */
static uint32_t s_manual_start_ms;     /* when the manual relay was energized */

/* Manual-run safety timeout: a manually started relay must not remain
 * energized indefinitely. Configurable via Kconfig; default 5 minutes. */
#ifndef CONFIG_WEIGHT_DEMO_MANUAL_PUMP_TIMEOUT_MS
#define CONFIG_WEIGHT_DEMO_MANUAL_PUMP_TIMEOUT_MS 300000U
#endif
#define MANUAL_PUMP_TIMEOUT_MS ((uint32_t)CONFIG_WEIGHT_DEMO_MANUAL_PUMP_TIMEOUT_MS)

static portMUX_TYPE s_profile_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- Name helpers ---- */

const char *dual_dispense_state_name(dual_channel_state_t state)
{
    static const char *names[] = {
        "IDLE", "WAITING_FOR_SCALE_MOVE", "WAIT_WEIGHT", "DISPENSING", "SETTLING",
        "COMPLETE", "PAUSED", "FAULT"
    };
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "FAULT";
}

const char *dispense_stage_name(dispense_stage_t stage)
{
    static const char *names[] = {
        "NONE", "COARSE", "FINE", "MICRO", "SETTLING"
    };
    return (unsigned)stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "NONE";
}

const char *scale_owner_name(scale_owner_t owner)
{
    switch (owner) {
    case SCALE_OWNER_CH1: return "CH1";
    case SCALE_OWNER_CH2: return "CH2";
    default:              return "NONE";
    }
}

const char *dual_ready_result_name(dual_ready_result_t r)
{
    switch (r) {
    case READY_OK:                  return "READY_OK";
    case READY_BAD_CHANNEL:         return "READY_BAD_CHANNEL";
    case READY_NOT_WAITING:         return "READY_NOT_WAITING";
    case READY_JOB_MISMATCH:        return "READY_JOB_MISMATCH";
    case READY_SCALE_BOOT_MISMATCH: return "READY_SCALE_BOOT_MISMATCH";
    case READY_STALE:               return "READY_STALE";
    case READY_SCALE_OFFLINE:       return "READY_SCALE_OFFLINE";
    case READY_SCALE_UNSTABLE:      return "READY_SCALE_UNSTABLE";
    case READY_SCALE_BUSY:          return "READY_SCALE_BUSY";
    case READY_BLOCKED:             return "READY_BLOCKED";
    case READY_START_WEIGHT:        return "READY_START_WEIGHT";
    case READY_NO_PROFILE:          return "READY_NO_PROFILE";
    case READY_LEGACY_DISABLED:     return "READY_LEGACY_DISABLED";
    case READY_MALFORMED:           return "READY_MALFORMED";
    default:                        return "READY_UNKNOWN";
    }
}

static scale_owner_t dual_dispense_controller_scale_owner_impl(void)
{ return s_scale_owner; }

static dispense_stage_t dual_dispense_controller_stage_impl(uint8_t channel_id)
{
    if (channel_id < 1U || channel_id > 2U) return DCH_STAGE_NONE;
    return s_ch[channel_id - 1U].stage;
}

/* ---- State machine helpers ---- */

static void state_set(channel_t *c, dual_channel_state_t next, uint32_t now)
{
    if (c->view.state == next) return;
    ESP_LOGI(TAG, "CH%u state %s -> %s | job=%lu | scale_owner=%s",
             c->view.channel_id, dual_dispense_state_name(c->view.state),
             dual_dispense_state_name(next), (unsigned long)c->view.active_job_id,
             scale_owner_name(s_scale_owner));
    c->view.state = next;
    c->state_since_ms = now;
}

static void stage_set(channel_t *c, dispense_stage_t next)
{
    if (c->stage == next) return;
    ESP_LOGI(TAG, "CH%u stage %s -> %s | error=%ld g | weight=%ld g",
             c->view.channel_id, dispense_stage_name(c->stage),
             dispense_stage_name(next), (long)c->view.error_g,
             (long)c->view.current_weight_g);
    c->stage = next;
    c->view.stage = next;
    /* Reset PID state on every stage change: the output range is different
     * and a stale integral from the previous stage would overshoot. */
    c->integral = 0.0f;
    c->have_last_error = false;
}

static void finish(channel_t *c, job_state_t result, const char *error, uint32_t now);

/* Time of the most recent tick/API call, used only to stamp a safety fault. */
static uint32_t s_now_ms;
/* Re-entrancy guard: the failure path itself drives relay OFF and finish(). */
static bool s_write_fail_active;

#ifdef BITS_QEMU_TEST_HOOKS
/* TEST-ONLY relay seam. Compiled only when tests/qemu/CMakeLists.txt defines
 * BITS_QEMU_TEST_HOOKS; the production project never defines it, so neither the
 * hook variables nor the setter exist in the firmware image. */
static esp_err_t (*s_test_relay_set)(uint8_t, bool);
static bool (*s_test_relay_get)(uint8_t);
static bool (*s_test_gate_allowed)(void);
void dual_dispense_controller_test_set_relay_hook(esp_err_t (*set_fn)(uint8_t, bool),
                                                  bool (*get_fn)(uint8_t))
{
    s_test_relay_set = set_fn;
    s_test_relay_get = get_fn;
}
/* M1 seam: what relay_actuation_allowed() answers while the relay fake is used. */
void dual_dispense_controller_test_set_gate_hook(bool (*allowed_fn)(void))
{
    s_test_gate_allowed = allowed_fn;
}
#endif

static esp_err_t relay_hw_set(uint8_t index, bool on)
{
#ifdef BITS_QEMU_TEST_HOOKS
    if (s_test_relay_set) return s_test_relay_set(index, on);
#endif
    return relay_set(index, on);
}

static bool relay_hw_get(uint8_t index)
{
#ifdef BITS_QEMU_TEST_HOOKS
    if (s_test_relay_get) return s_test_relay_get(index);
#endif
    return relay_get_state(index);
}

static bool relay_hw_gate_allows_on(void)
{
#ifdef BITS_QEMU_TEST_HOOKS
    if (s_test_gate_allowed) return s_test_gate_allowed();
#endif
    return relay_actuation_allowed();
}

/* PRE-04: a relay write failed. Retry OFF, finish any active job as FAILED
 * through the normal path, and latch a safety fault so relay_all_off runs and the
 * supervisor goes to emergency stop. The exception is an ON that was only
 * REFUSED by the actuation gate and whose OFF confirms: nothing was ever
 * energized, so the job fails and the channel faults, but there is no hardware
 * fault to latch. M1: the exception is decided by the gate itself
 * (relay_actuation_allowed), NEVER by the error code: ESP_ERR_INVALID_STATE can
 * also be a stuck bus after a write that may have latched, and that must latch.
 * H1: the OFF is also forced onto the bus unconditionally (a failed ON may be
 * latched while the driver thinks the relay is OFF).
 * Confirmation is the driver's commanded-state cache, which follows write
 * results (a failed write is UNKNOWN and reads ON); relay_pad_is_energized() is
 * NOT used because it is cache based. */
static void relay_write_failed(channel_t *c, uint8_t index, bool on, esp_err_t err)
{
    if (s_write_fail_active) {
        c->view.relay_on = relay_hw_get(index);
        return;
    }
    s_write_fail_active = true;
    ESP_LOGE(TAG, "CHANNEL_FAULT channel=CH%u relay write failed: %s",
             c->view.channel_id, esp_err_to_name(err));
    (void)relay_hw_set(index, false);
    (void)relay_force_all_off();
    c->view.relay_on = relay_hw_get(index);
    bool refused_only = on && !relay_hw_gate_allows_on() && !c->view.relay_on;
    if (!refused_only) safety_manager_raise(SAFETY_CONTROL_FAILURE, s_now_ms);
    c->fault = "RELAY_WRITE_FAILED";
    c->view.fault = c->fault;
    if (c->view.active_job_id) finish(c, JOB_FAILED, "RELAY_WRITE_FAILED", s_now_ms);
    else state_set(c, DCH_FAULT, c->state_since_ms);
    s_write_fail_active = false;
}

/* Enforce the single-scale mutual-exclusion invariant:
 * relay1_on && relay2_on must ALWAYS be false. */
static void relay(channel_t *c, bool on)
{
    if (on && (atomic_load(&s_stop_requested) || safety_manager_fault_active())) return;
    if (c->view.relay_on == on) return;

    uint8_t index = (uint8_t)(c->view.channel_id - 1U);

    /* Hard invariant: never allow both relays on simultaneously. */
    if (on) {
        for (uint8_t i = 0; i < CHANNELS; ++i) {
            if (&s_ch[i] != c && s_ch[i].view.relay_on) {
                ESP_LOGE(TAG, "INVARIANT VIOLATION: CH%u would energise while CH%u is on. "
                              "Refusing. relay1_on && relay2_on must be false.",
                         c->view.channel_id, s_ch[i].view.channel_id);
                return;
            }
        }
    }

    esp_err_t err = relay_hw_set(index, on);
    if (on && err == ESP_OK && (atomic_load(&s_stop_requested) || safety_manager_fault_active())) {
        /* Stop requested during the bounded I2C transaction: turn OFF before
         * publishing an ON view. A failed OFF remains visibly energized. */
        err = relay_hw_set(index, false);
        c->view.relay_on = relay_hw_get(index);
        if (err != ESP_OK) relay_write_failed(c, index, false, err);
        return;
    }
    if (err == ESP_OK) {
        c->view.relay_on = on;
        ESP_LOGI(TAG, "CH%u Relay%u -> %s | PCF P%u active-HIGH | scale_owner=%s",
                 c->view.channel_id, c->view.channel_id, on ? "ON" : "OFF",
                 c->view.channel_id == 1U ? 6U : 7U,
                 scale_owner_name(s_scale_owner));
    } else {
        relay_write_failed(c, index, on, err);
    }
}

/* Release scale ownership. Only called when the owner is in a terminal state,
 * its relay is OFF, and a final CAS sample has been captured. */
static void scale_release(channel_t *c, uint32_t now)
{
    (void)now;
    if (s_scale_owner == (scale_owner_t)c->view.channel_id) {
        s_scale_owner = SCALE_OWNER_NONE;
        c->view.owns_scale = false;
        ESP_LOGI(TAG, "CH%u released scale ownership | final=%ld g | job=%lu",
                 c->view.channel_id, (long)c->view.current_weight_g,
                 (unsigned long)c->view.active_job_id);
    }
}

static void finish(channel_t *c, job_state_t result, const char *error, uint32_t now)
{
    relay(c, false);
    /* A failed OFF write already finished this job (relay_write_failed): do not
     * overwrite that outcome. */
    if (c->view.active_job_id == 0U) return;
    /* Capture the final CAS sample before releasing ownership. */
    int32_t final_weight = c->view.current_weight_g;

    if (c->job.id) {
        (void)job_queue_finish(c->job.id, result, final_weight, error, now);
        if (result == JOB_FAILED)
            ESP_LOGE(TAG, "CHANNEL_FAULT channel=CH%u job=%lu reason=%s",
                     c->view.channel_id, (unsigned long)c->job.id,
                     error ? error : "unknown");
        ESP_LOGI(TAG, "CH%u job %lu %s final=%ld g target=%ld g error=%ld g",
                 c->view.channel_id, (unsigned long)c->job.id,
                 job_state_name(result), (long)final_weight,
                 (long)c->view.target_g, (long)c->view.error_g);
    }

    c->view.active_job_id = 0U;
    c->view.output = 0.0f;
    c->integral = 0.0f;
    c->have_last_error = false;
    c->stage = DCH_STAGE_NONE;
    c->view.stage = DCH_STAGE_NONE;
    c->awaiting_operator_ready = false;
    c->view.awaiting_operator_ready = false;
    c->moved_pending = false;
    c->np_active = false;

    if (result == JOB_COMPLETE) state_set(c, DCH_COMPLETE, now);
    else if (result == JOB_CANCELLED) state_set(c, DCH_IDLE, now);
    else {
        c->fault = error;
        c->view.fault = error;
        state_set(c, DCH_FAULT, now);
        s_station_valid = false;   /* any failure voids the scale confirmation */
    }

    /* Release ownership only after the relay is OFF and the final sample is
     * captured (done above). This is the barrier the waiting channel needs.
     * M6: a relay that is still ON or UNKNOWN (failed OFF) keeps the scale; the
     * emergency stop releases it once an OFF write is confirmed. */
    if (c->view.relay_on)
        ESP_LOGE(TAG, "CH%u relay still ON/UNKNOWN - scale ownership NOT released",
                 c->view.channel_id);
    else
        scale_release(c, now);
}

/* ---- Profile management: persistent, exact-match, never consumed ---- */

static void profile_apply_to_view(channel_t *c, const tuned_profile_t *p)
{
    c->view.kp = p->kp;
    c->view.ki = p->ki;
    c->view.kd = p->kd;
    c->view.tolerance_g = p->tolerance_g;
    c->view.max_overshoot_g = p->overshoot_g;
    c->view.max_duration_ms = p->max_duration_ms;
    c->view.window_ms = p->window_ms;
    c->view.min_on_ms = p->min_on_ms;
    c->view.min_off_ms = p->min_off_ms;
    c->view.profile_version = p->version;
    c->view.inflight_comp_g = p->inflight_comp_g;
    c->view.settle_time_ms = p->settle_time_ms;
    (void)snprintf(c->view.profile_id, sizeof(c->view.profile_id), "%s", p->profile_id);
}

static bool profile_find(channel_t *c, int32_t target_g, tuned_profile_t *out)
{
    int slot = profile_slot_for(target_g);
    if (slot < 0) return false;
    portENTER_CRITICAL(&s_profile_lock);
    if (!c->profiles[slot].valid) {
        portEXIT_CRITICAL(&s_profile_lock);
        return false;
    }
    *out = c->profiles[slot];
    portEXIT_CRITICAL(&s_profile_lock);
    return true;
}

/* Select a profile by EXACT target match. No fallback to target_g=0 or any
 * other slot. Returns false when no profile exists for this target. */
static bool profile_select(channel_t *c, int32_t target_g, tuned_profile_t *out)
{
    return profile_find(c, target_g, out);
}

/* Materialise a job's pinned gains as a tuned_profile_t. The pin is already
 * validated at parse/stage time; this is a straight copy. */
static bool profile_from_pin(const job_profile_t *pin, tuned_profile_t *out)
{
    if (!pin || !pin->valid) return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->version = pin->version;
    out->kp = pin->kp; out->ki = pin->ki; out->kd = pin->kd;
    out->tolerance_g = pin->tolerance_g;
    out->overshoot_g = pin->max_overshoot_g;
    out->max_duration_ms = pin->max_duration_ms;
    out->window_ms = pin->window_ms;
    out->min_on_ms = pin->min_on_ms;
    out->min_off_ms = pin->min_off_ms;
    out->coarse_threshold_g = pin->coarse_threshold_g;
    out->fine_threshold_g = pin->fine_threshold_g;
    out->micro_threshold_g = pin->micro_threshold_g;
    out->coarse_min_on_ms = pin->coarse_min_on_ms;
    out->fine_min_on_ms = pin->fine_min_on_ms;
    out->micro_min_on_ms = pin->micro_min_on_ms;
    out->settle_time_ms = pin->settle_time_ms;
    out->inflight_comp_g = pin->inflight_comp_g;
    for (uint8_t i = 0; i < 3U; ++i) {
        out->np_window_ms[i] = pin->np_window_ms[i];
        out->np_min_rise_g[i] = pin->np_min_rise_g[i];
    }
    (void)snprintf(out->profile_id, sizeof(out->profile_id), "%s", pin->profile_id);
    return true;
}

/* Which tuning this job actually runs with.
 *
 * A job that carries a pin always uses it — that is the whole point of the
 * pin. Only a pin-less (legacy) job falls back to the channel's per-target
 * profile slot, which is shared state and therefore cannot represent two
 * queued jobs with the same target_g and different versions. */
static bool profile_for_job(channel_t *c, const dispense_job_t *job,
                            tuned_profile_t *out)
{
    if (job->pin.valid) return profile_from_pin(&job->pin, out);
    return profile_select(c, job->target_g, out);
}

/* ---- Job loading with scale ownership ---- */

/* Defined below with the staged-control block; used here so a job's COARSE/
 * FINE/MICRO/SETTLING params are applied at the same moment its gains are,
 * whether those gains came from a pin or from the target slot. */
static void stage_params_apply(uint8_t channel_id, const tuned_profile_t *p);

/* The scale view is "online" while its newest sample is no older than the weight
 * timeout. Signed delta: a delayed sample acquired before the previous one still
 * counts as fresh. */
static bool scale_view_fresh(uint32_t now)
{
    return s_scale.have && (int32_t)(now - s_scale.rx_ms) <= (int32_t)WEIGHT_MESSAGE_TIMEOUT_MS;
}

/* Hand the scale to this pump's waiting job and start it. Used by an accepted
 * READY and by the automatic same-pump path (reconfirm_same_pump off). c->job and
 * active_job_id are already set. Returns false when the job was failed instead. */
static bool begin_job(channel_t *c, uint32_t now)
{
    const uint8_t ch = c->view.channel_id;

    /* Select the tuning this job must run with: its own pin if it has one,
     * otherwise the exact-target slot. No fallback to another target. */
    tuned_profile_t profile;
    if (!profile_for_job(c, &c->job, &profile)) {
        /* Refuse: do NOT bleed another target's tuning. Fail the job. */
        ESP_LOGE(TAG, "CH%u job %lu target=%ld g REFUSED: no profile for this target. "
                      "Profiles exist only for 5000/10000/15000/20000 g.",
                 ch, (unsigned long)c->job.id, (long)c->job.target_g);
        c->awaiting_operator_ready = false;
        c->view.awaiting_operator_ready = false;
        finish(c, JOB_FAILED, "NO_PROFILE_FOR_TARGET", now);
        return false;
    }
    profile_apply_to_view(c, &profile);
    stage_params_apply(ch, &profile);

    /* Acquire scale ownership and record the confirmed station. */
    s_scale_owner = (scale_owner_t)ch;
    c->view.owns_scale = true;
    c->awaiting_operator_ready = false;
    c->view.awaiting_operator_ready = false;
    s_station_valid = true;
    s_station_ch = ch;
    (void)snprintf(s_station_boot, sizeof(s_station_boot), "%s", s_scale.boot_id);

    c->started_ms = now;
    c->last_pid_ms = now;
    c->window_since_ms = now;
    c->integral = 0.0f;
    c->have_last_error = false;
    c->fault = NULL;
    c->view.fault = NULL;
    c->stage = DCH_STAGE_NONE;
    c->view.stage = DCH_STAGE_NONE;

    /* Require a fresh CAS sample before the first pulse. */
    c->have_cas_seq = false;
    c->have_seq = false;
    c->last_sample_has_seq = false;
    c->moved_pending = false;
    c->np_active = false;
    c->np_learned = false;
    c->np_learn_rise_g = 0;
    c->np_on_ms = 0U;
    c->np_last_ms = now;
    c->have_last_stable = false;

    /* From now on this pump is the scale owner and receives samples. Seed it from
     * the scale view (never from anything newer than what the scale really sent). */
    if (s_scale.have) {
        c->view.current_weight_g = s_scale.weight_g;
        c->view.have_weight = true;
        c->view.stable = s_scale.stable;
        c->last_weight_ms = s_scale.rx_ms;
        c->weight_gen++;
        if (s_scale.stable) {
            c->last_stable_g = s_scale.weight_g;
            c->have_last_stable = true;
        }
    } else {
        c->view.have_weight = false;
        c->view.current_weight_g = 0;
    }
    c->view.start_weight_g = c->view.current_weight_g;   /* recorded for SCALE_MOVED */
    s_now_ms = now;
    (void)job_queue_set_start_g(c->job.id, c->view.current_weight_g);

    ESP_LOGI(TAG, "JOB_STARTED channel=CH%u job=%lu target=%ld g priority=%u "
                  "profile=%s v%lu start=%ld g | scale_owner=%s",
             ch, (unsigned long)c->job.id, (long)c->job.target_g,
             (unsigned)c->job.priority, c->view.profile_id,
             (unsigned long)c->view.profile_version, (long)c->view.start_weight_g,
             scale_owner_name(s_scale_owner));
    ESP_LOGI(TAG, "JOB_ASSIGNED channel=CH%u pump=Pump%u relay=Relay%u scale=Scale%u job=%lu",
             ch, ch, ch, ch, (unsigned long)c->job.id);
    state_set(c, DCH_WAIT_WEIGHT, now);
    return true;
}

/* A same-pump job may follow without a fresh READY only when the operator turned
 * reconfirm_same_pump off AND the scale is still confirmed at this pump on the
 * same sender boot, online, with a plausible start weight. Anything else (the other
 * pump, a fault, a sender reboot, an offline scale) needs a fresh READY. */
static bool autostart_ok(const channel_t *c, uint32_t now)
{
    if (s_cfg.reconfirm_same_pump || !s_station_valid ||
        s_station_ch != c->view.channel_id) return false;
    if (!scale_view_fresh(now) || strcmp(s_scale.boot_id, s_station_boot) != 0) return false;
    if (s_cfg.start_max_g > 0 && s_scale.weight_g > s_cfg.start_max_g) return false;
    return true;
}

static void load_next(channel_t *c, uint32_t now)
{
    if (c->view.active_job_id || c->view.state == DCH_FAULT ||
        c->view.state == DCH_PAUSED || s_estopped) return;

    /* M6: nothing may start while a safety fault is raised or a stop is pending,
     * including a fault raised earlier in this same tick by the other channel
     * (e.g. a failed OFF); the next tick's emergency stop would be too late. */
    if (atomic_load(&s_stop_requested) || safety_manager_fault_active()) return;

    /* Manual pump hold: a job must not grab the nozzle while the operator
     * is running the relay by hand. The job stays QUEUED until Stop. */
    if (s_manual_relay_channel != 0U) return;

    /* Single-scale ownership: if the other channel owns the scale, this
     * channel must wait. The waiting job stays QUEUED in the job queue. */
    if (s_scale_owner != SCALE_OWNER_NONE &&
        s_scale_owner != (scale_owner_t)c->view.channel_id) {
        if (c->view.state != DCH_WAITING_FOR_SCALE_MOVE) {
            state_set(c, DCH_WAITING_FOR_SCALE_MOVE, now);
            ESP_LOGI(TAG, "CH%u WAITING_FOR_SCALE_MOVE | scale owned by %s",
                     c->view.channel_id, scale_owner_name(s_scale_owner));
        }
        return;
    }

    dispense_job_t job;
    if (!job_queue_start_next_available(c->view.channel_id, now, &job)) {
        if (c->view.state != DCH_COMPLETE && c->view.state != DCH_WAITING_FOR_SCALE_MOVE)
            state_set(c, DCH_IDLE, now);
        return;
    }

    /* The job is claimed (RUNNING in the queue) but NO relay may move until the
     * scale is confirmed at THIS pump. */
    c->job = job;
    c->view.active_job_id = job.id;
    c->view.target_g = job.target_g;

    if (autostart_ok(c, now)) {
        (void)begin_job(c, now);
        return;
    }

    c->awaiting_operator_ready = true;
    c->view.awaiting_operator_ready = true;
    c->wait_since_ms = now;
    state_set(c, DCH_WAITING_FOR_SCALE_MOVE, now);
    ESP_LOGI(TAG, "CH%u WAITING_FOR_SCALE_MOVE | job=%lu target=%ld g | "
                  "operator must move the scale to this pump and confirm READY",
             c->view.channel_id, (unsigned long)job.id, (long)job.target_g);
}

/* ---- Staged control ---- */

/* Cached staged thresholds per channel, set when a profile is applied. */
typedef struct {
    int32_t coarse_threshold_g, fine_threshold_g, micro_threshold_g;
    uint32_t coarse_min_on_ms, fine_min_on_ms, micro_min_on_ms;
    uint32_t np_window_ms[3];
    int32_t np_min_rise_g[3];
} stage_params_t;

static stage_params_t s_stage_params[CHANNELS];

static void stage_params_apply(uint8_t channel_id, const tuned_profile_t *p)
{
    uint8_t idx = (uint8_t)(channel_id - 1U);
    for (uint8_t i = 0; i < 3U; ++i) {
        s_stage_params[idx].np_window_ms[i] = p->np_window_ms[i];
        s_stage_params[idx].np_min_rise_g[i] = p->np_min_rise_g[i];
    }
    s_stage_params[idx].coarse_threshold_g = p->coarse_threshold_g;
    s_stage_params[idx].fine_threshold_g = p->fine_threshold_g;
    s_stage_params[idx].micro_threshold_g = p->micro_threshold_g;
    s_stage_params[idx].coarse_min_on_ms = p->coarse_min_on_ms;
    s_stage_params[idx].fine_min_on_ms = p->fine_min_on_ms;
    s_stage_params[idx].micro_min_on_ms = p->micro_min_on_ms;
}

static dispense_stage_t compute_stage(const channel_t *c)
{
    int32_t ae = c->view.error_g < 0 ? -c->view.error_g : c->view.error_g;
    const stage_params_t *sp = &s_stage_params[c->view.channel_id - 1U];

    if (ae <= c->view.tolerance_g) return DCH_STAGE_SETTLING;
    if (ae <= sp->micro_threshold_g) return DCH_STAGE_MICRO;
    if (ae <= sp->fine_threshold_g) return DCH_STAGE_FINE;
    return DCH_STAGE_COARSE;
}

static uint32_t stage_min_on_ms(const channel_t *c)
{
    const stage_params_t *sp = &s_stage_params[c->view.channel_id - 1U];
    switch (c->stage) {
    case DCH_STAGE_COARSE: return sp->coarse_min_on_ms;
    case DCH_STAGE_FINE:   return sp->fine_min_on_ms;
    case DCH_STAGE_MICRO:  return sp->micro_min_on_ms;
    default:               return c->view.min_on_ms;
    }
}

static float stage_duty_clamp(const channel_t *c, float duty)
{
    /* Progressively more conservative output near the target. */
    switch (c->stage) {
    case DCH_STAGE_COARSE:
        if (duty < 0.4f) duty = 0.4f;
        if (duty > DCH_COARSE_DUTY_CEIL) duty = DCH_COARSE_DUTY_CEIL;
        break;
    case DCH_STAGE_FINE:
        if (duty < 0.1f) duty = 0.1f;
        if (duty > DCH_FINE_DUTY_CEIL) duty = DCH_FINE_DUTY_CEIL;
        break;
    case DCH_STAGE_MICRO:
        if (duty < 0.0f) duty = 0.0f;
        if (duty > DCH_MICRO_DUTY_CEIL) duty = DCH_MICRO_DUTY_CEIL;
        break;
    default:
        duty = 0.0f;
        break;
    }
    return duty;
}

/* PRE-05a: record the relay-off edge into SETTLING. Only a sample accepted after
 * this point, acquired strictly later and (when the feed is sequenced) carrying
 * a strictly newer sequence, may let SETTLING decide. */
static void settle_cut(channel_t *c, uint32_t now)
{
    c->cut_gen = c->weight_gen;
    c->cut_ms = now;
    c->cut_have_seq = c->have_seq;
    c->cut_seq = c->last_seq;
}

static bool settle_sample_is_post_cut(const channel_t *c)
{
    if (c->weight_gen == c->cut_gen) return false;           /* nothing new accepted */
    if ((int32_t)(c->last_weight_ms - c->cut_ms) <= 0) return false; /* acquired at/before cut */
    if (c->cut_have_seq) {
        uint32_t d = c->last_seq - c->cut_seq;
        if (!c->last_sample_has_seq || d == 0U || d >= 0x80000000U) return false;
    }
    return true;
}

/* ---- Single-scale watchdogs (CONTRACT 9.11) ----------------------------------
 * Both end the same way: relay OFF in this very call (finish), the job FAILED with
 * the watchdog's own reason, and a LATCHED safety fault. The fault forces every relay
 * OFF, sends the supervisor into the emergency stop on its next tick, and only an
 * explicit operator CLEAR removes it. There is no automatic retry. */
static void trip_latched(channel_t *c, const char *reason, safety_fault_t fault, uint32_t now)
{
    ESP_LOGE(TAG, "CH%u WATCHDOG %s | job=%lu weight=%ld g target=%ld g | relay OFF, fault latched",
             c->view.channel_id, reason, (unsigned long)c->view.active_job_id,
             (long)c->view.current_weight_g, (long)c->view.target_g);
    finish(c, JOB_FAILED, reason, now);
    safety_manager_raise(fault, now);
}

/* SCALE_MOVED: the owner's job lost its scale. Called every tick before the safety
 * fault check, so a scale that vanished is named SCALE_MOVED even when a transient
 * stale/link fault was raised in the same instant. Sample-time findings (a drop
 * below the previous stable weight or the start weight, a new boot_id, a changed
 * scale_id) arrive as moved_pending; "offline" is a stale reading while the relay
 * can be ON or the job is SETTLING. */
static void scale_watch(channel_t *c, uint32_t now)
{
    if (c->view.active_job_id == 0U || !c->view.owns_scale) return;
    dual_channel_state_t st = c->view.state;
    if (st != DCH_WAIT_WEIGHT && st != DCH_DISPENSING && st != DCH_SETTLING) return;
    if (c->moved_pending) {
        trip_latched(c, "SCALE_MOVED", SAFETY_SCALE_MOVED, now);
        return;
    }
    if ((st == DCH_DISPENSING || st == DCH_SETTLING) && c->view.have_weight &&
        (now - c->last_weight_ms) > WEIGHT_MESSAGE_TIMEOUT_MS &&
        (int32_t)(now - c->last_weight_ms) > 0) {
        trip_latched(c, "SCALE_MOVED", SAFETY_SCALE_MOVED, now);
    }
}

/* NO_PROGRESS, independent of CHANNEL_TIMEOUT. The window is counted in relay-ON
 * time only (a micro pulse of a few ms still counts, an OFF gap does not), so a
 * pulsed stage is judged by what its pulses delivered. No flow rate is assumed:
 *   - a stage whose profile carries np_min_rise_g uses it (and np_window_ms);
 *   - otherwise the first np_learn_ms of relay-ON time measures the rise; if that
 *     rise is below np_abs_floor_g the job fails at once; afterwards each window
 *     needs 25 % of the observed rate, never less than NP_MIN_STEPS resolution steps. */
static int np_stage_index(dispense_stage_t s)
{
    switch (s) {
    case DCH_STAGE_COARSE: return 0;
    case DCH_STAGE_FINE:   return 1;
    case DCH_STAGE_MICRO:  return 2;
    default:               return -1;
    }
}

static void np_account(channel_t *c, uint32_t now)
{
    uint32_t dt = now - c->np_last_ms;
    c->np_last_ms = now;
    /* The relay state held now is what ran since the previous tick. */
    if (c->np_active && c->view.relay_on && (int32_t)dt > 0)
        c->np_on_ms += dt > NP_DT_CAP_MS ? NP_DT_CAP_MS : dt;
}

/* Returns true when the job was failed (relay already OFF). */
static bool np_check(channel_t *c, uint32_t now)
{
    int si = np_stage_index(c->stage);
    if (si < 0) return false;
    if (!c->np_active || c->np_stage != si) {   /* a stage entry starts a fresh window */
        c->np_active = true;
        c->np_stage = si;
        c->np_on_ms = 0U;
        c->np_base_g = c->view.current_weight_g;
        return false;
    }
    const stage_params_t *sp = &s_stage_params[c->view.channel_id - 1U];
    uint32_t window = sp->np_window_ms[si] > 0U ? sp->np_window_ms[si] : s_cfg.np_learn_ms;
    const int32_t profile_min = sp->np_min_rise_g[si];
    if (profile_min <= 0 && !c->np_learned) window = s_cfg.np_learn_ms;   /* the learn window */
    if (window == 0U || c->np_on_ms < window) return false;

    const int32_t rise = c->view.current_weight_g - c->np_base_g;
    int32_t need;
    if (profile_min > 0) {
        need = profile_min;
    } else if (!c->np_learned) {
        need = s_cfg.np_abs_floor_g;
    } else {
        int64_t q = (int64_t)c->np_learn_rise_g * (int64_t)window /
                    (int64_t)(s_cfg.np_learn_ms ? s_cfg.np_learn_ms : 1U) / 4;
        int32_t floor_g = NP_MIN_STEPS * (s_cfg.scale_res_g > 0 ? s_cfg.scale_res_g : 1);
        need = q > (int64_t)floor_g ? (int32_t)q : floor_g;
    }
    if (rise < need) {
        ESP_LOGE(TAG, "CH%u NO_PROGRESS stage=%s rise=%ld g in %lu ms of relay-ON time, need %ld g",
                 c->view.channel_id, dispense_stage_name(c->stage), (long)rise,
                 (unsigned long)c->np_on_ms, (long)need);
        trip_latched(c, "NO_PROGRESS", SAFETY_NO_PROGRESS, now);
        return true;
    }
    if (profile_min <= 0 && !c->np_learned) {
        c->np_learned = true;
        c->np_learn_rise_g = rise > 0 ? rise : 0;
        ESP_LOGI(TAG, "CH%u NO_PROGRESS learned %ld g per %lu ms of relay-ON time (UNVALIDATED bound)",
                 c->view.channel_id, (long)c->np_learn_rise_g, (unsigned long)window);
    }
    c->np_on_ms = 0U;
    c->np_base_g = c->view.current_weight_g;
    return false;
}

/* The job waited longer than scale_move_timeout for the scale to be moved. No relay
 * was ever turned on. A safe un-start returns it to the queue (parked, so the
 * automatic line does not re-prompt at once); failing that it is cancelled. */
static void wait_timeout(channel_t *c, uint32_t now)
{
    const uint32_t id = c->view.active_job_id;
    relay(c, false);
    if (c->view.active_job_id == 0U) return;   /* a failed OFF already finished it */
    ESP_LOGW(TAG, "CH%u job %lu: scale not moved within %lu ms - no relay was started",
             c->view.channel_id, (unsigned long)id, (unsigned long)s_cfg.scale_move_timeout_ms);
    c->awaiting_operator_ready = false;
    c->view.awaiting_operator_ready = false;
    if (job_queue_unstart(id, true)) {
        c->view.active_job_id = 0U;
        memset(&c->job, 0, sizeof(c->job));
        state_set(c, DCH_IDLE, now);
        return;
    }
    finish(c, JOB_CANCELLED, "SCALE_MOVE_TIMEOUT", now);
}

static void control_channel(channel_t *c, uint32_t now)
{
    c->view.queue_depth = job_queue_depth();
    c->view.weight_age_ms = c->view.have_weight ? now - c->last_weight_ms : UINT32_MAX;
    c->view.scale_owner = s_scale_owner;
    c->view.owns_scale = s_scale_owner == (scale_owner_t)c->view.channel_id;

    if (c->view.active_job_id == 0U) load_next(c, now);
    if (c->view.active_job_id == 0U || c->view.state == DCH_PAUSED ||
        c->view.state == DCH_FAULT) return;

    /* WAITING_FOR_SCALE_MOVE: hold. Do not energise. */
    if (c->view.state == DCH_WAITING_FOR_SCALE_MOVE) {
        relay(c, false);
        if (c->awaiting_operator_ready) {
            if ((now - c->wait_since_ms) > s_cfg.scale_move_timeout_ms) wait_timeout(c, now);
            return;
        }
        /* A job in WAITING_FOR_SCALE_MOVE never takes the scale without a fresh,
         * accepted READY (which also runs profile/reset init). Re-arm the gate
         * instead of handing off with uninitialised state. */
        c->awaiting_operator_ready = true;
        c->view.awaiting_operator_ready = true;
        c->wait_since_ms = now;
        return;
    }

    /* Single-scale safety: refuse to dispense without ownership. */
    if (!c->view.owns_scale) {
        /* Never sit in a non-gated state without the scale (READY would be
         * refused and the timeout below unreachable): re-arm the READY gate. */
        relay(c, false);
        if (c->view.state == DCH_FAULT) return; /* L3: a failed OFF faulted it */
        c->awaiting_operator_ready = true;
        c->view.awaiting_operator_ready = true;
        c->wait_since_ms = now;
        state_set(c, DCH_WAITING_FOR_SCALE_MOVE, now);
        return;
    }

    np_account(c, now);

    if (!c->view.have_weight) {
        relay(c, false);
        if (now - c->started_ms > WEIGHT_MESSAGE_TIMEOUT_MS)
            finish(c, JOB_FAILED, "CHANNEL_WEIGHT_MISSING", now);
        return;
    }
    if (c->view.weight_age_ms > WEIGHT_MESSAGE_TIMEOUT_MS) {
        finish(c, JOB_FAILED, "CHANNEL_WEIGHT_STALE", now);
        return;
    }
    /* PRE-03 (BLK-11): overweight cut-off, before any state logic and in every
     * state of an owned job (WAIT_WEIGHT, DISPENSING at any stage, SETTLING).
     * Weight is valid and fresh here. finish() drives the relay OFF in this same
     * tick and fails the job; the COMPLETE band elsewhere is unchanged. */
    if ((int64_t)c->view.current_weight_g >
        (int64_t)c->view.target_g + (int64_t)c->view.max_overshoot_g) {
        finish(c, JOB_FAILED, "OVERWEIGHT", now);
        return;
    }
    if ((now - c->started_ms) > c->view.max_duration_ms) {
        finish(c, JOB_FAILED, "CHANNEL_TIMEOUT", now);
        return;
    }

    c->view.error_g = c->view.target_g - c->view.current_weight_g;

    if (c->view.state == DCH_WAIT_WEIGHT) {
        state_set(c, DCH_DISPENSING, now);
        stage_set(c, compute_stage(c));
    }

    /* ---- SETTLING ---- */
    if (c->view.state == DCH_SETTLING) {
        relay(c, false);
        if (c->view.state == DCH_FAULT) return; /* OFF write failed: job finished */
        /* Require BOTH: the settle time has elapsed AND a fresh CAS sample
         * arrived after the relay closed (PRE-05a: acquired strictly after the
         * cut edge, not merely younger than the stale limit). Without it we
         * would be judging on stale data. */
        bool settle_elapsed = (now - c->settled_since_ms) >= c->view.settle_time_ms;
        bool fresh_sample = c->view.weight_age_ms < WEIGHT_MESSAGE_TIMEOUT_MS &&
                            settle_sample_is_post_cut(c);
        if (!settle_elapsed || !fresh_sample) return;

        int32_t overshoot = c->view.current_weight_g - c->view.target_g;
        if (overshoot > c->view.max_overshoot_g) {
            finish(c, JOB_FAILED, "OVERWEIGHT", now);
        } else if (c->view.error_g <= c->view.tolerance_g) {
            finish(c, JOB_COMPLETE, NULL, now);
        } else {
            /* Under target: re-enter dispensing at the appropriate stage. */
            state_set(c, DCH_DISPENSING, now);
            stage_set(c, compute_stage(c));
        }
        return;
    }

    /* ---- DISPENSING (staged) ---- */
    if (c->view.state == DCH_DISPENSING) {
        /* In-flight compensation: account for material still travelling to
         * the scale after the relay closes. Stop early by inflight_comp_g. */
        int32_t effective_error = c->view.error_g - c->view.inflight_comp_g;

        if (effective_error <= 0) {
            /* Close enough (accounting for in-flight material). */
            relay(c, false);
            if (c->view.state == DCH_FAULT) return; /* OFF write failed: job finished */
            c->settled_since_ms = now;
            settle_cut(c, now);
            stage_set(c, DCH_STAGE_SETTLING);
            state_set(c, DCH_SETTLING, now);
            return;
        }

        dispense_stage_t want = compute_stage(c);
        if (want != c->stage) stage_set(c, want);

        /* NO_PROGRESS watchdog (own window, relay-ON time): fails the job and
         * latches before another pulse can be issued. */
        if (np_check(c, now)) return;

        /* Require a fresh CAS sample before the first pulse of each stage. */
        if (!c->have_cas_seq && c->view.weight_age_ms > 2000U) {
            relay(c, false);
            return;
        }
        c->have_cas_seq = true;

        /* PID computation (same positional PID as before, with anti-windup). */
        uint32_t dt = now - c->last_pid_ms;
        if (!dt) dt = 1U;
        if (dt > 1000U) dt = 1000U;
        c->last_pid_ms = now;

        float e = (float)effective_error;
        float dt_s = (float)dt / 1000.0f;
        float d = c->have_last_error ? (e - c->last_error) / dt_s : 0.0f;
        float candidate_integral = c->integral + e * dt_s;
        if (candidate_integral > 200.0f) candidate_integral = 200.0f;
        if (candidate_integral < -200.0f) candidate_integral = -200.0f;

        float p_term = c->view.kp * e;
        float i_term = c->view.ki * candidate_integral;
        float d_term = c->view.kd * d;
        float unsaturated = p_term + i_term + d_term;

        /* Conditional integration anti-windup. */
        if ((unsaturated >= 0.0f && unsaturated <= 1.0f) ||
            (unsaturated > 1.0f && e < 0.0f) ||
            (unsaturated < 0.0f && e > 0.0f))
            c->integral = candidate_integral;
        i_term = c->view.ki * c->integral;

        float output = p_term + i_term + d_term;
        if (output < 0.0f) output = 0.0f;
        if (output > 1.0f) output = 1.0f;

        /* Stage-based output clamp: progressively more conservative. */
        output = stage_duty_clamp(c, output);

        c->view.p_term = p_term;
        c->view.i_term = i_term;
        c->view.d_term = d_term;
        c->view.output = output;
        c->last_error = e;
        c->have_last_error = true;

        if (now - c->window_since_ms >= c->view.window_ms)
            c->window_since_ms = now;

        uint32_t phase = now - c->window_since_ms;
        uint32_t min_on = stage_min_on_ms(c);
        bool on = dispense_window_output(phase, output, c->view.window_ms,
                                         min_on, c->view.min_off_ms);
        relay(c, on);

        ESP_LOGD(TAG, "CH%u stage=%s weight=%ld target=%ld error=%ld "
                      "P=%.4f I=%.4f D=%.4f out=%.4f min_on=%u",
                 c->view.channel_id, dispense_stage_name(c->stage),
                 (long)c->view.current_weight_g, (long)c->view.target_g,
                 (long)c->view.error_g, (double)p_term, (double)i_term,
                 (double)d_term, (double)output, (unsigned)min_on);
    }
}

/* ---- Public lifecycle ---- */

static void install_default_profile(channel_t *c, int32_t target_g)
{
    int slot = profile_slot_for(target_g);
    if (slot < 0) return;
    tuned_profile_t *p = &c->profiles[slot];
    portENTER_CRITICAL(&s_profile_lock);
    p->valid = true;
    p->version = 1U;
    p->target_g = target_g;
    p->kp = (float)CONFIG_WEIGHT_DEMO_PID_KP_X10000 / 10000.0f;
    p->ki = (float)CONFIG_WEIGHT_DEMO_PID_KI_X10000 / 10000.0f;
    p->kd = (float)CONFIG_WEIGHT_DEMO_PID_KD_X10000 / 10000.0f;
    p->tolerance_g = CONFIG_WEIGHT_DEMO_TOLERANCE_G;
    p->overshoot_g = CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G;
    p->max_duration_ms = CONFIG_WEIGHT_DEMO_MAX_DURATION_MS;
    p->window_ms = CONFIG_WEIGHT_DEMO_WINDOW_MS;
    p->min_on_ms = CONFIG_WEIGHT_DEMO_MIN_ON_MS;
    p->min_off_ms = CONFIG_WEIGHT_DEMO_MIN_OFF_MS;
    p->coarse_threshold_g = CONFIG_WEIGHT_DEMO_COARSE_THRESHOLD_G;
    p->fine_threshold_g = CONFIG_WEIGHT_DEMO_FINE_THRESHOLD_G;
    p->micro_threshold_g = CONFIG_WEIGHT_DEMO_MICRO_THRESHOLD_G;
    p->coarse_min_on_ms = CONFIG_WEIGHT_DEMO_COARSE_MIN_ON_MS;
    p->fine_min_on_ms = CONFIG_WEIGHT_DEMO_FINE_MIN_ON_MS;
    p->micro_min_on_ms = CONFIG_WEIGHT_DEMO_MICRO_MIN_ON_MS;
    p->settle_time_ms = CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS;
    p->inflight_comp_g = CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G;
    (void)snprintf(p->profile_id, sizeof(p->profile_id), "device-default-%ldg",
                   (long)target_g);
    portEXIT_CRITICAL(&s_profile_lock);
}

static esp_err_t dual_dispense_controller_init_impl(void)
{
    s_estopped = false;
    atomic_store(&s_stop_requested, false);
    s_manual_relay_channel = 0;
    s_manual_start_ms = 0;
    memset(s_ch, 0, sizeof(s_ch));
    s_scale_owner = SCALE_OWNER_NONE;
    memset(&s_scale, 0, sizeof(s_scale));
    memset(s_stage_params, 0, sizeof(s_stage_params));
    s_station_valid = false;
    s_station_ch = 0U;
    s_station_boot[0] = '\0';

    for (uint8_t i = 0; i < CHANNELS; ++i) {
        channel_t *c = &s_ch[i];
        dual_channel_snapshot_t *v = &c->view;
        v->channel_id = (uint8_t)(i + 1U);
        v->state = DCH_IDLE;
        v->stage = DCH_STAGE_NONE;
        v->scale_owner = SCALE_OWNER_NONE;
        v->owns_scale = false;
        v->profile_version = 1U;
        (void)snprintf(v->profile_id, sizeof(v->profile_id), "device-default");
        v->kp = (float)CONFIG_WEIGHT_DEMO_PID_KP_X10000 / 10000.0f;
        v->ki = (float)CONFIG_WEIGHT_DEMO_PID_KI_X10000 / 10000.0f;
        v->kd = (float)CONFIG_WEIGHT_DEMO_PID_KD_X10000 / 10000.0f;
        v->tolerance_g = CONFIG_WEIGHT_DEMO_TOLERANCE_G;
        v->max_overshoot_g = CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G;
        v->window_ms = CONFIG_WEIGHT_DEMO_WINDOW_MS;
        v->min_on_ms = CONFIG_WEIGHT_DEMO_MIN_ON_MS;
        v->min_off_ms = CONFIG_WEIGHT_DEMO_MIN_OFF_MS;
        v->max_duration_ms = CONFIG_WEIGHT_DEMO_MAX_DURATION_MS;
        v->inflight_comp_g = CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G;
        v->settle_time_ms = CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS;

        /* Install default profiles for all four canonical targets. These are
         * persistent — they survive job completion and are never consumed. */
        for (uint8_t t = 0; t < PROFILE_SLOT_COUNT; ++t)
            install_default_profile(c, k_canonical_targets[t]);

        relay(c, false);
    }
    return ESP_OK;
}

/* Feed is ONE physical CAS scale (CONTRACT 9.11). The sample updates the shared
 * scale view and ONLY THEN, if a job owns the scale, that owner's pump. It uses
 * weight_g and stable; the sender's UART tag and any per-channel field of a legacy
 * frame are ignored, so a sample is never attributed to a pump by its tag. With no
 * owner the pumps are never renewed: they stay stale and fail safe.
 * Simulated samples are rejected outright. */
static void dual_dispense_controller_on_weight_impl(const weight_msg_t *msg, uint32_t now)
{
    if (!msg || msg->simulated) return;

    scale_view_t *v = &s_scale;
    const char *sid = msg->has_scale_id ? msg->scale_id : "";
    const char *bid = msg->has_boot_id ? msg->boot_id : "";
    const bool ident_changed = v->have &&
        (strcmp(v->scale_id, sid) != 0 || strcmp(v->boot_id, bid) != 0);
    const bool prev_fresh = v->have && (int32_t)(now - v->rx_ms) <= (int32_t)WEIGHT_MESSAGE_TIMEOUT_MS;

    /* PRE-05a, per scale: a duplicate or reordered sequence is not a new sample. It
     * must not renew freshness or overwrite the newer weight. The guard restarts on
     * a new boot_id, and after the scale went silent (a restarted sender renumbers). */
    if (msg->has_sequence && v->have_seq && prev_fresh && !ident_changed) {
        uint32_t d = msg->sequence - v->last_seq;
        if (d == 0U || d >= 0x80000000U) return;
    }
    if (msg->has_sequence) {
        v->last_seq = msg->sequence;
        v->have_seq = true;
    } else if (ident_changed || !prev_fresh) {
        v->have_seq = false;
    }

    /* The station confirmation belongs to one sender boot on one scale. */
    if (ident_changed && s_station_valid) {
        ESP_LOGW(TAG, "scale identity changed (%s/%s -> %s/%s): station confirmation void",
                 v->scale_id, v->boot_id, sid, bid);
        s_station_valid = false;
    }

    /* Stable run: consecutive stable samples and when the run began. */
    if (msg->stable && !ident_changed) {
        if (v->stable_n == 0U) v->stable_since_ms = now;
        if (v->stable_n < UINT32_MAX) v->stable_n++;
    } else {
        v->stable_n = msg->stable ? 1U : 0U;
        v->stable_since_ms = now;
    }

    const int32_t w = msg->weight_g;
    (void)snprintf(v->scale_id, sizeof(v->scale_id), "%s", sid);
    (void)snprintf(v->boot_id, sizeof(v->boot_id), "%s", bid);
    v->have = true;
    v->weight_g = w;
    v->stable = msg->stable;
    v->rx_ms = now;

    if (s_scale_owner == SCALE_OWNER_NONE) return;   /* view only: no pump is renewed */

    channel_t *c = &s_ch[s_scale_owner - SCALE_OWNER_CH1];
    if (c->view.active_job_id != 0U) {
        const dual_channel_state_t st = c->view.state;
        if (st == DCH_WAIT_WEIGHT || st == DCH_DISPENSING || st == DCH_SETTLING) {
            /* SCALE_MOVED findings are taken at sample time (a flag, acted on by the
             * next watch) so no sample between two ticks can be missed. */
            if (ident_changed) c->moved_pending = true;
            if (c->have_last_stable && (int64_t)w < (int64_t)c->last_stable_g - s_cfg.drop_trip_g)
                c->moved_pending = true;
            if (c->view.start_weight_g > 0 &&
                (int64_t)w < (int64_t)c->view.start_weight_g - s_cfg.drop_trip_g)
                c->moved_pending = true;
        }
    }
    if (msg->has_sequence) {
        c->last_seq = msg->sequence;
        c->have_seq = true;
    }
    c->last_sample_has_seq = msg->has_sequence;
    c->weight_gen++;
    c->view.current_weight_g = w;
    c->view.have_weight = true;
    c->view.stable = msg->stable;
    c->last_weight_ms = now;
    if (msg->stable && !c->moved_pending) {
        c->last_stable_g = w;
        c->have_last_stable = true;
    }
}

/* Manual relay (pump) control — no dispense job required. Routes through the
 * mutual-exclusion-guarded relay() helper so relay1_on && relay2_on stays false.
 * ON is refused when a job is active on either channel, when E-Stop is latched,
 * or when a safety fault is active. Stop is always allowed. */
static esp_err_t dual_dispense_controller_manual_relay_impl(uint8_t channel_id, bool on, uint32_t now)
{
    s_now_ms = now;
    if (channel_id < 1U || channel_id > 2U) return ESP_ERR_INVALID_ARG;

    if (on) {
        if (s_estopped) return ESP_ERR_INVALID_STATE;
        for (uint8_t i = 0; i < CHANNELS; ++i) {
            if (s_ch[i].view.active_job_id) return ESP_ERR_INVALID_STATE;
            if (s_ch[i].view.state != DCH_IDLE && s_ch[i].view.state != DCH_FAULT &&
                s_ch[i].view.state != DCH_COMPLETE) {
                return ESP_ERR_INVALID_STATE;
            }
        }
        if (safety_manager_fault_active()) return ESP_ERR_INVALID_STATE;
        if (s_manual_relay_channel != 0U) return ESP_ERR_INVALID_STATE;
    }

    channel_t *c = &s_ch[channel_id - 1U];
    if (on) {
        relay(c, true);
        if (!c->view.relay_on) return ESP_ERR_INVALID_STATE;
        s_manual_relay_channel = channel_id;
        s_manual_start_ms = now;
        weight_receiver_set_manual_channel(channel_id);
    } else {
        relay(c, false);
        if (s_manual_relay_channel == channel_id) {
            s_manual_relay_channel = 0;
            s_manual_start_ms = 0;
            weight_receiver_set_manual_channel(0);
        }
    }
    return ESP_OK;
}

/* The confirmation that the scale sits at a pump lapses with a fault, an E-Stop, a
 * sender reboot (new boot_id) or the scale going offline. */
static void station_watch(uint32_t now)
{
    if (!s_station_valid) return;
    if (s_estopped || safety_manager_fault_active() || !scale_view_fresh(now) ||
        strcmp(s_scale.boot_id, s_station_boot) != 0) {
        ESP_LOGW(TAG, "scale station confirmation (CH%u) void: fault/E-Stop, scale offline or new boot_id",
                 (unsigned)s_station_ch);
        s_station_valid = false;
    }
}

static void dual_dispense_controller_tick_impl(uint32_t now)
{
    s_now_ms = now;
    safety_manager_note_control_tick(now);

    /* A job that lost its scale is named SCALE_MOVED (latched) BEFORE the generic
     * fault gate below can turn it into a plain emergency stop. */
    if (s_scale_owner != SCALE_OWNER_NONE && !s_estopped)
        scale_watch(&s_ch[s_scale_owner - SCALE_OWNER_CH1], now);
    station_watch(now);

    if (safety_manager_fault_active()) {
        dual_dispense_controller_emergency_stop(now);
        return;
    }

    /* Enforce the mutual-exclusion invariant every tick. */
    if (s_ch[0].view.relay_on && s_ch[1].view.relay_on) {
        ESP_LOGE(TAG, "INVARIANT VIOLATION: both relays ON. Emergency stop.");
        dual_dispense_controller_emergency_stop(now);
        return;
    }

    /* Manual-run safety timeout: a manually started relay must not remain
     * energized indefinitely. This fires even when communication is lost,
     * because the supervisor task runs locally regardless of network state. */
    if (s_manual_relay_channel != 0U) {
        uint32_t elapsed = now - s_manual_start_ms;
        if (elapsed >= MANUAL_PUMP_TIMEOUT_MS) {
            uint8_t ch = s_manual_relay_channel;
            ESP_LOGW(TAG, "MANUAL PUMP TIMEOUT: CH%u relay OFF after %lu ms (limit %lu ms)",
                     (unsigned)ch, (unsigned long)elapsed, (unsigned long)MANUAL_PUMP_TIMEOUT_MS);
            channel_t *c = &s_ch[ch - 1U];
            relay(c, false);
            s_manual_relay_channel = 0;
            s_manual_start_ms = 0;
            weight_receiver_set_manual_channel(0);
        }
    }

    /* Run the scale-owner channel first, then the other. This guarantees the
     * owner's control decisions are made before the waiting channel tries to
     * claim the scale. */
    if (s_scale_owner != SCALE_OWNER_NONE) {
        control_channel(&s_ch[s_scale_owner - SCALE_OWNER_CH1], now);
        control_channel(&s_ch[s_scale_owner == SCALE_OWNER_CH1 ? 1 : 0], now);
    } else {
        control_channel(&s_ch[0], now);
        control_channel(&s_ch[1], now);
    }
}

/* ---- Operator controls ---- */

/* READY (CONTRACT 9.11). Every precondition is checked first; a refused READY
 * changes nothing. `legacy` (only {channel}) skips the identity, age and stability
 * checks and is the caller's responsibility to allow (legacy_ready). */
static dual_ready_result_t scale_ready_core(uint8_t channel_id, const dual_ready_req_t *req,
                                            bool legacy, uint32_t now)
{
    if (channel_id < 1U || channel_id > 2U) return READY_BAD_CHANNEL;
    channel_t *c = &s_ch[channel_id - 1U];
    if (c->view.state != DCH_WAITING_FOR_SCALE_MOVE || !c->awaiting_operator_ready ||
        c->view.active_job_id == 0U) return READY_NOT_WAITING;
    if (s_estopped || c->fault || s_manual_relay_channel != 0U ||
        atomic_load(&s_stop_requested) || safety_manager_fault_active()) return READY_BLOCKED;
    if (!legacy) {
        if (req == NULL || !req->has_job_id || !req->has_boot) return READY_MALFORMED;
        if (req->age_ms > s_cfg.ready_max_age_ms) return READY_STALE;
        if (req->job_id != c->view.active_job_id) return READY_JOB_MISMATCH;
    }
    if (s_scale_owner != SCALE_OWNER_NONE) return READY_SCALE_BUSY;
    if (!legacy) {
        if (!scale_view_fresh(now)) return READY_SCALE_OFFLINE;
        if (s_scale.boot_id[0] == '\0' || strcmp(req->scale_boot_id, s_scale.boot_id) != 0)
            return READY_SCALE_BOOT_MISMATCH;
        if (!s_scale.stable || s_scale.stable_n < READY_MIN_STABLE_SAMPLES ||
            (uint32_t)(s_scale.rx_ms - s_scale.stable_since_ms) < s_cfg.ready_stable_ms)
            return READY_SCALE_UNSTABLE;
    }
    /* Plausibility of the start weight (only judged on a fresh reading). */
    if (s_cfg.start_max_g > 0 && scale_view_fresh(now) && s_scale.weight_g > s_cfg.start_max_g) {
        ESP_LOGW(TAG, "CH%u READY refused: start weight %ld g > start_max_g %ld g (UNVALIDATED limit)",
                 channel_id, (long)s_scale.weight_g, (long)s_cfg.start_max_g);
        return READY_START_WEIGHT;
    }

    ESP_LOGI(TAG, "CH%u scale READY%s | scale confirmed positioned at this pump | "
                  "job=%lu target=%ld g boot=%s",
             channel_id, legacy ? " (LEGACY, no job_id/scale_boot_id)" : "",
             (unsigned long)c->view.active_job_id, (long)c->view.target_g, s_scale.boot_id);
    s_now_ms = now;
    return begin_job(c, now) ? READY_OK : READY_NO_PROFILE;
}

static dual_ready_result_t dual_dispense_controller_scale_ready_impl(uint8_t channel_id,
    const dual_ready_req_t *req, uint32_t now)
{
    return scale_ready_core(channel_id, req, false, now);
}

static dual_ready_result_t dual_dispense_controller_legacy_ready_impl(uint8_t channel_id,
                                                                      uint32_t now)
{
    if (!s_cfg.legacy_ready) {
        ESP_LOGW(TAG, "legacy READY (channel only) refused: legacy_ready is off");
        return READY_LEGACY_DISABLED;
    }
    return scale_ready_core(channel_id, NULL, true, now);
}

static bool dual_dispense_controller_operator_ready_impl(uint8_t channel_id, uint32_t now)
{
    return dual_dispense_controller_legacy_ready_impl(channel_id, now) == READY_OK;
}

static bool dual_dispense_controller_pause_impl(uint8_t channel_id, uint32_t now)
{
    if (channel_id < 1U || channel_id > 2U) return false;
    channel_t *c = &s_ch[channel_id - 1U];
    if (!c->view.active_job_id) return false;
    relay(c, false);
    /* L3: a failed OFF faults the channel; never overwrite FAULT with PAUSED. */
    if (c->view.state == DCH_FAULT) return false;
    state_set(c, DCH_PAUSED, now);
    return true;
}

static bool dual_dispense_controller_resume_impl(uint8_t channel_id, uint32_t now)
{
    if (channel_id < 1U || channel_id > 2U) return false;
    channel_t *c = &s_ch[channel_id - 1U];
    if (c->view.state != DCH_PAUSED) return false;
    /* A job that does not own the scale (or is still awaiting operator READY)
     * must never reach WAIT_WEIGHT: return to the READY gate instead. */
    if (c->awaiting_operator_ready ||
        s_scale_owner != (scale_owner_t)channel_id) {
        relay(c, false);
        if (c->view.state == DCH_FAULT) return false; /* L3: keep FAULT */
        c->awaiting_operator_ready = true;
        c->view.awaiting_operator_ready = true;
        c->wait_since_ms = now;
        state_set(c, DCH_WAITING_FOR_SCALE_MOVE, now);
        return true;
    }
    c->last_pid_ms = now;
    c->window_since_ms = now;
    state_set(c, DCH_WAIT_WEIGHT, now);
    return true;
}

/* Stop the channel's ACTIVE job and nothing else.
 *
 * This is the "Cancel active" control. It must never walk the waiting line-up:
 * cancelling one job while silently dropping its siblings is the bug this
 * replaced. job_queue_cancel_channel_queued() used to run here and wiped every
 * QUEUED job on the channel — including concurrent and duplicate target_g
 * values, which are independent rows. Backlog removal is addressed by job id
 * alone (dual_dispense_controller_cancel_job / job_queue_cancel). */
static bool dual_dispense_controller_cancel_impl(uint8_t channel_id, uint32_t now)
{
    if (channel_id < 1U || channel_id > 2U) return false;
    channel_t *c = &s_ch[channel_id - 1U];
    if (c->view.active_job_id == 0U) return false;
    finish(c, JOB_CANCELLED, "cancelled", now);
    return true;
}

/* Cancel exactly one job, whatever state it is in.
 *
 * Strictly scoped to the given id: every other queued job — including ones
 * sharing its target_g — is left untouched. Covers both sides of the
 * QUEUED->RUNNING race between an operator click and the 1 s command poll: if
 * the job is still waiting, job_queue_cancel() takes it; if it has already
 * been claimed, the owning channel finishes it. */
static bool dual_dispense_controller_cancel_job_impl(uint32_t job_id, uint32_t now)
{
    if (job_id == 0U) return false;
    if (job_queue_cancel(job_id)) return true;
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        if (s_ch[i].view.active_job_id != job_id) continue;
        finish(&s_ch[i], JOB_CANCELLED, "cancelled", now);
        return true;
    }
    return false;
}

/* The channel fault text of a stop that came from a latched safety fault: the
 * watchdogs keep their own name so the operator sees what actually happened. */
static const char *stop_fault_text(void)
{
    safety_fault_t f = safety_manager_fault();
    if (f == SAFETY_NO_PROGRESS) return "NO_PROGRESS";
    if (f == SAFETY_SCALE_MOVED) return "SCALE_MOVED";
    return "EMERGENCY_STOP";
}

static void dual_dispense_controller_emergency_stop_impl(uint32_t now)
{
    if (!s_estopped) {
        ESP_LOGE(TAG, "GLOBAL EMERGENCY STOP | both relays OFF");
        /* H1: on entry the OFF pattern is forced onto the bus whatever the cache
         * says. Later ticks retry through the state-aware path below, which
         * writes whenever the state is UNKNOWN or ON (no per-tick bus noise). */
        (void)relay_force_all_off();
    }
    s_estopped = true;
    s_station_valid = false;
    s_manual_relay_channel = 0;
    s_manual_start_ms = 0;
    weight_receiver_set_manual_channel(0);
    const char *ftext = stop_fault_text();
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        relay(&s_ch[i], false);
        if (s_ch[i].view.active_job_id)
            finish(&s_ch[i], JOB_FAILED, "EMERGENCY_STOP", now);
        s_ch[i].fault = ftext;
        s_ch[i].view.fault = s_ch[i].fault;
        state_set(&s_ch[i], DCH_FAULT, now);
    }
    (void)relay_set(0U, false);
    (void)relay_set(1U, false);
    /* M6: keep the scale owned while either relay is still ON/UNKNOWN; the next
     * tick's retry releases it once an OFF write is confirmed. */
    for (uint8_t i = 0; i < CHANNELS; ++i)
        s_ch[i].view.relay_on = relay_hw_get(i);
    if (!s_ch[0].view.relay_on && !s_ch[1].view.relay_on) s_scale_owner = SCALE_OWNER_NONE;
}

/* ---- Job submission ---- */

static esp_err_t dual_dispense_controller_add_job_impl(uint8_t channel_id, int32_t target_g,
                                           uint8_t priority, uint32_t *id)
{
    return job_queue_add_for_channel(channel_id, target_g, priority,
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS), id);
}

static esp_err_t dual_dispense_controller_add_global_job_impl(int32_t target_g,
    uint8_t priority, uint32_t *id)
{
    return dual_dispense_controller_add_material_global_job(1U, target_g, priority, id);
}

static esp_err_t dual_dispense_controller_add_material_global_job_impl(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t *id)
{
    return job_queue_add_global_material(material_id, target_g, priority,
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS), id);
}

static esp_err_t dual_dispense_controller_add_server_job_impl(int32_t target_g,
    uint8_t priority, uint32_t command_id, uint32_t *id)
{
    return dual_dispense_controller_add_material_server_job(1U, target_g, priority,
                                                             command_id, id);
}

static esp_err_t dual_dispense_controller_add_material_server_job_impl(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t command_id, uint32_t *id)
{
    return dual_dispense_controller_add_material_server_job_pinned(
        material_id, target_g, priority, command_id, NULL, id);
}

static esp_err_t dual_dispense_controller_add_material_server_job_pinned_impl(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t command_id,
    const job_profile_t *pin, uint32_t *id)
{
    return job_queue_add_global_material_command_pinned(material_id, target_g,
        priority, (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
        command_id, pin, id);
}

/* ---- Observation ---- */

static bool dual_dispense_controller_snapshot_impl(uint8_t channel_id, uint32_t now,
                                       dual_channel_snapshot_t *out)
{
    if (channel_id < 1U || channel_id > 2U || !out) return false;
    channel_t *c = &s_ch[channel_id - 1U];
    *out = c->view;
    out->queue_depth = job_queue_depth();
    out->weight_age_ms = out->have_weight ? now - c->last_weight_ms : UINT32_MAX;
    out->elapsed_ms = out->active_job_id ? now - c->started_ms : 0U;
    out->fault = c->fault;
    out->paused = out->state == DCH_PAUSED;
    out->manual = (s_manual_relay_channel == channel_id);
    out->scale_owner = s_scale_owner;
    out->owns_scale = s_scale_owner == (scale_owner_t)channel_id;
    out->stage = c->stage;
    out->awaiting_operator_ready = c->awaiting_operator_ready;
    return true;
}

static void dual_dispense_controller_scale_info_impl(uint32_t now, dual_scale_info_t *out)
{
    memset(out, 0, sizeof(*out));
    (void)snprintf(out->scale_id, sizeof(out->scale_id), "%s", s_scale.scale_id);
    (void)snprintf(out->boot_id, sizeof(out->boot_id), "%s", s_scale.boot_id);
    out->online = scale_view_fresh(now);
    out->weight_g = s_scale.weight_g;
    out->stable = out->online && s_scale.stable;
    out->age_ms = s_scale.have ? now - s_scale.rx_ms : UINT32_MAX;
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        const channel_t *c = &s_ch[i];
        if (c->view.state == DCH_WAITING_FOR_SCALE_MOVE && c->awaiting_operator_ready &&
            c->view.active_job_id != 0U) {
            out->await_job_id = c->view.active_job_id;
            out->await_channel = c->view.channel_id;
            out->needs_station = c->view.channel_id;
            break;
        }
    }
    out->in_transit = out->await_channel != 0U ||
                      (s_scale_owner == SCALE_OWNER_NONE && !s_station_valid);
}

static bool dual_dispense_controller_clear_fault_impl(uint8_t channel_id, uint32_t now)
{
    if (channel_id < 1U || channel_id > 2U) return false;
    channel_t *c = &s_ch[channel_id - 1U];
    /* The scale is the sensor: a channel fault clears only while the scale itself
     * is online (no pump holds weight of its own when it does not own the scale). */
    if (c->view.state != DCH_FAULT || s_estopped || !scale_view_fresh(now))
        return false;
    /* PRE-04: never go IDLE with a stale active job id. A FAULT channel that
     * still holds one is finished (FAILED, relay OFF, scale released) first. */
    if (c->view.active_job_id != 0U) {
        finish(c, JOB_FAILED, "FAULT_CLEARED_WITH_ACTIVE_JOB", now);
        if (c->view.active_job_id != 0U) return false;
    }
    c->fault = NULL;
    c->view.fault = NULL;
    state_set(c, DCH_IDLE, now);
    return true;
}

static bool fault_is_stop_derived(const char *f)
{
    return f != NULL && (strcmp(f, "EMERGENCY_STOP") == 0 || strcmp(f, "NO_PROGRESS") == 0 ||
                         strcmp(f, "SCALE_MOVED") == 0);
}

static void dual_dispense_controller_clear_emergency_stop_impl(void)
{
    s_estopped = false;
    atomic_store(&s_stop_requested, false);
    s_scale_owner = SCALE_OWNER_NONE;
    s_station_valid = false;   /* the next job needs a fresh READY */
    s_manual_relay_channel = 0;
    s_manual_start_ms = 0;
    weight_receiver_set_manual_channel(0);
    for (uint8_t i = 0; i < CHANNELS; ++i) {
        if (s_ch[i].view.state == DCH_FAULT && fault_is_stop_derived(s_ch[i].fault)) {
            s_ch[i].fault = NULL;
            s_ch[i].view.fault = NULL;
            state_set(&s_ch[i], DCH_IDLE, 0U);
        }
    }
}

/* ---- Profile API ---- */

static bool dual_dispense_controller_set_pending_profile_impl(uint8_t channel_id,
    const char *profile_id, uint32_t version, int32_t target_g,
    float kp, float ki, float kd, int32_t tolerance_g,
    int32_t max_overshoot_g, uint32_t max_duration_ms,
    uint32_t window_ms, uint32_t min_on_ms, uint32_t min_off_ms)
{
    /* Derive staged parameters from the basic ones so existing callers work. */
    int32_t coarse_th = tolerance_g > 0 ? tolerance_g * 25 : CONFIG_WEIGHT_DEMO_COARSE_THRESHOLD_G;
    int32_t fine_th = tolerance_g > 0 ? tolerance_g * 5 : CONFIG_WEIGHT_DEMO_FINE_THRESHOLD_G;
    int32_t micro_th = tolerance_g > 0 ? tolerance_g * 2 : CONFIG_WEIGHT_DEMO_MICRO_THRESHOLD_G;
    return dual_dispense_controller_set_pending_profile_staged(
        channel_id, profile_id, version, target_g,
        kp, ki, kd, tolerance_g, max_overshoot_g, max_duration_ms,
        window_ms, min_on_ms, min_off_ms,
        coarse_th, fine_th, micro_th,
        CONFIG_WEIGHT_DEMO_COARSE_MIN_ON_MS,
        CONFIG_WEIGHT_DEMO_FINE_MIN_ON_MS,
        CONFIG_WEIGHT_DEMO_MICRO_MIN_ON_MS,
        CONFIG_WEIGHT_DEMO_SETTLE_TIME_MS,
        CONFIG_WEIGHT_DEMO_INFLIGHT_COMP_G);
}

static bool dual_dispense_controller_set_pending_profile_staged_impl(uint8_t channel_id,
    const char *profile_id, uint32_t version, int32_t target_g,
    float kp, float ki, float kd, int32_t tolerance_g,
    int32_t max_overshoot_g, uint32_t max_duration_ms,
    uint32_t window_ms, uint32_t min_on_ms, uint32_t min_off_ms,
    int32_t coarse_threshold_g, int32_t fine_threshold_g,
    int32_t micro_threshold_g,
    uint32_t coarse_min_on_ms, uint32_t fine_min_on_ms,
    uint32_t micro_min_on_ms,
    uint32_t settle_time_ms, int32_t inflight_comp_g)
{
    if (channel_id < 1U || channel_id > 2U || !profile_id || !profile_id[0] ||
        strlen(profile_id) >= 65U || version == 0U ||
        !isfinite(kp) || !isfinite(ki) || !isfinite(kd) ||
        kp < 0 || kp > 1 || ki < 0 || ki > 1 || kd < 0 || kd > 1 ||
        tolerance_g < 0 || tolerance_g > 100000 ||
        max_overshoot_g < 0 || max_overshoot_g > 100000 ||
        max_duration_ms == 0 || max_duration_ms > 3600000U ||
        window_ms < 50U || window_ms > 10000U || min_on_ms == 0 ||
        min_off_ms == 0 || min_on_ms > window_ms || min_off_ms > window_ms ||
        coarse_threshold_g < 0 || fine_threshold_g < 0 || micro_threshold_g < 0 ||
        coarse_min_on_ms == 0 || fine_min_on_ms == 0 || micro_min_on_ms == 0 ||
        settle_time_ms == 0 || settle_time_ms > 60000U ||
        inflight_comp_g < 0 || inflight_comp_g > 10000)
        return false;

    /* Only canonical targets are accepted. This prevents a target_g=0 bleed. */
    if (!target_is_canonical(target_g)) {
        ESP_LOGW(TAG, "CH%u profile %s REFUSED: target_g=%ld is not one of "
                      "5000/10000/15000/20000 g",
                 channel_id, profile_id, (long)target_g);
        return false;
    }

    /* M2/M3/M4: the same effective-profile rules the pin parser applies (stage
     * pulse, min_off saturation, threshold order and bounds, tolerance/overshoot
     * relative to the target). One shared function, so the two paths agree. */
    const dual_profile_limits_t limits = {
        .target_g = target_g, .tolerance_g = tolerance_g,
        .max_overshoot_g = max_overshoot_g, .window_ms = window_ms,
        .min_on_ms = min_on_ms, .min_off_ms = min_off_ms,
        .coarse_threshold_g = coarse_threshold_g, .fine_threshold_g = fine_threshold_g,
        .micro_threshold_g = micro_threshold_g, .coarse_min_on_ms = coarse_min_on_ms,
        .fine_min_on_ms = fine_min_on_ms, .micro_min_on_ms = micro_min_on_ms,
    };
    const char *why = dual_dispense_profile_consistency_error(&limits);
    if (why != NULL) {
        ESP_LOGW(TAG, "CH%u profile %s REFUSED: %s", channel_id, profile_id, why);
        return false;
    }

    int slot = profile_slot_for(target_g);
    channel_t *c = &s_ch[channel_id - 1U];
    tuned_profile_t *p = &c->profiles[slot];

    portENTER_CRITICAL(&s_profile_lock);
    p->valid = true;
    p->version = version;
    p->target_g = target_g;
    p->kp = kp; p->ki = ki; p->kd = kd;
    p->tolerance_g = tolerance_g;
    p->overshoot_g = max_overshoot_g;
    p->max_duration_ms = max_duration_ms;
    p->window_ms = window_ms;
    p->min_on_ms = min_on_ms;
    p->min_off_ms = min_off_ms;
    p->coarse_threshold_g = coarse_threshold_g;
    p->fine_threshold_g = fine_threshold_g;
    p->micro_threshold_g = micro_threshold_g;
    p->coarse_min_on_ms = coarse_min_on_ms;
    p->fine_min_on_ms = fine_min_on_ms;
    p->micro_min_on_ms = micro_min_on_ms;
    p->settle_time_ms = settle_time_ms;
    p->inflight_comp_g = inflight_comp_g;
    (void)snprintf(p->profile_id, sizeof(p->profile_id), "%s", profile_id);
    portEXIT_CRITICAL(&s_profile_lock);

    ESP_LOGI(TAG, "CH%u profile %s v%lu stored for target=%ld g (persistent, "
                  "not consumed)", channel_id, profile_id,
             (unsigned long)version, (long)target_g);
    return true;
}

static bool dual_dispense_controller_set_pending_default_impl(uint8_t channel_id, int32_t target_g)
{
    return dual_dispense_controller_set_pending_profile(
        channel_id, "device-default", 1U, target_g,
        (float)CONFIG_WEIGHT_DEMO_PID_KP_X10000 / 10000.0f,
        (float)CONFIG_WEIGHT_DEMO_PID_KI_X10000 / 10000.0f,
        (float)CONFIG_WEIGHT_DEMO_PID_KD_X10000 / 10000.0f,
        CONFIG_WEIGHT_DEMO_TOLERANCE_G,
        CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G,
        CONFIG_WEIGHT_DEMO_MAX_DURATION_MS,
        CONFIG_WEIGHT_DEMO_WINDOW_MS,
        CONFIG_WEIGHT_DEMO_MIN_ON_MS,
        CONFIG_WEIGHT_DEMO_MIN_OFF_MS);
}

static void dual_dispense_controller_discard_pending_profile_impl(uint8_t channel_id,
                                                      int32_t target_g)
{
    if (channel_id < 1U || channel_id > 2U) return;
    channel_t *c = &s_ch[channel_id - 1U];
    portENTER_CRITICAL(&s_profile_lock);
    if (target_g < 0) {
        /* Clear all. */
        for (uint8_t i = 0; i < PROFILE_SLOT_COUNT; ++i)
            c->profiles[i].valid = false;
    } else {
        int slot = profile_slot_for(target_g);
        if (slot >= 0) c->profiles[slot].valid = false;
    }
    portEXIT_CRITICAL(&s_profile_lock);
    ESP_LOGI(TAG, "CH%u profile(s) cleared for target=%ld g", channel_id, (long)target_g);
}

/* Serialized public API. Implementations above never perform network I/O. */
scale_owner_t dual_dispense_controller_scale_owner(void)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    scale_owner_t result = dual_dispense_controller_scale_owner_impl();
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

dispense_stage_t dual_dispense_controller_stage(uint8_t channel_id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dispense_stage_t result = dual_dispense_controller_stage_impl(channel_id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

esp_err_t dual_dispense_controller_init(void)
{
    if (!s_state_mutex) s_state_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_state_mutex) return ESP_ERR_NO_MEM;
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_init_impl();
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

void dual_dispense_controller_on_weight(const weight_msg_t *msg, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_dispense_controller_on_weight_impl(msg, now);
    xSemaphoreGiveRecursive(s_state_mutex);
}

esp_err_t dual_dispense_controller_manual_relay(uint8_t channel_id, bool on, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_manual_relay_impl(channel_id, on, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

void dual_dispense_controller_tick(uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_dispense_controller_tick_impl(now);
    xSemaphoreGiveRecursive(s_state_mutex);
}

bool dual_dispense_controller_operator_ready(uint8_t channel_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_operator_ready_impl(channel_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

dual_ready_result_t dual_dispense_controller_legacy_ready(uint8_t channel_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_ready_result_t result = dual_dispense_controller_legacy_ready_impl(channel_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

dual_ready_result_t dual_dispense_controller_scale_ready(uint8_t channel_id,
                                                         const dual_ready_req_t *req,
                                                         uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_ready_result_t result = dual_dispense_controller_scale_ready_impl(channel_id, req, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

void dual_dispense_controller_scale_info(uint32_t now, dual_scale_info_t *out)
{
    if (out == NULL) return;
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_dispense_controller_scale_info_impl(now, out);
    xSemaphoreGiveRecursive(s_state_mutex);
}

void dual_dispense_controller_scale_cfg_default(dual_scale_cfg_t *out)
{
    if (out == NULL) return;
    *out = (dual_scale_cfg_t){
        .ready_stable_ms = 3000U, .ready_max_age_ms = 120000U,
        .reconfirm_same_pump = true, .legacy_ready = true,
        .scale_move_timeout_ms = 600000U, .start_max_g = 500, .drop_trip_g = 200,
        .np_learn_ms = 10000U, .np_abs_floor_g = 10, .scale_res_g = 2,
    };
}

void dual_dispense_controller_set_scale_cfg(const dual_scale_cfg_t *cfg)
{
    if (cfg == NULL) return;
    if (s_state_mutex) xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    s_cfg = *cfg;
    if (s_state_mutex) xSemaphoreGiveRecursive(s_state_mutex);
}

void dual_dispense_controller_get_scale_cfg(dual_scale_cfg_t *out)
{
    if (out == NULL) return;
    if (s_state_mutex) xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    *out = s_cfg;
    if (s_state_mutex) xSemaphoreGiveRecursive(s_state_mutex);
}

bool dual_dispense_controller_pause(uint8_t channel_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_pause_impl(channel_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_resume(uint8_t channel_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_resume_impl(channel_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_cancel(uint8_t channel_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_cancel_impl(channel_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_cancel_job(uint32_t job_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_cancel_job_impl(job_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

void dual_dispense_controller_emergency_stop(uint32_t now)
{
    atomic_store(&s_stop_requested, true); /* latch before waiting for state */
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_dispense_controller_emergency_stop_impl(now);
    xSemaphoreGiveRecursive(s_state_mutex);
}

esp_err_t dual_dispense_controller_add_job(uint8_t channel_id, int32_t target_g,
                                           uint8_t priority, uint32_t *id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_add_job_impl(channel_id, target_g, priority, id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

esp_err_t dual_dispense_controller_add_global_job(int32_t target_g,
    uint8_t priority, uint32_t *id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_add_global_job_impl(target_g, priority, id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

esp_err_t dual_dispense_controller_add_material_global_job(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t *id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_add_material_global_job_impl(material_id, target_g, priority, id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

esp_err_t dual_dispense_controller_add_server_job(int32_t target_g,
    uint8_t priority, uint32_t command_id, uint32_t *id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_add_server_job_impl(target_g, priority, command_id, id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

esp_err_t dual_dispense_controller_add_material_server_job(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t command_id, uint32_t *id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_add_material_server_job_impl(material_id, target_g, priority, command_id, id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

esp_err_t dual_dispense_controller_add_material_server_job_pinned(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t command_id,
    const job_profile_t *pin, uint32_t *id)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    esp_err_t result = dual_dispense_controller_add_material_server_job_pinned_impl(material_id, target_g, priority, command_id, pin, id);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_snapshot(uint8_t channel_id, uint32_t now,
                                       dual_channel_snapshot_t *out)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_snapshot_impl(channel_id, now, out);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_clear_fault(uint8_t channel_id, uint32_t now)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_clear_fault_impl(channel_id, now);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

void dual_dispense_controller_clear_emergency_stop(void)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_dispense_controller_clear_emergency_stop_impl();
    xSemaphoreGiveRecursive(s_state_mutex);
}

bool dual_dispense_controller_set_pending_profile(uint8_t channel_id,
    const char *profile_id, uint32_t version, int32_t target_g,
    float kp, float ki, float kd, int32_t tolerance_g,
    int32_t max_overshoot_g, uint32_t max_duration_ms,
    uint32_t window_ms, uint32_t min_on_ms, uint32_t min_off_ms)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_set_pending_profile_impl(channel_id, profile_id, version, target_g, kp, ki, kd, tolerance_g, max_overshoot_g, max_duration_ms, window_ms, min_on_ms, min_off_ms);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_set_pending_profile_staged(uint8_t channel_id,
    const char *profile_id, uint32_t version, int32_t target_g,
    float kp, float ki, float kd, int32_t tolerance_g,
    int32_t max_overshoot_g, uint32_t max_duration_ms,
    uint32_t window_ms, uint32_t min_on_ms, uint32_t min_off_ms,
    int32_t coarse_threshold_g, int32_t fine_threshold_g,
    int32_t micro_threshold_g,
    uint32_t coarse_min_on_ms, uint32_t fine_min_on_ms,
    uint32_t micro_min_on_ms,
    uint32_t settle_time_ms, int32_t inflight_comp_g)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_set_pending_profile_staged_impl(channel_id, profile_id, version, target_g, kp, ki, kd, tolerance_g, max_overshoot_g, max_duration_ms, window_ms, min_on_ms, min_off_ms, coarse_threshold_g, fine_threshold_g, micro_threshold_g, coarse_min_on_ms, fine_min_on_ms, micro_min_on_ms, settle_time_ms, inflight_comp_g);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

bool dual_dispense_controller_set_pending_default(uint8_t channel_id, int32_t target_g)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    bool result = dual_dispense_controller_set_pending_default_impl(channel_id, target_g);
    xSemaphoreGiveRecursive(s_state_mutex);
    return result;
}

void dual_dispense_controller_discard_pending_profile(uint8_t channel_id,
                                                      int32_t target_g)
{
    configASSERT(s_state_mutex != NULL);
    xSemaphoreTakeRecursive(s_state_mutex, portMAX_DELAY);
    dual_dispense_controller_discard_pending_profile_impl(channel_id, target_g);
    xSemaphoreGiveRecursive(s_state_mutex);
}
