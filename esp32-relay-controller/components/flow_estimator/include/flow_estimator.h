/* Flow estimator: windowed least-squares slope of weight against ACCUMULATED
 * RELAY-ON TIME (docs/cas-audit/13 section D).
 *
 * *** OPERATIONAL AID ONLY. NEVER A SAFETY AUTHORITY. ***
 * The estimate may refine compensation / pulse sizing / display within bounds
 * the caller already enforces. It must never extend a deadline, relax a limit
 * or replace the stale-weight, max-duration, no-progress or E-stop checks.
 * FALLBACK: whenever flow_estimate_t.valid is false the caller MUST use its
 * static profile values. Invalid, stale, held-too-long or anomalous input
 * always ends in valid == false (fail safe).
 *
 * Pure library: no FreeRTOS, no heap, no blocking, no logging, no globals.
 * All state lives in flow_estimator_t. NOT thread safe: one owner context
 * (the caller serialises feed/note_relay/get). Time is caller supplied
 * monotonic milliseconds (uint32, wrap-safe signed differences).
 *
 * MATH
 *   x_i = accumulated relay-ON time (s) at the sample, W_i = weight (g).
 *   OFF periods do not advance x, so duty cycling does not corrupt the slope.
 *   Q = sum((x_i-xbar)(W_i-Wbar)) / sum((x_i-xbar)^2)         [g per ON-second]
 *   se(Q) = sqrt( (Syy - Q*Sxy) / (n-2) / Sxx ),  rel_se = se / Q
 *   Window rule: valid only if  span_x >= max(min_window_ms, T_need)  and
 *   (W_newest - W_oldest) >= min_delta_g_units * resolution_g, where
 *   T_need = min_delta_g_units * resolution_g / Q  (so slow materials need a
 *   long window; if that exceeds max_window_ms the estimate stays INVALID).
 *   The ring holds at most 16 points. Points are stored at least `spacing`
 *   ON-ms apart; when the ring is full and the window is still too short the
 *   spacing doubles and every second point is dropped (decimation), so 16
 *   points can cover up to max_window_ms.
 *   EWMA (applied once per stored point, not per feed):
 *   Q_filtered = (1-alpha)*Q_prev + alpha*Q_measured, first value = raw.
 *
 * CONFIDENCE
 *   NONE: not valid. LOW: valid but rel_se > ok_rel_se, or held (conditions
 *   lost, within hold_valid_ms) or older than hold_valid_ms/2. OK: valid,
 *   fresh and rel_se <= ok_rel_se. Valid needs n >= min_points and
 *   rel_se <= low_rel_se.
 * HOLD: after the last successful fit the value is held for hold_valid_ms of
 *   wall time, then get() reports valid = false.
 *
 * REJECTIONS (each counted; see flow_reject_counters_t)
 *   invalid sample; stale (age_ms > max_sample_age_ms, also discards the whole
 *   window and held estimate); non-monotonic / equal time; seq <= last seq
 *   (duplicate or reordered; a backwards jump bigger than seq_restart_gap is a
 *   sender restart: window discarded, sample accepted); unstable when
 *   require_stable; |step| > max_step_g vs the previous sample (first one is
 *   rejected alone; a second consecutive one is taken as a level shift and
 *   discards the window); relay OFF inside shutdown holdoff; relay OFF after
 *   it (counted as off_samples, never used; window dropped after
 *   off_reset_ms); relay ON inside startup holdoff; weight below the newest
 *   window point by more than decrease_tol_g while ON (anomaly: window and
 *   held estimate discarded); implausible pairwise or fitted slope.
 *
 * Blocked nozzle / no weight change while ON: slope ~0, delta too small, so
 * the estimate is never (or no longer) valid. It is NOT reported as q = 0;
 * detecting "no progress" is the separate NO-PROGRESS detector's job.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOW_ESTIMATOR_MAX_POINTS 16

typedef enum {
    FLOW_CONF_NONE = 0,
    FLOW_CONF_LOW  = 1,
    FLOW_CONF_OK   = 2,
} flow_confidence_t;

/* Every threshold is per instance. No material-specific default (the water
 * reference rate is deliberately NOT used anywhere). */
typedef struct {
    uint8_t  window_points;          /* ring size used, 3..16            (16)    */
    uint8_t  min_points;             /* points for validity, 3..window   (6)     */
    uint32_t min_window_ms;          /* min ON-time span                 (2000)  */
    uint32_t max_window_ms;          /* max ON-time span kept            (30000) */
    uint32_t point_spacing_ms;       /* base ON-ms between stored points (100)   */
    int32_t  resolution_g;           /* scale resolution, >=1            (1)     */
    uint16_t min_delta_g_units;      /* min delta, in resolution units   (10)    */
    int32_t  decrease_tol_g;         /* tolerated drop while ON, >=0     (1)     */
    float    alpha;                  /* EWMA, 0 < alpha <= 1             (0.3)   */
    uint32_t startup_holdoff_ms;     /* after relay-ON edge              (300)   */
    uint32_t shutdown_holdoff_ms;    /* after relay-OFF edge             (300)   */
    float    max_plausible_flow_g_per_s; /* reject slopes above           (1000)  */
    uint32_t plausible_min_dt_ms;    /* min dt for pairwise slope check  (100)   */
    int32_t  max_step_g;             /* single-sample discontinuity      (500)   */
    bool     require_stable;         /* skip unstable samples            (false) */
    uint32_t max_sample_age_ms;      /* reject older samples             (1000)  */
    uint32_t hold_valid_ms;          /* hold last valid, then INVALID    (5000)  */
    uint32_t off_reset_ms;           /* OFF this long drops the window   (10000) */
    uint32_t seq_restart_gap;        /* backwards seq jump = restart     (1000)  */
    float    ok_rel_se;              /* confidence OK if rel_se <=       (0.15)  */
    float    low_rel_se;             /* valid only if rel_se <=          (0.5)   */
} flow_estimator_cfg_t;

typedef struct {
    int32_t  weight_g;   /* integer grams as seen by the controller */
    uint32_t t_ms;       /* caller monotonic ms (sample receive/acquire time) */
    uint32_t seq;        /* cas_seq; 0 = unknown (seq checks skipped) */
    uint32_t age_ms;     /* sender-reported age */
    bool     stable;
    bool     valid;
} flow_sample_t;

typedef struct {
    uint32_t invalid, stale, seq, time, step, slope, decrease, unstable;
    uint32_t startup, shutdown;
    uint32_t off_samples;   /* relay OFF outside shutdown holdoff (not an error) */
    uint32_t seq_restarts;  /* sender restarts detected */
    uint32_t resets;        /* window discards */
    uint32_t accepted;      /* samples that passed every check */
} flow_reject_counters_t;

typedef struct {
    bool              valid;
    flow_confidence_t confidence;
    float q_raw_g_per_s;          /* last fit, per ON-second */
    float q_filtered_g_per_s;     /* EWMA, per ON-second (valve-open flow) */
    float q_at_full_duty_g_per_s; /* = filtered if duty known (0,1], else 0 */
    float q_avg_g_per_s;          /* filtered * duty; 0 if duty unknown */
    uint8_t  n_points;
    uint32_t window_ms;           /* ON-time span of the window */
    uint32_t last_valid_age_ms;   /* UINT32_MAX if never valid */
    flow_reject_counters_t rej;
} flow_estimate_t;

/* Internal state. Fields are implementation detail (tests may poke them). */
typedef struct {
    flow_estimator_cfg_t cfg;
    struct { uint32_t x_ms; int32_t w_g; } pts[FLOW_ESTIMATOR_MAX_POINTS];
    uint8_t  n;
    uint32_t spacing_ms;
    bool     relay_on, duty_known, have_on_edge, have_off_edge, adv_init;
    float    duty;
    uint32_t on_edge_ms, off_edge_ms, on_acc_ms, last_adv_ms;
    bool     have_t, have_seq, have_w, have_valid, holding, have_filt;
    uint32_t last_t_ms, last_seq, last_valid_ms, window_ms;
    int32_t  last_w;
    uint8_t  step_consec;
    flow_confidence_t last_conf;
    float    q_raw, q_filt, last_slope;
    flow_reject_counters_t rej;
} flow_estimator_t;

void flow_estimator_cfg_default(flow_estimator_cfg_t *cfg);

/* Copies cfg (NULL = defaults). Out-of-range/NaN fields are replaced by safe
 * values. Returns true if cfg was accepted unchanged, false if sanitised. */
bool flow_estimator_init(flow_estimator_t *est, const flow_estimator_cfg_t *cfg);

/* Clears all state and counters, keeps the config. */
void flow_estimator_reset(flow_estimator_t *est);

/* Mark a relay edge / state. duty_0_1 outside (0,1] or NaN = unknown. Call at
 * every command edge (also for each pulse of a duty-cycled output). */
void flow_estimator_note_relay(flow_estimator_t *est, bool on, uint32_t now_ms,
                               float duty_0_1);

/* Returns true if the sample passed every check. Never blocks or allocates. */
bool flow_estimator_feed(flow_estimator_t *est, const flow_sample_t *s);

/* Read-only; expiry (hold_valid_ms) is evaluated against now_ms. */
void flow_estimator_get(const flow_estimator_t *est, uint32_t now_ms,
                        flow_estimate_t *out);

#ifdef __cplusplus
}
#endif
