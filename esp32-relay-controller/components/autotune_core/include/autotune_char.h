/* Auto-tune characterization maths (Phase 3, SOFTWARE / SIMULATION ONLY).
 * docs/cas-audit/14-phase3-characterization.md has the long form.
 *
 * *** NOT WIRED INTO ANY CONTROL PATH. PURE LOGIC. ***
 * Consumes recorded pulse trials (caller-owned arrays of timestamped integer
 * gram samples plus the relay ON/OFF command times) and produces RESULT
 * STRUCTS. Nothing here touches a relay, task, heap, NVS or log. One
 * autotune_char_t belongs to exactly ONE channel/material/pump instance
 * (M1/CH1/Pump1 or M2/CH2/Pump2); a trial carrying another channel id is
 * refused and nothing is ever shared or averaged across instances.
 * No Kp/Ki/Kd and nothing from the water reference rate is derived here.
 *
 * TIME BASE. Sample i has receive time t_i (ms), sender age a_i (ms). The
 * measurement is corrected to the time the load was actually on the scale:
 *     tc_i = t_i - a_i - transport_ms,   x_i = tc_i - on_cmd_ms   (ms)
 * a_i is what the sender reports; transport_ms (e.g. MQTT/WebSocket one way
 * delay) is a CALLER-SUPPLIED estimate. It cannot be measured from the weight
 * data alone: weight data only gives apparent_delay = mechanical startup +
 * measurement latency, which cannot be separated without an independent
 * reference. Reported latency_ms = mean(a_i) + transport_ms is therefore only
 * the KNOWN part. Measuring it for real needs hardware (e.g. a timestamped
 * reference). With simulated data it only means "the delay the simulator was
 * told to add".
 *
 * EQUATIONS (x in ms unless noted, w in g)
 *  (a) noise. Baseline = samples with x < 0 (relay not yet commanded, load at
 *      rest). Least squares line w = a0 + b0*x over them, residual
 *      sigma = sqrt( SSE / (nb - 2) ) [g], W0 = mean(w_baseline).
 *      resolution r = cfg.resolution_g. Noise threshold  thr = k*sigma + r.
 *  (b) latency  L = mean(a_i) + transport_ms [ms]  (known part only).
 *  (c) startup delay  D = x of the first sample (0 <= x <= off) with
 *      w - W0 > thr and the next (sustain_n - 1) samples also > thr.
 *      D includes thr/Q of detection lag for slow flow and is quantised to the
 *      sample period; it is an UPPER-leaning figure by design.
 *      D_int = (W0 - a)/Q  from the steady line (a, Q below): extrapolated
 *      dead time (a ramp makes it later than the true onset).
 *  (d) ramp  R = x of the LAST ON sample lying below the steady line by more
 *      than thr, minus D, floored at 0 (residual-convergence time). Coarse.
 *  (e) Q_eff = b  of the LS fit w = a + b*(x/1000) over ON samples in
 *      [max(edge_excl, D + R), off]   [g per ON-second, valve open]
 *      se(Q) = sqrt( s2 / Sxx ),  s2 = max(SSE/(n-2), sigma^2).
 *      Q_at_duty = Q_eff * duty, duty = on_ms / period_ms (period 0 => 1).
 *  (f) W_off = a + b*(off/1000) (steady line at the latency-corrected OFF
 *      command time). W_settled = mean of the first settled window (g).
 *      C = W_settled - W_off   [g]  post-shutoff delivery,
 *      se(C) = sqrt( s2/n_win + s2*(1/n + (xo-xbar)^2/Sxx) ).
 *      tau_off: r_i = W_settled - w_i for tail samples (off <= x < settle
 *      start) with r_i > thr; weighted LS of ln r_i on x (weights r_i^2, as
 *      var(ln r) ~ sigma^2/r^2); slope m (1/s) => tau_off = -1000/m ms. This
 *      is the fit W = W_inf - A*exp(-t/tau). Failure modes: C <= thr (no
 *      measurable tail), < 3 points above thr, m >= 0, valve-closing dead time
 *      before decay (non exponential head), biased W_inf. Then tau is simply
 *      flagged invalid, C stays valid. Validity needs >= 4 points above thr AND
 *      max r_i >= 1.5*thr (dynamic range) AND settle_slope*tau <= 0.3*max r_i
 *      (bias check); a small or biased tail is not trusted.
 *      The settled mean also misses the tail still to come at the window start
 *      (bias about -slope_limit*tau), so C is slightly LOW, not high.
 *  (g) settled window: window of settle_win_ms starting at xs with
 *      min_settle <= xs - off <= settle_timeout, >= settle_min_n samples,
 *      max-min <= k_range*sigma + r AND |LS slope| <= settle_slope_g_per_s.
 *      None found: UNSETTLED (data covers the timeout) or TAIL_TOO_SHORT.
 *  (h) repeatability over the n valid trials: mean, sample sd
 *      s = sqrt( sum(v-mean)^2/(n-1) ), conservative ONE-SIDED bound for the
 *      next single pulse  upper = mean + k_n*s,  k_n = t_{0.95,n-1}*sqrt(1+1/n)
 *      (small table, autotune_t95_pred_k), lower = mean - k_n*s. n<2: no bound.
 *  (i) confidence: NONE (no valid trial), LOW (n < n_ok or scatter above
 *      cv_ok), OK (n >= n_ok and scatter within limits).
 *
 * REJECTION. A pulse with ON time < 2*edge_excl_ms (default 2*300 ms, the
 * flow_estimator startup + shutdown holdoffs) is REJECTED_TOO_SHORT and never
 * silently used. All numbers produced from SIMULATED data are simulation
 * results only, not real-world accuracy. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AT_CHAR_MAX_TRIALS 16

typedef struct {
    uint32_t t_ms;     /* receive time, caller monotonic ms */
    int32_t  w_g;      /* integer grams */
    uint32_t age_ms;   /* sender-reported age */
} at_sample_t;

typedef struct {
    uint8_t  channel;          /* 1 or 2, must match the autotune_char_t */
    uint32_t trial_id;
    const at_sample_t *s;      /* time ordered, caller owned */
    uint16_t n;
    uint32_t on_cmd_ms;        /* relay ON command time (same clock as t_ms) */
    uint32_t off_cmd_ms;       /* relay OFF command time */
    uint32_t transport_ms;     /* assumed one-way transport delay */
    uint32_t period_ms;        /* 0 = single pulse; else pulse-train period */
} at_trial_t;

typedef enum {
    AT_TRIAL_OK = 0,
    AT_TRIAL_REJECTED_TOO_SHORT,
    AT_TRIAL_BAD_INPUT,
    AT_TRIAL_CHANNEL_MISMATCH,
    AT_TRIAL_NO_BASELINE,
    AT_TRIAL_NO_RISE,
    AT_TRIAL_Q_FIT_FAILED,
    AT_TRIAL_TAIL_TOO_SHORT,
    AT_TRIAL_UNSETTLED,
    AT_TRIAL_NEGATIVE_C,
    AT_TRIAL_TABLE_FULL,
} at_trial_reason_t;

#define AT_V_SIGMA   0x01u
#define AT_V_LATENCY 0x02u
#define AT_V_STARTUP 0x04u
#define AT_V_RAMP    0x08u
#define AT_V_Q       0x10u
#define AT_V_SETTLED 0x20u
#define AT_V_C       0x40u
#define AT_V_TAU     0x80u

typedef struct {
    at_trial_reason_t reason;
    uint16_t valid;              /* AT_V_* flags */
    uint32_t trial_id;
    float sigma_g, noise_thr_g;
    int32_t resolution_g;
    float latency_ms;
    float startup_ms, startup_intercept_ms, ramp_ms;
    float q_eff, q_se, q_at_duty;
    float w_off_g, w_settled_g, c_g, c_se_g;
    float tau_off_ms;
    float settle_ms;             /* OFF command -> start of settled window */
    float dt_ms;                 /* mean sample period of the trial */
} at_trial_result_t;

typedef struct {
    int32_t  resolution_g;       /* scale resolution, >= 1              (1)    */
    float    k_sigma;            /* noise threshold factor              (3)    */
    float    k_range;            /* settle range factor                 (5)    */
    uint32_t edge_excl_ms;       /* startup/shutdown edge exclusion     (300)  */
    uint16_t min_baseline_n;     /* samples needed for sigma            (5)    */
    uint8_t  sustain_n;          /* samples for a sustained rise        (3)    */
    uint16_t min_q_points;       /* points for the Q fit                (4)    */
    uint32_t min_q_span_ms;      /* ON span of the Q fit                (500)  */
    uint32_t settle_win_ms;      /* settle window length                (2000) */
    uint32_t min_settle_ms;      /* earliest settle window start        (500)  */
    uint32_t settle_timeout_ms;  /* latest settle window start          (10000)*/
    float    settle_slope_g_per_s; /* settle slope limit                (0.5)  */
    uint16_t settle_min_n;       /* samples in a settle window          (4)    */
    uint8_t  n_ok;               /* valid trials for OK confidence      (5)    */
    float    cv_ok_q;            /* scatter limit for Q                 (0.15) */
    float    cv_ok_c;            /* scatter limit for C                 (0.35) */
    float    cv_ok_startup;      /* scatter limit for startup           (0.35) */
} autotune_char_cfg_t;

typedef enum {
    AT_CONF_NONE = 0,
    AT_CONF_LOW  = 1,
    AT_CONF_OK   = 2,
} at_confidence_t;

typedef struct {
    uint8_t n;
    float mean, sd;
    float upper, lower;          /* valid only if bound_valid */
    bool  bound_valid;           /* n >= 2 */
} at_stat_t;

/* confidence reason bits */
#define AT_CR_NO_VALID_TRIAL 0x01u
#define AT_CR_LOW_N          0x02u
#define AT_CR_SCATTER_Q      0x04u
#define AT_CR_SCATTER_C      0x08u
#define AT_CR_SCATTER_START  0x10u
#define AT_CR_NO_TAU         0x20u

typedef struct {
    uint8_t  channel;
    uint32_t material_id;
    at_confidence_t confidence;
    uint32_t conf_reasons;       /* AT_CR_* */
    uint8_t  n_trials;           /* accepted into the table (any reason) */
    uint8_t  n_valid;            /* fully valid */
    uint8_t  n_too_short, n_other_rejected;
    at_stat_t sigma, latency, startup, q_eff, c, tau;
    uint32_t trial_ids[AT_CHAR_MAX_TRIALS]; /* valid trials only */
    int32_t  resolution_g;
    uint32_t edge_excl_ms;
} autotune_char_result_t;

typedef struct {
    uint8_t  channel;
    uint32_t material_id;
    autotune_char_cfg_t cfg;
    at_trial_result_t res[AT_CHAR_MAX_TRIALS];
    uint8_t n;
} autotune_char_t;

/* Analysis defaults (thresholds of the maths, NOT actuation limits). */
void autotune_char_cfg_default(autotune_char_cfg_t *cfg);

/* channel must be 1 or 2. NULL cfg = defaults. Returns false (and leaves the
 * instance unusable: every add is refused) on a bad channel or bad cfg. */
bool autotune_char_init(autotune_char_t *c, uint8_t channel, uint32_t material_id,
                        const autotune_char_cfg_t *cfg);

/* Pure analysis of one trial. Does not touch any instance. */
at_trial_reason_t autotune_char_analyze_trial(const autotune_char_cfg_t *cfg,
                                              const at_trial_t *tr,
                                              at_trial_result_t *out);

/* Store a finished trial result in the table. TABLE_FULL if no room. */
at_trial_reason_t autotune_char_commit(autotune_char_t *c, const at_trial_result_t *r);

/* analyze + commit. CHANNEL_MISMATCH if tr->channel != c->channel (nothing is
 * stored). out may be NULL. */
at_trial_reason_t autotune_char_add_trial(autotune_char_t *c, const at_trial_t *tr,
                                          at_trial_result_t *out);

void autotune_char_summarize(const autotune_char_t *c, autotune_char_result_t *out);

/* k_n = t_{0.95,n-1} * sqrt(1 + 1/n); 0 for n < 2. n is clamped to the table. */
float autotune_t95_pred_k(uint8_t n);

#ifdef __cplusplus
}
#endif
