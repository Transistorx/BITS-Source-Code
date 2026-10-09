/* Auto-tune core tests (Phase 3). SIMULATION ONLY.
 *
 * Everything here is a SIMULATION RESULT. The plant/scale simulator and the
 * limit values below live ONLY under tests/. Every flow, delay, tau and noise
 * number is an "illustrative simulation fixture": not a hardware setting, not
 * a material property and not a real-world accuracy claim. The fixture limits
 * (100 g target, 3 s pulse, 20 s cumulative, 120 s wall, 150 g absolute) are
 * test fixtures only; production code contains no default limit values.
 * Nothing here energizes anything: the relay request is just a bool that the
 * simulated plant reads.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "autotune_auth.h"
#include "autotune_char.h"
#include "autotune_seq.h"
#include "flow_estimator.h"
#include "test_autotune.h"

extern void audit_check(bool condition, const char *name);
static const char *TAG = "TEST";

#define T_BASE 100000u
#define EPOCH  7u

static void ck(bool c, const char *name) { audit_check(c, name); }

static void ckf(bool c, const char *fmt, int a)
{
    char buf[96];
    snprintf(buf, sizeof(buf), fmt, a);
    audit_check(c, buf);
}

/* ===================== TEST-ONLY PLANT / SCALE SIMULATOR ===================== */

#define PQ 32
#define PHIST 1024u

typedef struct {
    uint32_t rng;
    float q;                      /* valve-open flow, g/s (illustrative) */
    uint32_t delay_ms, ramp_ms;   /* startup dead time, opening time constant */
    float tau_off_ms;             /* exponential shutoff tail */
    int noise;                    /* +-g uniform integer */
    uint32_t lat_ms;              /* measurement latency == sender age_ms */
    uint32_t transport_ms;        /* delivery delay (MQTT/WS) */
    uint32_t pub_ms;              /* sample period */
    int drop_pct, dup_pct, reorder_pct;
    uint32_t stall_from, stall_to, restart_at;
    float leak_g_s; uint32_t leak_from;
    int32_t surge_g; uint32_t surge_at;
    bool blocked;
    bool cmd;
    uint32_t now, on_edge, seq;
    float rate, w;
    float hist[PHIST];
    struct { bool used; uint32_t due; int32_t w; uint32_t age, seq; } qu[PQ];
} plant_t;

static plant_t g_plant[2];

static uint32_t lcg(plant_t *p)
{
    p->rng = p->rng * 1664525u + 1013904223u;
    return p->rng >> 8;
}

static void plant_init(plant_t *p, uint32_t seed, float q, uint32_t delay_ms, float tau_off_ms)
{
    memset(p, 0, sizeof(*p));
    p->rng = seed;
    p->q = q;
    p->delay_ms = delay_ms;
    p->tau_off_ms = tau_off_ms;
    p->noise = 1;
    p->lat_ms = 100;
    p->pub_ms = 100;
    p->w = 250.0f;
    for (uint32_t i = 0; i < PHIST; i++) p->hist[i] = p->w;   /* history before t=0 is the rest weight */
}

static void plant_push(plant_t *p, uint32_t due, int32_t w, uint32_t age, uint32_t seq)
{
    for (int i = 0; i < PQ; i++) {
        if (!p->qu[i].used) {
            p->qu[i].used = true; p->qu[i].due = due; p->qu[i].w = w;
            p->qu[i].age = age; p->qu[i].seq = seq;
            return;
        }
    }
}

static void plant_publish(plant_t *p)
{
    if (p->now >= p->stall_from && p->now < p->stall_to) return;
    p->seq++;
    if (p->restart_at && p->now == p->restart_at) p->seq = 1;
    uint32_t idx = (p->now >= p->lat_ms) ? p->now - p->lat_ms : 0;
    int n = 0;
    if (p->noise > 0) n = (int)(lcg(p) % (uint32_t)(2 * p->noise + 1)) - p->noise;
    int32_t wm = (int32_t)floorf(p->hist[idx % PHIST] + 0.5f) + n;
    if (p->drop_pct > 0 && (int)(lcg(p) % 100u) < p->drop_pct) return;
    uint32_t due = p->now + p->transport_ms;
    if (p->reorder_pct > 0 && (int)(lcg(p) % 100u) < p->reorder_pct) due += p->pub_ms + 30u;
    plant_push(p, due, wm, p->lat_ms, p->seq);
    if (p->dup_pct > 0 && (int)(lcg(p) % 100u) < p->dup_pct)
        plant_push(p, due + 20u, wm, p->lat_ms, p->seq);
}

/* One 1 ms physics step with relay command `cmd`. */
static void plant_step(plant_t *p, bool cmd)
{
    p->now++;
    if (cmd && !p->cmd) p->on_edge = p->now;
    p->cmd = cmd;
    float target = (cmd && !p->blocked && (p->now - p->on_edge) >= p->delay_ms) ? p->q : 0.0f;
    float tau = cmd ? (float)p->ramp_ms : p->tau_off_ms;
    if (tau <= 0.0f) p->rate = target;
    else p->rate += (target - p->rate) / tau;
    p->w += p->rate * 0.001f;
    if (!cmd && p->leak_g_s > 0.0f && p->now >= p->leak_from) p->w += p->leak_g_s * 0.001f;
    if (p->surge_at && p->now == p->surge_at) p->w += (float)p->surge_g;
    p->hist[p->now % PHIST] = p->w;
    if (p->now % p->pub_ms == 0) plant_publish(p);
}

static bool plant_pop(plant_t *p, int32_t *w, uint32_t *age, uint32_t *seq)
{
    int best = -1;
    for (int i = 0; i < PQ; i++) {
        if (p->qu[i].used && p->qu[i].due <= p->now &&
            (best < 0 || p->qu[i].due < p->qu[best].due)) best = i;
    }
    if (best < 0) return false;
    *w = p->qu[best].w; *age = p->qu[best].age; *seq = p->qu[best].seq;
    p->qu[best].used = false;
    return true;
}

/* ===================== fixtures (TEST-ONLY) ===================== */

static autotune_limits_t fixture_limits(void)
{
    autotune_limits_t l = {
        .target_mass_g = 100, .max_pulse_on_ms = 3000, .max_total_on_ms = 20000,
        .max_wall_ms = 120000, .max_delivered_g = 150,
    };
    return l;
}

static void fixture_cfg(autotune_seq_cfg_t *c, uint8_t ch, bool hw)
{
    autotune_seq_cfg_default(c);
    c->hw_enable = hw;
    c->channel = ch;
    c->material_id = ch;
    autotune_char_cfg_default(&c->ch);
    c->ch.n_ok = 4;
    c->pulse_on_ms = 2000;
    c->n_trials = 4;
    c->min_valid_trials = 3;
    c->rest_ms = 1500;
    c->off_guard_ms = 6000;
    c->stale_ms = 1000;
    c->transport_ms = 0;
    c->noprog_window_ms = 1800;
    c->noprog_n_res = 3;
    c->noprog_cum_on_ms = 2500;
    c->decrease_tol_g = 5;
    c->leak_tol_g = 4;
    c->max_step_g = 40;
    c->max_rate_g_per_s = 15.0f;
    c->surge_win_ms = 500;
    c->assumed_post_off_g = 12;
    c->tc_s = 4.0f; c->tc_min_s = 1.0f; c->tc_max_s = 30.0f;
}

/* ===================== rig: plant + auth + sequencer ===================== */

static uint8_t legal_next[11][11];   /* [from][to] = 1 if legal */
static uint32_t g_tr_seen[11][11];
static uint32_t g_illegal_seen;

static void legal_init(void)
{
#define L(a, b) legal_next[a][b] = 1
    memset(legal_next, 0, sizeof(legal_next));
    L(AT_S_IDLE, AT_S_ARMED);
    L(AT_S_ARMED, AT_S_PRECHECK); L(AT_S_ARMED, AT_S_FAILED); L(AT_S_ARMED, AT_S_CANCELLED);
    L(AT_S_PRECHECK, AT_S_MEASURING_STARTUP); L(AT_S_PRECHECK, AT_S_FAILED); L(AT_S_PRECHECK, AT_S_CANCELLED);
    L(AT_S_MEASURING_STARTUP, AT_S_CALIBRATING_FLOW);
    L(AT_S_MEASURING_STARTUP, AT_S_ESTIMATING_PARAMETERS);
    L(AT_S_MEASURING_STARTUP, AT_S_FAILED); L(AT_S_MEASURING_STARTUP, AT_S_CANCELLED);
    L(AT_S_CALIBRATING_FLOW, AT_S_MEASURING_SHUTOFF);
    L(AT_S_CALIBRATING_FLOW, AT_S_FAILED); L(AT_S_CALIBRATING_FLOW, AT_S_CANCELLED);
    L(AT_S_MEASURING_SHUTOFF, AT_S_MEASURING_STARTUP);
    L(AT_S_MEASURING_SHUTOFF, AT_S_ESTIMATING_PARAMETERS);
    L(AT_S_MEASURING_SHUTOFF, AT_S_FAILED); L(AT_S_MEASURING_SHUTOFF, AT_S_CANCELLED);
    L(AT_S_ESTIMATING_PARAMETERS, AT_S_VALIDATING_PARAMETERS);
    L(AT_S_ESTIMATING_PARAMETERS, AT_S_FAILED); L(AT_S_ESTIMATING_PARAMETERS, AT_S_CANCELLED);
    L(AT_S_VALIDATING_PARAMETERS, AT_S_COMPLETED);
    L(AT_S_VALIDATING_PARAMETERS, AT_S_FAILED); L(AT_S_VALIDATING_PARAMETERS, AT_S_CANCELLED);
#undef L
}

typedef struct {
    plant_t *pl;
    autotune_auth_t auth;
    autotune_seq_t seq;
    autotune_inputs_t in;
    autotune_seq_cfg_t cfg;
    autotune_limits_t lim;
    bool hw, cmd;
    uint32_t next_id;
    autotune_state_t prev;
    uint32_t first_on, last_on, last_off, on_ticks, viol;
    bool estimator_garbage;
    flow_estimator_t fe;
} rig_t;

static rig_t g_rig;
static uint32_t g_junk = 12345u;   /* separate RNG so the garbage estimator cannot disturb the plant */

static void rig_init(plant_t *pl, const autotune_seq_cfg_t *cfg, const autotune_limits_t *lim)
{
    rig_t *r = &g_rig;
    memset(r, 0, sizeof(*r));
    r->pl = pl;
    r->cfg = *cfg;
    r->lim = *lim;
    r->hw = cfg->hw_enable;
    autotune_auth_cfg_t ac = { .max_cmd_ttl_ms = 5000, .arm_window_ms = 10000, .hw_enable = cfg->hw_enable };
    autotune_auth_init(&r->auth, &ac);
    autotune_seq_init(&r->seq, cfg, lim);
    autotune_seq_set_origin(&r->seq, AT_ORIGIN_SIMULATED);
    r->in.boot_epoch = EPOCH;
    r->in.weight_fresh_valid = r->in.weight_link_ok = r->in.comm_ok = true;
    r->in.ownership_ok = true;
    r->next_id = 1;
    r->prev = AT_S_IDLE;
    legal_init();
}

static void rig_tick(void)
{
    rig_t *r = &g_rig;
    plant_t *p = r->pl;
    int32_t w; uint32_t age, seq;
    while (plant_pop(p, &w, &age, &seq)) {
        autotune_wsample_t s = { .weight_g = w, .t_ms = T_BASE + p->now, .seq = seq,
                                 .age_ms = age, .valid = true, .stable = true };
        autotune_seq_feed(&r->seq, &s);
        if (r->estimator_garbage) {
            /* deliberately wrong estimator: garbage weights, wrong relay edges */
            g_junk = g_junk * 1664525u + 1013904223u;
            flow_sample_t fs = { .weight_g = (int32_t)((g_junk >> 8) % 100000u), .t_ms = s.t_ms,
                                 .seq = seq, .age_ms = age, .stable = true, .valid = true };
            flow_estimator_note_relay(&r->fe, (p->now / 70u) & 1u, s.t_ms, 1.0f);
            flow_estimator_feed(&r->fe, &fs);
        }
    }
    r->in.now_ms = T_BASE + p->now;
    autotune_auth_tick(&r->auth, &r->in);
    bool on = autotune_seq_tick(&r->seq, &r->in, &r->auth.st);
    autotune_state_t st = r->seq.state;
    if (st != r->prev) {
        g_tr_seen[r->prev][st]++;
        if (!legal_next[r->prev][st]) g_illegal_seen++;
        r->prev = st;
    }
    if (on && !r->cmd) { if (!r->first_on) r->first_on = p->now; r->last_on = p->now; }
    if (!on && r->cmd) r->last_off = p->now;
    if (on) {
        r->on_ticks++;
        if (!r->hw || r->auth.st.state != AT_AUTH_RUNNING || st != AT_S_CALIBRATING_FLOW) r->viol++;
    }
    r->cmd = on;
}

static void rig_step(void)
{
    rig_t *r = &g_rig;
    if (r->pl->now % 10u == 0) rig_tick();
    plant_step(r->pl, r->cmd);
}

static bool terminal(autotune_state_t s)
{
    return s == AT_S_COMPLETED || s == AT_S_FAILED || s == AT_S_CANCELLED;
}

/* Run until sim time `until_ms` (absolute) or terminal. */
static void rig_run(uint32_t until_ms, bool stop_on_terminal)
{
    while (g_rig.pl->now < until_ms) {
        rig_step();
        if (stop_on_terminal && terminal(g_rig.seq.state)) break;
    }
}

static void rig_run_to_state(autotune_state_t s, uint32_t limit_ms)
{
    while (g_rig.pl->now < limit_ms && g_rig.seq.state != s && !terminal(g_rig.seq.state))
        rig_step();
}

static autotune_cmd_t mkcmd(rig_t *r, uint32_t ttl)
{
    autotune_cmd_t c = { .auth_ok = true, .retained = false, .cmd_id = r->next_id++,
                         .ttl_ms = ttl, .age_ms = 10, .rx_ms = T_BASE + r->pl->now,
                         .boot_epoch = EPOCH };
    return c;
}

static autotune_arm_req_t mkarm(rig_t *r)
{
    autotune_arm_req_t a;
    memset(&a, 0, sizeof(a));
    a.cmd = mkcmd(r, 2000);
    a.channel = r->cfg.channel;
    a.material_id = r->cfg.material_id;
    a.material_selected = a.scale_under_nozzle = a.container_present = true;
    a.ready = (autotune_ready_t){ true, true, true, true, true, true, true };
    return a;
}

static autotune_start_req_t mkstart(rig_t *r)
{
    autotune_start_req_t s;
    memset(&s, 0, sizeof(s));
    s.cmd = mkcmd(r, 2000);
    s.channel = r->cfg.channel;
    s.material_id = r->cfg.material_id;
    s.token = r->auth.st.token;
    s.ready = (autotune_ready_t){ true, true, true, true, true, true, true };
    return s;
}

/* Warm up the plant so a fresh sample exists, then ARM, let the sequencer see
 * it, START. Returns the START result. */
static autotune_reason_t rig_arm_start(void)
{
    rig_t *r = &g_rig;
    rig_run(r->pl->now + 300 + r->pl->transport_ms, false);
    autotune_arm_req_t a = mkarm(r);
    autotune_auth_arm(&r->auth, &a);
    rig_run(r->pl->now + 30, false);
    autotune_start_req_t s = mkstart(r);
    return autotune_auth_start(&r->auth, &s);
}

/* Standard run: plant p, fixture cfg/limits (optionally tweaked by caller
 * before via the pointers), hw approved, runs until terminal. */
static void std_setup(plant_t *p, uint8_t ch, bool hw)
{
    autotune_seq_cfg_t c;
    fixture_cfg(&c, ch, hw);
    autotune_limits_t l = fixture_limits();
    rig_init(p, &c, &l);
}

static void happy_plant(plant_t *p, uint32_t seed)
{
    plant_init(p, seed, 6.0f, 300, 1200.0f);   /* illustrative simulation fixture */
}

/* time of the first relay OFF of a clean run, to place faults deterministically */
static uint32_t probe_first_off(uint32_t seed, uint32_t *first_on)
{
    static plant_t pp;
    happy_plant(&pp, seed);
    std_setup(&pp, 1, true);
    rig_arm_start();
    while (pp.now < 60000 && g_rig.last_off == 0 && !terminal(g_rig.seq.state)) rig_step();
    if (first_on) *first_on = g_rig.first_on;
    return g_rig.last_off;
}

/* ===================== trial builder for characterization ===================== */

#define TBUF 320
static at_sample_t g_tbuf[TBUF];
static uint16_t g_tn;

static void collect(plant_t *p)
{
    int32_t w; uint32_t age, seq;
    while (plant_pop(p, &w, &age, &seq)) {
        if (g_tn < TBUF) {
            g_tbuf[g_tn].t_ms = T_BASE + p->now;
            g_tbuf[g_tn].w_g = w;
            g_tbuf[g_tn].age_ms = age;
            g_tn++;
        }
    }
}

static at_trial_t build_trial(plant_t *p, uint8_t ch, uint32_t id, uint32_t rest_ms,
                              uint32_t on_ms, uint32_t tail_ms, uint32_t assumed_transport)
{
    g_tn = 0;
    for (uint32_t i = 0; i < rest_ms; i++) { plant_step(p, false); collect(p); }
    at_trial_t t;
    memset(&t, 0, sizeof(t));
    t.channel = ch; t.trial_id = id; t.s = g_tbuf;
    t.on_cmd_ms = T_BASE + p->now;
    for (uint32_t i = 0; i < on_ms; i++) { plant_step(p, true); collect(p); }
    t.off_cmd_ms = T_BASE + p->now;
    for (uint32_t i = 0; i < tail_ms; i++) { plant_step(p, false); collect(p); }
    t.n = g_tn;
    t.transport_ms = assumed_transport;
    return t;
}

static int pm(float est, float truth) { return (int)(1000.0f * (est - truth) / truth); }

static void info_sim(const char *name, const at_trial_result_t *r, float q, float d, float tau)
{
    ESP_LOGI(TAG, "TEST_INFO char[SIM] %s: reason=%d Q=%d (true %d) se=%d mg/s",
             name, (int)r->reason, (int)(r->q_eff * 1000), (int)(q * 1000), (int)(r->q_se * 1000));
    ESP_LOGI(TAG, "TEST_INFO char[SIM] %s: C=%d (true %d) mg startup=%d int=%d (true %d) ms",
             name, (int)(r->c_g * 1000), (int)(q * tau), (int)r->startup_ms,
             (int)r->startup_intercept_ms, (int)d);
    ESP_LOGI(TAG, "TEST_INFO char[SIM] %s: tau=%d (true %d) ms sigma=%d mg lat=%d ms",
             name, (int)r->tau_off_ms, (int)tau, (int)(r->sigma_g * 1000), (int)r->latency_ms);
}

/* ===================== test groups ===================== */

static void t_limits_fail_closed(void)
{
    static autotune_seq_t s;
    autotune_seq_cfg_t c;
    autotune_limits_t l = fixture_limits();
    fixture_cfg(&c, 1, true);
    ck(autotune_limits_validate(&l) == AT_LIM_OK, "at_limits_fixture_valid_in_tests_only");
    ck(autotune_seq_init(&s, &c, &l), "at_limits_full_cfg_inits");

    ck(!autotune_seq_init(&s, &c, NULL), "at_limits_null_fails_closed");
    ck(!autotune_seq_init(&s, NULL, &l), "at_cfg_null_fails_closed");
    autotune_seq_cfg_t d;
    autotune_seq_cfg_default(&d);
    ck(!d.hw_enable, "at_cfg_default_hw_enable_false");
    ck(d.pulse_on_ms == 0 && d.stale_ms == 0 && d.assumed_post_off_g == 0,
       "at_cfg_default_has_no_limit_values");
    ck(!autotune_seq_init(&s, &d, &l), "at_cfg_default_unfilled_fails_closed");
    autotune_limits_t z;
    memset(&z, 0, sizeof(z));
    ck(!autotune_seq_init(&s, &c, &z), "at_limits_all_zero_fails_closed");
    ck(autotune_limits_validate(&z) != AT_LIM_OK, "at_limits_validate_all_zero");

    for (int i = 0; i < 5; i++) {
        autotune_limits_t m = l;
        switch (i) {
        case 0: m.target_mass_g = 0; break;
        case 1: m.max_pulse_on_ms = 0; break;
        case 2: m.max_total_on_ms = 0; break;
        case 3: m.max_wall_ms = 0; break;
        default: m.max_delivered_g = 0; break;
        }
        ckf(!autotune_seq_init(&s, &c, &m), "at_limit_zero_member_%d_fails_closed", i);
    }
    autotune_limits_t o = l;
    o.max_delivered_g = o.target_mass_g;
    ck(!autotune_seq_init(&s, &c, &o), "at_limits_delivered_not_above_target_fails");
    o = l; o.max_wall_ms = o.max_total_on_ms;
    ck(!autotune_seq_init(&s, &c, &o), "at_limits_wall_not_above_total_fails");

    for (int i = 0; i < 12; i++) {
        autotune_seq_cfg_t m = c;
        switch (i) {
        case 0: m.channel = 0; break;
        case 1: m.material_id = 2; break;           /* mapping: M2 on CH1 */
        case 2: m.pulse_on_ms = 0; break;
        case 3: m.n_trials = 0; break;
        case 4: m.rest_ms = 0; break;
        case 5: m.stale_ms = 0; break;
        case 6: m.noprog_window_ms = 0; break;
        case 7: m.max_rate_g_per_s = 0.0f; break;
        case 8: m.assumed_post_off_g = 0; break;
        case 9: m.tc_s = 0.0f; break;
        case 10: m.pulse_on_ms = l.max_pulse_on_ms; break;   /* plan not below limit */
        default: m.n_trials = 12; break;                      /* 12*2s >= 20s total */
        }
        ckf(!autotune_seq_init(&s, &m, &l), "at_cfg_missing_or_bad_member_%d_fails_closed", i);
    }

    /* a failed init never leaves IDLE, never requests ON even with RUNNING auth */
    autotune_seq_init(&s, &d, &l);
    autotune_auth_status_t as;
    memset(&as, 0, sizeof(as));
    as.state = AT_AUTH_RUNNING; as.channel = 1; as.material_id = 1;
    autotune_inputs_t in = { .now_ms = 1000, .boot_epoch = EPOCH, .weight_fresh_valid = true,
                             .weight_link_ok = true, .comm_ok = true, .ownership_ok = true };
    bool any_on = false;
    for (int i = 0; i < 50; i++) { in.now_ms += 10; any_on |= autotune_seq_tick(&s, &in, &as); }
    ck(!any_on && s.state == AT_S_IDLE, "at_failed_init_stays_idle_relay_off");
}

static void t_hw_enable(void)
{
    happy_plant(&g_plant[0], 11);
    std_setup(&g_plant[0], 1, false);
    autotune_reason_t r = rig_arm_start();
    ck(r == AT_R_HW_NOT_APPROVED, "at_hw_false_start_refused_HW_NOT_APPROVED");
    ck(g_rig.auth.st.state == AT_AUTH_ARMED && g_rig.auth.st.refusal == AT_R_HW_NOT_APPROVED,
       "at_hw_false_status_reports_gate");
    rig_run(g_rig.pl->now + 20000, false);
    ck(g_rig.on_ticks == 0 && g_rig.viol == 0 && !g_rig.seq.st.relay_on,
       "at_hw_false_relay_request_always_off_20s");

    /* even if an auth RUNNING status were forged, the sequencer itself refuses */
    static autotune_seq_t s;
    autotune_seq_cfg_t c; autotune_limits_t l = fixture_limits();
    fixture_cfg(&c, 1, false);
    autotune_seq_init(&s, &c, &l);
    autotune_auth_status_t as = { .state = AT_AUTH_ARMED, .channel = 1, .material_id = 1 };
    autotune_inputs_t in = { .now_ms = 1000, .boot_epoch = EPOCH, .weight_fresh_valid = true,
                             .weight_link_ok = true, .comm_ok = true, .ownership_ok = true };
    autotune_wsample_t w = { .weight_g = 250, .t_ms = 1000, .seq = 1, .valid = true };
    autotune_seq_feed(&s, &w);
    autotune_seq_tick(&s, &in, &as);
    as.state = AT_AUTH_RUNNING;
    bool on = false;
    for (int i = 0; i < 20; i++) { in.now_ms += 10; w.t_ms = in.now_ms; w.seq++; autotune_seq_feed(&s, &w); on |= autotune_seq_tick(&s, &in, &as); }
    ck(!on && s.state == AT_S_FAILED && s.reason == AT_R_HW_NOT_APPROVED,
       "at_hw_false_forged_running_sequencer_FAILED_HW_NOT_APPROVED");
}

static void auth_fresh(autotune_auth_t *a, bool hw)
{
    autotune_auth_cfg_t ac = { .max_cmd_ttl_ms = 5000, .arm_window_ms = 10000, .hw_enable = hw };
    autotune_auth_init(a, &ac);
}

static autotune_arm_req_t base_arm(uint32_t id, uint8_t ch)
{
    autotune_arm_req_t a;
    memset(&a, 0, sizeof(a));
    a.cmd = (autotune_cmd_t){ .auth_ok = true, .cmd_id = id, .ttl_ms = 2000, .age_ms = 5,
                              .rx_ms = 1000, .boot_epoch = EPOCH };
    a.channel = ch; a.material_id = ch;
    a.material_selected = a.scale_under_nozzle = a.container_present = true;
    a.ready = (autotune_ready_t){ true, true, true, true, true, true, true };
    return a;
}

static autotune_start_req_t base_start(uint32_t id, const autotune_auth_t *a)
{
    autotune_start_req_t s;
    memset(&s, 0, sizeof(s));
    s.cmd = (autotune_cmd_t){ .auth_ok = true, .cmd_id = id, .ttl_ms = 2000, .age_ms = 5,
                              .rx_ms = 1500, .boot_epoch = EPOCH };
    s.channel = a->st.channel; s.material_id = a->st.material_id; s.token = a->st.token;
    s.ready = (autotune_ready_t){ true, true, true, true, true, true, true };
    return s;
}

static void t_auth(void)
{
    static autotune_auth_t a;
    autotune_arm_req_t r;

    auth_fresh(&a, true);
    ck(!autotune_auth_init(&a, NULL), "at_auth_init_null_cfg_fails_closed");
    autotune_auth_cfg_t bad = { .max_cmd_ttl_ms = 0, .arm_window_ms = 100, .hw_enable = true };
    ck(!autotune_auth_init(&a, &bad), "at_auth_init_zero_ttl_fails_closed");
    r = base_arm(1, 1);
    ck(autotune_auth_arm(&a, &r) != AT_R_NONE && a.st.state == AT_AUTH_IDLE,
       "at_auth_failed_init_refuses_everything");

    auth_fresh(&a, true);
    r = base_arm(1, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_NONE && a.st.state == AT_AUTH_ARMED && a.st.token != 0,
       "at_auth_arm_all_confirmations_ok");

    /* each missing confirmation refused, one at a time */
    for (int i = 0; i < 12; i++) {
        auth_fresh(&a, true);
        r = base_arm(10 + (uint32_t)i, 1);
        autotune_reason_t want = AT_R_NONE;
        switch (i) {
        case 0: r.cmd.auth_ok = false; want = AT_R_AUTH_NOT_OK; break;
        case 1: r.material_selected = false; want = AT_R_NO_MATERIAL_SELECTED; break;
        case 2: r.scale_under_nozzle = false; want = AT_R_NO_SCALE_UNDER_NOZZLE; break;
        case 3: r.container_present = false; want = AT_R_NO_CONTAINER; break;
        case 4: r.ready.no_active_job = false; want = AT_R_JOB_ACTIVE; break;
        case 5: r.ready.ownership_ok = false; want = AT_R_OWNERSHIP; break;
        case 6: r.ready.no_safety_fault = false; want = AT_R_SAFETY_FAULT; break;
        case 7: r.ready.no_estop = false; want = AT_R_ESTOP; break;
        case 8: r.ready.weight_fresh_valid = false; want = AT_R_WEIGHT_NOT_FRESH; break;
        case 9: r.ready.relays_all_off = false; want = AT_R_RELAYS_NOT_OFF; break;
        case 10: r.ready.no_manual_pump = false; want = AT_R_MANUAL_PUMP; break;
        default: r.channel = 3; want = AT_R_CHANNEL_INVALID; break;
        }
        autotune_reason_t g = autotune_auth_arm(&a, &r);
        ckf(g == want && a.st.state == AT_AUTH_IDLE && a.st.refusal == want,
            "at_auth_arm_refused_without_confirmation_%d", i);
    }

    /* fixed mapping */
    auth_fresh(&a, true);
    r = base_arm(1, 1); r.material_id = 2;
    ck(autotune_auth_arm(&a, &r) == AT_R_MATERIAL_MISMATCH, "at_auth_arm_CH1_with_M2_refused");
    r = base_arm(2, 2); r.material_id = 1;
    ck(autotune_auth_arm(&a, &r) == AT_R_MATERIAL_MISMATCH, "at_auth_arm_CH2_with_M1_refused");
    r = base_arm(3, 2);
    ck(autotune_auth_arm(&a, &r) == AT_R_NONE, "at_auth_arm_CH2_M2_accepted");

    /* envelope: retained, ttl, expiry, id 0 */
    auth_fresh(&a, true);
    r = base_arm(1, 1); r.cmd.retained = true;
    ck(autotune_auth_arm(&a, &r) == AT_R_RETAINED, "at_auth_retained_arm_rejected");
    r = base_arm(2, 1); r.cmd.ttl_ms = 0;
    ck(autotune_auth_arm(&a, &r) == AT_R_TTL_INVALID, "at_auth_ttl_zero_rejected");
    r = base_arm(3, 1); r.cmd.ttl_ms = 5001;
    ck(autotune_auth_arm(&a, &r) == AT_R_TTL_INVALID, "at_auth_ttl_above_bound_rejected");
    r = base_arm(4, 1); r.cmd.age_ms = r.cmd.ttl_ms;
    ck(autotune_auth_arm(&a, &r) == AT_R_CMD_EXPIRED, "at_auth_expired_arm_rejected");
    r = base_arm(0, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_INVALID, "at_auth_id_zero_rejected");

    /* START gates */
    auth_fresh(&a, true);
    autotune_start_req_t s = base_start(5, &a);
    ck(autotune_auth_start(&a, &s) == AT_R_NO_ARM, "at_auth_start_without_arm_refused");
    r = base_arm(6, 1);
    autotune_auth_arm(&a, &r);
    s = base_start(7, &a); s.cmd.retained = true;
    ck(autotune_auth_start(&a, &s) == AT_R_RETAINED && a.st.state == AT_AUTH_ARMED,
       "at_auth_retained_start_rejected");
    s = base_start(8, &a); s.cmd.age_ms = s.cmd.ttl_ms + 1;
    ck(autotune_auth_start(&a, &s) == AT_R_CMD_EXPIRED, "at_auth_expired_start_rejected");
    s = base_start(9, &a); s.token ^= 1u;
    ck(autotune_auth_start(&a, &s) == AT_R_TOKEN_MISMATCH, "at_auth_start_wrong_token_refused");
    s = base_start(10, &a); s.channel = 2;
    ck(autotune_auth_start(&a, &s) == AT_R_WRONG_CHANNEL, "at_auth_start_wrong_channel_refused");
    s = base_start(11, &a); s.material_id = 2;
    ck(autotune_auth_start(&a, &s) == AT_R_WRONG_MATERIAL, "at_auth_start_wrong_material_refused");
    s = base_start(12, &a); s.ready.relays_all_off = false;
    ck(autotune_auth_start(&a, &s) == AT_R_RELAYS_NOT_OFF, "at_auth_start_rechecks_ready");
    s = base_start(13, &a); s.cmd.boot_epoch = EPOCH + 1;
    ck(autotune_auth_start(&a, &s) == AT_R_RESTART_EPOCH && a.st.state == AT_AUTH_CANCELLED,
       "at_auth_start_epoch_mismatch_cancels");

    /* expired ARM */
    auth_fresh(&a, true);
    r = base_arm(1, 1);
    autotune_auth_arm(&a, &r);
    s = base_start(2, &a); s.cmd.rx_ms = r.cmd.rx_ms + 10000;
    ck(autotune_auth_start(&a, &s) == AT_R_ARM_EXPIRED && a.st.state == AT_AUTH_CANCELLED,
       "at_auth_start_with_expired_arm_refused");
    r = base_arm(3, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_NONE, "at_auth_rearm_after_expiry_ok");
    autotune_inputs_t in = { .now_ms = r.cmd.rx_ms + 10000, .boot_epoch = EPOCH,
                             .weight_fresh_valid = true, .weight_link_ok = true,
                             .comm_ok = true, .ownership_ok = true };
    autotune_auth_tick(&a, &in);
    ck(a.st.state == AT_AUTH_CANCELLED && a.st.reason == AT_R_ARM_EXPIRED,
       "at_auth_arm_auto_expires_on_tick");

    /* success, duplicate and replay */
    auth_fresh(&a, true);
    r = base_arm(1, 1);
    autotune_auth_arm(&a, &r);
    s = base_start(2, &a);
    ck(autotune_auth_start(&a, &s) == AT_R_NONE && a.st.state == AT_AUTH_RUNNING,
       "at_auth_start_with_valid_arm_runs");
    ck(autotune_auth_start(&a, &s) == AT_R_ID_DUPLICATE, "at_auth_duplicate_start_id_rejected");
    r = base_arm(1, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_DUPLICATE, "at_auth_replayed_arm_id_rejected");
    r = base_arm(2, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_DUPLICATE, "at_auth_start_id_cannot_be_reused_as_arm");

    /* a refused command's id is consumed too */
    auth_fresh(&a, true);
    r = base_arm(1, 1); r.container_present = false;
    autotune_auth_arm(&a, &r);
    r = base_arm(1, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_DUPLICATE, "at_auth_refused_command_id_is_consumed");

    /* seen-ring wrap: ids below the floor are rejected */
    auth_fresh(&a, true);
    for (uint32_t id = 1; id <= 20; id++) {
        r = base_arm(id, 1); r.container_present = false;
        autotune_auth_arm(&a, &r);
    }
    r = base_arm(2, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_BELOW_FLOOR, "at_auth_id_below_floor_rejected");
    r = base_arm(4, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_BELOW_FLOOR, "at_auth_id_equal_floor_rejected");
    r = base_arm(5, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_ID_DUPLICATE, "at_auth_id_still_in_ring_is_duplicate");
    r = base_arm(21, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_NONE, "at_auth_new_id_after_wrap_accepted");

    /* CANCEL: always accepted, idempotent, no auth needed */
    auth_fresh(&a, true);
    ck(autotune_auth_cancel(&a) && a.st.state == AT_AUTH_IDLE, "at_auth_cancel_idle_accepted_noop");
    r = base_arm(1, 1);
    autotune_auth_arm(&a, &r);
    ck(autotune_auth_cancel(&a) && a.st.state == AT_AUTH_CANCELLED && a.st.reason == AT_R_USER_CANCEL,
       "at_auth_cancel_armed");
    ck(autotune_auth_cancel(&a) && a.st.state == AT_AUTH_CANCELLED, "at_auth_cancel_idempotent");
    r = base_arm(2, 1);
    autotune_auth_arm(&a, &r);
    s = base_start(3, &a);
    autotune_auth_start(&a, &s);
    ck(autotune_auth_cancel(&a) && a.st.state == AT_AUTH_CANCELLED, "at_auth_cancel_running");
    ck(autotune_auth_cancel(NULL), "at_auth_cancel_null_still_true");
    r = base_arm(4, 1);
    ck(autotune_auth_arm(&a, &r) == AT_R_NONE, "at_auth_arm_after_cancel_ok");
    autotune_start_req_t old = s; old.cmd.cmd_id = 5;
    ck(autotune_auth_start(&a, &old) == AT_R_TOKEN_MISMATCH, "at_auth_old_token_invalid_after_rearm");
}

static void t_auth_cancel_triggers(void)
{
    static autotune_auth_t a;
    for (int running = 0; running < 2; running++) {
        for (int t = 0; t < 9; t++) {
            auth_fresh(&a, true);
            autotune_arm_req_t r = base_arm(1, 1);
            autotune_auth_arm(&a, &r);
            if (running) {
                autotune_start_req_t s = base_start(2, &a);
                autotune_auth_start(&a, &s);
            }
            autotune_inputs_t in = { .now_ms = 1100, .boot_epoch = EPOCH, .weight_fresh_valid = true,
                                     .weight_link_ok = true, .comm_ok = true, .ownership_ok = true };
            autotune_auth_tick(&a, &in);
            bool still = a.st.state == (running ? AT_AUTH_RUNNING : AT_AUTH_ARMED);
            autotune_reason_t want = AT_R_NONE;
            switch (t) {
            case 0: in.weight_fresh_valid = false; want = AT_R_WEIGHT_STALE; break;
            case 1: in.weight_link_ok = false; want = AT_R_WEIGHT_LINK_LOST; break;
            case 2: in.boot_epoch = EPOCH + 1; want = AT_R_RESTART; break;
            case 3: in.safety_fault = true; want = AT_R_SAFETY_FAULT; break;
            case 4: in.estop = true; want = AT_R_ESTOP; break;
            case 5: in.ownership_ok = false; want = AT_R_OWNERSHIP_LOST; break;
            case 6: in.manual_pump_active = true; want = AT_R_MANUAL_PUMP; break;
            case 7: in.comm_ok = false; want = AT_R_COMM_LOSS; break;
            default: in.job_active = true; want = AT_R_JOB_ACTIVE; break;
            }
            autotune_auth_tick(&a, &in);
            ckf(still && a.st.state == AT_AUTH_CANCELLED && a.st.reason == want && a.st.token == 0,
                running ? "at_auth_running_cancelled_by_trigger_%d" : "at_auth_armed_cancelled_by_trigger_%d", t);
        }
    }
}

/* ---- sequencer cancellation / failure on triggers, in each active state ---- */

static void apply_trigger(int t)
{
    autotune_inputs_t *in = &g_rig.in;
    switch (t) {
    case 0: in->estop = true; break;
    case 1: in->safety_fault = true; break;
    case 2: in->ownership_ok = false; break;
    case 3: in->boot_epoch = EPOCH + 1; break;
    case 4: in->manual_pump_active = true; break;
    case 5: in->comm_ok = false; break;
    case 6: in->weight_link_ok = false; break;     /* -> FAILED */
    default: in->weight_fresh_valid = false; break; /* -> FAILED */
    }
}

static void t_seq_triggers(void)
{
    static const autotune_state_t states[4] = {
        AT_S_PRECHECK, AT_S_MEASURING_STARTUP, AT_S_CALIBRATING_FLOW, AT_S_MEASURING_SHUTOFF };
    for (int si = 0; si < 4; si++) {
        for (int t = 0; t < 8; t++) {
            happy_plant(&g_plant[0], 21);
            std_setup(&g_plant[0], 1, true);
            rig_arm_start();
            rig_run_to_state(states[si], 40000);
            bool reached = g_rig.seq.state == states[si];
            bool was_on = g_rig.cmd;
            apply_trigger(t);
            rig_run(g_rig.pl->now + 10, false);   /* exactly one tick */
            autotune_state_t want = (t >= 6) ? AT_S_FAILED : AT_S_CANCELLED;
            char nm[80];
            snprintf(nm, sizeof(nm), "at_seq_trigger_%d_in_state_%d_stops_relay_same_tick", t, (int)states[si]);
            ck(reached && g_rig.seq.state == want && !g_rig.seq.st.relay_on && !g_rig.cmd, nm);
            if (states[si] == AT_S_CALIBRATING_FLOW && t == 0)
                ck(was_on, "at_seq_trigger_hit_while_relay_request_was_ON");
            /* terminal is sticky */
            rig_run(g_rig.pl->now + 500, false);
            ck(g_rig.seq.state == want && !g_rig.cmd,
               "at_seq_terminal_state_sticky_relay_off");
        }
    }

    /* ARMED: stale weight cancels (not FAILED), restart epoch cancels */
    for (int t = 0; t < 2; t++) {
        happy_plant(&g_plant[0], 22);
        std_setup(&g_plant[0], 1, true);
        rig_run(300, false);
        autotune_arm_req_t a = mkarm(&g_rig);
        autotune_auth_arm(&g_rig.auth, &a);
        rig_run(g_rig.pl->now + 30, false);
        bool armed = g_rig.seq.state == AT_S_ARMED;
        if (t == 0) g_rig.in.weight_fresh_valid = false; else g_rig.in.boot_epoch = EPOCH + 2;
        rig_run(g_rig.pl->now + 10, false);
        ckf(armed && g_rig.seq.state == AT_S_CANCELLED && !g_rig.cmd,
            "at_seq_armed_cancelled_by_trigger_%d", t);
    }
}

static void t_seq_transitions(void)
{
    /* IDLE + RUNNING auth without a prior ARMED observation: never starts */
    happy_plant(&g_plant[0], 31);
    std_setup(&g_plant[0], 1, true);
    rig_run(300, false);
    autotune_arm_req_t a = mkarm(&g_rig);
    autotune_auth_arm(&g_rig.auth, &a);
    autotune_start_req_t s = mkstart(&g_rig);
    ck(autotune_auth_start(&g_rig.auth, &s) == AT_R_NONE, "at_seq_tt_auth_running_without_idle_armed_step");
    rig_run(g_rig.pl->now + 5000, false);
    ck(g_rig.seq.state == AT_S_IDLE && g_rig.on_ticks == 0, "at_seq_tt_IDLE_cannot_skip_ARMED");

    /* wrong channel RUNNING handoff for an ARMED sequencer */
    happy_plant(&g_plant[0], 32);
    std_setup(&g_plant[0], 1, true);
    rig_run(300, false);
    a = mkarm(&g_rig);
    autotune_auth_arm(&g_rig.auth, &a);
    rig_run(g_rig.pl->now + 30, false);
    g_rig.auth.st.state = AT_AUTH_RUNNING;      /* forged, wrong channel below */
    g_rig.auth.st.channel = 2; g_rig.auth.st.material_id = 2;
    rig_run(g_rig.pl->now + 100, false);
    ck(g_rig.seq.state == AT_S_CANCELLED && g_rig.on_ticks == 0, "at_seq_tt_ARMED_wrong_channel_handoff_cancelled");

    /* terminal states never revive, even with a fresh ARM/RUNNING */
    happy_plant(&g_plant[0], 33);
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(g_rig.pl->now + 200, false);
    g_rig.in.estop = true;
    rig_run(g_rig.pl->now + 20, false);
    g_rig.in.estop = false;
    autotune_auth_finish(&g_rig.auth);
    a = mkarm(&g_rig);
    autotune_auth_arm(&g_rig.auth, &a);
    rig_run(g_rig.pl->now + 50, false);
    s = mkstart(&g_rig);
    autotune_auth_start(&g_rig.auth, &s);
    uint32_t on0 = g_rig.on_ticks;
    rig_run(g_rig.pl->now + 3000, false);
    ck(g_rig.seq.state == AT_S_CANCELLED && g_rig.on_ticks == on0, "at_seq_tt_CANCELLED_is_terminal");

    /* full run: collect legal transitions only */
    happy_plant(&g_plant[0], 34);
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(120000, true);
    ck(g_rig.seq.state == AT_S_COMPLETED, "at_seq_tt_happy_run_reaches_COMPLETED");
    uint32_t after = g_rig.on_ticks;
    rig_run(g_rig.pl->now + 2000, false);
    ck(g_rig.seq.state == AT_S_COMPLETED && g_rig.on_ticks == after, "at_seq_tt_COMPLETED_is_terminal");
    ck(g_tr_seen[AT_S_IDLE][AT_S_ARMED] > 0 && g_tr_seen[AT_S_ARMED][AT_S_PRECHECK] > 0 &&
       g_tr_seen[AT_S_PRECHECK][AT_S_MEASURING_STARTUP] > 0 &&
       g_tr_seen[AT_S_MEASURING_STARTUP][AT_S_CALIBRATING_FLOW] > 0 &&
       g_tr_seen[AT_S_CALIBRATING_FLOW][AT_S_MEASURING_SHUTOFF] > 0 &&
       g_tr_seen[AT_S_MEASURING_SHUTOFF][AT_S_MEASURING_STARTUP] > 0 &&
       g_tr_seen[AT_S_MEASURING_SHUTOFF][AT_S_ESTIMATING_PARAMETERS] > 0 &&
       g_tr_seen[AT_S_ESTIMATING_PARAMETERS][AT_S_VALIDATING_PARAMETERS] > 0 &&
       g_tr_seen[AT_S_VALIDATING_PARAMETERS][AT_S_COMPLETED] > 0,
       "at_seq_tt_every_legal_forward_transition_observed");
    ck(g_tr_seen[AT_S_CALIBRATING_FLOW][AT_S_FAILED] > 0 && g_tr_seen[AT_S_CALIBRATING_FLOW][AT_S_CANCELLED] > 0 &&
       g_tr_seen[AT_S_MEASURING_STARTUP][AT_S_CANCELLED] > 0 && g_tr_seen[AT_S_PRECHECK][AT_S_CANCELLED] > 0 &&
       g_tr_seen[AT_S_MEASURING_SHUTOFF][AT_S_FAILED] > 0 && g_tr_seen[AT_S_ARMED][AT_S_CANCELLED] > 0,
       "at_seq_tt_abort_transitions_observed");
    /* the aggregated matrix over EVERY run so far */
    uint32_t illegal_cells = 0;
    for (int f = 0; f < 11; f++)
        for (int t = 0; t < 11; t++)
            if (g_tr_seen[f][t] && !legal_next[f][t]) illegal_cells++;
    ck(illegal_cells == 0 && g_illegal_seen == 0, "at_seq_tt_no_illegal_transition_in_any_run");
    ck(!legal_next[AT_S_COMPLETED][AT_S_IDLE] && !legal_next[AT_S_FAILED][AT_S_ARMED] &&
       !legal_next[AT_S_CANCELLED][AT_S_PRECHECK] && !legal_next[AT_S_IDLE][AT_S_CALIBRATING_FLOW] &&
       !legal_next[AT_S_ARMED][AT_S_CALIBRATING_FLOW] && !legal_next[AT_S_PRECHECK][AT_S_CALIBRATING_FLOW],
       "at_seq_tt_table_marks_illegal_pairs_illegal");
}

/* ---- independent limits ---- */

static void t_limits(void)
{
    uint32_t first_on = 0;
    uint32_t first_off = probe_first_off(41, &first_on);
    ESP_LOGI(TAG, "TEST_INFO autotune[SIMULATED] clean run first ON at %u ms, first OFF at %u ms",
             (unsigned)first_on, (unsigned)first_off);

    /* single pulse 3 s: plan poked above the limit; hard limit still stops it */
    happy_plant(&g_plant[0], 41);
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.pulse_on_ms = 3500;
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LIMIT_PULSE_ON, "at_limit_single_pulse_3s_FAILED");
    ck(g_rig.last_off - g_rig.last_on == 3000, "at_limit_single_pulse_relay_off_on_exact_tick");
    ck(!g_rig.cmd && g_rig.seq.st.cum_on_ms == 3000, "at_limit_single_pulse_accounted");

    /* cumulative 20 s: hit with the real 20 s fixture; others raised so they cannot fire */
    happy_plant(&g_plant[0], 42);
    g_plant[0].q = 3.5f;
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.pulse_on_ms = 2900;
    g_rig.seq.cfg.n_trials = 16;
    g_rig.seq.lim.target_mass_g = 9000;
    g_rig.seq.lim.max_delivered_g = 10000;
    g_rig.seq.lim.max_wall_ms = 400000;
    rig_arm_start();
    rig_run(300000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LIMIT_CUM_ON, "at_limit_cumulative_20s_FAILED");
    ck(g_rig.seq.st.cum_on_ms == 17400 && !g_rig.cmd, "at_limit_cumulative_20s_over_limit_pulse_refused_never_exceeded");
    ESP_LOGI(TAG, "TEST_INFO cum test: cum_on=%u trials_done=%d state=%d", (unsigned)g_rig.seq.st.cum_on_ms, (int)g_rig.seq.trials_done, (int)g_rig.seq.state);
    /* backstop while ON: limit shrunk mid third pulse, hit on the exact tick */
    happy_plant(&g_plant[0], 42);
    g_plant[0].q = 3.5f;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    while (g_rig.pl->now < 100000 && !(g_rig.seq.trials_done == 2 && g_rig.seq.state == AT_S_CALIBRATING_FLOW)) rig_step();
    g_rig.seq.lim.max_total_on_ms = 5000;
    rig_run(100000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LIMIT_CUM_ON, "at_limit_cumulative_while_on_FAILED");
    ck(g_rig.seq.st.cum_on_ms == 5000 && !g_rig.cmd, "at_limit_cumulative_relay_off_on_exact_tick");

    /* wall 120 s with relay OFF (waiting): stops exactly at 120000 ms after START */
    happy_plant(&g_plant[0], 43);
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.rest_ms = 8000;   /* long rests so the 120 s fixture wall limit is what ends the run */
    g_rig.seq.cfg.n_trials = 16;
    g_rig.seq.lim.target_mass_g = 9000;
    g_rig.seq.lim.max_delivered_g = 10000;
    rig_arm_start();
    rig_run(400000, true);
    ESP_LOGI(TAG, "TEST_INFO wall test: state=%d reason=%d wall=%u", (int)g_rig.seq.state, (int)g_rig.seq.reason, (unsigned)g_rig.seq.st.wall_ms);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LIMIT_WALL, "at_limit_wall_120s_FAILED");
    ck(g_rig.seq.st.wall_ms == 120000 && !g_rig.cmd, "at_limit_wall_stops_on_exact_tick_relay_off");

    /* wall limit while the relay is ON: shrunk by poke, aimed mid first pulse */
    happy_plant(&g_plant[0], 41);
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    uint32_t started = g_rig.pl->now;
    g_rig.seq.lim.max_wall_ms = (first_on - started) + 1000 + 300;   /* mid pulse */
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LIMIT_WALL && !g_rig.cmd && g_rig.on_ticks > 0,
       "at_limit_wall_while_relay_on_turns_off_same_tick");
    ck(g_rig.seq.st.wall_ms == g_rig.seq.lim.max_wall_ms, "at_limit_wall_while_on_exact_tick");

    /* delivered 150 g with stop margin; others raised */
    uint32_t off_tick[2] = {0, 0};
    int32_t before_ok[2] = {0, 0}, final_g[2] = {0, 0};
    for (int garbage = 0; garbage < 2; garbage++) {
        happy_plant(&g_plant[0], 44);
        g_plant[0].q = 9.0f;
        std_setup(&g_plant[0], 1, true);
        flow_estimator_init(&g_rig.fe, NULL);
        g_rig.estimator_garbage = garbage != 0;
        g_rig.seq.cfg.pulse_on_ms = 2900;
        g_rig.seq.cfg.n_trials = 16;
        g_rig.seq.lim.target_mass_g = 9000;
        g_rig.seq.lim.max_total_on_ms = 400000;
        g_rig.seq.lim.max_wall_ms = 800000;
        rig_arm_start();
        float w0 = g_plant[0].w;
        int32_t prev_sum = 0, fail_sum = 0;
        while (g_rig.pl->now < 300000 && !terminal(g_rig.seq.state)) {
            prev_sum = g_rig.seq.st.delivered_g + g_rig.seq.st.margin_g;
            rig_step();
            fail_sum = g_rig.seq.st.delivered_g + g_rig.seq.st.margin_g;
        }
        rig_run(g_rig.pl->now + 6000, false);
        bool lim = g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LIMIT_DELIVERED;
        ESP_LOGI(TAG, "TEST_INFO delivered test: st=%d rs=%d dl=%d mg=%d prev=%d", (int)g_rig.seq.state, (int)g_rig.seq.reason, (int)g_rig.seq.st.delivered_g, (int)g_rig.seq.st.margin_g, (int)prev_sum);
        off_tick[garbage] = g_rig.pl->now;
        before_ok[garbage] = prev_sum;
        final_g[garbage] = (int32_t)(g_plant[0].w - w0);
        ckf(lim && !g_rig.cmd, garbage ? "at_limit_delivered_150g_FAILED_with_garbage_estimator"
                                       : "at_limit_delivered_150g_FAILED_%d", garbage);
        ckf(fail_sum >= 150 && prev_sum < 150,
            "at_limit_delivered_stopped_when_delivered_plus_margin_reached_%d", garbage);
        ckf(final_g[garbage] <= 150, "at_limit_delivered_final_mass_not_above_150g_%d", garbage);
    }
    ESP_LOGI(TAG, "TEST_INFO autotune[SIMULATED] delivered-limit final mass %d g (limit 150), prev sum %d",
             (int)final_g[0], (int)before_ok[0]);
    ck(off_tick[0] == off_tick[1] && final_g[0] == final_g[1],
       "at_limit_delivered_identical_with_wrong_or_invalid_estimator");

    /* target 100 g: stops pulsing when delivered+margin reaches it, no more ON after */
    happy_plant(&g_plant[0], 45);
    g_plant[0].q = 8.0f;
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.pulse_on_ms = 2000;
    g_rig.seq.cfg.n_trials = 16;
    g_rig.seq.lim.max_total_on_ms = 400000;
    g_rig.seq.lim.max_wall_ms = 800000;
    rig_arm_start();
    float w0 = g_plant[0].w;
    rig_run(400000, true);
    uint32_t on_end = g_rig.on_ticks;
    rig_run(g_rig.pl->now + 3000, false);
    ck(g_rig.seq.st.target_reached && g_rig.on_ticks == on_end, "at_limit_target_100g_reached_no_more_ON");
    ck((int32_t)(g_plant[0].w - w0) <= 100, "at_limit_target_final_mass_not_above_100g");
    ESP_LOGI(TAG, "TEST_INFO autotune[SIMULATED] target run: final mass %d g, trials_done %d, state %d reason %d",
             (int)(g_plant[0].w - w0), (int)g_rig.seq.trials_done, (int)g_rig.seq.state, (int)g_rig.seq.reason);
}

static void t_watchdogs(void)
{
    uint32_t first_on = 0;
    uint32_t first_off = probe_first_off(51, &first_on);

    /* blocked nozzle: weight window watchdog; no estimator exists in the path */
    happy_plant(&g_plant[0], 51);
    g_plant[0].blocked = true;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_NO_PROGRESS && !g_rig.cmd,
       "at_watchdog_blocked_nozzle_FAILED_NO_PROGRESS");
    ck(g_rig.last_off - g_rig.last_on == 1800, "at_watchdog_blocked_nozzle_window_exact_tick");

    /* same fault with the weight window disabled: ON-time accounting still catches it */
    happy_plant(&g_plant[0], 51);
    g_plant[0].blocked = true;
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.noprog_window_ms = 100000000u;
    g_rig.seq.cfg.pulse_on_ms = 1000;
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_NO_PROGRESS && !g_rig.cmd,
       "at_watchdog_on_time_accounting_catches_no_progress");
    ck(g_rig.seq.st.cum_on_ms == 2500, "at_watchdog_on_time_accounting_exact_cumulative");

    /* no weight response while ON and flow estimator garbage: still caught */
    happy_plant(&g_plant[0], 52);
    g_plant[0].blocked = true;
    std_setup(&g_plant[0], 1, true);
    flow_estimator_init(&g_rig.fe, NULL);
    g_rig.estimator_garbage = true;
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.reason == AT_R_NO_PROGRESS, "at_watchdog_independent_of_estimator");

    /* weight decreasing */
    happy_plant(&g_plant[0], 53);
    g_plant[0].surge_g = -20; g_plant[0].surge_at = (first_on + 1200) / 100u * 100u + 100u;
    g_plant[0].surge_at += 0;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_WEIGHT_DECREASING && !g_rig.cmd,
       "at_watchdog_weight_decreasing_FAILED_relay_off");

    /* leak while at rest (before the first pulse) */
    happy_plant(&g_plant[0], 54);
    g_plant[0].leak_g_s = 6.0f; g_plant[0].leak_from = 100;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_LEAK_OFF_RISE && g_rig.on_ticks == 0,
       "at_watchdog_leak_at_rest_FAILED_before_any_ON");

    /* leak after the first pulse (relay OFF) */
    happy_plant(&g_plant[0], 51);
    g_plant[0].leak_g_s = 3.0f; g_plant[0].leak_from = first_off + 200;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(120000, true);
    ck(g_rig.seq.state == AT_S_FAILED && (g_rig.seq.reason == AT_R_LEAK_OFF_RISE || g_rig.seq.reason == AT_R_SETTLE_TIMEOUT)
       && !g_rig.cmd, "at_watchdog_weight_rise_with_relay_off_FAILED");
    ck(g_rig.seq.reason == AT_R_LEAK_OFF_RISE, "at_watchdog_leak_after_shutoff_reason_LEAK");

    /* surge: one-sample step */
    happy_plant(&g_plant[0], 55);
    g_plant[0].surge_g = 60; g_plant[0].surge_at = (first_on + 900) / 100u * 100u + 100u;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_SURGE && !g_rig.cmd,
       "at_watchdog_surge_step_FAILED_relay_off");

    /* surge: sustained implausible rate while ON */
    happy_plant(&g_plant[0], 56);
    g_plant[0].q = 40.0f;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_SURGE && !g_rig.cmd,
       "at_watchdog_surge_rate_FAILED_relay_off");

    /* sender restart (seq reset) */
    happy_plant(&g_plant[0], 57);
    g_plant[0].restart_at = (first_on + 600) / 100u * 100u + 100u;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_SENDER_RESTART && !g_rig.cmd,
       "at_watchdog_sender_restart_FAILED_relay_off");

    /* stale data: sender goes silent in the middle of a pulse */
    happy_plant(&g_plant[0], 58);
    g_plant[0].stall_from = first_on + 500; g_plant[0].stall_to = first_on + 5000;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(60000, true);
    ck(g_rig.seq.state == AT_S_FAILED && g_rig.seq.reason == AT_R_WEIGHT_STALE && !g_rig.cmd,
       "at_watchdog_stale_weight_mid_pulse_FAILED_relay_off");
    ck(g_rig.last_off - first_on < 500 + 1000 + 100 + 120, "at_watchdog_stale_detected_within_stale_ms");

    /* drop / duplicate / reorder tolerance: still completes */
    happy_plant(&g_plant[0], 59);
    g_plant[0].drop_pct = 5; g_plant[0].dup_pct = 10; g_plant[0].reorder_pct = 5;
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(120000, true);
    ck(g_rig.seq.state == AT_S_COMPLETED, "at_sim_dropped_duplicate_reordered_samples_tolerated");
}

/* ---- characterization maths vs simulator truth ---- */

typedef struct { const char *name; float q; uint32_t delay, ramp; float tau; int noise; uint32_t lat, transport, on_ms, tail_ms; } cc_t;

static void t_char_accuracy(void)
{
    static const cc_t cases[] = {
        { "fast_q8_noise1",   8.0f, 300, 0,   1000.0f, 1, 100, 0,   4000, 8000  },
        { "mid_q3_lat200",    3.0f, 500, 0,   2000.0f, 1, 200, 0,   8000, 14000 },
        { "noiseless_q5",     5.0f, 200, 0,   600.0f,  0, 50,  0,   3000, 8000  },
        { "ramp400_q6",       6.0f, 300, 400, 1500.0f, 1, 100, 0,   6000, 10000 },
        { "mqtt400_q6",       6.0f, 300, 0,   1500.0f, 1, 100, 400, 6000, 10000 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const cc_t *c = &cases[i];
        plant_init(&g_plant[0], 100 + i, c->q, c->delay, c->tau);
        g_plant[0].ramp_ms = c->ramp; g_plant[0].noise = c->noise;
        g_plant[0].lat_ms = c->lat; g_plant[0].transport_ms = c->transport;
        at_trial_t t = build_trial(&g_plant[0], 1, 1, 2000, c->on_ms, c->tail_ms, c->transport);
        autotune_char_cfg_t cfg; autotune_char_cfg_default(&cfg);
        at_trial_result_t r;
        at_trial_reason_t rr = autotune_char_analyze_trial(&cfg, &t, &r);
        info_sim(c->name, &r, c->q, (float)c->delay, c->tau);
        char nm[72];
        snprintf(nm, sizeof(nm), "at_char_%s_trial_ok", c->name);
        ck(rr == AT_TRIAL_OK, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_Q_within_5pct", c->name);
        ck(fabsf(r.q_eff - c->q) <= 0.05f * c->q, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_Q_error_within_4_reported_se", c->name);
        ck(fabsf(r.q_eff - c->q) <= 4.0f * r.q_se + 0.02f, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_C_within_25pct_of_Q_tau", c->name);
        float ctruth = c->q * c->tau / 1000.0f;
        ck(fabsf(r.c_g - ctruth) <= 0.25f * ctruth + 1.0f, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_startup_detect_not_before_truth", c->name);
        ck(r.startup_ms >= (float)c->delay - 100.0f, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_startup_intercept_within_250ms", c->name);
        ck(fabsf(r.startup_intercept_ms - (float)(c->delay + c->ramp)) <= 250.0f, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_latency_known_part", c->name);
        ck(fabsf(r.latency_ms - (float)(c->lat + c->transport)) < 1.0f, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_tau_flagged_invalid_or_within_45pct", c->name);
        ck(!(r.valid & AT_V_TAU) || fabsf(r.tau_off_ms - c->tau) <= 0.45f * c->tau, nm);
        snprintf(nm, sizeof(nm), "at_char_%s_tau_valid_when_tail_large", c->name);
        ck(c->q * c->tau / 1000.0f < 8.0f || (r.valid & AT_V_TAU), nm);
        snprintf(nm, sizeof(nm), "at_char_%s_sigma_and_flags", c->name);
        ck((r.valid & AT_V_SIGMA) && (r.valid & AT_V_Q) && (r.valid & AT_V_C) && (r.valid & AT_V_SETTLED)
           && (c->noise == 0 ? r.sigma_g < 0.2f : (r.sigma_g > 0.5f && r.sigma_g < 1.2f)), nm);
    }

    /* transport mis-assumption shifts the startup by the error (documented limit) */
    plant_init(&g_plant[0], 120, 6.0f, 300, 1000.0f);
    g_plant[0].transport_ms = 400;
    at_trial_t t = build_trial(&g_plant[0], 1, 1, 2000, 3000, 8000, 0);
    autotune_char_cfg_t cfg; autotune_char_cfg_default(&cfg);
    at_trial_result_t r0, r1;
    autotune_char_analyze_trial(&cfg, &t, &r0);
    t.transport_ms = 400;
    autotune_char_analyze_trial(&cfg, &t, &r1);
    ck(r0.startup_ms - r1.startup_ms > 300.0f && r0.startup_ms - r1.startup_ms < 500.0f,
       "at_char_unknown_transport_inflates_apparent_startup_by_its_value");

    /* viscous / slow flow: long pulse, long settle window. SIMULATION fixture only. */
    static const float vq[3] = { 2.0f, 1.0f, 0.3f };
    for (int i = 0; i < 3; i++) {
        plant_init(&g_plant[0], 130 + (uint32_t)i, vq[i], 1500, 4000.0f);
        g_plant[0].pub_ms = 500;
        at_trial_t vt = build_trial(&g_plant[0], 1, 1, 5000, i == 2 ? 60000 : 30000, 70000, 0);
        autotune_char_cfg_t vc; autotune_char_cfg_default(&vc);
        vc.settle_win_ms = 15000; vc.settle_slope_g_per_s = 0.1f; vc.settle_timeout_ms = 50000;
        vc.min_settle_ms = 5000; vc.min_q_span_ms = 5000;
        at_trial_result_t vr;
        at_trial_reason_t vrr = autotune_char_analyze_trial(&vc, &vt, &vr);
        ESP_LOGI(TAG, "TEST_INFO viscous[SIM] true q=%d mg/s: reason=%d Q=%d mg/s startup=%d ms",
                 (int)(vq[i] * 1000), (int)vrr, (int)(vr.q_eff * 1000), (int)vr.startup_ms);
        ESP_LOGI(TAG, "TEST_INFO viscous[SIM] C=%d mg (true %d) tau_valid=%d (illustrative fixture)",
                 (int)(vr.c_g * 1000), (int)(vq[i] * 4000), (vr.valid & AT_V_TAU) ? 1 : 0);
        ckf(vrr == AT_TRIAL_OK, "at_char_viscous_case_%d_trial_ok", i);
        ckf(fabsf(vr.q_eff - vq[i]) <= 0.10f * vq[i], "at_char_viscous_case_%d_Q_within_10pct", i);
        ckf(fabsf(vr.c_g - vq[i] * 4.0f) <= 0.35f * vq[i] * 4.0f + 1.2f, "at_char_viscous_case_%d_C_plausible", i);
    }
}

static void t_char_instances_independent(void)
{
    static autotune_char_t c1, c2;
    autotune_char_cfg_t cfg; autotune_char_cfg_default(&cfg);
    ck(autotune_char_init(&c1, 1, 1, &cfg) && autotune_char_init(&c2, 2, 2, &cfg), "at_char_two_instances_init");
    ck(!autotune_char_init(&c1, 3, 1, &cfg), "at_char_bad_channel_unusable");
    autotune_char_init(&c1, 1, 1, &cfg);
    plant_init(&g_plant[0], 201, 8.0f, 300, 1000.0f);   /* M1 / CH1 / Pump1 */
    plant_init(&g_plant[1], 202, 3.0f, 700, 1800.0f);   /* M2 / CH2 / Pump2 */
    for (int k = 0; k < 5; k++) {
        at_trial_t t1 = build_trial(&g_plant[0], 1, (uint32_t)(10 + k), 2000, 6000, 8000, 0);
        ck(autotune_char_add_trial(&c1, &t1, NULL) == AT_TRIAL_OK, "at_char_M1_trial_accepted");
        at_trial_t t2 = build_trial(&g_plant[1], 2, (uint32_t)(20 + k), 2000, 6000, 12000, 0);
        ck(autotune_char_add_trial(&c2, &t2, NULL) == AT_TRIAL_OK, "at_char_M2_trial_accepted");
    }
    static autotune_char_result_t r1, r2, r1b;
    autotune_char_summarize(&c1, &r1);
    autotune_char_summarize(&c2, &r2);
    ESP_LOGI(TAG, "TEST_INFO char[SIM] M1 Q=%d mg/s C=%d mg conf=%d | M2 Q=%d mg/s C=%d mg conf=%d",
             (int)(r1.q_eff.mean * 1000), (int)(r1.c.mean * 1000), (int)r1.confidence,
             (int)(r2.q_eff.mean * 1000), (int)(r2.c.mean * 1000), (int)r2.confidence);
    ck(r1.channel == 1 && r2.channel == 2 && r1.material_id == 1 && r2.material_id == 2, "at_char_results_carry_channel_ids");
    ck(r1.n_valid == 5 && r2.n_valid == 5, "at_char_both_instances_five_valid");
    ck(fabsf(r1.q_eff.mean - 8.0f) < 0.4f && fabsf(r2.q_eff.mean - 3.0f) < 0.15f, "at_char_each_instance_tracks_its_own_plant");
    ck(fabsf(r1.q_eff.mean - r2.q_eff.mean) > 3.0f && fabsf(r1.startup.mean - r2.startup.mean) > 200.0f,
       "at_char_M1_and_M2_results_differ");
    ck(r1.confidence == AT_CONF_OK && r2.confidence == AT_CONF_OK, "at_char_five_trials_low_scatter_confidence_OK");
    /* nothing leaks: feeding M2 data into M1 is refused and changes nothing */
    at_trial_t x = build_trial(&g_plant[1], 2, 99, 2000, 6000, 12000, 0);
    uint8_t n_before = c1.n;
    ck(autotune_char_add_trial(&c1, &x, NULL) == AT_TRIAL_CHANNEL_MISMATCH && c1.n == n_before,
       "at_char_cross_channel_trial_refused");
    autotune_char_summarize(&c1, &r1b);
    ck(memcmp(&r1, &r1b, sizeof(r1)) == 0, "at_char_M1_unchanged_by_M2_activity");
    ck(r1.trial_ids[0] == 10 && r2.trial_ids[0] == 20, "at_char_trial_references_kept_per_instance");
}

static void t_char_edge_cases(void)
{
    autotune_char_cfg_t cfg; autotune_char_cfg_default(&cfg);
    static autotune_char_t c;
    autotune_char_init(&c, 1, 1, &cfg);

    /* short pulses inside the 300 ms edge exclusion */
    uint32_t shorts[3] = { 200, 300, 599 };
    for (int i = 0; i < 3; i++) {
        plant_init(&g_plant[0], 300 + (uint32_t)i, 8.0f, 100, 800.0f);
        at_trial_t t = build_trial(&g_plant[0], 1, (uint32_t)(40 + i), 2000, shorts[i], 8000, 0);
        ckf(autotune_char_add_trial(&c, &t, NULL) == AT_TRIAL_REJECTED_TOO_SHORT,
            "at_char_short_pulse_%d_ms_REJECTED_TOO_SHORT", (int)shorts[i]);
    }
    static autotune_char_result_t sr;
    autotune_char_summarize(&c, &sr);
    ck(sr.n_too_short == 3 && sr.n_valid == 0 && sr.confidence == AT_CONF_NONE &&
       (sr.conf_reasons & AT_CR_NO_VALID_TRIAL), "at_char_short_pulses_counted_never_used_confidence_NONE");

    /* settle timeout: weight keeps rising after OFF */
    plant_init(&g_plant[0], 310, 6.0f, 300, 800.0f);
    g_plant[0].leak_g_s = 2.0f; g_plant[0].leak_from = 1;
    at_trial_t t = build_trial(&g_plant[0], 1, 50, 2000, 3000, 20000, 0);
    at_trial_result_t r;
    ck(autotune_char_analyze_trial(&cfg, &t, &r) != AT_TRIAL_OK, "at_char_rising_tail_not_ok");
    /* leak also contaminates the baseline here; make a clean baseline instead */
    plant_init(&g_plant[0], 311, 6.0f, 300, 800.0f);
    t = build_trial(&g_plant[0], 1, 51, 2000, 3000, 0, 0);
    g_plant[0].leak_g_s = 2.0f; g_plant[0].leak_from = g_plant[0].now;
    for (uint32_t i = 0; i < 20000; i++) { plant_step(&g_plant[0], false); collect(&g_plant[0]); }
    t.n = g_tn;
    ck(autotune_char_analyze_trial(&cfg, &t, &r) == AT_TRIAL_UNSETTLED, "at_char_settle_timeout_UNSETTLED");
    ck(r.reason == AT_TRIAL_UNSETTLED && !(r.valid & AT_V_SETTLED), "at_char_unsettled_no_settled_value");
    t.n = (uint16_t)(t.n / 4);
    ck(autotune_char_analyze_trial(&cfg, &t, &r) == AT_TRIAL_TAIL_TOO_SHORT, "at_char_short_tail_TAIL_TOO_SHORT");

    /* no baseline / no rise */
    plant_init(&g_plant[0], 312, 6.0f, 300, 800.0f);
    t = build_trial(&g_plant[0], 1, 52, 200, 3000, 8000, 0);
    ck(autotune_char_analyze_trial(&cfg, &t, &r) == AT_TRIAL_NO_BASELINE, "at_char_no_baseline_rejected");
    plant_init(&g_plant[0], 313, 6.0f, 300, 800.0f);
    g_plant[0].blocked = true;
    t = build_trial(&g_plant[0], 1, 53, 2000, 3000, 8000, 0);
    ck(autotune_char_analyze_trial(&cfg, &t, &r) == AT_TRIAL_NO_RISE, "at_char_blocked_nozzle_NO_RISE");
    ck(autotune_char_analyze_trial(&cfg, NULL, &r) == AT_TRIAL_BAD_INPUT, "at_char_null_trial_bad_input");

    /* uncertainty bound vs n, synthetic per-trial results */
    ck(autotune_t95_pred_k(1) == 0.0f, "at_char_k_n1_no_bound");
    ck(fabsf(autotune_t95_pred_k(2) - 6.314f * sqrtf(1.5f)) < 1e-3f, "at_char_k_n2_formula");
    ck(fabsf(autotune_t95_pred_k(10) - 1.833f * sqrtf(1.1f)) < 1e-3f, "at_char_k_n10_formula");
    ck(autotune_t95_pred_k(2) > autotune_t95_pred_k(3) && autotune_t95_pred_k(3) > autotune_t95_pred_k(6) &&
       autotune_t95_pred_k(6) > autotune_t95_pred_k(16), "at_char_k_decreases_with_n");
    static autotune_char_t cs;
    float qs[6] = { 5.00f, 5.10f, 4.90f, 5.05f, 4.95f, 5.02f };
    for (int n = 1; n <= 6; n++) {
        autotune_char_init(&cs, 1, 1, &cfg);
        for (int i = 0; i < n; i++) {
            at_trial_result_t sr2;
            memset(&sr2, 0, sizeof(sr2));
            sr2.reason = AT_TRIAL_OK; sr2.trial_id = (uint32_t)i + 1;
            sr2.valid = AT_V_SIGMA | AT_V_STARTUP | AT_V_Q | AT_V_C | AT_V_SETTLED;
            sr2.sigma_g = 0.8f; sr2.q_eff = qs[i]; sr2.c_g = 4.0f + 0.1f * (float)i;
            sr2.startup_ms = 400.0f + 10.0f * (float)i; sr2.dt_ms = 100.0f;
            autotune_char_commit(&cs, &sr2);
        }
        static autotune_char_result_t rs;
        autotune_char_summarize(&cs, &rs);
        if (n == 1) {
            ck(!rs.q_eff.bound_valid && rs.confidence == AT_CONF_LOW && (rs.conf_reasons & AT_CR_LOW_N),
               "at_char_n1_no_bound_flagged_LOW");
        } else if (n == 2) {
            float exp_up = rs.q_eff.mean + autotune_t95_pred_k(2) * rs.q_eff.sd;
            ck(rs.q_eff.bound_valid && fabsf(rs.q_eff.upper - exp_up) < 1e-3f && rs.confidence == AT_CONF_LOW,
               "at_char_n2_bound_is_mean_plus_k_s_LOW");
        } else if (n == 3) {
            ck(rs.confidence == AT_CONF_LOW && (rs.conf_reasons & AT_CR_LOW_N), "at_char_n3_LOW_confidence");
        } else if (n == 5) {
            ck(rs.confidence == AT_CONF_OK, "at_char_n5_low_scatter_OK");
        } else if (n == 6) {
            ck(rs.confidence == AT_CONF_OK && rs.q_eff.upper - rs.q_eff.mean < 0.5f, "at_char_n6_tight_bound_OK");
        }
    }
    autotune_char_init(&cs, 1, 1, &cfg);
    for (int i = 0; i < 4; i++) {
        at_trial_result_t sr2;
        memset(&sr2, 0, sizeof(sr2));
        sr2.reason = AT_TRIAL_OK; sr2.trial_id = (uint32_t)i + 1;
        sr2.valid = AT_V_SIGMA | AT_V_STARTUP | AT_V_Q | AT_V_C | AT_V_SETTLED;
        sr2.sigma_g = 0.8f; sr2.q_eff = qs[i]; sr2.c_g = 4.0f; sr2.startup_ms = 400.0f; sr2.dt_ms = 100.0f;
        autotune_char_commit(&cs, &sr2);
    }
    static autotune_char_result_t rs4;
    autotune_char_summarize(&cs, &rs4);
    ck(rs4.confidence == AT_CONF_LOW && (rs4.conf_reasons & AT_CR_LOW_N), "at_char_default_n_ok_5_makes_4_trials_LOW");
    /* high scatter -> LOW even with many trials */
    autotune_char_init(&cs, 1, 1, &cfg);
    for (int i = 0; i < 6; i++) {
        at_trial_result_t sr2;
        memset(&sr2, 0, sizeof(sr2));
        sr2.reason = AT_TRIAL_OK; sr2.trial_id = (uint32_t)i + 1;
        sr2.valid = AT_V_SIGMA | AT_V_STARTUP | AT_V_Q | AT_V_C | AT_V_SETTLED;
        sr2.sigma_g = 0.8f; sr2.q_eff = (i & 1) ? 8.0f : 4.0f; sr2.c_g = 4.0f; sr2.startup_ms = 400.0f; sr2.dt_ms = 100.0f;
        autotune_char_commit(&cs, &sr2);
    }
    autotune_char_summarize(&cs, &rs4);
    ck(rs4.confidence == AT_CONF_LOW && (rs4.conf_reasons & AT_CR_SCATTER_Q), "at_char_high_scatter_flags_LOW");
}

/* ---- full runs, candidate, determinism ---- */

typedef struct { float q, c, start, kp, tau; int n_valid; } run_sig_t;

static run_sig_t run_happy(uint32_t seed, uint32_t transport, uint32_t assumed_transport, bool *ok)
{
    happy_plant(&g_plant[0], seed);
    g_plant[0].transport_ms = transport;
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.transport_ms = assumed_transport;
    rig_arm_start();
    rig_run(150000, true);
    run_sig_t s;
    memset(&s, 0, sizeof(s));
    const autotune_result_t *res = autotune_seq_result(&g_rig.seq);
    *ok = res != NULL;
    if (res) {
        s.q = res->ch.q_eff.mean; s.c = res->ch.c.mean; s.start = res->ch.startup.mean;
        s.kp = res->cand.kp_per_g; s.tau = res->ch.tau.mean; s.n_valid = res->ch.n_valid;
    }
    return s;
}

static void t_full_runs(void)
{
    bool ok;
    run_sig_t a = run_happy(61, 0, 0, &ok);
    ck(ok && g_rig.seq.state == AT_S_COMPLETED && g_rig.viol == 0, "at_happy_path_ARM_START_pulses_COMPLETED");
    const autotune_result_t *res = autotune_seq_result(&g_rig.seq);
    ESP_LOGI(TAG, "TEST_INFO happy[SIM] fixture q=6 g/s delay=300 tau_off=1200 (illustrative): n_valid=%d conf=%d",
             a.n_valid, res ? (int)res->ch.confidence : -1);
    ESP_LOGI(TAG, "TEST_INFO happy[SIM] Q=%d mg/s (err %d permille) C=%d mg (true 7200) startup=%d ms tau=%d ms",
             (int)(a.q * 1000), pm(a.q, 6.0f), (int)(a.c * 1000), (int)a.start, (int)a.tau);
    ESP_LOGI(TAG, "TEST_INFO happy[SIM] wall=%u ms cumulative ON=%u ms",
             (unsigned)g_rig.seq.st.wall_ms, (unsigned)g_rig.seq.st.cum_on_ms);
    if (!res) { ck(false, "at_happy_path_result_present"); return; }
    ck(a.n_valid == 4 && res->ch.confidence == AT_CONF_OK, "at_happy_path_4_valid_trials_confidence_OK");
    ck(fabsf(a.q - 6.0f) < 0.72f, "at_happy_path_Q_within_12pct_of_simulator_truth_4_pulses_of_2s");
    ck(fabsf(a.c - 7.2f) < 2.5f, "at_happy_path_C_close_to_simulator_truth");
    ck(res->channel == 1 && res->material_id == 1, "at_result_channel_material_mapping");
    ck(res->origin == AT_ORIGIN_SIMULATED && res->verification == AT_VERIF_UNVERIFIED, "at_result_flags_SIMULATED_UNVERIFIED");
    ck(res->limits.target_mass_g == 100 && res->limits.max_delivered_g == 150 && res->limits.max_pulse_on_ms == 3000,
       "at_result_carries_limits_used");
    ck(res->ch.trial_ids[0] == 1 && res->ch.trial_ids[3] == 4, "at_result_carries_trial_references");
    ck(g_rig.seq.st.cum_on_ms < 20000 && g_rig.seq.st.wall_ms < 120000, "at_happy_path_within_all_limits");
    autotune_auth_finish(&g_rig.auth);
    ck(g_rig.auth.st.state == AT_AUTH_IDLE, "at_auth_finish_returns_to_IDLE");

    /* candidate */
    const autotune_candidate_t *c = &res->cand;
    ck(c->status == AT_CAND_UNTESTED && c->no_cand_reasons == 0, "at_candidate_available_and_flagged_UNTESTED");
    ck(c->c_cut_valid && c->c_cut_upper_g >= c->c_cut_g && c->c_cut_g > 0.0f, "at_candidate_Ccut_with_upper_bound");
    ck(c->tau_valid == (res->ch.tau.n >= 1) && (!c->tau_valid || c->tau_off_ms > 0.0f), "at_candidate_tau_off_present_only_when_fitted");
    ck(c->flow_lb_valid && c->min_expected_flow_g_per_min > 0.0f &&
       c->min_expected_flow_g_per_min < res->ch.q_eff.mean * 60.0f, "at_candidate_min_expected_flow_is_lower_bound");
    ck(c->min_on_pulse_ms > c->startup_delay_ms + (uint32_t)res->ch.latency.mean &&
       c->min_on_pulse_ms >= res->ch.edge_excl_ms, "at_candidate_min_on_pulse_above_startup_plus_latency_and_edge_excl");
    ck(c->kp_valid && fabsf(c->kp_per_g - 1.0f / (c->q_used_g_per_s * c->tc_s)) < 1e-6f,
       "at_candidate_Kp_equals_1_over_Q_Tc");
    ck(c->q_used_g_per_s >= res->ch.q_eff.mean && c->tc_s == 4.0f, "at_candidate_Kp_uses_conservative_upper_Q");
    ck(c->kp_per_g * c->q_used_g_per_s * c->tc_s > 0.999f && c->kp_per_g * 100.0f > 0.0f,
       "at_candidate_Kp_units_per_gram_for_0_1_output");
    ESP_LOGI(TAG, "TEST_INFO cand[SIM] UNTESTED never applied: Ccut=%d (upper %d) mg tau_valid=%d tau=%d ms",
             (int)(c->c_cut_g * 1000), (int)(c->c_cut_upper_g * 1000), (int)c->tau_valid, (int)c->tau_off_ms);
    ESP_LOGI(TAG, "TEST_INFO cand[SIM] minflow=%d mg/min startup_ub=%u ms minON=%u ms",
             (int)(c->min_expected_flow_g_per_min * 1000), (unsigned)c->startup_delay_ms, (unsigned)c->min_on_pulse_ms);
    ESP_LOGI(TAG, "TEST_INFO cand[SIM] Kp=%d e-6 per g (Q used %d mg/s, Tc %d s): error g -> output 0..1",
             (int)(c->kp_per_g * 1e6f), (int)(c->q_used_g_per_s * 1000), (int)c->tc_s);

    /* Tc outside the caller bounds => no Kp, nothing else changes */
    autotune_candidate_t cc;
    autotune_candidate_build(&res->ch, 100.0f, 1.0f, 30.0f, &cc);
    ck(cc.status == AT_CAND_UNTESTED && !cc.kp_valid, "at_candidate_Tc_out_of_bounds_no_Kp");
    autotune_candidate_build(&res->ch, 0.5f, 1.0f, 30.0f, &cc);
    ck(!cc.kp_valid, "at_candidate_Tc_below_min_no_Kp");
    autotune_candidate_build(NULL, 4.0f, 1.0f, 30.0f, &cc);
    ck(cc.status == AT_CAND_NO_CANDIDATE, "at_candidate_null_result_NO_CANDIDATE");

    /* insufficient confidence: n_ok 6 with 4 trials => LOW => NO_CANDIDATE with reasons */
    happy_plant(&g_plant[0], 62);
    std_setup(&g_plant[0], 1, true);
    g_rig.seq.cfg.ch.n_ok = 6;
    g_rig.seq.ch.cfg.n_ok = 6;
    rig_arm_start();
    rig_run(150000, true);
    res = autotune_seq_result(&g_rig.seq);
    ck(res && g_rig.seq.state == AT_S_COMPLETED && res->ch.confidence == AT_CONF_LOW, "at_low_confidence_run_still_completes_UNVERIFIED");
    ck(res && res->cand.status == AT_CAND_NO_CANDIDATE && (res->cand.no_cand_reasons & AT_NC_CONF_NOT_OK) &&
       (res->cand.conf_reasons & AT_CR_LOW_N) && !res->cand.kp_valid, "at_candidate_NO_CANDIDATE_when_confidence_insufficient");
    ck(res && res->verification == AT_VERIF_UNVERIFIED && res->origin == AT_ORIGIN_SIMULATED, "at_result_unverified_even_without_candidate");

    /* origin default is UNKNOWN */
    happy_plant(&g_plant[0], 63);
    std_setup(&g_plant[0], 1, true);
    autotune_seq_set_origin(&g_rig.seq, AT_ORIGIN_UNKNOWN);
    rig_arm_start();
    rig_run(150000, true);
    res = autotune_seq_result(&g_rig.seq);
    ck(res && res->origin == AT_ORIGIN_UNKNOWN, "at_result_origin_defaults_UNKNOWN");

    /* too few valid trials: only 2 pulses -> FAILED INSUFFICIENT_TRIALS is impossible by cfg, so use blocked trials */
    ck(autotune_seq_result(NULL) == NULL, "at_result_null_sequencer_null");

    /* MQTT / transport delay: 400 ms, assumed correctly */
    run_sig_t m1 = run_happy(64, 400, 400, &ok);
    if (!ok) ESP_LOGI(TAG, "TEST_INFO mqtt run not completed: state=%d reason=%d", (int)g_rig.seq.state, (int)g_rig.seq.reason);
    ck(ok && fabsf(m1.q - 6.0f) < 0.72f, "at_mqtt_delay_400ms_assumed_correctly_completes_accurate");
    run_sig_t m0 = run_happy(64, 400, 0, &ok);
    ck(ok && m0.start - m1.start > 250.0f, "at_mqtt_delay_unmodelled_shows_as_apparent_startup");
    ESP_LOGI(TAG, "TEST_INFO autotune[SIMULATED] MQTT 400 ms: startup %d ms (delay modelled) vs %d ms (not modelled)",
             (int)m1.start, (int)m0.start);

    /* M2 on CH2 / Pump2 with a different plant */
    plant_init(&g_plant[1], 65, 4.0f, 500, 1500.0f);
    std_setup(&g_plant[1], 2, true);
    g_rig.seq.cfg.ch.n_ok = 4;
    rig_arm_start();
    rig_run(150000, true);
    res = autotune_seq_result(&g_rig.seq);
    ck(res && res->channel == 2 && res->material_id == 2 && fabsf(res->ch.q_eff.mean - 4.0f) < 0.3f,
       "at_happy_path_CH2_M2_independent_result");

    /* determinism */
    run_sig_t d1 = run_happy(77, 0, 0, &ok);
    run_sig_t d2 = run_happy(77, 0, 0, &ok);
    ck(memcmp(&d1, &d2, sizeof(d1)) == 0, "at_determinism_same_seed_bit_identical");
    run_sig_t d3 = run_happy(78, 0, 0, &ok);
    ck(memcmp(&d1, &d3, sizeof(d1)) != 0, "at_determinism_different_seed_differs");
}

static void t_isolation(void)
{
    /* The module is pure: it has no relay/task/NVS dependency. Behavioural check:
     * the only effect of a whole run is on the simulator via the request bool. */
    happy_plant(&g_plant[0], 90);
    std_setup(&g_plant[0], 1, true);
    rig_arm_start();
    rig_run(150000, true);
    ck(g_rig.viol == 0, "at_relay_request_only_in_CALIBRATING_FLOW_with_RUNNING_auth_and_hw_enable");
    ck(g_illegal_seen == 0, "at_no_illegal_transition_across_whole_suite");
}

static void run_all(void)
{
    ESP_LOGI(TAG, "TEST_INFO autotune: ALL RESULTS BELOW ARE SIMULATION RESULTS.");
    ESP_LOGI(TAG, "TEST_INFO autotune: limits and plant numbers are illustrative simulation fixtures (tests/ only),");
    ESP_LOGI(TAG, "TEST_INFO autotune: not hardware settings and not real-world accuracy. Relay request is a bool only.");
    legal_init();
    t_limits_fail_closed();
    t_hw_enable();
    t_auth();
    t_auth_cancel_triggers();
    t_seq_triggers();
    t_limits();
    t_watchdogs();
    t_char_accuracy();
    t_char_instances_independent();
    t_char_edge_cases();
    t_full_runs();
    t_seq_transitions();
    t_isolation();
    ESP_LOGI(TAG, "TEST_INFO autotune: simulation groups done");
}

/* The QEMU main task has a 3.5 KB stack; run the suite on its own task. Test
 * scaffolding only: the production components create no task. */
static SemaphoreHandle_t s_done;

static void at_task(void *arg)
{
    (void)arg;
    run_all();
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

void test_autotune_run(void)
{
    s_done = xSemaphoreCreateBinary();
    xTaskCreate(at_task, "at_test", 16384, NULL, tskIDLE_PRIORITY + 1, NULL);
    xSemaphoreTake(s_done, portMAX_DELAY);
    vSemaphoreDelete(s_done);
}
