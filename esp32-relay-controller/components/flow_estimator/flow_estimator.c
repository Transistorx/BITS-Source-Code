/* See flow_estimator.h for the contract. Operational aid only, never a safety
 * authority. No FreeRTOS, no heap, no logging, no static mutable state. */

#include "flow_estimator.h"

#include <math.h>
#include <string.h>

static bool f_ok(float v) { return isfinite(v); }

void flow_estimator_cfg_default(flow_estimator_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->window_points = FLOW_ESTIMATOR_MAX_POINTS;
    cfg->min_points = 6;
    cfg->min_window_ms = 2000;
    cfg->max_window_ms = 30000;
    cfg->point_spacing_ms = 100;
    cfg->resolution_g = 1;
    cfg->min_delta_g_units = 10;
    cfg->decrease_tol_g = 1;
    cfg->alpha = 0.3f;
    cfg->startup_holdoff_ms = 300;
    cfg->shutdown_holdoff_ms = 300;
    cfg->max_plausible_flow_g_per_s = 1000.0f;
    cfg->plausible_min_dt_ms = 100;
    cfg->max_step_g = 500;
    cfg->require_stable = false;
    cfg->max_sample_age_ms = 1000;
    cfg->hold_valid_ms = 5000;
    cfg->off_reset_ms = 10000;
    cfg->seq_restart_gap = 1000;
    cfg->ok_rel_se = 0.15f;
    cfg->low_rel_se = 0.5f;
}

/* Returns true when nothing had to be changed. */
static bool sanitize(flow_estimator_cfg_t *c)
{
    flow_estimator_cfg_t d;
    bool same = true;
    flow_estimator_cfg_default(&d);

    if (c->window_points < 3 || c->window_points > FLOW_ESTIMATOR_MAX_POINTS) {
        c->window_points = d.window_points; same = false;
    }
    if (c->min_points < 3 || c->min_points > c->window_points) {
        c->min_points = (d.min_points <= c->window_points) ? d.min_points : c->window_points;
        same = false;
    }
    if (c->min_window_ms == 0) { c->min_window_ms = d.min_window_ms; same = false; }
    if (c->max_window_ms < c->min_window_ms) { c->max_window_ms = c->min_window_ms; same = false; }
    if (c->point_spacing_ms == 0) { c->point_spacing_ms = d.point_spacing_ms; same = false; }
    if (c->resolution_g < 1) { c->resolution_g = 1; same = false; }
    if (c->min_delta_g_units < 1) { c->min_delta_g_units = 1; same = false; }
    if (c->decrease_tol_g < 0) { c->decrease_tol_g = 0; same = false; }
    if (!f_ok(c->alpha) || c->alpha <= 0.0f || c->alpha > 1.0f) { c->alpha = d.alpha; same = false; }
    if (!f_ok(c->max_plausible_flow_g_per_s) || c->max_plausible_flow_g_per_s <= 0.0f) {
        c->max_plausible_flow_g_per_s = d.max_plausible_flow_g_per_s; same = false;
    }
    if (c->max_step_g < 1) { c->max_step_g = d.max_step_g; same = false; }
    if (!f_ok(c->ok_rel_se) || c->ok_rel_se <= 0.0f) { c->ok_rel_se = d.ok_rel_se; same = false; }
    if (!f_ok(c->low_rel_se) || c->low_rel_se < c->ok_rel_se) { c->low_rel_se = c->ok_rel_se; same = false; }
    return same;
}

bool flow_estimator_init(flow_estimator_t *est, const flow_estimator_cfg_t *cfg)
{
    flow_estimator_cfg_t c;
    bool same = true;
    if (!est) return false;
    if (cfg) { c = *cfg; } else { flow_estimator_cfg_default(&c); }
    same = sanitize(&c);
    memset(est, 0, sizeof(*est));
    est->cfg = c;
    flow_estimator_reset(est);
    return same && cfg != NULL;
}

void flow_estimator_reset(flow_estimator_t *est)
{
    flow_estimator_cfg_t c;
    if (!est) return;
    c = est->cfg;
    memset(est, 0, sizeof(*est));
    est->cfg = c;
    est->spacing_ms = c.point_spacing_ms;
}

/* Discard the fit window only (counts a reset). */
static void window_clear(flow_estimator_t *e)
{
    if (e->n) e->rej.resets++;
    e->n = 0;
    e->spacing_ms = e->cfg.point_spacing_ms;
    e->window_ms = 0;
    e->last_slope = 0.0f;
}

/* Discard window AND held/filtered estimate: next valid needs a fresh fit. */
static void invalidate(flow_estimator_t *e)
{
    window_clear(e);
    e->have_valid = false;
    e->holding = false;
    e->have_filt = false;
    e->q_raw = 0.0f;
    e->q_filt = 0.0f;
    e->last_conf = FLOW_CONF_NONE;
}

/* Advance accumulated ON time to `now` (wrap-safe, never goes backwards). */
static void advance(flow_estimator_t *e, uint32_t now)
{
    if (!e->adv_init) { e->adv_init = true; e->last_adv_ms = now; return; }
    int32_t d = (int32_t)(now - e->last_adv_ms);
    if (d > 0) {
        if (e->relay_on) e->on_acc_ms += (uint32_t)d;
        e->last_adv_ms = now;
    }
}

void flow_estimator_note_relay(flow_estimator_t *e, bool on, uint32_t now_ms, float duty)
{
    if (!e) return;
    advance(e, now_ms);
    e->duty_known = f_ok(duty) && duty > 0.0f && duty <= 1.0f;
    e->duty = e->duty_known ? duty : 0.0f;
    if (on && !e->relay_on) {
        e->relay_on = true;
        e->have_on_edge = true;
        e->on_edge_ms = now_ms;
    } else if (!on && e->relay_on) {
        e->relay_on = false;
        e->have_off_edge = true;
        e->off_edge_ms = now_ms;
    }
}

/* Compact: keep every second point counting back from the newest. */
static void compact(flow_estimator_t *e)
{
    uint8_t k = 0;
    for (uint8_t i = 0; i < e->n; i++) {
        if (((e->n - 1u - i) & 1u) == 0) e->pts[k++] = e->pts[i];
    }
    e->n = k;
}

static void drop_oldest(flow_estimator_t *e)
{
    for (uint8_t i = 1; i < e->n; i++) e->pts[i - 1] = e->pts[i];
    if (e->n) e->n--;
}

/* ON-time span the window should reach before points are dropped. */
static uint32_t desired_span_ms(const flow_estimator_t *e)
{
    const flow_estimator_cfg_t *c = &e->cfg;
    int64_t delta = (int64_t)e->pts[e->n - 1].w_g - e->pts[0].w_g;
    int64_t mind = (int64_t)c->min_delta_g_units * c->resolution_g;
    if (delta < mind || !(e->last_slope > 1e-6f)) return c->max_window_ms;
    float need = (float)mind / e->last_slope * 1000.0f;
    uint32_t d = c->min_window_ms;
    if (f_ok(need) && need > (float)d) d = (need >= (float)c->max_window_ms) ? c->max_window_ms : (uint32_t)need;
    return d;
}

static void fit(flow_estimator_t *e, uint32_t now_ms)
{
    const flow_estimator_cfg_t *c = &e->cfg;
    uint8_t n = e->n;
    if (n < 2) { e->window_ms = 0; return; }
    uint32_t x0 = e->pts[0].x_ms;
    int32_t w0 = e->pts[0].w_g;
    uint32_t span = e->pts[n - 1].x_ms - x0;
    e->window_ms = span;

    float xm = 0.0f, ym = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        xm += (float)(uint32_t)(e->pts[i].x_ms - x0) * 0.001f;
        ym += (float)((int64_t)e->pts[i].w_g - w0);
    }
    xm /= (float)n; ym /= (float)n;
    float sxx = 0.0f, sxy = 0.0f, syy = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        float dx = (float)(uint32_t)(e->pts[i].x_ms - x0) * 0.001f - xm;
        float dy = (float)((int64_t)e->pts[i].w_g - w0) - ym;
        sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
    }
    bool ok = false;
    float slope = 0.0f, rel = 0.0f;
    if (sxx > 1e-6f) {
        slope = sxy / sxx;
        if (f_ok(slope)) {
            e->last_slope = slope > 0.0f ? slope : 0.0f;
            if (slope > c->max_plausible_flow_g_per_s) {
                e->rej.slope++;
            } else if (slope > 0.0f && n >= c->min_points) {
                float ssr = syy - slope * sxy;
                if (ssr < 0.0f) ssr = 0.0f;
                float se = sqrtf(ssr / (float)(n - 2) / sxx);
                rel = se / slope;
                int64_t delta = (int64_t)e->pts[n - 1].w_g - w0;
                int64_t mind = (int64_t)c->min_delta_g_units * c->resolution_g;
                float need = (float)mind / slope * 1000.0f;
                uint32_t need_ms = (f_ok(need) && need < 4.0e9f) ? (uint32_t)need : 0xFFFFFFFFu;
                uint32_t req = c->min_window_ms > need_ms ? c->min_window_ms : need_ms;
                ok = f_ok(rel) && rel <= c->low_rel_se && delta >= mind && span >= req;
            }
        }
    } else {
        e->last_slope = 0.0f;
    }

    if (ok) {
        e->q_raw = slope;
        if (!e->have_filt) { e->q_filt = slope; e->have_filt = true; }
        else e->q_filt = (1.0f - c->alpha) * e->q_filt + c->alpha * slope;
        e->have_valid = true;
        e->holding = false;
        e->last_valid_ms = now_ms;
        e->last_conf = (rel <= c->ok_rel_se) ? FLOW_CONF_OK : FLOW_CONF_LOW;
    } else if (e->have_valid) {
        e->holding = true;
    }
}

bool flow_estimator_feed(flow_estimator_t *e, const flow_sample_t *s)
{
    if (!e || !s) return false;
    const flow_estimator_cfg_t *c = &e->cfg;

    if (!s->valid) { e->rej.invalid++; return false; }
    if (s->age_ms > c->max_sample_age_ms) {
        e->rej.stale++;
        invalidate(e);
        return false;
    }
    if (s->seq != 0) {
        if (e->have_seq) {
            int32_t d = (int32_t)(s->seq - e->last_seq);
            if (d <= 0) {
                if (c->seq_restart_gap > 0 && d <= -(int32_t)c->seq_restart_gap) {
                    e->rej.seq_restarts++;
                    invalidate(e);
                    e->have_t = false;
                    e->have_w = false;
                } else {
                    e->rej.seq++;
                    return false;
                }
            }
        }
        e->last_seq = s->seq;
        e->have_seq = true;
    }
    if (e->have_t && (int32_t)(s->t_ms - e->last_t_ms) <= 0) {
        e->rej.time++;
        return false;
    }
    e->last_t_ms = s->t_ms;
    e->have_t = true;
    advance(e, s->t_ms);

    if (c->require_stable && !s->stable) { e->rej.unstable++; return false; }

    int32_t w = s->weight_g;
    int32_t prev_w = w;
    if (e->have_w) {
        int64_t diff = (int64_t)w - e->last_w;
        if (diff < 0) diff = -diff;
        if (diff > c->max_step_g) {
            e->rej.step++;
            if (++e->step_consec >= 2) {      /* persistent: new level */
                invalidate(e);
                e->last_w = w;
                e->step_consec = 0;
            }
            return false;
        }
        prev_w = e->last_w;
    }
    e->step_consec = 0;
    e->last_w = w;
    e->have_w = true;

    uint32_t t = s->t_ms;
    if (!e->relay_on) {
        if (e->have_off_edge && (uint32_t)(t - e->off_edge_ms) < c->shutdown_holdoff_ms) {
            e->rej.shutdown++;
            return false;
        }
        e->rej.off_samples++;
        if (e->n && e->have_off_edge && (uint32_t)(t - e->off_edge_ms) > c->off_reset_ms) {
            window_clear(e);
        }
        return false;
    }
    if (e->have_on_edge && (uint32_t)(t - e->on_edge_ms) < c->startup_holdoff_ms) {
        e->rej.startup++;
        return false;
    }

    int32_t ref_w = e->n ? e->pts[e->n - 1].w_g : prev_w;
    if ((int64_t)w < (int64_t)ref_w - c->decrease_tol_g) {
        e->rej.decrease++;
        invalidate(e);
        return false;
    }

    uint32_t x = e->on_acc_ms;
    if (e->n) {
        uint32_t dx = x - e->pts[e->n - 1].x_ms;
        if (dx >= c->plausible_min_dt_ms) {
            float sl = (float)((int64_t)w - e->pts[e->n - 1].w_g) / ((float)dx * 0.001f);
            if (!f_ok(sl) || sl > c->max_plausible_flow_g_per_s) {
                e->rej.slope++;
                return false;
            }
        }
        e->rej.accepted++;
        if (dx < e->spacing_ms) return true;     /* accepted, not stored */
    } else {
        e->rej.accepted++;
    }

    while (e->n && (uint32_t)(x - e->pts[0].x_ms) > c->max_window_ms) drop_oldest(e);
    if (e->n >= c->window_points) {
        uint32_t span = e->pts[e->n - 1].x_ms - e->pts[0].x_ms;
        uint32_t want = desired_span_ms(e);
        if (span < want && e->spacing_ms < c->max_window_ms) {
            compact(e);
            e->spacing_ms = (e->spacing_ms > c->max_window_ms / 2) ? c->max_window_ms : e->spacing_ms * 2u;
        } else {
            if (e->spacing_ms > c->point_spacing_ms && span > 2u * want) {
                e->spacing_ms /= 2u;
                if (e->spacing_ms < c->point_spacing_ms) e->spacing_ms = c->point_spacing_ms;
            }
            drop_oldest(e);
        }
    }
    e->pts[e->n].x_ms = x;
    e->pts[e->n].w_g = w;
    e->n++;
    fit(e, t);
    return true;
}

void flow_estimator_get(const flow_estimator_t *e, uint32_t now_ms, flow_estimate_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->last_valid_age_ms = 0xFFFFFFFFu;
    out->confidence = FLOW_CONF_NONE;
    if (!e) return;

    out->rej = e->rej;
    out->n_points = e->n;
    out->window_ms = e->window_ms;
    if (!e->have_valid) return;

    int32_t d = (int32_t)(now_ms - e->last_valid_ms);
    uint32_t age = d > 0 ? (uint32_t)d : 0u;
    out->last_valid_age_ms = age;
    if (age > e->cfg.hold_valid_ms) return;           /* expired: INVALID */
    if (!f_ok(e->q_filt) || !f_ok(e->q_raw)) return;

    out->valid = true;
    out->q_raw_g_per_s = e->q_raw;
    out->q_filtered_g_per_s = e->q_filt;
    if (e->duty_known) {
        out->q_at_full_duty_g_per_s = e->q_filt;
        out->q_avg_g_per_s = e->q_filt * e->duty;
    }
    out->confidence = e->last_conf;
    if (e->holding || age > e->cfg.hold_valid_ms / 2u) out->confidence = FLOW_CONF_LOW;
}
