/* Flow estimator tests.
 *
 * ===== TEST-ONLY PLANT / SCALE SIMULATOR (lives only under tests/) =====
 * Deterministic: fixed-seed LCG noise, integer-gram quantisation, configurable
 * flow, valve startup delay, shutdown residual, duty-cycled relay, measurement
 * latency, dropped samples, flow interruption. 1 ms physics step, one sample
 * published every 100 ms with an incrementing cas_seq.
 * All accuracy numbers printed here are SIMULATED, never real-world claims.
 * The 13.33 g/s case is an ILLUSTRATIVE water-like value only.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"

#include "flow_estimator.h"
#include "test_flow_estimator.h"

extern void audit_check(bool condition, const char *name);
static const char *TAG = "TEST";

#define SIM_HIST 512u

typedef struct {
    uint32_t rng;
    float q, q_late;            /* valve-open flow g/s; q_late after q_change_ms */
    uint32_t q_change_ms;       /* 0 = never */
    uint32_t delay_ms, residual_ms, latency_ms;
    int noise;                  /* +-g uniform integer noise */
    float leak_g_s;             /* weight rise while relay OFF */
    int drop_pct;
    uint32_t age_ms;
    int stable_mode;            /* 0 stable only when valve shut, 1 always stable */
    float report_duty;
    bool cmd, pulsing, flowing_at_off;
    uint32_t period_ms;
    float duty;
    uint32_t pulse_start_ms;
    uint32_t on_edge, off_edge;
    float w_true;
    float hist[SIM_HIST];
    uint32_t now, seq, t_base;
    uint32_t first_valid_ms;    /* sim ms of first valid estimate, 0 = none */
    float first_valid_q;
    int32_t last_w;             /* last published sample */
    uint32_t last_t, last_seq;
} sim_t;

static sim_t g_sim;              /* test-only static, reused by every case */
static flow_estimator_t g_est;

static uint32_t lcg(sim_t *s)
{
    s->rng = s->rng * 1664525u + 1013904223u;
    return s->rng >> 8;
}

static void sim_setup(float q, uint32_t seed)
{
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.rng = seed;
    g_sim.q = q;
    g_sim.delay_ms = 150;
    g_sim.residual_ms = 150;
    g_sim.latency_ms = 100;
    g_sim.age_ms = 40;
    g_sim.report_duty = 1.0f;
    g_sim.w_true = 250.0f;
    g_sim.seq = 0;
    g_sim.t_base = 5000;
    flow_estimator_cfg_t c;
    flow_estimator_cfg_default(&c);
    flow_estimator_init(&g_est, &c);
}

static void sim_init_est(const flow_estimator_cfg_t *c)
{
    flow_estimator_init(&g_est, c);
}

static void sim_cmd(bool on, float duty)
{
    if (on == g_sim.cmd) return;
    uint32_t now = g_sim.now;
    if (on) {
        g_sim.on_edge = now;
    } else {
        g_sim.flowing_at_off = (now - g_sim.on_edge) >= g_sim.delay_ms;
        g_sim.off_edge = now;
    }
    g_sim.cmd = on;
    flow_estimator_note_relay(&g_est, on, g_sim.t_base + now, duty);
}

static void sim_publish(void)
{
    sim_t *s = &g_sim;
    uint32_t idx = (s->now >= s->latency_ms) ? s->now - s->latency_ms : 0;
    float w = s->hist[idx % SIM_HIST];
    int n = 0;
    if (s->noise > 0) n = (int)(lcg(s) % (uint32_t)(2 * s->noise + 1)) - s->noise;
    int32_t wm = (int32_t)floorf(w + 0.5f) + n;
    s->seq++;
    s->last_w = wm;
    s->last_t = s->t_base + s->now;
    s->last_seq = s->seq;
    if (s->drop_pct > 0 && (int)(lcg(s) % 100u) < s->drop_pct) return;
    bool open = s->cmd ? ((s->now - s->on_edge) >= s->delay_ms) : false;
    flow_sample_t smp = {
        .weight_g = wm, .t_ms = s->last_t, .seq = s->seq, .age_ms = s->age_ms,
        .stable = (s->stable_mode == 1) ? true : !open, .valid = true,
    };
    flow_estimator_feed(&g_est, &smp);
    if (!s->first_valid_ms) {
        flow_estimate_t o;
        flow_estimator_get(&g_est, s->last_t, &o);
        if (o.valid) { s->first_valid_ms = s->now; s->first_valid_q = o.q_raw_g_per_s; }
    }
}

static void sim_run(uint32_t until_ms)
{
    sim_t *s = &g_sim;
    while (s->now < until_ms) {
        s->now++;
        if (s->pulsing && s->now >= s->pulse_start_ms) {
            uint32_t ph = (s->now - s->pulse_start_ms) % s->period_ms;
            sim_cmd(ph < (uint32_t)(s->duty * (float)s->period_ms), s->report_duty);
        }
        bool open;
        if (s->cmd) open = (s->now - s->on_edge) >= s->delay_ms;
        else open = s->flowing_at_off && (s->now - s->off_edge) < s->residual_ms;
        float q = (s->q_change_ms && s->now >= s->q_change_ms) ? s->q_late : s->q;
        float rate = open ? q : 0.0f;
        if (!s->cmd) rate += s->leak_g_s;
        s->w_true += rate * 0.001f;
        s->hist[s->now % SIM_HIST] = s->w_true;
        if (s->now % 100u == 0) sim_publish();
    }
}

static void sim_get(flow_estimate_t *o)
{
    flow_estimator_get(&g_est, g_sim.t_base + g_sim.now, o);
}

static int err_permille(float est, float truth)
{
    return (int)(1000.0f * (est - truth) / truth);
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void info(const char *name, const flow_estimate_t *o, float truth)
{
    ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] %s: est_mg_s=%d true_mg_s=%d err_permille=%d conf=%d n=%u win_ms=%u",
             name, (int)(o->q_filtered_g_per_s * 1000.0f), (int)(truth * 1000.0f),
             o->valid ? err_permille(o->q_filtered_g_per_s, truth) : 0,
             (int)o->confidence, (unsigned)o->n_points, (unsigned)o->window_ms);
}

/* Water-like valid estimator ready for direct-injection tests. */
static void prime(void)
{
    sim_setup(13.33f, 1u);
    sim_cmd(true, 1.0f);
    sim_run(8000);
}

static flow_sample_t next_sample(int32_t dw, uint32_t dt, uint32_t dseq)
{
    flow_sample_t s = {
        .weight_g = g_sim.last_w + dw, .t_ms = g_sim.last_t + dt,
        .seq = g_sim.last_seq + dseq, .age_ms = 40, .stable = false, .valid = true,
    };
    return s;
}

static bool finite_est(const flow_estimate_t *o)
{
    return isfinite(o->q_raw_g_per_s) && isfinite(o->q_filtered_g_per_s) &&
           isfinite(o->q_at_full_duty_g_per_s) && isfinite(o->q_avg_g_per_s);
}

void test_flow_estimator_run(void)
{
    flow_estimate_t o, o2;
    char nm[96];

    /* ---- water-like (illustrative) ---- */
    sim_setup(13.33f, 1u);
    sim_run(1000); sim_cmd(true, 1.0f); sim_run(15000);
    sim_get(&o); info("water_like_illustrative_13.33", &o, 13.33f);
    audit_check(o.valid && o.confidence == FLOW_CONF_OK, "flow_est_water_like_valid_conf_ok");
    audit_check(iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 50, "flow_est_water_like_within_5pct");
    ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] water_like first_valid_after_on_ms=%u",
             (unsigned)(g_sim.first_valid_ms - 1000));
    audit_check(o.q_at_full_duty_g_per_s == o.q_filtered_g_per_s, "flow_est_q_full_duty_equals_filtered_when_duty_known");
    audit_check(g_est.rej.off_samples >= 8, "flow_est_samples_before_relay_on_counted_not_used");

    /* ---- low flow ---- */
    sim_setup(2.0f, 2u);
    sim_cmd(true, 1.0f); sim_run(21000);
    sim_get(&o); info("low_flow_2.0", &o, 2.0f);
    audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 2.0f)) <= 100, "flow_est_low_flow_2gps_within_10pct");

    /* ---- very slow viscous: default max window cannot span 10 g ---- */
    sim_setup(0.3f, 3u);
    sim_cmd(true, 1.0f); sim_run(40000);
    sim_get(&o);
    audit_check(!o.valid && o.confidence == FLOW_CONF_NONE, "flow_est_slow_0p3gps_default_window_reports_none");
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.max_window_ms = 90000;
        sim_setup(0.3f, 3u); sim_init_est(&c);
        sim_cmd(true, 1.0f); sim_run(10000);
        sim_get(&o);
        audit_check(!o.valid && o.confidence == FLOW_CONF_NONE, "flow_est_slow_0p3gps_none_before_enough_delta");
        sim_run(80000);
        sim_get(&o); info("slow_viscous_0.3_long_window", &o, 0.3f);
        ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] slow first_valid_ms=%u window_ms=%u",
                 (unsigned)g_sim.first_valid_ms, (unsigned)o.window_ms);
        audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 0.3f)) <= 300, "flow_est_slow_0p3gps_long_window_within_30pct");
        audit_check(g_sim.first_valid_ms >= 25000, "flow_est_slow_valid_only_after_min_delta_span");
    }

    /* ---- noisy scale ---- */
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.decrease_tol_g = 4;       /* noise peak-to-peak; default 1 g would reset constantly */
        sim_setup(13.33f, 4u); sim_init_est(&c); g_sim.noise = 2;
        sim_cmd(true, 1.0f); sim_run(20000);
        sim_get(&o); info("noisy_pm2g_13.33", &o, 13.33f);
        audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 100, "flow_est_noisy_pm2g_water_like_within_10pct");
        sim_setup(5.0f, 5u); sim_init_est(&c); g_sim.noise = 2;
        sim_cmd(true, 1.0f); sim_run(30000);
        sim_get(&o); info("noisy_pm2g_5.0", &o, 5.0f);
        audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 5.0f)) <= 150, "flow_est_noisy_pm2g_5gps_within_15pct");
        sim_setup(13.33f, 4u); g_sim.noise = 2;    /* default tolerance of 1 g */
        sim_cmd(true, 1.0f); sim_run(20000);
        audit_check(g_est.rej.decrease > 0, "flow_est_noise_above_decrease_tol_is_flagged_as_anomaly");
    }

    /* ---- intermittent pulsing, normalised by ON time ---- */
    {
        static const float duties[3] = { 0.25f, 0.5f, 0.75f };
        for (int i = 0; i < 3; i++) {
            sim_setup(13.33f, 10u + (uint32_t)i);
            g_sim.pulsing = true; g_sim.period_ms = 2000; g_sim.duty = duties[i];
            g_sim.pulse_start_ms = 1000; g_sim.report_duty = duties[i];
            sim_run(61000);
            sim_get(&o);
            snprintf(nm, sizeof(nm), "pulsing_duty_%d_pct", (int)(duties[i] * 100.0f));
            info(nm, &o, 13.33f);
            snprintf(nm, sizeof(nm), "flow_est_pulsing_duty_%d_pct_within_10pct_of_on_flow", (int)(duties[i] * 100.0f));
            audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 100, nm);
            if (i == 1) {
                audit_check(fabsf(o.q_avg_g_per_s - 13.33f * 0.5f) < 0.1f * 13.33f * 0.5f + 0.01f,
                            "flow_est_pulsing_average_rate_is_flow_times_duty");
            }
        }
    }

    /* ---- startup transient excluded ---- */
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.startup_holdoff_ms = 600;
        sim_setup(13.33f, 20u); g_sim.delay_ms = 400; sim_init_est(&c);
        sim_run(1000); sim_cmd(true, 1.0f); sim_run(15000);
        sim_get(&o); info("startup_transient_delay400_holdoff600", &o, 13.33f);
        float with_q = g_sim.first_valid_q;
        audit_check(g_est.rej.startup >= 5 && iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 50,
                    "flow_est_startup_transient_excluded_by_holdoff");
        c.startup_holdoff_ms = 0;
        sim_setup(13.33f, 20u); g_sim.delay_ms = 400; sim_init_est(&c);
        sim_run(1000); sim_cmd(true, 1.0f); sim_run(15000);
        ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] first_valid_q_mg_s holdoff600=%d holdoff0=%d (true 13330)",
                 (int)(with_q * 1000.0f), (int)(g_sim.first_valid_q * 1000.0f));
    }

    /* ---- shutdown residual excluded, relay-OFF weight rise not fed ---- */
    {
        sim_setup(13.33f, 30u); g_sim.residual_ms = 250;
        sim_cmd(true, 1.0f); sim_run(10000);
        sim_get(&o);
        float q_before = g_est.q_filt;
        sim_cmd(false, 1.0f); sim_run(11000);
        sim_get(&o2);
        audit_check(o2.valid && iabs(err_permille(o2.q_filtered_g_per_s, 13.33f)) <= 50, "flow_est_shutdown_residual_not_in_estimate");
        sim_run(14000);
        audit_check(g_est.rej.shutdown >= 2 && g_est.rej.off_samples > 0 && g_est.q_filt == q_before,
                    "flow_est_relay_off_samples_never_update_estimate");
        g_sim.leak_g_s = 5.0f;     /* weight rising with relay OFF: other detector's job */
        sim_run(24000);
        audit_check(g_est.q_filt == q_before && g_est.rej.step == 0, "flow_est_relay_off_weight_rise_does_not_feed_estimate");
        sim_get(&o2);
        audit_check(!o2.valid, "flow_est_expires_after_hold_when_relay_stays_off");
    }

    /* ---- stopped flow (pump stops, relay still ON) ---- */
    sim_setup(13.33f, 40u);
    sim_cmd(true, 1.0f); sim_run(15000);
    sim_get(&o);
    g_sim.q_change_ms = g_sim.now + 1; g_sim.q_late = 0.0f;
    sim_run(15000 + 12000);
    sim_get(&o2);
    audit_check(o.valid && !o2.valid && o2.confidence == FLOW_CONF_NONE, "flow_est_stopped_flow_becomes_invalid_after_hold");
    ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] stopped: last_valid_age_ms=%u (hold_valid_ms=5000)", (unsigned)o2.last_valid_age_ms);

    /* ---- blocked nozzle: relay ON, no weight change ---- */
    sim_setup(0.0f, 41u);
    sim_cmd(true, 1.0f); sim_run(25000);
    sim_get(&o);
    audit_check(!o.valid && o.confidence == FLOW_CONF_NONE && g_sim.first_valid_ms == 0 && finite_est(&o),
                "flow_est_blocked_nozzle_never_valid_not_reported_as_zero_flow");

    /* ---- changing flow tracks within EWMA/window lag ---- */
    sim_setup(13.33f, 50u); g_sim.q_change_ms = 12000; g_sim.q_late = 6.0f;
    sim_cmd(true, 1.0f); sim_run(12300);
    sim_get(&o);
    audit_check(o.valid && o.q_filtered_g_per_s > 10.0f, "flow_est_change_lags_not_instant");
    {
        uint32_t t_within = 0;
        for (uint32_t t = 12300; t <= 22000; t += 100) {
            sim_run(t); sim_get(&o2);
            if (o2.valid && iabs(err_permille(o2.q_filtered_g_per_s, 6.0f)) <= 100) { t_within = t - 12000; break; }
        }
        ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] flow 13.33->6.0: within 10pct after %u ms", (unsigned)t_within);
        audit_check(t_within > 0 && t_within <= 8000, "flow_est_change_tracked_within_10pct_in_8s");
    }

    /* ---- dropped samples ---- */
    sim_setup(13.33f, 60u); g_sim.drop_pct = 30;
    sim_cmd(true, 1.0f); sim_run(20000);
    sim_get(&o); info("30pct_dropped_samples", &o, 13.33f);
    audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 100, "flow_est_30pct_dropped_samples_within_10pct");

    /* ---- stale ---- */
    prime(); sim_get(&o);
    {
        flow_sample_t s = next_sample(1, 100, 1);
        s.age_ms = 5000;
        bool r = flow_estimator_feed(&g_est, &s);
        flow_estimator_get(&g_est, s.t_ms, &o2);
        audit_check(o.valid && !r && g_est.rej.stale == 1 && !o2.valid, "flow_est_stale_sample_rejected_and_invalidates");
    }

    /* ---- duplicate and reordered seq ---- */
    prime(); sim_get(&o);
    {
        flow_sample_t d = next_sample(0, 100, 0);          /* same seq as last */
        bool r1 = flow_estimator_feed(&g_est, &d);
        flow_sample_t re = next_sample(1, 100, 1);
        re.seq = g_sim.last_seq - 3;                      /* older seq, newer time */
        bool r2 = flow_estimator_feed(&g_est, &re);
        sim_get(&o2);
        audit_check(!r1 && !r2 && g_est.rej.seq == 2 && o2.valid && o2.q_raw_g_per_s == o.q_raw_g_per_s,
                    "flow_est_duplicate_and_reordered_seq_rejected_estimate_unchanged");
        flow_sample_t rs = next_sample(1, 100, 1);
        rs.seq = 1;                                       /* far backwards = sender restart */
        g_est.last_seq = 5000;
        bool r3 = flow_estimator_feed(&g_est, &rs);
        audit_check(r3 && g_est.rej.seq_restarts == 1 && g_est.rej.seq == 2 && g_est.n == 1,
                    "flow_est_seq_restart_detected_and_window_discarded");
    }

    /* ---- non-monotonic time ---- */
    prime();
    {
        flow_sample_t eq = next_sample(0, 0, 1);
        bool a = flow_estimator_feed(&g_est, &eq);
        flow_sample_t back = next_sample(0, 0, 2);
        back.t_ms = g_sim.last_t - 50;
        bool b = flow_estimator_feed(&g_est, &back);
        audit_check(!a && !b && g_est.rej.time == 2, "flow_est_non_monotonic_time_rejected");
    }

    /* ---- invalid sample ---- */
    prime();
    {
        flow_sample_t iv = next_sample(1, 100, 1);
        iv.valid = false;
        bool a = flow_estimator_feed(&g_est, &iv);
        sim_get(&o);
        audit_check(!a && g_est.rej.invalid == 1 && o.valid, "flow_est_invalid_sample_rejected");
    }

    /* ---- step discontinuity ---- */
    prime();
    {
        flow_sample_t sp = next_sample(1000, 100, 1);
        bool a = flow_estimator_feed(&g_est, &sp);
        flow_sample_t ok = next_sample(2, 200, 2);
        bool b = flow_estimator_feed(&g_est, &ok);
        flow_estimator_get(&g_est, ok.t_ms, &o);
        audit_check(!a && b && g_est.rej.step == 1 && o.valid, "flow_est_single_spike_rejected_estimate_survives");
    }
    prime();
    {
        flow_sample_t s1 = next_sample(1000, 100, 1);
        flow_sample_t s2 = next_sample(1001, 200, 2);
        flow_sample_t s3 = next_sample(1002, 300, 3);
        bool a = flow_estimator_feed(&g_est, &s1);
        bool b = flow_estimator_feed(&g_est, &s2);
        flow_estimator_get(&g_est, s2.t_ms, &o);
        bool inval = !o.valid;
        bool c = flow_estimator_feed(&g_est, &s3);
        audit_check(!a && !b && c && inval && g_est.rej.step == 2,
                    "flow_est_persistent_step_is_level_shift_invalidates_then_resumes");
    }

    /* ---- weight decreasing while ON ---- */
    prime();
    {
        flow_sample_t dn = next_sample(-5, 100, 1);
        bool a = flow_estimator_feed(&g_est, &dn);
        flow_estimator_get(&g_est, dn.t_ms, &o);
        audit_check(!a && g_est.rej.decrease == 1 && !o.valid && o.n_points == 0,
                    "flow_est_weight_decrease_while_on_resets_window_and_invalidates");
    }

    /* ---- implausible slope ---- */
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.max_plausible_flow_g_per_s = 8.0f;     /* plant runs 13.33: every pair too fast */
        sim_setup(13.33f, 70u); sim_init_est(&c);
        sim_cmd(true, 1.0f); sim_run(10000);
        sim_get(&o);
        audit_check(!o.valid && g_est.rej.slope > 0, "flow_est_implausible_slope_rejected_never_valid");
    }

    /* ---- require_stable ---- */
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.require_stable = true;
        sim_setup(13.33f, 80u); sim_init_est(&c);   /* stable only when valve shut */
        sim_cmd(true, 1.0f); sim_run(10000);
        sim_get(&o);
        audit_check(!o.valid && g_est.rej.unstable > 0 && g_est.rej.accepted == 0, "flow_est_require_stable_skips_unstable_samples");
        sim_setup(13.33f, 80u); sim_init_est(&c); g_sim.stable_mode = 1;
        sim_cmd(true, 1.0f); sim_run(10000);
        sim_get(&o);
        audit_check(o.valid && g_est.rej.unstable == 0, "flow_est_require_stable_accepts_stable_samples");
        flow_estimator_cfg_default(&c);
        sim_setup(13.33f, 80u); sim_init_est(&c);
        sim_cmd(true, 1.0f); sim_run(10000);
        sim_get(&o);
        audit_check(o.valid && g_est.rej.unstable == 0, "flow_est_default_does_not_require_stable");
    }

    /* ---- EWMA convergence and alpha bounds ---- */
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.alpha = 1.0f;
        sim_setup(13.33f, 90u); sim_init_est(&c);
        sim_cmd(true, 1.0f); sim_run(12000);
        sim_get(&o);
        audit_check(o.valid && o.q_filtered_g_per_s == o.q_raw_g_per_s, "flow_est_alpha_1_filtered_equals_raw");
        float a_err[2];
        float alphas[2] = { 0.1f, 0.9f };
        for (int i = 0; i < 2; i++) {
            c.alpha = alphas[i];
            sim_setup(13.33f, 91u); sim_init_est(&c);
            g_sim.q_change_ms = 12000; g_sim.q_late = 6.0f;
            sim_cmd(true, 1.0f); sim_run(15500);
            sim_get(&o);
            a_err[i] = fabsf(o.q_filtered_g_per_s - 6.0f);
        }
        ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] ewma err 3.5s after change: alpha0.1=%d mg/s alpha0.9=%d mg/s",
                 (int)(a_err[0] * 1000.0f), (int)(a_err[1] * 1000.0f));
        audit_check(a_err[1] < a_err[0], "flow_est_larger_alpha_converges_faster");
        flow_estimator_cfg_default(&c);
        sim_setup(13.33f, 92u); sim_init_est(&c);
        sim_cmd(true, 1.0f); sim_run(20000);
        sim_get(&o);
        audit_check(iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 50, "flow_est_ewma_converges_to_constant_flow");
        float bad[4] = { 0.0f, -0.5f, 1.5f, NAN };
        bool all = true;
        for (int i = 0; i < 4; i++) {
            c.alpha = bad[i];
            bool r = flow_estimator_init(&g_est, &c);
            if (r || !(g_est.cfg.alpha > 0.0f && g_est.cfg.alpha <= 1.0f)) all = false;
        }
        audit_check(all, "flow_est_alpha_out_of_bounds_sanitised_and_init_reports_it");
    }

    /* ---- NaN / null / overflow guards ---- */
    {
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c);
        c.max_plausible_flow_g_per_s = NAN; c.ok_rel_se = INFINITY; c.window_points = 99;
        c.min_points = 0; c.resolution_g = 0; c.min_delta_g_units = 0;
        bool r = flow_estimator_init(&g_est, &c);
        audit_check(!r && isfinite(g_est.cfg.max_plausible_flow_g_per_s) && isfinite(g_est.cfg.ok_rel_se) &&
                    g_est.cfg.window_points == FLOW_ESTIMATOR_MAX_POINTS && g_est.cfg.min_points >= 3 &&
                    g_est.cfg.resolution_g >= 1 && g_est.cfg.min_delta_g_units >= 1,
                    "flow_est_bad_config_sanitised");
        flow_estimator_cfg_default(&c);
        flow_estimator_init(&g_est, &c);
        flow_estimator_note_relay(&g_est, true, 1000, 1.0f);
        flow_sample_t big = { .weight_g = INT32_MAX, .t_ms = 3000, .seq = 1, .age_ms = 0, .valid = true };
        flow_sample_t small = { .weight_g = INT32_MIN, .t_ms = 3100, .seq = 2, .age_ms = 0, .valid = true };
        flow_estimator_feed(&g_est, &big);
        flow_estimator_feed(&g_est, &small);
        flow_estimator_get(&g_est, 3200, &o);
        audit_check(finite_est(&o) && g_est.rej.step == 1 && !o.valid, "flow_est_extreme_weights_no_overflow_or_nan");
        flow_estimator_get(NULL, 0, &o);
        flow_estimator_get(&g_est, 0, NULL);
        flow_estimator_note_relay(NULL, true, 0, 0.5f);
        audit_check(!flow_estimator_feed(NULL, &big) && !flow_estimator_feed(&g_est, NULL) && !o.valid,
                    "flow_est_null_arguments_are_safe");
        sim_setup(13.33f, 100u);
        g_sim.report_duty = NAN;
        sim_cmd(true, NAN); sim_run(12000);
        sim_get(&o);
        audit_check(o.valid && o.q_at_full_duty_g_per_s == 0.0f && o.q_avg_g_per_s == 0.0f && finite_est(&o) &&
                    o.q_filtered_g_per_s > 0.0f, "flow_est_unknown_duty_gives_zero_full_duty_value");
    }

    /* ---- accumulated ON time and time wrap ---- */
    sim_setup(13.33f, 110u);
    g_sim.t_base = 0xFFFFFC00u;                /* sample clock wraps ~1 s in */
    g_est.on_acc_ms = 0xFFFFFA00u;             /* ON-time accumulator wraps ~1.5 s in */
    sim_cmd(true, 1.0f); sim_run(15000);
    sim_get(&o); info("on_time_and_clock_wrap", &o, 13.33f);
    audit_check(o.valid && iabs(err_permille(o.q_filtered_g_per_s, 13.33f)) <= 50 && g_est.on_acc_ms < 0xFFFFFA00u,
                "flow_est_on_time_and_clock_wrap_safe");

    /* ---- invalid-estimate fallback signalling ---- */
    flow_estimator_init(&g_est, NULL);
    flow_estimator_get(&g_est, 1000, &o);
    audit_check(!o.valid && o.confidence == FLOW_CONF_NONE && o.last_valid_age_ms == 0xFFFFFFFFu &&
                o.q_filtered_g_per_s == 0.0f && o.q_at_full_duty_g_per_s == 0.0f,
                "flow_est_fresh_estimator_signals_invalid_use_static_values");
    prime(); sim_get(&o);
    flow_estimator_reset(&g_est);
    flow_estimator_get(&g_est, g_sim.last_t, &o2);
    audit_check(o.valid && !o2.valid && o2.rej.accepted == 0 && g_est.cfg.alpha > 0.0f,
                "flow_est_reset_clears_estimate_keeps_config");
    prime();
    flow_estimator_get(&g_est, g_sim.last_t + 6000, &o);
    flow_estimator_get(&g_est, g_sim.last_t + 100, &o2);
    audit_check(!o.valid && o.confidence == FLOW_CONF_NONE && o2.valid,
                "flow_est_hold_expires_to_invalid_after_hold_valid_ms");

    /* ---- determinism ---- */
    {
        flow_estimate_t a, b, d;
        flow_estimator_cfg_t c; flow_estimator_cfg_default(&c); c.decrease_tol_g = 4;
        sim_setup(7.0f, 123u); sim_init_est(&c); g_sim.noise = 2; g_sim.drop_pct = 10;
        sim_cmd(true, 1.0f); sim_run(20000); sim_get(&a);
        sim_setup(7.0f, 123u); sim_init_est(&c); g_sim.noise = 2; g_sim.drop_pct = 10;
        sim_cmd(true, 1.0f); sim_run(20000); sim_get(&b);
        sim_setup(7.0f, 124u); sim_init_est(&c); g_sim.noise = 2; g_sim.drop_pct = 10;
        sim_cmd(true, 1.0f); sim_run(20000); sim_get(&d);
        audit_check(memcmp(&a.q_raw_g_per_s, &b.q_raw_g_per_s, sizeof(float)) == 0 &&
                    memcmp(&a.q_filtered_g_per_s, &b.q_filtered_g_per_s, sizeof(float)) == 0 &&
                    a.n_points == b.n_points && a.window_ms == b.window_ms &&
                    a.rej.accepted == b.rej.accepted && a.rej.decrease == b.rej.decrease,
                    "flow_est_deterministic_same_seed_same_result");
        ESP_LOGI(TAG, "TEST_INFO flow_est[SIMULATED] seed123=%d seed124=%d mg/s (true 7000)",
                 (int)(a.q_filtered_g_per_s * 1000.0f), (int)(d.q_filtered_g_per_s * 1000.0f));
    }

    /* ---- no heap use ---- */
    {
        uint32_t h0 = esp_get_free_heap_size();
        sim_setup(13.33f, 130u);
        sim_cmd(true, 1.0f); sim_run(10000);
        uint32_t h1 = esp_get_free_heap_size();
        audit_check(h0 == h1, "flow_est_no_heap_use_during_feed");
        ESP_LOGI(TAG, "TEST_INFO flow_estimator_t sizeof=%u bytes", (unsigned)sizeof(flow_estimator_t));
    }
}
