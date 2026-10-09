/* Auto-tune characterization maths. See include/autotune_char.h for the
 * equations. Pure logic: no heap, no globals, no logging, no relay access. */

#include "autotune_char.h"

#include <math.h>
#include <string.h>

typedef struct {
    uint16_t n;
    float xb, yb, sxx, sxy, syy, b, a, s2, xmin, xmax;   /* x in seconds */
} ls_t;

static float xms(const at_trial_t *tr, uint16_t i)
{
    return (float)(int32_t)(tr->s[i].t_ms - tr->s[i].age_ms - tr->transport_ms
                            - tr->on_cmd_ms);
}

/* LS fit of w on x (seconds) over samples with lo <= x_ms <= hi. */
static void ls_fit(const at_trial_t *tr, float lo, float hi, ls_t *o)
{
    memset(o, 0, sizeof(*o));
    float sx = 0.0f, sy = 0.0f;
    for (uint16_t i = 0; i < tr->n; i++) {
        float xm = xms(tr, i);
        if (xm < lo || xm > hi) continue;
        float x = xm * 0.001f;
        if (o->n == 0) { o->xmin = x; o->xmax = x; }
        if (x < o->xmin) o->xmin = x;
        if (x > o->xmax) o->xmax = x;
        o->n++;
        sx += x;
        sy += (float)tr->s[i].w_g;
    }
    if (o->n == 0) return;
    o->xb = sx / (float)o->n;
    o->yb = sy / (float)o->n;
    for (uint16_t i = 0; i < tr->n; i++) {
        float xm = xms(tr, i);
        if (xm < lo || xm > hi) continue;
        float dx = xm * 0.001f - o->xb;
        float dy = (float)tr->s[i].w_g - o->yb;
        o->sxx += dx * dx;
        o->sxy += dx * dy;
        o->syy += dy * dy;
    }
    if (o->n >= 2 && o->sxx > 1e-9f) {
        o->b = o->sxy / o->sxx;
        o->a = o->yb - o->b * o->xb;
        float sse = o->syy - o->b * o->sxy;
        if (sse < 0.0f) sse = 0.0f;
        if (o->n > 2) o->s2 = sse / (float)(o->n - 2);
    }
}

static bool fit_ok(const autotune_char_cfg_t *cfg, const ls_t *f)
{
    return f->n >= cfg->min_q_points && f->sxx > 1e-9f
        && (f->xmax - f->xmin) * 1000.0f >= (float)cfg->min_q_span_ms;
}

void autotune_char_cfg_default(autotune_char_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->resolution_g = 1;
    cfg->k_sigma = 3.0f;
    cfg->k_range = 5.0f;
    cfg->edge_excl_ms = 300;
    cfg->min_baseline_n = 5;
    cfg->sustain_n = 3;
    cfg->min_q_points = 4;
    cfg->min_q_span_ms = 500;
    cfg->settle_win_ms = 2000;
    cfg->min_settle_ms = 500;
    cfg->settle_timeout_ms = 10000;
    cfg->settle_slope_g_per_s = 0.5f;
    cfg->settle_min_n = 4;
    cfg->n_ok = 5;
    cfg->cv_ok_q = 0.15f;
    cfg->cv_ok_c = 0.35f;
    cfg->cv_ok_startup = 0.35f;
}

static bool cfg_valid(const autotune_char_cfg_t *c)
{
    return c && c->resolution_g >= 1 && c->k_sigma > 0.0f && c->k_range > 0.0f
        && c->edge_excl_ms > 0 && c->min_baseline_n >= 3 && c->sustain_n >= 1
        && c->min_q_points >= 3 && c->min_q_span_ms > 0 && c->settle_win_ms > 0
        && c->settle_timeout_ms > 0 && c->settle_slope_g_per_s > 0.0f
        && c->settle_min_n >= 3 && c->n_ok >= 2
        && c->cv_ok_q > 0.0f && c->cv_ok_c > 0.0f && c->cv_ok_startup > 0.0f;
}

bool autotune_char_init(autotune_char_t *c, uint8_t channel, uint32_t material_id,
                        const autotune_char_cfg_t *cfg)
{
    if (!c) return false;
    memset(c, 0, sizeof(*c));
    if (cfg) c->cfg = *cfg; else autotune_char_cfg_default(&c->cfg);
    if ((channel != 1 && channel != 2) || !cfg_valid(&c->cfg)) {
        c->channel = 0;   /* unusable: every add is refused */
        return false;
    }
    c->channel = channel;
    c->material_id = material_id;
    return true;
}

#define TFAIL(r) do { out->reason = (r); return (r); } while (0)

at_trial_reason_t autotune_char_analyze_trial(const autotune_char_cfg_t *cfg,
                                              const at_trial_t *tr,
                                              at_trial_result_t *out)
{
    if (!out) return AT_TRIAL_BAD_INPUT;
    memset(out, 0, sizeof(*out));
    if (!cfg_valid(cfg) || !tr || !tr->s || tr->n < 2) TFAIL(AT_TRIAL_BAD_INPUT);
    out->trial_id = tr->trial_id;
    out->resolution_g = cfg->resolution_g;
    int32_t on_ms = (int32_t)(tr->off_cmd_ms - tr->on_cmd_ms);
    if (on_ms <= 0) TFAIL(AT_TRIAL_BAD_INPUT);
    for (uint16_t i = 1; i < tr->n; i++) {
        if ((int32_t)(tr->s[i].t_ms - tr->s[i - 1].t_ms) <= 0) TFAIL(AT_TRIAL_BAD_INPUT);
    }
    if ((uint32_t)on_ms < 2u * cfg->edge_excl_ms) TFAIL(AT_TRIAL_REJECTED_TOO_SHORT);
    const float off_f = (float)on_ms;
    const float res = (float)cfg->resolution_g;

    /* (b) latency (known part) and mean sample period */
    float age_sum = 0.0f;
    for (uint16_t i = 0; i < tr->n; i++) age_sum += (float)tr->s[i].age_ms;
    out->latency_ms = age_sum / (float)tr->n + (float)tr->transport_ms;
    out->valid |= AT_V_LATENCY;
    out->dt_ms = (float)(int32_t)(tr->s[tr->n - 1].t_ms - tr->s[0].t_ms)
                 / (float)(tr->n - 1);

    /* (a) noise from the at-rest baseline */
    ls_t bl;
    ls_fit(tr, -1.0e9f, -1.0f, &bl);
    if (bl.n < cfg->min_baseline_n) TFAIL(AT_TRIAL_NO_BASELINE);
    const float sigma = sqrtf(bl.s2);
    const float w0 = bl.yb;
    const float thr = cfg->k_sigma * sigma + res;
    out->sigma_g = sigma;
    out->noise_thr_g = thr;
    out->valid |= AT_V_SIGMA;

    /* (c) startup: first sustained rise above thr while ON */
    int detect = -1;
    for (uint16_t i = 0; i < tr->n; i++) {
        float x = xms(tr, i);
        if (x < 0.0f) continue;
        if (x > off_f) break;
        if ((float)tr->s[i].w_g - w0 <= thr) continue;
        uint8_t ok = 1;
        for (uint16_t j = (uint16_t)(i + 1); j < tr->n && ok < cfg->sustain_n; j++) {
            if ((float)tr->s[j].w_g - w0 > thr) ok++; else break;
        }
        if (ok >= cfg->sustain_n) { detect = (int)i; break; }
    }
    if (detect < 0) TFAIL(AT_TRIAL_NO_RISE);
    const float xd = xms(tr, (uint16_t)detect);
    out->startup_ms = xd;
    out->valid |= AT_V_STARTUP;

    /* (e) steady slope: pass 1 over the later half of the flowing span */
    const float excl = (float)cfg->edge_excl_ms;
    float lo1 = xd + 0.5f * (off_f - xd);
    if (lo1 < excl) lo1 = excl;
    ls_t f1;
    ls_fit(tr, lo1, off_f, &f1);
    if (!fit_ok(cfg, &f1)) {
        lo1 = (xd > excl) ? xd : excl;
        ls_fit(tr, lo1, off_f, &f1);
        if (!fit_ok(cfg, &f1)) TFAIL(AT_TRIAL_Q_FIT_FAILED);
    }
    if (!(f1.b > 0.0f)) TFAIL(AT_TRIAL_Q_FIT_FAILED);

    /* (d) ramp: last ON sample more than thr below the pass-1 line */
    float last_off = -1.0f;
    for (uint16_t i = 0; i < tr->n; i++) {
        float x = xms(tr, i);
        if (x < xd || x > off_f) continue;
        float r = (float)tr->s[i].w_g - (f1.a + f1.b * x * 0.001f);
        if (r < -thr) last_off = x;
    }
    out->ramp_ms = (last_off >= xd) ? (last_off - xd) : 0.0f;
    out->valid |= AT_V_RAMP;

    /* pass 2 with the ramp removed, if that gives more points */
    ls_t fq = f1;
    float lo2 = xd + out->ramp_ms;
    if (lo2 < excl) lo2 = excl;
    if (lo2 < lo1) {
        ls_t f2;
        ls_fit(tr, lo2, off_f, &f2);
        if (fit_ok(cfg, &f2) && f2.b > 0.0f) fq = f2;
    }
    const float s2e = (fq.s2 > sigma * sigma) ? fq.s2 : sigma * sigma;
    out->q_eff = fq.b;
    out->q_se = sqrtf(s2e / fq.sxx);
    float duty = 1.0f;
    if (tr->period_ms > 0) {
        duty = (float)on_ms / (float)tr->period_ms;
        if (duty > 1.0f) duty = 1.0f;
    }
    out->q_at_duty = fq.b * duty;
    out->startup_intercept_ms = (w0 - fq.a) / fq.b * 1000.0f;
    out->valid |= AT_V_Q;
    const float xo = off_f * 0.001f;
    out->w_off_g = fq.a + fq.b * xo;
    const float var_off = s2e * (1.0f / (float)fq.n + (xo - fq.xb) * (xo - fq.xb) / fq.sxx);

    /* (g) first settled window after OFF */
    int win_i = -1;
    float w_set = 0.0f, set_x = 0.0f, nwin = 1.0f;
    for (uint16_t i = 0; i < tr->n; i++) {
        float x = xms(tr, i);
        float rel = x - off_f;
        if (rel < (float)cfg->min_settle_ms) continue;
        if (rel > (float)cfg->settle_timeout_ms) break;
        float mn = (float)tr->s[i].w_g, mx = mn, last_x = x;
        uint16_t cnt = 0;
        for (uint16_t j = i; j < tr->n; j++) {
            float xj = xms(tr, j);
            if (xj - x > (float)cfg->settle_win_ms) break;
            float wj = (float)tr->s[j].w_g;
            if (wj < mn) mn = wj;
            if (wj > mx) mx = wj;
            last_x = xj;
            cnt++;
        }
        if (cnt < cfg->settle_min_n) continue;
        if (last_x - x < 0.75f * (float)cfg->settle_win_ms) continue;
        if (mx - mn > cfg->k_range * sigma + res) continue;
        ls_t sw;
        ls_fit(tr, x, x + (float)cfg->settle_win_ms, &sw);
        if (fabsf(sw.b) > cfg->settle_slope_g_per_s) continue;
        win_i = (int)i;
        w_set = sw.yb;
        set_x = x;
        nwin = (float)sw.n;
        break;
    }
    if (win_i < 0) {
        float x_last = xms(tr, (uint16_t)(tr->n - 1));
        if (x_last - off_f >= (float)(cfg->settle_timeout_ms + cfg->settle_win_ms))
            TFAIL(AT_TRIAL_UNSETTLED);
        TFAIL(AT_TRIAL_TAIL_TOO_SHORT);
    }
    out->w_settled_g = w_set;
    out->settle_ms = set_x - off_f;
    out->valid |= AT_V_SETTLED;

    /* (f) post-shutoff delivery */
    out->c_g = w_set - out->w_off_g;
    out->c_se_g = sqrtf(s2e / nwin + var_off);
    out->valid |= AT_V_C;
    if (out->c_g < -thr) TFAIL(AT_TRIAL_NEGATIVE_C);

    /* tau_off: weighted LS of ln(W_inf - w) over tail points above thr */
    float sw_ = 0.0f, swx = 0.0f, swy = 0.0f;
    uint16_t np = 0;
    float rmax = 0.0f;
    for (uint16_t i = 0; i < tr->n; i++) {
        float x = xms(tr, i);
        if (x < off_f || x >= set_x) continue;
        float r = w_set - (float)tr->s[i].w_g;
        if (r <= thr) continue;
        float wt = r * r;
        sw_ += wt; swx += wt * x * 0.001f; swy += wt * logf(r);
        if (r > rmax) rmax = r;
        np++;
    }
    /* dynamic range gate: the tail must start well above the noise threshold */
    if (np >= 4 && rmax >= 1.5f * thr) {
        float xb = swx / sw_, yb = swy / sw_;
        float sxx = 0.0f, sxy = 0.0f;
        for (uint16_t i = 0; i < tr->n; i++) {
            float x = xms(tr, i);
            if (x < off_f || x >= set_x) continue;
            float r = w_set - (float)tr->s[i].w_g;
            if (r <= thr) continue;
            float wt = r * r, dx = x * 0.001f - xb;
            sxx += wt * dx * dx;
            sxy += wt * dx * (logf(r) - yb);
        }
        if (sxx > 1e-9f) {
            float m = sxy / sxx;
            if (m < -1e-3f) {
                float tau = -1000.0f / m;
                /* W_inf is the settled mean, which misses about slope_limit*tau of
                 * tail still to come. If that bias is a large part of the tail
                 * amplitude the fit is not trusted. */
                if (cfg->settle_slope_g_per_s * tau * 0.001f <= 0.3f * rmax) {
                    out->tau_off_ms = tau;
                    out->valid |= AT_V_TAU;
                }
            }
        }
    }
    out->reason = AT_TRIAL_OK;
    return AT_TRIAL_OK;
}

at_trial_reason_t autotune_char_commit(autotune_char_t *c, const at_trial_result_t *r)
{
    if (!c || !r || c->channel == 0) return AT_TRIAL_BAD_INPUT;
    if (c->n >= AT_CHAR_MAX_TRIALS) return AT_TRIAL_TABLE_FULL;
    c->res[c->n++] = *r;
    return r->reason;
}

at_trial_reason_t autotune_char_add_trial(autotune_char_t *c, const at_trial_t *tr,
                                          at_trial_result_t *out)
{
    at_trial_result_t tmp;
    at_trial_result_t *o = out ? out : &tmp;
    if (!c || !tr || c->channel == 0) {
        if (out) { memset(out, 0, sizeof(*out)); out->reason = AT_TRIAL_BAD_INPUT; }
        return AT_TRIAL_BAD_INPUT;
    }
    if (tr->channel != c->channel) {
        memset(o, 0, sizeof(*o));
        o->reason = AT_TRIAL_CHANNEL_MISMATCH;
        return AT_TRIAL_CHANNEL_MISMATCH;
    }
    at_trial_reason_t r = autotune_char_analyze_trial(&c->cfg, tr, o);
    at_trial_reason_t cr = autotune_char_commit(c, o);
    return (cr == AT_TRIAL_TABLE_FULL) ? cr : r;
}

float autotune_t95_pred_k(uint8_t n)
{
    /* one-sided 95% Student t, df = 1..15 */
    static const float t95[15] = {
        6.314f, 2.920f, 2.353f, 2.132f, 2.015f, 1.943f, 1.895f, 1.860f,
        1.833f, 1.812f, 1.796f, 1.782f, 1.771f, 1.761f, 1.753f,
    };
    if (n < 2) return 0.0f;
    uint8_t df = (uint8_t)(n - 1);
    if (df > 15) df = 15;
    return t95[df - 1] * sqrtf(1.0f + 1.0f / (float)n);
}

static bool trial_fully_valid(const at_trial_result_t *r)
{
    const uint16_t need = AT_V_SIGMA | AT_V_STARTUP | AT_V_Q | AT_V_C | AT_V_SETTLED;
    return r->reason == AT_TRIAL_OK && (r->valid & need) == need;
}

static float pick(const at_trial_result_t *r, int which)
{
    switch (which) {
    case 0: return r->sigma_g;
    case 1: return r->latency_ms;
    case 2: return r->startup_ms;
    case 3: return r->q_eff;
    case 4: return r->c_g;
    default: return r->tau_off_ms;
    }
}

static void stat_calc(const autotune_char_t *c, int which, at_stat_t *s)
{
    memset(s, 0, sizeof(*s));
    float sum = 0.0f;
    for (uint8_t i = 0; i < c->n; i++) {
        const at_trial_result_t *r = &c->res[i];
        if (!trial_fully_valid(r)) continue;
        if (which == 5 && !(r->valid & AT_V_TAU)) continue;
        sum += pick(r, which);
        s->n++;
    }
    if (s->n == 0) return;
    s->mean = sum / (float)s->n;
    if (s->n < 2) return;
    float ss = 0.0f;
    for (uint8_t i = 0; i < c->n; i++) {
        const at_trial_result_t *r = &c->res[i];
        if (!trial_fully_valid(r)) continue;
        if (which == 5 && !(r->valid & AT_V_TAU)) continue;
        float d = pick(r, which) - s->mean;
        ss += d * d;
    }
    s->sd = sqrtf(ss / (float)(s->n - 1));
    float k = autotune_t95_pred_k(s->n);
    s->upper = s->mean + k * s->sd;
    s->lower = s->mean - k * s->sd;
    s->bound_valid = true;
}

void autotune_char_summarize(const autotune_char_t *c, autotune_char_result_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!c) return;
    out->channel = c->channel;
    out->material_id = c->material_id;
    out->resolution_g = c->cfg.resolution_g;
    out->edge_excl_ms = c->cfg.edge_excl_ms;
    out->n_trials = c->n;
    float dt_sum = 0.0f;
    for (uint8_t i = 0; i < c->n; i++) {
        const at_trial_result_t *r = &c->res[i];
        if (r->reason == AT_TRIAL_REJECTED_TOO_SHORT) out->n_too_short++;
        else if (r->reason != AT_TRIAL_OK) out->n_other_rejected++;
        if (trial_fully_valid(r)) {
            out->trial_ids[out->n_valid++] = r->trial_id;
            dt_sum += r->dt_ms;
        }
    }
    stat_calc(c, 0, &out->sigma);
    stat_calc(c, 1, &out->latency);
    stat_calc(c, 2, &out->startup);
    stat_calc(c, 3, &out->q_eff);
    stat_calc(c, 4, &out->c);
    stat_calc(c, 5, &out->tau);
    if (out->n_valid == 0) {
        out->confidence = AT_CONF_NONE;
        out->conf_reasons = AT_CR_NO_VALID_TRIAL;
        return;
    }
    out->confidence = AT_CONF_OK;
    const autotune_char_cfg_t *g = &c->cfg;
    if (out->n_valid < g->n_ok) out->conf_reasons |= AT_CR_LOW_N;
    if (out->n_valid < 2 ||
        out->q_eff.sd > g->cv_ok_q * fabsf(out->q_eff.mean))
        out->conf_reasons |= AT_CR_SCATTER_Q;
    if (out->n_valid < 2 ||
        out->c.sd > g->cv_ok_c * fabsf(out->c.mean) + out->sigma.mean)
        out->conf_reasons |= AT_CR_SCATTER_C;
    if (out->n_valid < 2 ||
        out->startup.sd > g->cv_ok_startup * out->startup.mean
                          + dt_sum / (float)out->n_valid)
        out->conf_reasons |= AT_CR_SCATTER_START;
    if (out->conf_reasons) out->confidence = AT_CONF_LOW;
    if (out->tau.n == 0) out->conf_reasons |= AT_CR_NO_TAU;   /* informational */
}
