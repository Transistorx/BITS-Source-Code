#ifndef DUAL_DISPENSE_CONTROLLER_H
#define DUAL_DISPENSE_CONTROLLER_H

#include "esp_err.h"
#include "job_queue.h"
#include "weight_receiver.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Single-scale, two-nozzle dispense controller.
 *
 * The field hardware has ONE physical CAS scale and TWO dispensing relays.
 * The operator moves the scale between nozzles. The controller therefore
 * enforces a single SCALE OWNER at all times: at most one job may hold the
 * scale, and while it does the other nozzle is never energised. Ownership is
 * released only after the owner reaches a terminal state (COMPLETE / FAULT /
 * CANCELLED), its relay is OFF, and a final CAS sample has been captured.
 *
 * Control is staged: COARSE -> FINE -> MICRO -> SETTLING, with progressively
 * more conservative output near the target. The local PID runs on this
 * controller (never on the server).
 *
 * Profiles are a persistent four-target table (5000 / 10000 / 15000 / 20000 g).
 * Entries are selected by exact target match at job start and are NEVER
 * consumed. There is no target_g=0 fallback — a missing profile is a refusal,
 * not a silent bleed of another target's tuning.
 * ------------------------------------------------------------------------ */

/* Channel / nozzle states. WAITING_FOR_SCALE_MOVE is entered when a job is
 * ready but the scale is owned by the other channel, or the operator has not yet
 * confirmed (READY, CONTRACT 9.11) that the scale is positioned at this pump. */
typedef enum {
    DCH_IDLE = 0,
    DCH_WAITING_FOR_SCALE_MOVE, /* job ready; waiting for scale ownership / operator READY */
    DCH_WAIT_WEIGHT,     /* owns the scale; waiting for a fresh CAS sample */
    DCH_DISPENSING,      /* staged dispense in progress */
    DCH_SETTLING,        /* relay OFF; waiting for the reading to settle */
    DCH_COMPLETE,
    DCH_PAUSED,
    DCH_FAULT
} dual_channel_state_t;
/* Pre-9.11 spelling, kept so older call sites and tests keep compiling. */
#define DCH_WAITING_SCALE DCH_WAITING_FOR_SCALE_MOVE

/* Staged dispense phases inside DCH_DISPENSING / DCH_SETTLING. */
typedef enum {
    DCH_STAGE_NONE = 0,
    DCH_STAGE_COARSE,     /* large error: high duty, long pulses */
    DCH_STAGE_FINE,       /* medium error: moderate duty */
    DCH_STAGE_MICRO,      /* small error: very conservative, short pulses */
    DCH_STAGE_SETTLING    /* relay OFF; waiting for fresh post-pulse CAS sample */
} dispense_stage_t;

/* Which nozzle owns the physical scale right now. */
typedef enum {
    SCALE_OWNER_NONE = 0,
    SCALE_OWNER_CH1 = 1,
    SCALE_OWNER_CH2 = 2
} scale_owner_t;

const char *dual_dispense_state_name(dual_channel_state_t state);
const char *dispense_stage_name(dispense_stage_t stage);
const char *scale_owner_name(scale_owner_t owner);

typedef struct {
    uint8_t channel_id;
    uint32_t active_job_id;
    int32_t target_g, current_weight_g, start_weight_g, error_g;
    uint32_t weight_age_ms, queue_depth, profile_version, elapsed_ms;
    char profile_id[65];
    float kp, ki, kd, p_term, i_term, d_term, output;
    int32_t tolerance_g, max_overshoot_g;
    uint32_t window_ms, min_on_ms, min_off_ms, max_duration_ms;
    bool have_weight, stable, relay_on, paused, manual;
    dual_channel_state_t state;
    /* Staged-dispense observability. */
    dispense_stage_t stage;
    scale_owner_t scale_owner;
    bool owns_scale;
    bool awaiting_operator_ready;
    int32_t inflight_comp_g;
    uint32_t settle_time_ms;
    const char *fault;
} dual_channel_snapshot_t;

esp_err_t dual_dispense_controller_init(void);

/* Feed is ONE physical CAS scale (CONTRACT 9.11). The single-scale sample
 * (weight_g, stable, scale_id, boot_id, sequence) goes to the scale OWNER only.
 * With no owner it updates only the shared scale view and NEVER renews either
 * pump's weight, so both pumps stay stale and fail safe. The sender's UART tag is
 * not a pump; per-channel fields of a legacy frame are ignored. Simulated samples
 * are rejected. A duplicate/older sequence is dropped without moving freshness. */
void dual_dispense_controller_on_weight(const weight_msg_t *msg, uint32_t now_ms);

/* ---- Single-scale configuration (CONTRACT 9.11) ----
 * Every number is an UNVALIDATED conservative default; app_main loads the NVS
 * values and installs them here. Zero in start_max_g disables that check. */
typedef struct {
    uint32_t ready_stable_ms;        /* stable run required before READY            (3000)   */
    uint32_t ready_max_age_ms;       /* READY older than this is refused            (120000) */
    bool     reconfirm_same_pump;    /* READY also needed between jobs on one pump  (true)   */
    bool     legacy_ready;           /* accept a READY with only {channel}          (true)   */
    uint32_t scale_move_timeout_ms;  /* waiting for READY longer than this          (600000) */
    int32_t  start_max_g;            /* plausibility: start weight <= this, 0 = off (500)    */
    int32_t  drop_trip_g;            /* SCALE_MOVED drop threshold                  (200)    */
    uint32_t np_learn_ms;            /* NO_PROGRESS learn window, relay-ON time     (10000)  */
    int32_t  np_abs_floor_g;         /* first-window rise below this fails at once  (10)     */
    int32_t  scale_res_g;            /* scale resolution step                       (2)      */
} dual_scale_cfg_t;
void dual_dispense_controller_scale_cfg_default(dual_scale_cfg_t *out);
void dual_dispense_controller_set_scale_cfg(const dual_scale_cfg_t *cfg);
void dual_dispense_controller_get_scale_cfg(dual_scale_cfg_t *out);

/* READY with identity (CONTRACT 9.11). Pure refusal codes; a refused READY changes
 * nothing. READY_OK is the only result that moves a job toward energising. */
typedef enum {
    READY_OK = 0,
    READY_BAD_CHANNEL,
    READY_NOT_WAITING,          /* no job is waiting for this channel's READY */
    READY_JOB_MISMATCH,         /* job_id is not the job waiting on this channel */
    READY_SCALE_BOOT_MISMATCH,  /* scale_boot_id is not the live scale boot_id */
    READY_STALE,                /* READY older than ready_max_age */
    READY_SCALE_OFFLINE,        /* no fresh weight from the scale */
    READY_SCALE_UNSTABLE,       /* not stable for ready_stable_ms */
    READY_SCALE_BUSY,           /* another job owns the scale */
    READY_BLOCKED,              /* E-Stop, safety fault, channel fault or manual hold */
    READY_START_WEIGHT,         /* start weight above start_max_g */
    READY_NO_PROFILE,           /* no profile for the target: the job was failed */
    READY_LEGACY_DISABLED,      /* identity missing and legacy_ready is off */
    READY_MALFORMED             /* only one of job_id / scale_boot_id given */
} dual_ready_result_t;
const char *dual_ready_result_name(dual_ready_result_t r);

typedef struct {
    bool     has_job_id, has_boot;
    uint32_t job_id;
    char     scale_boot_id[9];
    uint32_t age_ms;            /* time since the READY was received */
} dual_ready_req_t;
dual_ready_result_t dual_dispense_controller_scale_ready(uint8_t channel_id,
                                                         const dual_ready_req_t *req,
                                                         uint32_t now_ms);
/* READY carrying only {channel} (migration). READY_LEGACY_DISABLED while legacy_ready is off. */
dual_ready_result_t dual_dispense_controller_legacy_ready(uint8_t channel_id, uint32_t now_ms);

/* Shared scale view + station, for the retained status. */
typedef struct {
    bool     online;            /* a sample no older than the weight timeout */
    bool     stable;
    int32_t  weight_g;
    uint32_t age_ms;
    char     scale_id[17];
    char     boot_id[9];        /* "" = legacy / unknown */
    bool     in_transit;        /* scale not confirmed at a pump, or a move is awaited */
    uint8_t  needs_station;     /* 0 = none, else the pump (1/2) the scale must be moved to */
    uint32_t await_job_id;      /* job waiting for READY, 0 = none */
    uint8_t  await_channel;     /* its pump, 0 = none */
} dual_scale_info_t;
void dual_dispense_controller_scale_info(uint32_t now_ms, dual_scale_info_t *out);
void dual_dispense_controller_tick(uint32_t now_ms);

/* Manual relay (pump) control — no dispense job required. Turns the channel's
 * relay on/off directly through the mutual-exclusion-guarded relay() helper.
 * Refuses ON when a job is active on either channel, when E-Stop is latched,
 * or when a safety fault is active. Stop is always allowed. */
esp_err_t dual_dispense_controller_manual_relay(uint8_t channel_id, bool on, uint32_t now_ms);

/* ---- Job submission ---- */
esp_err_t dual_dispense_controller_add_job(uint8_t channel_id, int32_t target_g,
                                           uint8_t priority, uint32_t *job_id);
esp_err_t dual_dispense_controller_add_global_job(int32_t target_g,
    uint8_t priority, uint32_t *job_id);
esp_err_t dual_dispense_controller_add_material_global_job(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t *job_id);
esp_err_t dual_dispense_controller_add_server_job(int32_t target_g,
    uint8_t priority, uint32_t command_id, uint32_t *job_id);
esp_err_t dual_dispense_controller_add_material_server_job(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t command_id, uint32_t *job_id);
/* Same, but the job carries its own immutable PID gains. The server pins the
 * exact tuning version the operator chose onto every JOB command; passing it
 * here is what makes that version the one that runs. `pin` may be NULL for a
 * legacy command with no pin — the job then falls back to the channel's
 * per-target profile slot at start time. */
esp_err_t dual_dispense_controller_add_material_server_job_pinned(uint8_t material_id,
    int32_t target_g, uint8_t priority, uint32_t command_id,
    const job_profile_t *pin, uint32_t *job_id);

/* ---- Operator control ---- */
bool dual_dispense_controller_pause(uint8_t channel_id, uint32_t now_ms);
bool dual_dispense_controller_resume(uint8_t channel_id, uint32_t now_ms);
/* Stop this channel's ACTIVE job only. Never touches the waiting backlog. */
bool dual_dispense_controller_cancel(uint8_t channel_id, uint32_t now_ms);
/* Cancel exactly one job by id, queued or running. Sibling jobs — including
 * ones with a duplicate target_g — are left untouched. */
bool dual_dispense_controller_cancel_job(uint32_t job_id, uint32_t now_ms);
void dual_dispense_controller_emergency_stop(uint32_t now_ms);

/* LEGACY READY (only {channel}): operator confirms the scale is positioned at
 * this pump. Honoured ONLY while cfg.legacy_ready is on (migration) and logged; it
 * skips the job_id/scale_boot_id/stability checks of dual_dispense_controller_scale_ready
 * but still needs a waiting job, a free scale and no ESTOP/fault/manual hold.
 * Returns true when the confirmation was accepted and the job may proceed. */
bool dual_dispense_controller_operator_ready(uint8_t channel_id, uint32_t now_ms);

/* ---- Observation ---- */
bool dual_dispense_controller_snapshot(uint8_t channel_id, uint32_t now_ms,
                                       dual_channel_snapshot_t *out);
bool dual_dispense_controller_clear_fault(uint8_t channel_id, uint32_t now_ms);
scale_owner_t dual_dispense_controller_scale_owner(void);
dispense_stage_t dual_dispense_controller_stage(uint8_t channel_id);

/* ---- Profile management ----
 *
 * Persistent four-target table. Entries are NEVER consumed after use. Only an
 * exact target_g match selects a profile; there is no target_g=0 fallback.
 * Canonical targets are 5000 / 10000 / 15000 / 20000 grams.
 */

/* Store (or replace) the profile for target_g. Returns false on any invalid
 * parameter or when target_g is not one of the canonical targets. */
bool dual_dispense_controller_set_pending_profile(uint8_t channel_id,
    const char *profile_id, uint32_t version, int32_t target_g,
    float kp, float ki, float kd, int32_t tolerance_g,
    int32_t max_overshoot_g, uint32_t max_duration_ms,
    uint32_t window_ms, uint32_t min_on_ms, uint32_t min_off_ms);

/* Full staged-profile variant with per-stage thresholds and timings. */
bool dual_dispense_controller_set_pending_profile_staged(uint8_t channel_id,
    const char *profile_id, uint32_t version, int32_t target_g,
    float kp, float ki, float kd, int32_t tolerance_g,
    int32_t max_overshoot_g, uint32_t max_duration_ms,
    uint32_t window_ms, uint32_t min_on_ms, uint32_t min_off_ms,
    int32_t coarse_threshold_g, int32_t fine_threshold_g,
    int32_t micro_threshold_g,
    uint32_t coarse_min_on_ms, uint32_t fine_min_on_ms,
    uint32_t micro_min_on_ms,
    uint32_t settle_time_ms, int32_t inflight_comp_g);

/* M2/M3/M4: effective-profile consistency shared by the pin parser (telemetry
 * client) and the store path above, so both apply identical rules. Pure, no
 * state. Returns NULL when acceptable, else a static refusal text (<= 79 chars,
 * starts with the PIN_* code). Rules, in order: min_on/min_off <= window;
 * micro <= fine <= coarse thresholds (each <= 100000) and stage min_on <= 10000;
 * each stage can pulse at its duty ceiling (coarse 1.0, fine 0.5, micro 0.15);
 * fine/micro min_off must leave a gap at the ceiling (min_off <= window -
 * round(ceiling*window)), otherwise the valve is held ON all window and the
 * ceiling is defeated; tolerance_g < target_g; max_overshoot_g <=
 * min(target_g, DCH_ABS_MAX_OVERSHOOT_G). */
typedef struct {
    int32_t target_g, tolerance_g, max_overshoot_g;
    uint32_t window_ms, min_on_ms, min_off_ms;
    int32_t coarse_threshold_g, fine_threshold_g, micro_threshold_g;
    uint32_t coarse_min_on_ms, fine_min_on_ms, micro_min_on_ms;
} dual_profile_limits_t;
const char *dual_dispense_profile_consistency_error(const dual_profile_limits_t *p);

/* PLACEHOLDER pending operator decision (rationale: the smallest canonical
 * target is 5000 g; an overshoot allowance beyond that is never a tolerance). */
#define DCH_ABS_MAX_OVERSHOOT_G 5000

/* Install the device-default profile for target_g (used when the server has
 * no profile for that target). Defaults are stored persistently like any
 * other profile — they are not a runtime fallback bleed. */
bool dual_dispense_controller_set_pending_default(uint8_t channel_id, int32_t target_g);

/* Explicitly clear a stored profile for target_g (or all when target_g < 0).
 * This is an operator action, never automatic. */
void dual_dispense_controller_discard_pending_profile(uint8_t channel_id,
                                                      int32_t target_g);

void dual_dispense_controller_clear_emergency_stop(void);

#endif
