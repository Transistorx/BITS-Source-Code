#ifndef DISPENSE_CONTROLLER_H
#define DISPENSE_CONTROLLER_H

#include "esp_err.h"
#include "weight_receiver.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Explicit, non-blocking dispensing state machine.
 *
 * Every relay command in the dispense path is issued from exactly one place —
 * the single switch in dispense_controller_tick() — so there are no scattered
 * conditionals that can disagree about which valve should be open. The tick
 * never blocks and never waits: it reads the latest state, decides, and
 * returns, which is what lets it run inside the supervisor loop alongside the
 * weight failsafe.
 *
 *   IDLE ──queue non-empty──► LOAD_JOB ──► WAIT_FOR_SCALE
 *                                              │ fresh weight
 *                                              ▼
 *                                        COARSE_DISPENSE ──near target──► FINE_DISPENSE
 *                                                                              │ in tolerance
 *                                                                              ▼
 *                            ◄──NEXT_JOB── COMPLETE ◄──in tolerance── SETTLING
 *                                                          │ under target
 *                                                          ▼
 *                                                 FINE_CORRECTION ──► SETTLING
 *
 * FAULT and EMERGENCY_STOP are reachable from every state: both close every
 * valve and are latched by the safety manager.
 * ------------------------------------------------------------------------ */

typedef enum {
    DISPENSE_IDLE = 0,
    DISPENSE_LOAD_JOB,
    DISPENSE_WAIT_FOR_SCALE,
    DISPENSE_COARSE_DISPENSE,
    DISPENSE_FINE_DISPENSE,
    DISPENSE_SETTLING,
    DISPENSE_FINE_CORRECTION,
    DISPENSE_COMPLETE,
    DISPENSE_NEXT_JOB,
    DISPENSE_FAULT,
    DISPENSE_EMERGENCY_STOP
} dispense_state_t;

/* Coarse/high-flow and fine/low-flow outputs. Relay 1 and Relay 2 on this
 * board (AO1 / AO2), matching the requested semantics. */
#ifndef CONFIG_WEIGHT_DEMO_COARSE_RELAY_INDEX
#define CONFIG_WEIGHT_DEMO_COARSE_RELAY_INDEX 0
#endif
#ifndef CONFIG_WEIGHT_DEMO_FINE_RELAY_INDEX
#define CONFIG_WEIGHT_DEMO_FINE_RELAY_INDEX 1
#endif
#define DISPENSE_COARSE_RELAY_INDEX CONFIG_WEIGHT_DEMO_COARSE_RELAY_INDEX
#define DISPENSE_FINE_RELAY_INDEX   CONFIG_WEIGHT_DEMO_FINE_RELAY_INDEX

/* How a job is judged once the target is reached.
 *
 * PROCESS is the real-machine behaviour: hold at the setpoint, let the reading
 * settle, and treat an overshoot beyond the limit as a fault. It is correct
 * whenever the weight can actually fall or hold — a real CAS scale, or the
 * DYNAMIC valve-coupled vessel.
 *
 * RAMP_TEST is for the independent ramp simulator, where readings keep rising
 * on their own after the valves close and no amount of closing will stop them.
 * Judging that stream with the process rules would fail every job with
 * "overshoot after settling" for a behaviour the controller caused correctly.
 * So: target/tolerance reached -> both relays OFF -> COMPLETE, with later
 * rising readings ignored for that job.
 *
 * This is an EXPLICIT, logged selection, never inferred from the payload — a
 * real scale must not be able to inherit the relaxed overshoot rules by
 * sending a field. */
typedef enum {
    DISPENSE_COMPLETION_PROCESS = 0,
    DISPENSE_COMPLETION_RAMP_TEST
} dispense_completion_mode_t;

typedef struct {
    int32_t  target_g;            /* requested dispense target */
    dispense_completion_mode_t completion_mode;
    int32_t  tolerance_g;         /* |weight - target| accepted as done */
    int32_t  coarse_transition_g; /* close coarse when this close to target */
    uint32_t settle_ms;           /* valves closed before judging the result */
    uint32_t max_duration_ms;     /* whole-job budget, waiting included */
    int32_t  max_overshoot_g;     /* beyond this over target is a fault */
    int32_t  ramp_top_g;          /* top of the simulator ramp (reset heuristic) */
    uint32_t stale_timeout_ms;    /* no fresh weight for this long -> fault */
    uint32_t correction_limit;    /* max FINE_CORRECTION attempts per job */
    uint32_t window_ms;           /* time-proportional window */
    uint32_t min_on_ms;           /* anti-chatter: shortest useful burst */
    uint32_t min_off_ms;          /* anti-chatter: shortest useful gap */
    float    kp, ki, kd;          /* PID gains, fine region only */
    float    integral_max;        /* anti-windup clamp */
    /* Milliseconds between periodic PID control lines while dispensing. 0
     * disables them. One per second by default: enough to follow the loop,
     * far too little to be spam. */
    uint32_t control_log_period_ms;
} dispense_config_t;

const char *dispense_completion_mode_name(dispense_completion_mode_t mode);

/* ---- Pure control math (unit-tested in the QEMU suite) ---- */

typedef struct { float kp, ki, kd; } dispense_pid_gains_t;

typedef struct {
    float   integral;
    int32_t last_error;
    bool    have_last;
} dispense_pid_state_t;

/* Time-proportional duty in [0,1] for the fine valve. The error is in grams
 * and POSITIVE when more product is needed. The output is a duty cycle, never
 * a valve command: a solenoid is binary, so the duty is realised by the
 * windowed output below. Integral windup is clamped to +/-integral_max. */
float dispense_pid_duty(const dispense_pid_gains_t *gains, dispense_pid_state_t *state,
                        int32_t error_g, uint32_t dt_ms, float integral_max);

/* Windowed (time-proportional) output. `elapsed_ms` is the position inside the
 * current window. Returns true when the valve should be open.
 *
 * Anti-chatter is structural rather than a timer: a burst shorter than
 * min_on_ms is dropped entirely, and a gap shorter than min_off_ms is closed
 * by extending the burst to the whole window. Either way the valve sees at
 * most one transition per window. */
bool dispense_window_output(uint32_t elapsed_ms, float duty, uint32_t window_ms,
                            uint32_t min_on_ms, uint32_t min_off_ms);

const char *dispense_state_name(dispense_state_t state);

/* ---- Lifecycle ---- */

/* Populates a config from the Kconfig thresholds, with the documented defaults
 * as fallbacks so the component also builds and tests standalone. One source
 * of truth: app_main and the QEMU suite both start from this. */
dispense_config_t dispense_config_default(void);

esp_err_t dispense_controller_init(const dispense_config_t *config);

/* The weight sink registered with weight_receiver. Runs in the WebSocket
 * client task; only stores the reading. */
void dispense_controller_on_weight(const weight_msg_t *msg, uint32_t now_ms);

/* One step of the state machine. Non-blocking. */
void dispense_controller_tick(uint32_t now_ms);

/* ---- Controls ---- */
void dispense_controller_emergency_stop(uint32_t now_ms);
bool dispense_controller_clear_fault(uint32_t now_ms);
bool dispense_controller_pause(uint32_t now_ms);
bool dispense_controller_resume(uint32_t now_ms);
bool dispense_controller_is_paused(void);

/* Cancels the running job and every queued one, leaving the machine IDLE
 * with both valves closed. */
void dispense_controller_cancel_all(uint32_t now_ms);

/* ---- Observation ---- */
dispense_state_t dispense_controller_state(void);
uint32_t dispense_controller_job_id(void);
int32_t  dispense_controller_target_g(void);
int32_t  dispense_controller_last_weight_g(void);
uint32_t dispense_controller_last_weight_ms(void);
bool     dispense_controller_have_weight(void);
bool     dispense_controller_coarse_on(void);
bool     dispense_controller_fine_on(void);
/* Current fine-valve duty in [0,1]; 0 outside the fine region. */
float    dispense_controller_duty(void);
void     dispense_controller_pid_terms(float *p, float *i, float *d);
uint32_t dispense_controller_corrections(void);

/* ---- Ramp / simulation observation ---- */

/* True while the controller will not start the next queued job because it is
 * waiting for a fresh simulation cycle at a confirmed 0 g reading. */
bool dispense_controller_waiting_for_zero(void);

/* Latest simulation_cycle seen, and how many resets have been observed. */
uint32_t dispense_controller_cycle(void);
uint32_t dispense_controller_cycle_resets(void);

/* True when the last accepted reading came from the simulator. */
bool dispense_controller_simulated(void);

/* PID setpoint / process value / error, for the periodic control line and the
 * status API. Error is positive when more product is needed. */
int32_t dispense_controller_error_g(void);

/* Notified whenever the valve state changes, so a transport can publish it to
 * the weight sender (which needs it to drive the dynamic virtual scale). The
 * controller stays free of any networking dependency. `reset` marks a job
 * boundary, where the sender should empty its virtual vessel. */
typedef void (*dispense_valve_notify_fn)(bool coarse, bool fine, bool reset);
void dispense_controller_set_valve_notify(dispense_valve_notify_fn fn);

#endif /* DISPENSE_CONTROLLER_H */
