#include "dispense_controller.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "job_queue.h"
#include "relay_driver.h"
#include "safety_manager.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "RELAY_CTRL";

static dispense_config_t s_cfg;
static dispense_state_t s_state = DISPENSE_IDLE;
static uint32_t s_state_entry_ms;
static bool s_paused;

/* Latest weight, as reported by the WebSocket client task.
 *
 * Written ONLY from that task, under s_weight_lock. Every other reader — the
 * supervisor tick and the web API — takes a consistent (value, timestamp,
 * validity) snapshot instead of reading the three fields independently, so a
 * torn pair can never make a fresh reading look stale for one tick or report a
 * weight against the wrong instant. */
static portMUX_TYPE s_weight_lock = portMUX_INITIALIZER_UNLOCKED;
static bool     s_have_weight;
static int32_t  s_last_weight_g;
static uint32_t s_last_weight_ms;
static bool     s_simulated;
static uint32_t s_sim_cycle;
static bool     s_have_cycle;

/* Snapshot taken once per tick and used by ALL tick logic and by the public
 * getters, so every consumer of a tick sees the same instant. */
static bool     s_pv_have;
static int32_t  s_pv_g;
static uint32_t s_pv_ms;

/* Ramp bookkeeping. */
static uint32_t s_cycle_resets;
static bool     s_cycle_reset_pending;   /* a reset seen since the last job ended */
static bool     s_await_zero;            /* waiting for a fresh cycle at 0 g */
static uint32_t s_last_ctrl_log_ms;

static void weight_store(const weight_msg_t *msg, uint32_t now_ms,
                        bool *out_reset, int32_t *out_previous)
{
    bool reset = false;
    int32_t previous = 0;

    portENTER_CRITICAL(&s_weight_lock);
    previous = s_have_weight ? s_last_weight_g : 0;
    if (msg->simulated && msg->has_cycle) {
        if (s_have_cycle && msg->simulation_cycle != s_sim_cycle) reset = true;
        s_sim_cycle = msg->simulation_cycle;
        s_have_cycle = true;
    }
    /* The gate flags live under the same lock as the reading they qualify:
     * they are written from the WebSocket task and read and cleared by the
     * supervisor, and keeping one discipline for the whole ramp-gate state is
     * easier to reason about than a per-field argument. */
    if (reset) {
        s_cycle_resets++;
        s_cycle_reset_pending = true;
    }
    s_simulated = msg->simulated;
    s_last_weight_g = msg->weight_g;
    s_last_weight_ms = now_ms;
    s_have_weight = true;
    portEXIT_CRITICAL(&s_weight_lock);

    if (out_reset != NULL) *out_reset = reset;
    if (out_previous != NULL) *out_previous = previous;
}

static void weight_snapshot(uint32_t now_ms)
{
    (void)now_ms;
    portENTER_CRITICAL(&s_weight_lock);
    s_pv_have = s_have_weight;
    s_pv_g = s_last_weight_g;
    s_pv_ms = s_last_weight_ms;
    portEXIT_CRITICAL(&s_weight_lock);
}

/* Valve state as commanded. Kept here (not read back from the driver) so the
 * PWM comparison is against what this controller asked for. */
static bool s_coarse_on;
static bool s_fine_on;

/* Running job. */
static uint32_t s_active_job_id;
static int32_t  s_active_target_g;
static uint32_t s_job_start_ms;
static uint32_t s_corrections;
/* The reading at the moment the target was reached. In RAMP_TEST the reading
 * keeps climbing while the machine settles, so recording the value at COMPLETE
 * would report a final weight the controller never actually aimed at. */
static int32_t  s_reached_g;
static bool     s_have_reached;

/* Fine-region control. */
static uint32_t s_window_start_ms;
static uint32_t s_last_pid_ms;
static float    s_duty;
static dispense_pid_state_t s_pid;
static float s_p_term, s_i_term, s_d_term;

static dispense_valve_notify_fn s_notify;

const char *dispense_state_name(dispense_state_t state)
{
    switch (state) {
    case DISPENSE_IDLE:            return "IDLE";
    case DISPENSE_LOAD_JOB:        return "LOAD_JOB";
    case DISPENSE_WAIT_FOR_SCALE:  return "WAIT_FOR_SCALE";
    case DISPENSE_COARSE_DISPENSE: return "COARSE_DISPENSE";
    case DISPENSE_FINE_DISPENSE:   return "FINE_DISPENSE";
    case DISPENSE_SETTLING:        return "SETTLING";
    case DISPENSE_FINE_CORRECTION: return "FINE_CORRECTION";
    case DISPENSE_COMPLETE:        return "COMPLETE";
    case DISPENSE_NEXT_JOB:        return "NEXT_JOB";
    case DISPENSE_FAULT:           return "FAULT";
    case DISPENSE_EMERGENCY_STOP:  return "EMERGENCY_STOP";
    default:                       return "UNKNOWN";
    }
}

const char *dispense_completion_mode_name(dispense_completion_mode_t mode)
{
    return (mode == DISPENSE_COMPLETION_RAMP_TEST) ? "RAMP_TEST (independent simulator)"
                                                   : "PROCESS (real setpoint)";
}

/* ---------------------------------------------------------------------------
 * Pure control math
 * ------------------------------------------------------------------------ */

float dispense_pid_duty(const dispense_pid_gains_t *gains, dispense_pid_state_t *state,
                        int32_t error_g, uint32_t dt_ms, float integral_max)
{
    if (gains == NULL || state == NULL) return 0.0f;

    float dt_s = (float)dt_ms / 1000.0f;
    if (dt_s <= 0.0f) dt_s = 0.001f;
    if (integral_max < 0.0f) integral_max = 0.0f;

    float e = (float)error_g;

    /* Integral with anti-windup: a long saturated approach would otherwise
     * bank a huge term that overshoots the instant the error finally shrinks. */
    state->integral += e * dt_s;
    if (state->integral >  integral_max) state->integral =  integral_max;
    if (state->integral < -integral_max) state->integral = -integral_max;

    float derivative = 0.0f;
    if (state->have_last) {
        derivative = (e - (float)state->last_error) / dt_s;
    }
    state->last_error = error_g;
    state->have_last = true;

    float duty = gains->kp * e + gains->ki * state->integral + gains->kd * derivative;

    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    return duty;
}

bool dispense_window_output(uint32_t elapsed_ms, float duty, uint32_t window_ms,
                            uint32_t min_on_ms, uint32_t min_off_ms)
{
    if (duty <= 0.0f) return false;
    if (duty >= 1.0f) return true;
    if (window_ms == 0U) return true;

    uint32_t on_ms = (uint32_t)(duty * (float)window_ms + 0.5f);
    if (on_ms > window_ms) on_ms = window_ms;

    /* Anti-chatter is structural, not a timer: a burst shorter than min_on_ms
     * is dropped (it would not move the needle anyway, and every transition
     * wears the solenoid), and a gap shorter than min_off_ms is eliminated by
     * holding the valve open for the whole window. Either way the valve sees
     * at most one transition per window. */
    if (on_ms < min_on_ms) return false;
    if ((window_ms - on_ms) < min_off_ms) return true;

    return (elapsed_ms % window_ms) < on_ms;
}

/* ---------------------------------------------------------------------------
 * Internals
 * ------------------------------------------------------------------------ */

static void set_state(dispense_state_t next, uint32_t now_ms)
{
    if (s_state == next) return;

    ESP_LOGI(TAG, "Dispense %s -> %s | weight=%ld g | target=%ld g",
             dispense_state_name(s_state), dispense_state_name(next),
             (long)s_pv_g, (long)s_active_target_g);

    s_state = next;
    s_state_entry_ms = now_ms;
}

/*
 * The ONLY place a dispensing relay is commanded.
 *
 * A failed relay write is logged but does NOT fault the controller: this board
 * gates automatic actuation until the mapping/polarity have been field-verified
 * (WEIGHT_DEMO_POLARITY_VERIFIED), so a failure here is a known, expected bench
 * condition. Faulting on it would replace an observable, debuggable state
 * machine with an immediate latched fault before the operator has run PCF_TEST.
 * The safety property is unaffected: the driver performs ZERO bus traffic when
 * gated, so no valve can energize.
 */
static void set_valves(bool coarse, bool fine, bool reset, uint32_t now_ms)
{
    (void)now_ms;

    bool coarse_changed = (coarse != s_coarse_on);
    bool fine_changed = (fine != s_fine_on);

    /* Cache the ACCEPTED state, not the requested one.
     *
     * A rejected command must never be reported as an achieved state: the web
     * UI, the status JSON, the periodic control line and the log all read
     * these two flags, and an ON that the driver refused (gate closed, backend
     * inactive, bus error) used to leave them saying OPEN — the controller
     * claiming a valve was open while the hardware was untouched. On rejection
     * the flag keeps describing reality. */
    bool accepted_coarse = s_coarse_on;
    bool accepted_fine = s_fine_on;

    if (coarse_changed || reset) {
        esp_err_t err = relay_set((uint8_t)DISPENSE_COARSE_RELAY_INDEX, coarse);
        if (err == ESP_OK) {
            accepted_coarse = coarse;
        } else {
            ESP_LOGE(TAG, "coarse valve command REJECTED (%s) - valve is still %s, "
                          "not %s", esp_err_to_name(err),
                     s_coarse_on ? "OPEN" : "CLOSED", coarse ? "OPEN" : "CLOSED");
        }
    }
    if (fine_changed || reset) {
        esp_err_t err = relay_set((uint8_t)DISPENSE_FINE_RELAY_INDEX, fine);
        if (err == ESP_OK) {
            accepted_fine = fine;
        } else {
            ESP_LOGE(TAG, "fine valve command REJECTED (%s) - valve is still %s, "
                          "not %s", esp_err_to_name(err),
                     s_fine_on ? "OPEN" : "CLOSED", fine ? "OPEN" : "CLOSED");
        }
    }

    s_coarse_on = accepted_coarse;
    s_fine_on = accepted_fine;

    /* Everything downstream reports what the hardware accepted. */
    coarse = accepted_coarse;
    fine = accepted_fine;

    if (coarse_changed) {
        ESP_LOGI(TAG, "Coarse valve -> %s (fine=%s)", coarse ? "OPEN" : "CLOSED",
                 fine ? "OPEN" : "CLOSED");
    } else if (fine_changed) {
        /* The PWM toggles the fine valve up to twice per window, so its
         * transitions stay at debug level; the duty is reported in the
         * periodic control line instead. */
        ESP_LOGD(TAG, "Fine valve -> %s", fine ? "OPEN" : "CLOSED");
    }

    if ((coarse_changed || fine_changed || reset) && s_notify != NULL) {
        /* Report the ACCEPTED pair: the simulated vessel follows the valves
         * that actually moved, not the ones that were asked to. */
        s_notify(accepted_coarse, accepted_fine, reset);
    }
}

static bool weight_is_fresh(uint32_t now_ms)
{
    if (!s_pv_have) return false;
    return (now_ms - s_pv_ms) <= s_cfg.stale_timeout_ms;
}

/* A confirmed zero: the reading the next job is allowed to start on. */
/* "Zero" is judged against tolerance_g, not equality: the ramp publishes an
 * exact 0 g, but a real scale's zero has noise, and gate tolerance must not be
 * tighter than the completion tolerance or the two would disagree.
 *
 * CAUTION for future tuners: the gate is only as tight as this band. Setting
 * WEIGHT_DEMO_TOLERANCE_G above the simulator's WEIGHT_DEMO_RAMP_STEP_G would
 * let a job start mid-ramp, because a reading one whole step up would still
 * count as zero. Keep tolerance well below the step. */
static bool weight_confirmed_zero(void)
{
    return s_pv_have && (s_pv_g <= s_cfg.tolerance_g) && (s_pv_g >= 0);
}

/* Arm/disarm/read the next-job gate under the weight lock. */
static void gate_arm(void)
{
    portENTER_CRITICAL(&s_weight_lock);
    s_await_zero = true;
    s_cycle_reset_pending = false;
    portEXIT_CRITICAL(&s_weight_lock);
}

static void gate_clear(void)
{
    portENTER_CRITICAL(&s_weight_lock);
    s_await_zero = false;
    s_cycle_reset_pending = false;
    portEXIT_CRITICAL(&s_weight_lock);
}

static void gate_read(bool *await_zero, bool *reset_pending)
{
    portENTER_CRITICAL(&s_weight_lock);
    if (await_zero != NULL) *await_zero = s_await_zero;
    if (reset_pending != NULL) *reset_pending = s_cycle_reset_pending;
    portEXIT_CRITICAL(&s_weight_lock);
}

/*
 * May a queued job be started right now?
 *
 * In RAMP_TEST a job may only begin at a confirmed 0 g reading, and a job
 * queued AFTER another finished additionally waits for a fresh simulation
 * cycle. That is what makes a queued-job run deterministic instead of dropping
 * the job at whatever point of the 0-to-20 kg ramp it happens to be at.
 *
 * Checked in BOTH places that can start a job. It has to be: a finished job
 * chains COMPLETE -> NEXT_JOB -> LOAD_JOB inside a single tick, so a gate
 * living only in IDLE would never be consulted on the automatic handover —
 * which is exactly the path the requirement is about.
 */
static bool next_job_may_start(void)
{
    if (s_cfg.completion_mode != DISPENSE_COMPLETION_RAMP_TEST) return true;
    if (!weight_confirmed_zero()) return false;

    bool await_zero = false, reset_pending = false;
    gate_read(&await_zero, &reset_pending);
    return !await_zero || reset_pending;
}

/* Rate-limited "why is nothing starting" line. */
static void log_waiting_for_zero(uint32_t now_ms)
{
    if (s_cfg.control_log_period_ms == 0U) return;
    if ((now_ms - s_last_ctrl_log_ms) < s_cfg.control_log_period_ms) return;
    s_last_ctrl_log_ms = now_ms;

    bool await_zero = false, reset_pending = false;
    gate_read(&await_zero, &reset_pending);
    ESP_LOGI(TAG, "waiting for next 0-kg cycle | weight=%ld g | reset_seen=%d | "
                  "queue=%u | priority=%u",
             (long)s_pv_g, (int)reset_pending,
             (unsigned)job_queue_depth(),
             (unsigned)job_queue_depth_at(JOB_PRIORITY_HIGH));
}

static bool job_expired(uint32_t now_ms)
{
    return (now_ms - s_job_start_ms) > s_cfg.max_duration_ms;
}

static bool job_overshot(void)
{
    /* Disabled entirely in RAMP_TEST: the independent ramp keeps rising after
     * the valves close, so "over target" is the simulator's behaviour, not a
     * control failure, and faulting on it would fail every job. */
    if (s_cfg.completion_mode == DISPENSE_COMPLETION_RAMP_TEST) return false;
    return s_pv_g > (s_active_target_g + s_cfg.max_overshoot_g);
}

static void clear_active_job(void)
{
    s_active_job_id = 0U;
    s_active_target_g = 0;
    s_duty = 0.0f;
}

static void finish_active_job(job_state_t result, const char *error, uint32_t now_ms)
{
    if (s_active_job_id == 0U) return;

    /* The value the job was judged on: target-reached where one was recorded,
     * otherwise the current reading (a failed job has no reached value and the
     * current reading is the honest one to record). */
    int32_t recorded = (result == JOB_COMPLETE && s_have_reached) ? s_reached_g : s_pv_g;
    (void)job_queue_finish(s_active_job_id, result, recorded, error, now_ms);

    /* Log the value that was RECORDED, not the live reading. Under RAMP_TEST
     * those differ - the simulator keeps climbing while the machine settles -
     * and a log that announced a different final weight than the job record
     * holds would be exactly the kind of untrue line this firmware avoids. The
     * live reading is visible in the state-transition line above. */
    if (result == JOB_COMPLETE) {
        ESP_LOGI(TAG, "Job %u COMPLETE | target=%ld g | reached=%ld g | "
                      "corrections=%u",
                 (unsigned)s_active_job_id, (long)s_active_target_g,
                 (long)recorded, (unsigned)s_corrections);
    } else {
        ESP_LOGE(TAG, "Job %u %s | target=%ld g | final=%ld g | error=%s",
                 (unsigned)s_active_job_id, job_state_name(result),
                 (long)s_active_target_g, (long)recorded,
                 (error != NULL) ? error : "-");
    }

    clear_active_job();

    /* Arm the "wait for a fresh 0 g cycle" gate. Harmless in PROCESS mode
     * (never consulted) and essential in RAMP_TEST, where it is what stops a
     * later rising reading from handing the next job a part-filled scale. */
    gate_arm();
}

/* Fails the running job, closes every valve and latches the fault. The FAULT
 * state is entered here rather than on the next tick so the log ordering shows
 * cause and effect in one place. */
static void fail_job(const char *error, safety_fault_t fault, uint32_t now_ms)
{
    set_valves(false, false, false, now_ms);
    finish_active_job(JOB_FAILED, error, now_ms);
    safety_manager_raise(fault, now_ms);
    set_state(DISPENSE_FAULT, now_ms);
}

/* One PID step plus the windowed output decision for the fine valve. */
static void run_fine_control(uint32_t now_ms)
{
    int32_t error = s_active_target_g - s_pv_g; /* positive = need more */

    uint32_t dt = now_ms - s_last_pid_ms;
    if (dt == 0U) dt = 1U;
    if (dt > 1000U) dt = 1000U; /* a stalled loop must not integrate a huge step */
    s_last_pid_ms = now_ms;

    bool had_last = s_pid.have_last;
    int32_t previous_error = s_pid.last_error;
    const dispense_pid_gains_t gains = { s_cfg.kp, s_cfg.ki, s_cfg.kd };
    s_duty = dispense_pid_duty(&gains, &s_pid, error, dt, s_cfg.integral_max);
    float dt_s = (float)dt / 1000.0f;
    if (dt_s <= 0.0f) dt_s = 0.001f;
    s_p_term = s_cfg.kp * (float)error;
    s_i_term = s_cfg.ki * s_pid.integral;
    s_d_term = s_cfg.kd * (had_last
        ? ((float)(error - previous_error) / dt_s) : 0.0f);

    uint32_t elapsed = now_ms - s_window_start_ms;
    bool open = dispense_window_output(elapsed, s_duty, s_cfg.window_ms,
                                       s_cfg.min_on_ms, s_cfg.min_off_ms);
    set_valves(false, open, false, now_ms);
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

/* Every threshold is guarded: the fallback keeps this component (and the QEMU
 * suite, which has no Kconfig) buildable standalone and matches the documented
 * firmware default. */
#ifndef CONFIG_WEIGHT_DEMO_TARGET_G
#define CONFIG_WEIGHT_DEMO_TARGET_G 5000
#endif
#ifndef CONFIG_WEIGHT_DEMO_TOLERANCE_G
#define CONFIG_WEIGHT_DEMO_TOLERANCE_G 20
#endif
#ifndef CONFIG_WEIGHT_DEMO_COARSE_TRANSITION_G
#define CONFIG_WEIGHT_DEMO_COARSE_TRANSITION_G 1000
#endif
#ifndef CONFIG_WEIGHT_DEMO_SETTLE_MS
#define CONFIG_WEIGHT_DEMO_SETTLE_MS 1500
#endif
#ifndef CONFIG_WEIGHT_DEMO_MAX_DURATION_MS
#define CONFIG_WEIGHT_DEMO_MAX_DURATION_MS 120000
#endif
#ifndef CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G
#define CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G 100
#endif
#ifndef CONFIG_WEIGHT_DEMO_CORRECTION_LIMIT
#define CONFIG_WEIGHT_DEMO_CORRECTION_LIMIT 5
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
#ifndef CONFIG_WEIGHT_DEMO_PID_KP_X10000
#define CONFIG_WEIGHT_DEMO_PID_KP_X10000 25
#endif
#ifndef CONFIG_WEIGHT_DEMO_PID_KI_X10000
#define CONFIG_WEIGHT_DEMO_PID_KI_X10000 3
#endif
#ifndef CONFIG_WEIGHT_DEMO_PID_KD_X10000
#define CONFIG_WEIGHT_DEMO_PID_KD_X10000 1
#endif
#ifndef CONFIG_WEIGHT_DEMO_PID_INTEGRAL_MAX
#define CONFIG_WEIGHT_DEMO_PID_INTEGRAL_MAX 200
#endif
#ifndef CONFIG_WEIGHT_DEMO_CONTROL_LOG_MS
#define CONFIG_WEIGHT_DEMO_CONTROL_LOG_MS 1000
#endif
/* Top of the simulator's ramp. Used only as the secondary reset heuristic
 * (top -> 0 while simulated); the primary signal is the simulation_cycle
 * field itself. */
#ifndef CONFIG_WEIGHT_DEMO_RAMP_TOP_G
#define CONFIG_WEIGHT_DEMO_RAMP_TOP_G 20000
#endif

dispense_config_t dispense_config_default(void)
{
    const dispense_config_t cfg = {
        .target_g            = CONFIG_WEIGHT_DEMO_TARGET_G,
        .tolerance_g         = CONFIG_WEIGHT_DEMO_TOLERANCE_G,
        .coarse_transition_g = CONFIG_WEIGHT_DEMO_COARSE_TRANSITION_G,
        .settle_ms           = CONFIG_WEIGHT_DEMO_SETTLE_MS,
        .max_duration_ms     = CONFIG_WEIGHT_DEMO_MAX_DURATION_MS,
        .max_overshoot_g     = CONFIG_WEIGHT_DEMO_MAX_OVERSHOOT_G,
        .ramp_top_g          = CONFIG_WEIGHT_DEMO_RAMP_TOP_G,
        .stale_timeout_ms    = WEIGHT_MESSAGE_TIMEOUT_MS,
        .correction_limit    = CONFIG_WEIGHT_DEMO_CORRECTION_LIMIT,
        /* PID gains are integers in Kconfig, scaled by 10000 so a gain like
         * 0.0025 is expressible without a float Kconfig type. */
        .window_ms           = CONFIG_WEIGHT_DEMO_WINDOW_MS,
        .min_on_ms           = CONFIG_WEIGHT_DEMO_MIN_ON_MS,
        .min_off_ms          = CONFIG_WEIGHT_DEMO_MIN_OFF_MS,
        .kp                  = (float)CONFIG_WEIGHT_DEMO_PID_KP_X10000 / 10000.0f,
        .ki                  = (float)CONFIG_WEIGHT_DEMO_PID_KI_X10000 / 10000.0f,
        .kd                  = (float)CONFIG_WEIGHT_DEMO_PID_KD_X10000 / 10000.0f,
        .integral_max        = (float)CONFIG_WEIGHT_DEMO_PID_INTEGRAL_MAX,
        .control_log_period_ms = CONFIG_WEIGHT_DEMO_CONTROL_LOG_MS,
#if defined(CONFIG_WEIGHT_DEMO_COMPLETION_PROCESS)
        .completion_mode     = DISPENSE_COMPLETION_PROCESS,
#else
        /* RAMP_TEST is the default because the independent ramp simulator is
         * the authoritative bench mode. An explicit choice, logged at boot —
         * never inferred from the payload, so a real scale cannot inherit the
         * relaxed overshoot rules by sending a field. */
        .completion_mode     = DISPENSE_COMPLETION_RAMP_TEST,
#endif
    };
    ESP_LOGI(TAG, "completion semantics: %s",
             dispense_completion_mode_name(cfg.completion_mode));
    return cfg;
}

esp_err_t dispense_controller_init(const dispense_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    s_cfg = *config;
    s_state = DISPENSE_IDLE;
    s_state_entry_ms = 0U;
    s_paused = false;
    s_have_weight = false;
    s_last_weight_g = 0;
    s_last_weight_ms = 0U;
    s_coarse_on = false;
    s_fine_on = false;
    s_window_start_ms = 0U;
    s_last_pid_ms = 0U;
    s_reached_g = 0;
    s_have_reached = false;
    s_duty = 0.0f;
    memset(&s_pid, 0, sizeof(s_pid));
    clear_active_job();

    /* Init runs before any task is created, so these writes are not racing
     * anything; the lock is still used for the shared weight fields above so
     * there is exactly one write discipline to reason about. */
    s_have_weight = false;
    s_last_weight_g = 0;
    s_last_weight_ms = 0U;
    s_simulated = false;
    s_have_cycle = false;
    s_sim_cycle = 0U;
    s_pv_have = false;
    s_pv_g = 0;
    s_pv_ms = 0U;
    s_cycle_resets = 0U;
    s_cycle_reset_pending = false;
    s_await_zero = false;
    s_last_ctrl_log_ms = 0U;

    (void)relay_all_off();

    ESP_LOGI(TAG, "Dispense controller ready | tolerance=%ld g | coarse_transition=%ld g",
             (long)s_cfg.tolerance_g, (long)s_cfg.coarse_transition_g);
    return ESP_OK;
}

void dispense_controller_set_valve_notify(dispense_valve_notify_fn fn)
{
    s_notify = fn;
}

void dispense_controller_on_weight(const weight_msg_t *msg, uint32_t now_ms)
{
    if (msg == NULL) return;

    bool cycle_reset = false;
    int32_t previous_g = 0;
    weight_store(msg, now_ms, &cycle_reset, &previous_g);

    /* Secondary heuristic for a sender that reports simulated data but no
     * cycle field: the deliberate top-of-ramp -> 0 transition, judged against
     * the immediately PREVIOUS reading of this same stream. It is logged as a
     * cycle reset and explicitly NOT treated as a fault. */
    if (msg->simulated && !msg->has_cycle &&
        previous_g >= s_cfg.ramp_top_g && msg->weight_g == 0) {
        cycle_reset = true;
    }

    if (cycle_reset) {
        /* The counter and the gate flag were updated inside weight_store, under
         * the same lock as the reading; only the logging happens out here. */
        ESP_LOGW(TAG, "SIMULATION_CYCLE_RESET | cycle=%u | reading restarted at 0 g "
                      "- deliberate simulator reset, NOT a fault",
                 (unsigned)msg->simulation_cycle);
    }

    /* A fresh valid weight is exactly what the transient link faults are
     * waiting for, so let them clear here rather than making the operator
     * acknowledge a Wi-Fi blip. */
    safety_manager_on_valid_weight(now_ms);
}

void dispense_controller_tick(uint32_t now_ms)
{
    safety_manager_note_control_tick(now_ms);

    /* One consistent view of the reading for this whole tick. */
    weight_snapshot(now_ms);

    /* While a job is running, a weight feed that stops updating is itself a
     * fault: a valve must never keep pouring blind. Distinguishing "went
     * stale" from "never arrived" picks the honest fault name. */
    if (s_active_job_id != 0U) {
        uint32_t reference = s_pv_have ? s_pv_ms : s_job_start_ms;
        if ((now_ms - reference) > s_cfg.stale_timeout_ms) {
            safety_manager_raise(s_pv_have ? SAFETY_WEIGHT_STALE
                                           : SAFETY_WEIGHT_DISCONNECTED,
                                 now_ms);
        }
    }

    safety_fault_t fault = safety_manager_fault();

    /* Fault handling dominates: no branch below may reopen a valve. */
    if (fault == SAFETY_EMERGENCY_STOP) {
        set_valves(false, false, false, now_ms);
        set_state(DISPENSE_EMERGENCY_STOP, now_ms);
        return;
    }

    if (fault != SAFETY_NONE) {
        set_valves(false, false, false, now_ms);

        if (s_state != DISPENSE_FAULT) {
            set_state(DISPENSE_FAULT, now_ms);
            /* A transient link fault describes the LINK, not the machine, so
             * the job is parked rather than destroyed — a two-second Wi-Fi
             * blip must not throw away a half-dispensed vessel. A latched
             * fault means the machine did something an operator must look at,
             * so that job is failed here. */
            if (!safety_fault_is_transient(fault) && s_active_job_id != 0U) {
                finish_active_job(JOB_FAILED, safety_fault_name(fault), now_ms);
            }
        }

        /* A parked job must not stay parked forever. The state machine is
         * frozen here, so the ordinary expiry check never runs — without this
         * a link that never recovers would leave the job RUNNING (and its
         * record reserved) for as long as the device is powered. The budget
         * deliberately keeps running through the outage. */
        if (s_active_job_id != 0U && job_expired(now_ms)) {
            finish_active_job(JOB_FAILED, "timeout during fault", now_ms);
        }
        return;
    }

    /* Fault cleared. A parked job resumes at WAIT_FOR_SCALE, which re-checks
     * freshness before anything reopens; a failed/cleared job leaves the
     * machine idle. */
    if (s_state == DISPENSE_FAULT) {
        set_state((s_active_job_id != 0U) ? DISPENSE_WAIT_FOR_SCALE : DISPENSE_IDLE,
                  now_ms);
        s_duty = 0.0f;
        memset(&s_pid, 0, sizeof(s_pid));
        s_last_pid_ms = now_ms;
    } else if (s_state == DISPENSE_EMERGENCY_STOP) {
        set_state(DISPENSE_IDLE, now_ms);
    }

    if (s_paused) {
        set_valves(false, false, false, now_ms);
        return;
    }

    /* LOAD_JOB, COMPLETE and NEXT_JOB are bookkeeping steps, not physical
     * waits, so they are re-dispatched inside the SAME tick. Without this a
     * job would burn one 250 ms supervisor period per hop, and the handover
     * from a finished job to the next queued one would visibly stall. The
     * guard bounds the loop so a state that ever set itself could not hang the
     * supervisor. */
    for (int guard = 0; guard < 8; guard++) {
        bool transient = false;

        switch (s_state) {
    case DISPENSE_IDLE:
        if (job_queue_depth() > 0U) {
            /* RAMP_TEST starts a job ONLY at a confirmed 0 g reading, and a
             * job queued after another finished waits for a FRESH cycle as
             * well. Starting at an arbitrary point in the 0-to-20 kg ramp
             * would hand the job a part-filled scale and could report a
             * success that dispensed nothing. */
            if (!next_job_may_start()) {
                log_waiting_for_zero(now_ms);
                break;
            }
            gate_clear();
            set_state(DISPENSE_LOAD_JOB, now_ms);
            transient = true;
        }
        break;

    case DISPENSE_LOAD_JOB: {
        dispense_job_t job;
        if (!job_queue_start_next(now_ms, &job)) {
            set_state(DISPENSE_IDLE, now_ms);
            break;
        }

        s_active_job_id = job.id;
        s_active_target_g = job.target_g;
        s_job_start_ms = now_ms;
        s_corrections = 0U;
        s_reached_g = 0;
        s_have_reached = false;
        s_duty = 0.0f;
        memset(&s_pid, 0, sizeof(s_pid));
        s_last_pid_ms = now_ms;
        s_window_start_ms = now_ms;

        /* Only relays 1 (coarse) and 2 (fine) dose. The other ten channels are
         * held OFF for the whole job, so a leftover group state from the level
         * mode cannot energize anything. */
        (void)relay_all_off();
        set_valves(false, false, true, now_ms);

        (void)job_queue_set_start_g(job.id, s_pv_have ? s_pv_g : 0);

        ESP_LOGI(TAG, "Job %u RUNNING | target=%ld g | start=%ld g",
                 (unsigned)job.id, (long)job.target_g,
                 (long)(s_pv_have ? s_pv_g : 0));

        set_state(DISPENSE_WAIT_FOR_SCALE, now_ms);
        break;
    }

    case DISPENSE_WAIT_FOR_SCALE: {
        if (!weight_is_fresh(now_ms)) break;          /* wait; safety manager watches */
        if (job_expired(now_ms)) {
            fail_job("timeout waiting for scale", SAFETY_TIMEOUT, now_ms);
            break;
        }
        if (job_overshot()) {
            fail_job("already over target", SAFETY_OVERSHOOT, now_ms);
            break;
        }
        if (s_pv_g >= (s_active_target_g - s_cfg.tolerance_g)) {
            if (s_cfg.completion_mode == DISPENSE_COMPLETION_RAMP_TEST) {
                /* The gate above should have prevented this, so reaching here
                 * means the ramp moved between the cycle check and now. Fail
                 * it: reporting COMPLETE would be exactly the silent fake
                 * success this mode exists to prevent. */
                ESP_LOGE(TAG, "Job %u started at %ld g, already at/over target "
                              "%ld g - refusing to report a zero-dose success",
                         (unsigned)s_active_job_id, (long)s_pv_g,
                         (long)s_active_target_g);
                fail_job("started above target", SAFETY_INVALID_START, now_ms);
                break;
            }
            /* PROCESS mode: the vessel genuinely already holds enough. No
             * valve is opened, and the recorded start and final weights are
             * equal — the job record shows plainly that nothing was added. */
            ESP_LOGW(TAG, "Job %u target already satisfied at start (%ld g) - "
                          "no dosing performed",
                     (unsigned)s_active_job_id, (long)s_pv_g);
            finish_active_job(JOB_COMPLETE, NULL, now_ms);
            set_state(DISPENSE_COMPLETE, now_ms);
            break;
        }
        /* Far from target: open BOTH valves for the fast fill. */
        set_valves(true, true, false, now_ms);
        set_state(DISPENSE_COARSE_DISPENSE, now_ms);
        break;
    }

    case DISPENSE_COARSE_DISPENSE: {
        if (!weight_is_fresh(now_ms)) break;
        if (job_expired(now_ms)) {
            fail_job("max duration exceeded", SAFETY_TIMEOUT, now_ms);
            break;
        }
        if (job_overshot()) {
            fail_job("overshoot", SAFETY_OVERSHOOT, now_ms);
            break;
        }

        int32_t remaining = s_active_target_g - s_pv_g;
        if (remaining > s_cfg.coarse_transition_g) {
            set_valves(true, true, false, now_ms);
        } else {
            /* Close the high-flow valve and hand over to fine control. */
            set_valves(false, true, false, now_ms);
            s_window_start_ms = now_ms;
            s_last_pid_ms = now_ms;
            memset(&s_pid, 0, sizeof(s_pid));
            ESP_LOGI(TAG, "Coarse -> fine handover | remaining=%ld g", (long)remaining);
            set_state(DISPENSE_FINE_DISPENSE, now_ms);
        }
        break;
    }

    case DISPENSE_FINE_DISPENSE: {
        if (!weight_is_fresh(now_ms)) break;
        if (job_expired(now_ms)) {
            fail_job("max duration exceeded", SAFETY_TIMEOUT, now_ms);
            break;
        }
        if (job_overshot()) {
            fail_job("overshoot", SAFETY_OVERSHOOT, now_ms);
            break;
        }

        int32_t error = s_active_target_g - s_pv_g;
        if (error <= s_cfg.tolerance_g) {
            /* Capture the reading the target was reached at, BEFORE settling:
             * under RAMP_TEST the reading keeps rising, and this is the value
             * the job actually achieved. */
            if (!s_have_reached) { s_reached_g = s_pv_g; s_have_reached = true; }
            set_valves(false, false, false, now_ms);
            set_state(DISPENSE_SETTLING, now_ms);
            break;
        }

        /* PID drives the fine valve ONLY: the coarse relay has been closed and
         * is binary, so nothing here ever writes an analog value to it. */
        run_fine_control(now_ms);
        break;
    }

    case DISPENSE_SETTLING: {
        if (!weight_is_fresh(now_ms)) break;
        if ((now_ms - s_state_entry_ms) < s_cfg.settle_ms) break;

        int32_t error = s_pv_g - s_active_target_g;

        if (s_cfg.completion_mode == DISPENSE_COMPLETION_RAMP_TEST) {
            /* The target was reached, both valves are closed, and the settle
             * window has elapsed. Later rising readings belong to the
             * simulator, so they are ignored for this job rather than judged:
             * judging them would fail every job with a behaviour the
             * controller caused correctly. */
            finish_active_job(JOB_COMPLETE, NULL, now_ms);
            set_state(DISPENSE_COMPLETE, now_ms);
            break;
        }

        if (error > s_cfg.max_overshoot_g) {
            fail_job("overshoot after settling", SAFETY_OVERSHOOT, now_ms);
            break;
        }
        if (error >= -s_cfg.tolerance_g) {
            finish_active_job(JOB_COMPLETE, NULL, now_ms);
            set_state(DISPENSE_COMPLETE, now_ms);
            break;
        }

        /* Short of target after settling: bounded fine correction. */
        if (s_corrections >= s_cfg.correction_limit) {
            fail_job("correction limit reached", SAFETY_TIMEOUT, now_ms);
            break;
        }
        s_corrections++;
        s_window_start_ms = now_ms;
        s_last_pid_ms = now_ms;
        memset(&s_pid, 0, sizeof(s_pid));
        ESP_LOGI(TAG, "Fine correction %u/%u | short by %ld g",
                 (unsigned)s_corrections, (unsigned)s_cfg.correction_limit,
                 (long)(-error));
        set_state(DISPENSE_FINE_CORRECTION, now_ms);
        break;
    }

    case DISPENSE_FINE_CORRECTION: {
        if (!weight_is_fresh(now_ms)) break;
        if (job_expired(now_ms)) {
            fail_job("max duration exceeded", SAFETY_TIMEOUT, now_ms);
            break;
        }
        if (job_overshot()) {
            fail_job("overshoot", SAFETY_OVERSHOOT, now_ms);
            break;
        }

        int32_t error = s_active_target_g - s_pv_g;
        if (error <= s_cfg.tolerance_g) {
            if (!s_have_reached) { s_reached_g = s_pv_g; s_have_reached = true; }
            set_valves(false, false, false, now_ms);
            set_state(DISPENSE_SETTLING, now_ms);
            break;
        }
        run_fine_control(now_ms);
        break;
    }

    case DISPENSE_COMPLETE:
        set_state(DISPENSE_NEXT_JOB, now_ms);
        transient = true;
        break;

    case DISPENSE_NEXT_JOB:
        /* The next queued job starts on its own — that is the whole point of
         * the queue. With nothing queued the machine parks in IDLE, and in
         * RAMP_TEST it also parks (in NEXT_JOB) until the gate opens, so the
         * handover cannot hand a job a part-filled scale. */
        if (job_queue_depth() == 0U) {
            set_state(DISPENSE_IDLE, now_ms);
            break;
        }
        if (!next_job_may_start()) {
            log_waiting_for_zero(now_ms);
            break;      /* stay here; the gate is re-checked every tick */
        }
        s_await_zero = false;
        s_cycle_reset_pending = false;
        set_state(DISPENSE_LOAD_JOB, now_ms);
        transient = true;
        break;

    case DISPENSE_FAULT:
    case DISPENSE_EMERGENCY_STOP:
    default:
        set_valves(false, false, false, now_ms);
        break;
        }

        if (!transient) break;
    }

    /* Periodic control line. One per second while a valve can still move:
     * enough to follow the loop from the serial monitor, far too little to be
     * spam. This is what makes "is the PID doing something sensible?" a
     * question the log answers. */
    if (s_active_job_id != 0U && !s_paused &&
        s_cfg.control_log_period_ms > 0U &&
        (now_ms - s_last_ctrl_log_ms) >= s_cfg.control_log_period_ms) {
        s_last_ctrl_log_ms = now_ms;
        ESP_LOGI(TAG, "control | setpoint=%ld g | PV=%ld g | error=%ld g | "
                      "duty=%d%% | state=%s | relay1=%d relay2=%d",
                 (long)s_active_target_g, (long)s_pv_g,
                 (long)(s_active_target_g - s_pv_g),
                 (int)(s_duty * 100.0f), dispense_state_name(s_state),
                 (int)s_coarse_on, (int)s_fine_on);
    }
}

/* ---------------------------------------------------------------------------
 * Controls
 * ------------------------------------------------------------------------ */

void dispense_controller_emergency_stop(uint32_t now_ms)
{
    ESP_LOGW(TAG, "EMERGENCY STOP requested");
    set_valves(false, false, false, now_ms);
    finish_active_job(JOB_FAILED, "emergency stop", now_ms);

    uint32_t cancelled = job_queue_cancel_all();
    if (cancelled > 0U) {
        ESP_LOGW(TAG, "Emergency stop cancelled %u queued job(s)", (unsigned)cancelled);
    }

    safety_manager_raise(SAFETY_EMERGENCY_STOP, now_ms);
    set_state(DISPENSE_EMERGENCY_STOP, now_ms);
    s_paused = false;
}

bool dispense_controller_clear_fault(uint32_t now_ms)
{
    (void)now_ms;

    if (safety_manager_fault() == SAFETY_EMERGENCY_STOP) {
        /* An emergency stop is released, not cleared: clearing it would let a
         * generic "clear fault" button silently restart dispensing. */
        if (!safety_manager_release_estop()) return false;
        ESP_LOGI(TAG, "Emergency stop released - machine idle");
        set_state(DISPENSE_IDLE, now_ms);
        return true;
    }

    if (!safety_manager_clear()) return false;

    if (s_state == DISPENSE_FAULT) {
        set_state(DISPENSE_IDLE, now_ms);
    }
    set_valves(false, false, false, now_ms);
    return true;
}

bool dispense_controller_pause(uint32_t now_ms)
{
    if (s_paused) return true;
    if (s_active_job_id == 0U) {
        ESP_LOGW(TAG, "Pause ignored - no job running");
        return false;
    }

    /* Safe by construction: pausing closes both valves and freezes the state
     * machine where it stands, so the job resumes from the same place. */
    s_paused = true;
    set_valves(false, false, false, now_ms);
    ESP_LOGI(TAG, "Dispense paused | job %u at %s", (unsigned)s_active_job_id,
             dispense_state_name(s_state));
    return true;
}

bool dispense_controller_resume(uint32_t now_ms)
{
    if (!s_paused) return true;

    s_paused = false;
    /* Re-check freshness before anything reopens: the pause may have outlasted
     * the weight link. WAIT_FOR_SCALE is the honest place to resume because it
     * gates on a fresh reading. */
    if (s_active_job_id != 0U) {
        set_state(DISPENSE_WAIT_FOR_SCALE, now_ms);
    }
    ESP_LOGI(TAG, "Dispense resumed");
    return true;
}

bool dispense_controller_is_paused(void)
{
    return s_paused;
}

void dispense_controller_cancel_all(uint32_t now_ms)
{
    set_valves(false, false, false, now_ms);
    finish_active_job(JOB_CANCELLED, "cancelled", now_ms);
    (void)job_queue_cancel_all();
    if (s_state != DISPENSE_FAULT && s_state != DISPENSE_EMERGENCY_STOP) {
        set_state(DISPENSE_IDLE, now_ms);
    }
}

/* ---------------------------------------------------------------------------
 * Observation
 * ------------------------------------------------------------------------ */

dispense_state_t dispense_controller_state(void)        { return s_state; }
uint32_t dispense_controller_job_id(void)               { return s_active_job_id; }
int32_t  dispense_controller_target_g(void)             { return s_active_target_g; }
/* These three are read by the web API from a different task, so they take the
 * same lock the writer uses rather than exposing the raw fields. */
int32_t dispense_controller_last_weight_g(void)
{
    int32_t g;
    portENTER_CRITICAL(&s_weight_lock);
    g = s_last_weight_g;
    portEXIT_CRITICAL(&s_weight_lock);
    return g;
}

uint32_t dispense_controller_last_weight_ms(void)
{
    uint32_t ms;
    portENTER_CRITICAL(&s_weight_lock);
    ms = s_last_weight_ms;
    portEXIT_CRITICAL(&s_weight_lock);
    return ms;
}

bool dispense_controller_have_weight(void)
{
    bool have;
    portENTER_CRITICAL(&s_weight_lock);
    have = s_have_weight;
    portEXIT_CRITICAL(&s_weight_lock);
    return have;
}
bool     dispense_controller_coarse_on(void)            { return s_coarse_on; }
bool     dispense_controller_fine_on(void)              { return s_fine_on; }
float    dispense_controller_duty(void)                 { return s_duty; }
void dispense_controller_pid_terms(float *p, float *i, float *d)
{
    if (p) *p = s_p_term;
    if (i) *i = s_i_term;
    if (d) *d = s_d_term;
}
uint32_t dispense_controller_corrections(void)          { return s_corrections; }

bool dispense_controller_waiting_for_zero(void)
{
    return (s_cfg.completion_mode == DISPENSE_COMPLETION_RAMP_TEST) && s_await_zero;
}

uint32_t dispense_controller_cycle(void)
{
    uint32_t c;
    portENTER_CRITICAL(&s_weight_lock);
    c = s_sim_cycle;
    portEXIT_CRITICAL(&s_weight_lock);
    return c;
}

uint32_t dispense_controller_cycle_resets(void) { return s_cycle_resets; }

bool dispense_controller_simulated(void)
{
    bool sim;
    portENTER_CRITICAL(&s_weight_lock);
    sim = s_simulated;
    portEXIT_CRITICAL(&s_weight_lock);
    return sim;
}

int32_t dispense_controller_error_g(void)
{
    return s_active_target_g - s_pv_g;
}
