/* Supervised AUTO_TUNE sequencer. See include/autotune_seq.h. Pure logic: the
 * only output is the relay_on request in the status struct / return value. */

#include "autotune_seq.h"

#include <math.h>
#include <string.h>

#define AT_SENDER_RESTART_GAP 8u   /* seq jump backwards by more than this = sender restart */

autotune_limits_err_t autotune_limits_validate(const autotune_limits_t *l)
{
    if (!l) return AT_LIM_NULL;
    if (l->target_mass_g <= 0) return AT_LIM_TARGET;
    if (l->max_pulse_on_ms == 0) return AT_LIM_PULSE;
    if (l->max_total_on_ms == 0) return AT_LIM_TOTAL;
    if (l->max_wall_ms == 0) return AT_LIM_WALL;
    if (l->max_delivered_g <= 0) return AT_LIM_DELIVERED;
    if (l->max_delivered_g <= l->target_mass_g || l->max_total_on_ms < l->max_pulse_on_ms
        || l->max_wall_ms <= l->max_total_on_ms)
        return AT_LIM_ORDER;
    return AT_LIM_OK;
}

void autotune_seq_cfg_default(autotune_seq_cfg_t *cfg)
{
    if (cfg) memset(cfg, 0, sizeof(*cfg));   /* hw_enable = false, rest missing */
}

static autotune_cfg_err_t cfg_check(autotune_seq_t *s, const autotune_seq_cfg_t *c,
                                    const autotune_limits_t *l)
{
    if (!c) return AT_CFG_NULL;
    if (autotune_limits_validate(l) != AT_LIM_OK) return AT_CFG_LIMITS;
    if ((c->channel != 1 && c->channel != 2) || c->material_id != (uint32_t)c->channel)
        return AT_CFG_CHANNEL;
    if (!autotune_char_init(&s->ch, c->channel, c->material_id, &c->ch)) return AT_CFG_CHAR;
    if (c->pulse_on_ms < 2u * c->ch.edge_excl_ms || c->pulse_on_ms >= l->max_pulse_on_ms
        || c->min_valid_trials < 2 || c->n_trials < c->min_valid_trials
        || c->n_trials > AT_CHAR_MAX_TRIALS
        || (uint64_t)c->n_trials * c->pulse_on_ms >= l->max_total_on_ms)
        return AT_CFG_PLAN;
    if (c->rest_ms == 0 || c->off_guard_ms == 0 || c->stale_ms == 0) return AT_CFG_TIMING;
    if (c->noprog_window_ms == 0 || c->noprog_n_res == 0 || c->noprog_cum_on_ms == 0)
        return AT_CFG_WATCHDOG;
    if (c->decrease_tol_g <= 0 || c->leak_tol_g <= 0 || c->max_step_g <= 0
        || !(c->max_rate_g_per_s > 0.0f) || c->surge_win_ms == 0)
        return AT_CFG_ANOMALY;
    if (c->assumed_post_off_g <= 0
        || c->assumed_post_off_g + c->ch.resolution_g >= l->target_mass_g)
        return AT_CFG_MARGIN;
    if (!(c->tc_s > 0.0f) || !(c->tc_min_s > 0.0f) || c->tc_max_s < c->tc_min_s
        || c->tc_s < c->tc_min_s || c->tc_s > c->tc_max_s)
        return AT_CFG_TC;
    return AT_CFG_OK;
}

bool autotune_seq_init(autotune_seq_t *s, const autotune_seq_cfg_t *cfg,
                       const autotune_limits_t *lim)
{
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    s->state = AT_S_IDLE;
    s->origin = AT_ORIGIN_UNKNOWN;
    if (cfg) s->cfg = *cfg;
    if (lim) s->lim = *lim;
    s->cfg_err = cfg_check(s, cfg, lim);
    s->init_ok = (s->cfg_err == AT_CFG_OK);
    if (!s->init_ok) {
        /* fail closed: nothing usable is left behind */
        bool hw = false;
        memset(&s->lim, 0, sizeof(s->lim));
        memset(&s->cfg, 0, sizeof(s->cfg));
        s->cfg.hw_enable = hw;
        return false;
    }
    s->margin_g = cfg->assumed_post_off_g + cfg->ch.resolution_g;
    return true;
}

void autotune_seq_set_origin(autotune_seq_t *s, at_origin_t o)
{
    if (s) s->origin = o;
}

bool autotune_seq_feed(autotune_seq_t *s, const autotune_wsample_t *w)
{
    if (!s || !w) return false;
    if (!w->valid) { s->invalid_seen = true; return false; }
    if (w->seq != 0 && s->have_seq) {
        int32_t d = (int32_t)(w->seq - s->last_seq);
        if (d == 0) return false;                       /* duplicate */
        if (d < 0) {
            if ((uint32_t)(-d) > AT_SENDER_RESTART_GAP) s->sender_restart = true;
            return false;                               /* reordered / restart */
        }
    }
    if (s->have_w && (int32_t)(w->t_ms - s->last.t_ms) <= 0) return false;
    if (w->seq != 0) { s->have_seq = true; s->last_seq = w->seq; }
    s->last = *w;
    s->have_w = true;
    s->new_sample = true;
    if (s->state == AT_S_MEASURING_STARTUP || s->state == AT_S_CALIBRATING_FLOW
        || s->state == AT_S_MEASURING_SHUTOFF) {
        if (s->nbuf < AT_SEQ_MAX_SAMPLES) {
            s->buf[s->nbuf].t_ms = w->t_ms;
            s->buf[s->nbuf].w_g = w->weight_g;
            s->buf[s->nbuf].age_ms = w->age_ms;
            s->nbuf++;
        } else {
            s->buf_overflow = true;
        }
    }
    return true;
}

static bool is_terminal(autotune_state_t st)
{
    return st == AT_S_COMPLETED || st == AT_S_FAILED || st == AT_S_CANCELLED;
}

static void end_state(autotune_seq_t *s, autotune_state_t st, autotune_reason_t r, uint32_t now)
{
    if (s->req_on) s->cum_on_ms += (uint32_t)(now - s->on_start);
    s->req_on = false;
    s->state = st;
    s->reason = r;
}

static void update_margin(autotune_seq_t *s)
{
    int32_t m = s->cfg.assumed_post_off_g;
    autotune_char_result_t r;
    autotune_char_summarize(&s->ch, &r);
    if (r.c.bound_valid && isfinite(r.c.upper)) {
        int32_t c = (int32_t)ceilf(r.c.upper);
        if (c > m) m = c;
    }
    s->margin_g = m + s->cfg.ch.resolution_g;
}

static void begin_trial(autotune_seq_t *s, uint32_t now)
{
    s->nbuf = 0;
    s->buf_overflow = false;
    s->trial_id++;
    s->rest_start = now;
    s->leak_ref_w = s->last.weight_g;
    s->state = AT_S_MEASURING_STARTUP;
}

static autotune_reason_t cancel_trigger(const autotune_seq_t *s, const autotune_inputs_t *in)
{
    if (in->boot_epoch != s->boot_epoch) return AT_R_RESTART;
    if (in->estop) return AT_R_ESTOP;
    if (in->safety_fault) return AT_R_SAFETY_FAULT;
    if (!in->ownership_ok) return AT_R_OWNERSHIP_LOST;
    if (in->manual_pump_active) return AT_R_MANUAL_PUMP;
    if (in->job_active) return AT_R_JOB_ACTIVE;
    if (!in->comm_ok) return AT_R_COMM_LOSS;
    return AT_R_NONE;
}

static autotune_reason_t weight_fault(const autotune_seq_t *s, const autotune_inputs_t *in)
{
    if (!in->weight_link_ok) return AT_R_WEIGHT_LINK_LOST;
    if (s->sender_restart) return AT_R_SENDER_RESTART;
    if (s->invalid_seen || !in->weight_fresh_valid || !s->have_w || !s->last.valid)
        return AT_R_WEIGHT_STALE;
    int32_t dt = (int32_t)(in->now_ms - s->last.t_ms);
    if (dt < 0) dt = 0;
    if ((uint32_t)dt + s->last.age_ms > s->cfg.stale_ms) return AT_R_WEIGHT_STALE;
    if (s->buf_overflow) return AT_R_BUFFER_FULL;
    return AT_R_NONE;
}

/* Per-sample plausibility: decrease, surge (step and rate). */
static autotune_reason_t sample_anomaly(autotune_seq_t *s)
{
    const int32_t w = s->last.weight_g;
    if (w > s->w_peak) s->w_peak = w;
    else if (s->w_peak - w > s->cfg.decrease_tol_g) return AT_R_WEIGHT_DECREASING;
    if (s->prev_set && (w - s->prev_w) > s->cfg.max_step_g) return AT_R_SURGE;
    s->prev_w = w;
    s->prev_set = true;
    if (!s->surge_ref_set) {
        s->surge_ref_set = true;
        s->surge_ref_t = s->last.t_ms;
        s->surge_ref_w = w;
    } else {
        uint32_t dt = s->last.t_ms - s->surge_ref_t;
        if (dt >= s->cfg.surge_win_ms) {
            float rate = (float)(w - s->surge_ref_w) * 1000.0f / (float)dt;
            if (rate > s->cfg.max_rate_g_per_s) return AT_R_SURGE;
            s->surge_ref_t = s->last.t_ms;
            s->surge_ref_w = w;
        }
    }
    return AT_R_NONE;
}

void autotune_candidate_build(const autotune_char_result_t *r, float tc_s,
                              float tc_min_s, float tc_max_s,
                              autotune_candidate_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->status = AT_CAND_NO_CANDIDATE;
    if (!r) { out->no_cand_reasons = AT_NC_CONF_NOT_OK; return; }
    out->conf_reasons = r->conf_reasons;
    if (r->confidence != AT_CONF_OK) out->no_cand_reasons |= AT_NC_CONF_NOT_OK;
    if (r->sigma.n == 0) out->no_cand_reasons |= AT_NC_NO_SIGMA;
    if (r->q_eff.n == 0 || !(r->q_eff.mean > 0.0f)) out->no_cand_reasons |= AT_NC_NO_Q;
    if (r->c.n == 0) out->no_cand_reasons |= AT_NC_NO_C;
    if (r->startup.n == 0) out->no_cand_reasons |= AT_NC_NO_STARTUP;
    if (!r->q_eff.bound_valid || !r->c.bound_valid || !r->startup.bound_valid)
        out->no_cand_reasons |= AT_NC_NO_BOUND;
    if (out->no_cand_reasons) return;

    out->status = AT_CAND_UNTESTED;
    out->c_cut_valid = true;
    out->c_cut_g = r->c.mean;
    out->c_cut_upper_g = (r->c.upper > 0.0f) ? r->c.upper : 0.0f;
    if (r->tau.n >= 1) {
        out->tau_valid = true;
        out->tau_off_ms = r->tau.bound_valid ? r->tau.upper : r->tau.mean;
    }
    if (r->q_eff.lower > 0.0f) {
        out->flow_lb_valid = true;
        out->min_expected_flow_g_per_min = r->q_eff.lower * 60.0f;
    }
    float su = (r->startup.upper > 0.0f) ? r->startup.upper : 0.0f;
    out->startup_delay_ms = (uint32_t)ceilf(su);
    float base = su + r->latency.mean;
    if (base < (float)r->edge_excl_ms) base = (float)r->edge_excl_ms;
    out->min_on_pulse_ms = (uint32_t)ceilf(base) + 1u;
    out->q_used_g_per_s = r->q_eff.upper;   /* conservative: larger Q => smaller Kp */
    out->tc_s = tc_s;
    if (out->q_used_g_per_s > 0.0f && tc_s > 0.0f && tc_s >= tc_min_s && tc_s <= tc_max_s) {
        out->kp_valid = true;
        out->kp_per_g = 1.0f / (out->q_used_g_per_s * tc_s);
    }
}

static void update_status(autotune_seq_t *s, uint32_t now, bool relay_on)
{
    autotune_seq_status_t *t = &s->st;
    bool running = s->state >= AT_S_PRECHECK && s->state <= AT_S_VALIDATING_PARAMETERS;
    t->state = s->state;
    t->reason = s->reason;
    t->relay_on = relay_on;
    t->target_reached = s->target_reached;
    t->hw_enable = s->cfg.hw_enable;
    t->trials_done = s->trials_done;
    t->trials_valid = s->trials_valid;
    t->trial_id = s->trial_id;
    t->margin_g = s->margin_g;
    t->cum_on_ms = s->cum_on_ms + (s->req_on ? (uint32_t)(now - s->on_start) : 0u);
    if (running || is_terminal(s->state)) {
        t->wall_ms = s->started ? (uint32_t)(now - s->t_start) : 0u;
        if (s->started) t->delivered_g = s->have_w ? s->last.weight_g - s->w_start : 0;
    }
}

bool autotune_seq_tick(autotune_seq_t *s, const autotune_inputs_t *in,
                       const autotune_auth_status_t *auth)
{
    if (!s) return false;
    const uint32_t now = in ? in->now_ms : s->last.t_ms;
    const bool nv = s->new_sample;
    s->new_sample = false;

    if (is_terminal(s->state) || !s->init_ok) { s->req_on = false; goto out; }
    if (s->state == AT_S_IDLE) {
        if (in && auth && auth->state == AT_AUTH_ARMED) {
            s->boot_epoch = in->boot_epoch;
            s->state = AT_S_ARMED;
        }
        goto out;
    }
    if (!in) { end_state(s, AT_S_CANCELLED, AT_R_COMM_LOSS, now); goto out; }

    autotune_reason_t r = cancel_trigger(s, in);
    if (r) { end_state(s, AT_S_CANCELLED, r, now); goto out; }
    r = weight_fault(s, in);
    if (r) {
        end_state(s, (s->state == AT_S_ARMED) ? AT_S_CANCELLED : AT_S_FAILED, r, now);
        goto out;
    }

    if (s->state == AT_S_ARMED) {
        if (auth && auth->state == AT_AUTH_ARMED) goto out;
        if (!auth || auth->state != AT_AUTH_RUNNING) {
            end_state(s, AT_S_CANCELLED,
                      (auth && auth->reason) ? auth->reason : AT_R_AUTH_LOST, now);
            goto out;
        }
        if (auth->channel != s->cfg.channel || auth->material_id != s->cfg.material_id) {
            end_state(s, AT_S_CANCELLED, AT_R_WRONG_CHANNEL, now);
            goto out;
        }
        if (!s->cfg.hw_enable) { end_state(s, AT_S_FAILED, AT_R_HW_NOT_APPROVED, now); goto out; }
        s->t_start = now;
        s->started = true;
        s->w_start = s->last.weight_g;
        s->w_peak = s->w_start;
        s->precheck_n = 0;
        s->state = AT_S_PRECHECK;
        goto out;
    }

    /* running states: authorization must still be the RUNNING handoff */
    if (!auth || auth->state != AT_AUTH_RUNNING || auth->channel != s->cfg.channel
        || auth->material_id != s->cfg.material_id) {
        end_state(s, AT_S_CANCELLED, (auth && auth->reason) ? auth->reason : AT_R_AUTH_LOST, now);
        goto out;
    }
    if (!s->cfg.hw_enable) { end_state(s, AT_S_FAILED, AT_R_HW_NOT_APPROVED, now); goto out; }

    /* independent hard limits (wall, delivered) in every running state */
    if ((uint32_t)(now - s->t_start) >= s->lim.max_wall_ms) {
        end_state(s, AT_S_FAILED, AT_R_LIMIT_WALL, now); goto out;
    }
    const int32_t delivered = s->last.weight_g - s->w_start;
    if (delivered >= s->lim.max_delivered_g) {
        end_state(s, AT_S_FAILED, AT_R_LIMIT_DELIVERED, now); goto out;
    }
    if (nv) {
        r = sample_anomaly(s);
        if (r) { end_state(s, AT_S_FAILED, r, now); goto out; }
        if (s->state == AT_S_PRECHECK) s->precheck_n++;
    }

    switch (s->state) {
    case AT_S_PRECHECK:
        if (s->precheck_n >= 3) begin_trial(s, now);
        break;

    case AT_S_MEASURING_STARTUP:
        if (s->last.weight_g - s->leak_ref_w > s->cfg.leak_tol_g) {
            end_state(s, AT_S_FAILED, AT_R_LEAK_OFF_RISE, now); break;
        }
        if ((uint32_t)(now - s->rest_start) >= s->cfg.rest_ms
            && s->nbuf >= s->cfg.ch.min_baseline_n) {
            if (delivered + s->margin_g >= s->lim.max_delivered_g) {
                end_state(s, AT_S_FAILED, AT_R_LIMIT_DELIVERED, now); break;
            }
            if (s->cum_on_ms + s->cfg.pulse_on_ms >= s->lim.max_total_on_ms) {
                end_state(s, AT_S_FAILED, AT_R_LIMIT_CUM_ON, now); break;
            }
            if (delivered + s->margin_g >= s->lim.target_mass_g) {
                s->target_reached = true;
                s->state = AT_S_ESTIMATING_PARAMETERS;
                break;
            }
            s->req_on = true;
            s->on_start = now;
            s->np_mark_elapsed = 0;
            s->np_mark_w = s->last.weight_g;
            s->state = AT_S_CALIBRATING_FLOW;
        }
        break;

    case AT_S_CALIBRATING_FLOW: {
        const uint32_t el = (uint32_t)(now - s->on_start);
        if (el >= s->lim.max_pulse_on_ms) {
            end_state(s, AT_S_FAILED, AT_R_LIMIT_PULSE_ON, now); break;
        }
        if (s->cum_on_ms + el >= s->lim.max_total_on_ms) {
            end_state(s, AT_S_FAILED, AT_R_LIMIT_CUM_ON, now); break;
        }
        if (delivered + s->margin_g >= s->lim.max_delivered_g) {
            end_state(s, AT_S_FAILED, AT_R_LIMIT_DELIVERED, now); break;
        }
        const int32_t need = (int32_t)s->cfg.noprog_n_res * s->cfg.ch.resolution_g;
        if (s->last.weight_g - s->np_mark_w >= need) {
            s->np_mark_w = s->last.weight_g;
            s->np_mark_elapsed = el;
        } else if (el - s->np_mark_elapsed >= s->cfg.noprog_window_ms) {
            end_state(s, AT_S_FAILED, AT_R_NO_PROGRESS, now); break;
        }
        if (s->cum_on_ms + el >= s->cfg.noprog_cum_on_ms && delivered < need) {
            end_state(s, AT_S_FAILED, AT_R_NO_PROGRESS, now); break;
        }
        bool tgt = (delivered + s->margin_g >= s->lim.target_mass_g);
        if (tgt || el >= s->cfg.pulse_on_ms) {
            if (tgt) s->target_reached = true;
            s->cum_on_ms += el;
            s->req_on = false;
            s->off_cmd = now;
            s->guard_set = false;
            s->state = AT_S_MEASURING_SHUTOFF;
        }
        break;
    }

    case AT_S_MEASURING_SHUTOFF: {
        const uint32_t since = (uint32_t)(now - s->off_cmd);
        if (since >= s->cfg.off_guard_ms) {
            if (!s->guard_set) { s->guard_set = true; s->guard_w = s->last.weight_g; }
            else if (s->last.weight_g - s->guard_w > s->cfg.leak_tol_g) {
                end_state(s, AT_S_FAILED, AT_R_LEAK_OFF_RISE, now); break;
            }
        }
        if (!nv || since < s->cfg.ch.min_settle_ms + s->cfg.ch.settle_win_ms) break;
        at_trial_t tr = {
            .channel = s->cfg.channel, .trial_id = s->trial_id, .s = s->buf,
            .n = s->nbuf, .on_cmd_ms = s->on_start, .off_cmd_ms = s->off_cmd,
            .transport_ms = s->cfg.transport_ms, .period_ms = 0,
        };
        at_trial_result_t res;
        at_trial_reason_t tre = autotune_char_analyze_trial(&s->cfg.ch, &tr, &res);
        if (tre == AT_TRIAL_UNSETTLED) {
            end_state(s, AT_S_FAILED, AT_R_SETTLE_TIMEOUT, now); break;
        }
        if (tre == AT_TRIAL_TAIL_TOO_SHORT) {
            if (since >= s->cfg.ch.settle_timeout_ms + s->cfg.ch.settle_win_ms)
                end_state(s, AT_S_FAILED, AT_R_SETTLE_TIMEOUT, now);
            break;
        }
        autotune_char_commit(&s->ch, &res);
        s->trials_done++;
        if (tre == AT_TRIAL_OK) s->trials_valid++;
        update_margin(s);
        if (s->target_reached || s->trials_done >= s->cfg.n_trials)
            s->state = AT_S_ESTIMATING_PARAMETERS;
        else
            begin_trial(s, now);
        break;
    }

    case AT_S_ESTIMATING_PARAMETERS:
        autotune_char_summarize(&s->ch, &s->result.ch);
        s->state = AT_S_VALIDATING_PARAMETERS;
        break;

    case AT_S_VALIDATING_PARAMETERS: {
        const autotune_char_result_t *c = &s->result.ch;
        if (c->n_valid < s->cfg.min_valid_trials) {
            end_state(s, AT_S_FAILED, AT_R_INSUFFICIENT_TRIALS, now); break;
        }
        if (c->sigma.n == 0 || c->q_eff.n == 0 || !(c->q_eff.mean > 0.0f)
            || c->c.n == 0 || c->startup.n == 0
            || c->c.mean > (float)s->cfg.assumed_post_off_g) {
            end_state(s, AT_S_FAILED, AT_R_VALIDATION, now); break;
        }
        s->result.channel = s->cfg.channel;
        s->result.material_id = s->cfg.material_id;
        s->result.limits = s->lim;
        s->result.origin = s->origin;
        s->result.verification = AT_VERIF_UNVERIFIED;
        autotune_candidate_build(c, s->cfg.tc_s, s->cfg.tc_min_s, s->cfg.tc_max_s,
                                 &s->result.cand);
        s->result_ready = true;
        s->state = AT_S_COMPLETED;
        break;
    }
    default:
        break;
    }

out:
    {
        /* single funnel: ON only in CALIBRATING_FLOW, approved, authorised */
        bool on = s->req_on && s->state == AT_S_CALIBRATING_FLOW && s->cfg.hw_enable
                  && auth && auth->state == AT_AUTH_RUNNING;
        if (!on) s->req_on = false;
        update_status(s, now, on);
        return on;
    }
}

const autotune_seq_status_t *autotune_seq_status(const autotune_seq_t *s)
{
    return s ? &s->st : NULL;
}

const autotune_result_t *autotune_seq_result(const autotune_seq_t *s)
{
    return (s && s->result_ready && s->state == AT_S_COMPLETED) ? &s->result : NULL;
}
