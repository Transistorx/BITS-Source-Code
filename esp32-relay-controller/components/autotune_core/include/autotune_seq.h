/* Supervised AUTO_TUNE sequencer (Phase 3, SOFTWARE / SIMULATION ONLY).
 *
 * *** PURE LOGIC. NOT WIRED INTO ANY CONTROL PATH. ***
 * Its only actuation is the OUTPUT `relay_on` request returned by
 * autotune_seq_tick(). Nothing in production calls this module or reads that
 * output. It never calls a relay API, task, heap, NVS or logger. Physical
 * relay energization is NOT approved: `cfg.hw_enable` defaults to FALSE and
 * with it false the request is always OFF and the sequencer ends FAILED with
 * AT_R_HW_NOT_APPROVED.
 *
 * LIMITS. There are NO default limit values in production code. The caller
 * must fill an autotune_limits_t completely; any member 0 (or NULL struct)
 * makes autotune_seq_init() fail closed (the instance never leaves IDLE and
 * never requests ON). The numbers used by the QEMU tests are SIMULATION
 * FIXTURES that live only under tests/ and are not hardware settings. They are
 * separate from the normal production job range (500-20000 g).
 *
 * STATES (every transition below is the only legal one)
 *   IDLE -> ARMED                 auth module reports ARMED
 *   ARMED -> PRECHECK             auth RUNNING handoff for this channel+material
 *                                 AND hw_enable (else FAILED HW_NOT_APPROVED)
 *   PRECHECK -> MEASURING_STARTUP >= 3 fresh valid samples
 *   MEASURING_STARTUP (relay OFF, at-rest baseline of rest_ms; then ON edge)
 *        -> CALIBRATING_FLOW      baseline complete, pre-ON limit checks pass
 *        -> ESTIMATING_PARAMETERS target mass reached (no more pulses)
 *   CALIBRATING_FLOW (relay ON)   -> MEASURING_SHUTOFF  planned pulse or target
 *   MEASURING_SHUTOFF (relay OFF, record the tail until it settles)
 *        -> MEASURING_STARTUP     next trial   |  -> ESTIMATING_PARAMETERS
 *   ESTIMATING_PARAMETERS -> VALIDATING_PARAMETERS -> COMPLETED | FAILED
 *   any non-terminal state -> FAILED or CANCELLED (relay request OFF, same tick)
 *   COMPLETED / FAILED / CANCELLED are terminal; ticks keep the request OFF.
 * Each trial = one pulse (rest baseline, ON pulse_on_ms, OFF tail until settled).
 *
 * INDEPENDENT LIMITS, each checked on its own every tick against the
 * sequencer's OWN counters and the raw scale weight; none uses a flow estimate
 * or the characterization result:
 *   max_pulse_on_ms  : now - ON edge >= limit          -> OFF + FAILED
 *   max_total_on_ms  : cumulative ON + current pulse   -> OFF + FAILED
 *   max_wall_ms      : now - START >= limit            -> OFF + FAILED
 *   max_delivered_g  : delivered + margin >= limit while ON (and >= limit at
 *                      any time) -> OFF + FAILED
 *   target_mass_g    : delivered + margin >= target    -> OFF, no more pulses
 * delivered = newest weight - weight at START (scale feedback only).
 * STOP-MARGIN RULE: margin = max(cfg.assumed_post_off_g, ceil(upper bound of
 * the characterised C, once >= 2 valid trials)) + resolution. The relay is
 * turned OFF on the tick where delivered + margin reaches the limit, so the
 * mass still arriving after shutoff (post-off delivery, measurement latency
 * and in-flight volume) lands at or below the limit IF assumed_post_off_g is
 * a true worst case. The characterised value can only RAISE the margin.
 *
 * WATCHDOGS (weight AND ON-time based; the flow estimator is never the sole
 * detector): (1) no-progress window: while ON, weight must rise by
 * noprog_n_res*resolution within noprog_window_ms of ON time since the last
 * progress mark; (2) ON-time accounting: once cumulative ON >=
 * noprog_cum_on_ms the delivered mass must be >= noprog_n_res*resolution.
 * Both -> OFF + FAILED(NO_PROGRESS). Also: weight below its peak by more than
 * decrease_tol_g, weight rise while OFF beyond leak_tol_g (at rest, or after
 * off_guard_ms past the OFF edge), rate above max_rate_g_per_s or one-sample
 * step above max_step_g (surge) -> FAILED. E-stop, safety fault, ownership
 * loss, boot epoch change, manual pump, comm loss, auth lost -> CANCELLED.
 * Stale/invalid weight, weight link lost, sender restart -> FAILED (CANCELLED
 * while only ARMED).
 *
 * OUTPUT. After COMPLETED the result record carries the autotune_char result,
 * the limits used, trial ids, origin (default UNKNOWN) and verification
 * UNVERIFIED, plus a CANDIDATE parameter suggestion (UNTESTED, never applied,
 * no profile write, no Ki/Kd). Insufficient confidence => NO_CANDIDATE.
 * Kp candidate: Kp = 1/(Q*Tc), error in grams, output normalised 0..1
 * (duty = kp*error_g, clamp 0..1). Q = conservative UPPER bound of Q_eff,
 * Tc = caller-supplied closed-loop time constant in [tc_min_s, tc_max_s]. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "autotune_auth.h"
#include "autotune_char.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AT_SEQ_MAX_SAMPLES 200

/* All members REQUIRED, caller initialised; 0 = missing = fail closed. */
typedef struct {
    int32_t  target_mass_g;      /* calibration target mass */
    uint32_t max_pulse_on_ms;    /* max single relay-ON pulse */
    uint32_t max_total_on_ms;    /* max cumulative relay-ON time */
    uint32_t max_wall_ms;        /* max calibration wall time */
    int32_t  max_delivered_g;    /* absolute delivered calibration mass */
} autotune_limits_t;

typedef enum {
    AT_LIM_OK = 0, AT_LIM_NULL, AT_LIM_TARGET, AT_LIM_PULSE, AT_LIM_TOTAL,
    AT_LIM_WALL, AT_LIM_DELIVERED, AT_LIM_ORDER,
} autotune_limits_err_t;

autotune_limits_err_t autotune_limits_validate(const autotune_limits_t *lim);

typedef enum {
    AT_CFG_OK = 0, AT_CFG_NULL, AT_CFG_LIMITS, AT_CFG_CHANNEL, AT_CFG_CHAR,
    AT_CFG_PLAN, AT_CFG_TIMING, AT_CFG_WATCHDOG, AT_CFG_ANOMALY, AT_CFG_MARGIN,
    AT_CFG_TC,
} autotune_cfg_err_t;

/* All members REQUIRED (non zero) except hw_enable and transport_ms. */
typedef struct {
    bool     hw_enable;          /* FALSE: relay request always OFF, START refused */
    uint8_t  channel;            /* 1 or 2 */
    uint32_t material_id;        /* must equal channel (fixed mapping) */
    autotune_char_cfg_t ch;      /* analysis config (autotune_char_cfg_default) */
    uint32_t pulse_on_ms;        /* planned ON pulse, < max_pulse_on_ms */
    uint8_t  n_trials;           /* planned pulses */
    uint8_t  min_valid_trials;   /* >= 2, needed to complete */
    uint32_t rest_ms;            /* at-rest baseline before each ON edge */
    uint32_t off_guard_ms;       /* after this past OFF, no further rise allowed */
    uint32_t stale_ms;           /* newest sample age (incl. age_ms) limit */
    uint32_t transport_ms;       /* assumed transport delay (0 allowed) */
    uint32_t noprog_window_ms;
    uint8_t  noprog_n_res;
    uint32_t noprog_cum_on_ms;
    int32_t  decrease_tol_g, leak_tol_g, max_step_g;
    float    max_rate_g_per_s;
    uint32_t surge_win_ms;
    int32_t  assumed_post_off_g; /* worst-case post-shutoff mass, > 0 */
    float    tc_s, tc_min_s, tc_max_s;
} autotune_seq_cfg_t;

typedef enum {
    AT_S_IDLE = 0, AT_S_ARMED, AT_S_PRECHECK, AT_S_CALIBRATING_FLOW,
    AT_S_MEASURING_STARTUP, AT_S_MEASURING_SHUTOFF, AT_S_ESTIMATING_PARAMETERS,
    AT_S_VALIDATING_PARAMETERS, AT_S_COMPLETED, AT_S_FAILED, AT_S_CANCELLED,
} autotune_state_t;

typedef struct {
    int32_t weight_g;
    uint32_t t_ms;       /* receive time */
    uint32_t seq;        /* 0 = unknown */
    uint32_t age_ms;
    bool valid, stable;
} autotune_wsample_t;

typedef struct {
    autotune_state_t state;
    autotune_reason_t reason;    /* why FAILED / CANCELLED, else NONE */
    bool relay_on;               /* THE relay request. Nothing wires it. */
    bool target_reached;
    bool hw_enable;
    uint8_t trials_done, trials_valid;
    uint32_t trial_id;
    int32_t delivered_g, margin_g;
    uint32_t cum_on_ms, wall_ms;
} autotune_seq_status_t;

typedef enum { AT_ORIGIN_UNKNOWN = 0, AT_ORIGIN_SIMULATED, AT_ORIGIN_HARDWARE } at_origin_t;
typedef enum { AT_VERIF_UNVERIFIED = 0 } at_verif_t;
typedef enum { AT_CAND_NO_CANDIDATE = 0, AT_CAND_UNTESTED } at_cand_status_t;

#define AT_NC_CONF_NOT_OK 0x01u
#define AT_NC_NO_Q        0x02u
#define AT_NC_NO_C        0x04u
#define AT_NC_NO_STARTUP  0x08u
#define AT_NC_NO_SIGMA    0x10u
#define AT_NC_NO_BOUND    0x20u

/* Suggestion only. Never applied, never written to a profile. No Ki/Kd. */
typedef struct {
    at_cand_status_t status;     /* NO_CANDIDATE or UNTESTED */
    uint32_t no_cand_reasons;    /* AT_NC_* */
    uint32_t conf_reasons;       /* AT_CR_* of the characterization */
    bool  c_cut_valid;
    float c_cut_g, c_cut_upper_g;           /* in-flight compensation, bound */
    bool  tau_valid;
    float tau_off_ms;                       /* conservative (upper bound) */
    bool  flow_lb_valid;
    float min_expected_flow_g_per_min;      /* lower prediction bound, > 0 */
    uint32_t startup_delay_ms;              /* upper bound */
    uint32_t min_on_pulse_ms;               /* > startup + latency, >= edge excl. */
    bool  kp_valid;
    float q_used_g_per_s, tc_s;
    float kp_per_g;              /* error in g -> output 0..1 */
} autotune_candidate_t;

typedef struct {
    uint8_t  channel;
    uint32_t material_id;
    autotune_char_result_t ch;   /* includes trial ids */
    autotune_limits_t limits;
    at_origin_t origin;          /* default UNKNOWN */
    at_verif_t verification;     /* always UNVERIFIED */
    autotune_candidate_t cand;
} autotune_result_t;

typedef struct {
    autotune_seq_cfg_t cfg;
    autotune_limits_t  lim;
    autotune_cfg_err_t cfg_err;
    bool init_ok;
    autotune_state_t state;
    autotune_reason_t reason;
    /* weight intake */
    bool have_w, have_seq, sender_restart, invalid_seen, new_sample, buf_overflow;
    autotune_wsample_t last;
    uint32_t last_seq;
    /* run bookkeeping */
    bool req_on, target_reached, started;
    uint32_t boot_epoch, t_start, on_start, off_cmd, rest_start, cum_on_ms;
    int32_t w_start, w_peak, leak_ref_w, guard_w, margin_g, np_mark_w;
    bool guard_set;
    uint32_t np_mark_elapsed;
    bool surge_ref_set, prev_set;
    uint32_t surge_ref_t;
    int32_t surge_ref_w, prev_w;
    uint8_t precheck_n, trials_done, trials_valid;
    uint32_t trial_id;
    uint16_t nbuf;
    at_sample_t buf[AT_SEQ_MAX_SAMPLES];
    autotune_char_t ch;
    autotune_seq_status_t st;
    autotune_result_t result;
    bool result_ready;
    at_origin_t origin;
} autotune_seq_t;

/* Zeroes cfg and sets hw_enable = false. All other members are then missing
 * and init fails closed until the caller fills them. */
void autotune_seq_cfg_default(autotune_seq_cfg_t *cfg);

/* false = fail closed (instance stays IDLE, request always OFF). */
bool autotune_seq_init(autotune_seq_t *s, const autotune_seq_cfg_t *cfg,
                       const autotune_limits_t *lim);

void autotune_seq_set_origin(autotune_seq_t *s, at_origin_t o);

/* Newest scale sample (raw weight as received). false = ignored (invalid,
 * duplicate, reordered, sender restart, non-monotonic). Never blocks. */
bool autotune_seq_feed(autotune_seq_t *s, const autotune_wsample_t *w);

/* One supervision step. Returns the relay request (true = ON). `auth` is the
 * authorization module status; NULL is treated as not authorised. */
bool autotune_seq_tick(autotune_seq_t *s, const autotune_inputs_t *in,
                       const autotune_auth_status_t *auth);

const autotune_seq_status_t *autotune_seq_status(const autotune_seq_t *s);

/* NULL unless COMPLETED. */
const autotune_result_t *autotune_seq_result(const autotune_seq_t *s);

/* Candidate derivation (also used by the sequencer). Pure. */
void autotune_candidate_build(const autotune_char_result_t *r, float tc_s,
                              float tc_min_s, float tc_max_s,
                              autotune_candidate_t *out);

#ifdef __cplusplus
}
#endif
