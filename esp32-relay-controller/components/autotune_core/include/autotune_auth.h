/* Auto-tune two-stage authorization (Phase 3, SOFTWARE / SIMULATION ONLY).
 *
 * *** PURE LOGIC. NOT WIRED INTO ANY CONTROL PATH. NO RELAY ACCESS. ***
 *
 *   IDLE --ARM--> ARMED --START(token)--> RUNNING (handoff to the sequencer)
 *   ARMED/RUNNING --any cancel trigger--> CANCELLED (relay request OFF)
 *
 * KNOWN LIMITATION (doc 13): firmware cannot prove WHO a human is. `auth_ok`
 * is a boolean supplied by the transport layer after IT authenticated the
 * sender; a compromised or mis-configured transport defeats it. The explicit
 * confirmations (scale beneath the nozzle, container present, material) are
 * operator ATTESTATIONS carried in the ARM request, not physical checks.
 *
 * ARM needs: auth_ok, not retained, unique id, ttl within (0, max_cmd_ttl_ms],
 * not expired (age_ms < ttl_ms), channel 1|2, material_selected, material_id
 * == channel (fixed M1<->CH1/Pump1, M2<->CH2/Pump2), scale_under_nozzle,
 * container_present and every READY boolean (no active job, ownership free or
 * owned by this channel, no safety fault, no E-stop, weight fresh and valid,
 * relays all off, no manual pump).
 * START is a SEPARATE command: same envelope checks plus hw_enable, a live
 * (unexpired) ARM, the ARM token, matching channel/material, matching boot
 * epoch and the READY booleans re-evaluated at START time.
 *
 * COMMAND IDS. Fixed seen-ring of AT_AUTH_SEEN_RING (16) ids. A duplicate of a
 * ring id is refused. When the ring wraps, the evicted (oldest) id raises
 * `floor_id` to max(floor, evicted); every id <= floor_id is refused as a
 * possible replay. Ids must therefore be issued strictly increasing per
 * source; a fresh but non-monotonic id below the floor is refused (fail safe).
 * Ids are consumed once they pass the envelope checks, even if a later gate
 * refuses the command, so a refused command cannot be replayed either.
 * Retained messages (caller flag) are refused. CANCEL is always accepted.
 *
 * CANCEL TRIGGERS (tick, state ARMED or RUNNING): boot epoch change, E-stop,
 * safety fault, scale ownership lost, manual pump command, active job, comm
 * loss, weight link lost, stale/invalid weight, ARM window expiry, explicit
 * CANCEL. Cancel reasons dominate: call autotune_auth_tick() BEFORE the
 * sequencer tick in the same cycle. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_AUTH_SEEN_RING 16

/* One reason code space for refusals (gate that failed), cancellations and
 * sequencer failures. */
typedef enum {
    AT_R_NONE = 0,
    /* refusals: which gate failed */
    AT_R_ID_INVALID, AT_R_RETAINED, AT_R_TTL_INVALID, AT_R_CMD_EXPIRED,
    AT_R_AUTH_NOT_OK, AT_R_ID_DUPLICATE, AT_R_ID_BELOW_FLOOR,
    AT_R_HW_NOT_APPROVED, AT_R_BAD_STATE, AT_R_NO_ARM, AT_R_ARM_EXPIRED,
    AT_R_TOKEN_MISMATCH, AT_R_WRONG_CHANNEL, AT_R_WRONG_MATERIAL, AT_R_RESTART_EPOCH,
    AT_R_CHANNEL_INVALID, AT_R_NO_MATERIAL_SELECTED, AT_R_MATERIAL_MISMATCH,
    AT_R_NO_SCALE_UNDER_NOZZLE, AT_R_NO_CONTAINER,
    AT_R_JOB_ACTIVE, AT_R_OWNERSHIP, AT_R_SAFETY_FAULT, AT_R_ESTOP,
    AT_R_WEIGHT_NOT_FRESH, AT_R_RELAYS_NOT_OFF, AT_R_MANUAL_PUMP,
    /* cancellations */
    AT_R_USER_CANCEL, AT_R_WEIGHT_STALE, AT_R_WEIGHT_LINK_LOST, AT_R_RESTART,
    AT_R_COMM_LOSS, AT_R_OWNERSHIP_LOST, AT_R_AUTH_LOST,
    /* sequencer failures */
    AT_R_LIMIT_PULSE_ON, AT_R_LIMIT_CUM_ON, AT_R_LIMIT_WALL, AT_R_LIMIT_DELIVERED,
    AT_R_NO_PROGRESS, AT_R_WEIGHT_DECREASING, AT_R_LEAK_OFF_RISE, AT_R_SURGE,
    AT_R_SETTLE_TIMEOUT, AT_R_SENDER_RESTART, AT_R_BUFFER_FULL,
    AT_R_INSUFFICIENT_TRIALS, AT_R_VALIDATION, AT_R_BAD_CONFIG,
} autotune_reason_t;

typedef enum {
    AT_AUTH_IDLE = 0,
    AT_AUTH_ARMED,
    AT_AUTH_RUNNING,
    AT_AUTH_CANCELLED,
} autotune_auth_state_t;

typedef struct {
    uint32_t max_cmd_ttl_ms;     /* upper bound for a command ttl (required, >0) */
    uint32_t arm_window_ms;      /* ARM auto-expiry (required, >0) */
    bool     hw_enable;          /* hardware energization approved. FALSE = START refused */
} autotune_auth_cfg_t;

typedef struct {
    bool     auth_ok;            /* transport authenticated the sender */
    bool     retained;           /* message was a retained delivery */
    uint32_t cmd_id;             /* unique, strictly increasing per source */
    uint32_t ttl_ms;
    uint32_t age_ms;             /* age when received (transport timestamp) */
    uint32_t rx_ms;              /* receive time, caller monotonic ms */
    uint32_t boot_epoch;         /* device boot counter / epoch now */
} autotune_cmd_t;

typedef struct {
    bool no_active_job, ownership_ok, no_safety_fault, no_estop,
         weight_fresh_valid, relays_all_off, no_manual_pump;
} autotune_ready_t;

typedef struct {
    autotune_cmd_t cmd;
    uint8_t  channel;
    uint32_t material_id;
    bool     material_selected, scale_under_nozzle, container_present;
    autotune_ready_t ready;
} autotune_arm_req_t;

typedef struct {
    autotune_cmd_t cmd;
    uint8_t  channel;
    uint32_t material_id;
    uint32_t token;
    autotune_ready_t ready;
} autotune_start_req_t;

/* Continuous inputs, every cycle. */
typedef struct {
    uint32_t now_ms;
    uint32_t boot_epoch;
    bool weight_fresh_valid, weight_link_ok, comm_ok;
    bool safety_fault, estop, ownership_ok, manual_pump_active, job_active;
} autotune_inputs_t;

/* Published later by the caller; nothing here publishes. */
typedef struct {
    autotune_auth_state_t state;
    autotune_reason_t reason;    /* last cancel / transition reason */
    autotune_reason_t refusal;   /* gate that failed on the last refused command */
    uint32_t token;              /* valid while ARMED */
    uint8_t  channel;
    uint32_t material_id;
    uint32_t arm_cmd_id, last_cmd_id;
    uint32_t arm_rx_ms, boot_epoch;
} autotune_auth_status_t;

typedef struct {
    autotune_auth_cfg_t cfg;
    autotune_auth_status_t st;
    uint32_t seen[AT_AUTH_SEEN_RING];
    uint8_t  seen_n, seen_head;
    uint32_t floor_id;
    uint32_t arm_counter;
    bool     ok;                 /* init succeeded */
} autotune_auth_t;

/* Fails closed: returns false (and every command is refused) if cfg is NULL
 * or any required field is 0. */
bool autotune_auth_init(autotune_auth_t *a, const autotune_auth_cfg_t *cfg);

/* Return AT_R_NONE when accepted, else the failing gate (also in st.refusal). */
autotune_reason_t autotune_auth_arm(autotune_auth_t *a, const autotune_arm_req_t *r);
autotune_reason_t autotune_auth_start(autotune_auth_t *a, const autotune_start_req_t *r);

/* Always accepted, idempotent, needs no auth. ARMED/RUNNING -> CANCELLED. */
bool autotune_auth_cancel(autotune_auth_t *a);

/* Evaluate cancel triggers and ARM expiry. */
void autotune_auth_tick(autotune_auth_t *a, const autotune_inputs_t *in);

/* Sequencer reached a terminal state: RUNNING -> IDLE so a new ARM is possible. */
void autotune_auth_finish(autotune_auth_t *a);

#ifdef __cplusplus
}
#endif
