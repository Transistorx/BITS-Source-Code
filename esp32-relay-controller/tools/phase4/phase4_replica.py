"""Phase 4 offline analysis: LABELLED REPLICA of the dual_dispense_controller control law.

*** REPLICA SIMULATION, hypothetical plants, NOT hardware evidence. ***
Python standard library only. Deterministic (fixed seeds). No network, no files written.
Run:  <server venv python> -I phase4_replica.py [selfcheck|gen|sweep|robust|stallcase|all]

Nothing here is applied to any profile. Every plant number below is a HYPOTHETICAL SWEEP
RANGE, never a measurement. 13.33 g/s is a water-only illustrative reference.

REPLICATED SOURCE (esp32-relay-controller/components/dual_dispense_controller/
dual_dispense_controller.c unless noted):
  compute_stage 525-534, stage_min_on_ms 536-545, stage_duty_clamp 547-568,
  control_channel 570-731 (timeout 616, settle 629-649, cutoff 655-664, PID 676-713),
  stage_set 203-216 (PID reset on stage change),
  dispense_window_output (components/dispense_controller/dispense_controller.c 171-190).
NOT replicated (stated, not hidden): relay mutex/ownership/READY, WEIGHT_STALE (sim samples
never go stale), have_cas_seq gate (samples always fresh), telemetry, profile store.
float32 is emulated on the PID / duty arithmetic (struct round trip).
"""
import math
import random
import struct
import sys

# --------------------------------------------------------------------------------------
# Replica constants (every one listed; source in the comment)
# --------------------------------------------------------------------------------------
TICK_MS = 10                 # supervisor vTaskDelay(10 ms)            app_main.c:43
SAMPLE_MS = 100              # weight sender publish period            task statement
CLAMPS = {                   # stage_duty_clamp 547-568: (lo, hi)
    'COARSE': (0.4, 1.0), 'FINE': (0.1, 0.5), 'MICRO': (0.0, 0.15), 'SETTLING': (0.0, 0.0)}
FW_DEFAULT = dict(          # Kconfig defaults, DDC:21-76
    kp=0.0025, ki=0.0003, kd=0.0001, tol=20, os=100, win=500, min_on=40, min_off=40,
    coarse_on=80, fine_on=30, micro_on=15, micro_thr=30, fine_thr=100,
    settle=1500, comp=15, max_dur=3_600_000, law='EXISTING')
# Server placeholder profile: kp .47 ki .47 kd 0 window 1000 min_on/off 50 tol 20 os 100
# max_duration 300000 (doc 13 B.2). Staged fields default to Kconfig (TC:303-336).
SRV_PLACEHOLDER = dict(FW_DEFAULT, kp=0.47, ki=0.47, kd=0.0, win=1000, min_on=50, min_off=50)
FW_MAX_DUR_CAP_MS = 3_600_000   # PROFILE store cap DDC:1232
T95 = [6.314, 2.920, 2.353, 2.132, 2.015, 1.943, 1.895, 1.860, 1.833, 1.812,
       1.796, 1.782, 1.771, 1.761, 1.753]          # autotune_char.h (h), df 1..15


def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]


def k_n(n):
    """k_n = t(0.95, n-1)*sqrt(1+1/n)  (autotune_t95_pred_k)."""
    if n < 2:
        return 0.0
    return T95[min(n - 1, 16) - 1] * math.sqrt(1.0 + 1.0 / n)


# --------------------------------------------------------------------------------------
# Control-law primitives (1:1 with the C source)
# --------------------------------------------------------------------------------------
def compute_stage(E, p):
    ae = -E if E < 0 else E
    if ae <= p['tol']:
        return 'SETTLING'
    if ae <= p['micro_thr']:
        return 'MICRO'
    if ae <= p['fine_thr']:
        return 'FINE'
    return 'COARSE'


def stage_clamp(stage, d):
    lo, hi = CLAMPS[stage]
    if stage == 'SETTLING':
        return 0.0
    if d < lo:
        d = lo
    if d > hi:
        d = hi
    return f32(d)


def stage_min_on(stage, p):
    return {'COARSE': p['coarse_on'], 'FINE': p['fine_on'], 'MICRO': p['micro_on']}.get(stage, p['min_on'])


def window_out(phase, duty, win, min_on, min_off, floor_pulse=False):
    """dispense_window_output. floor_pulse = PROPOSED doc-13 C.6.6 (NOT existing behaviour)."""
    if duty <= 0.0:
        return False
    if duty >= 1.0:
        return True
    if win == 0:
        return True
    on_ms = int(f32(f32(duty * float(win)) + 0.5))
    if on_ms > win:
        on_ms = win
    if on_ms < min_on:
        if not floor_pulse:
            return False
        on_ms = min_on
    if (win - on_ms) < min_off:
        return True
    return (phase % win) < on_ms


def first_tick_duty(E, p, comp=None):
    """Duty/stage/on_ms the firmware commands on the FIRST tick after a stage change
    (integral 0, derivative 0) for rest error E. Used by the self-check and the e_min tables."""
    comp = p['comp'] if comp is None else comp
    eff = E - comp
    if eff <= 0:
        return 'CUT', 0.0, 0
    stage = compute_stage(E, p)
    e = float(eff)
    out = f32(f32(p['kp']) * f32(e))
    if out < 0.0:
        out = 0.0
    if out > 1.0:
        out = 1.0
    out = stage_clamp(stage, out)
    if out <= 0.0:
        return stage, out, 0
    if out >= 1.0:
        return stage, out, p['win']
    on = int(f32(f32(out * float(p['win'])) + 0.5))
    if on < stage_min_on(stage, p):
        on = 0
    elif p['win'] - on < p['min_off']:
        on = p['win']
    return stage, out, on


def e_min_exact(kp, win, min_on):
    """Smallest PID input e [g, real] with on_ms>=min_on: kp*e*win+0.5 >= min_on."""
    return (min_on - 0.5) / (kp * win)


# --------------------------------------------------------------------------------------
# Hypothetical plant (integrating, dead time, lag, scale latency, integer grams)
# --------------------------------------------------------------------------------------
class Plant:
    """dW/dt = q(t); q = Q*open(t); valve opens D_up after ON cmd and closes D_dn after OFF
    cmd (a pulse shorter than D_up-D_dn never opens). Material arrives through a first-order
    lag tau. Scale reads W(t-L)+noise, rounded to integer grams every 100 ms. Post-shutoff
    delivery (full flow) = Q*(D_dn+tau). HYPOTHETICAL parameters only."""

    def __init__(self, Q, d_up, d_dn, tau, lat, sigma, scen='none', target=0):
        self.Q, self.du, self.dd, self.tau, self.lat, self.sigma = Q, d_up, d_dn, tau, lat, sigma
        self.scen, self.target = scen, target
        self.t = 0.0          # s
        self.W = 0.0
        self.r = 0.0          # lagged flow g/s
        self.open = False
        self.ev = []          # (t_exec, bool)
        self.tail = 0.0

    def qscale(self):
        s = self.scen
        if s == 'surge' and self.W >= self.target - 300:
            return 3.0
        if s == 'halve' and self.W >= self.target - 200:
            return 0.5
        if s == 'blocked' and self.W >= 0.5 * self.target:
            return 0.0
        if s == 'zero':
            return 0.0
        return 1.0

    def cmd(self, now_ms, on):
        t = now_ms / 1000.0
        if on:
            self.ev.append((t + self.du, True))
        else:
            te = t + self.dd
            if self.ev and self.ev[-1][1] and self.ev[-1][0] >= te:
                self.ev.pop()                   # pulse too short to open
            else:
                self.ev.append((te, False))
            self.ev.sort(key=lambda x: x[0])

    def _seg(self, dt):
        if dt <= 0:
            return
        q = self.Q * self.qscale() if self.open else 0.0
        if self.tau <= 1e-9:
            self.W += q * dt
            self.r = q
            return
        ex = math.exp(-dt / self.tau)
        self.W += q * dt + (self.r - q) * self.tau * (1.0 - ex)
        self.r = q + (self.r - q) * ex

    def advance(self, to_ms):
        to = to_ms / 1000.0
        while self.ev and self.ev[0][0] <= to:
            te, v = self.ev.pop(0)
            self._seg(te - self.t)
            self.t = te
            self.open = v
        self._seg(to - self.t)
        self.t = to

    def quiet(self):
        return (not self.ev) and (not self.open) and self.r < 1e-4


# --------------------------------------------------------------------------------------
# Closed-loop run
# --------------------------------------------------------------------------------------
def run(p, pl, target, seed, max_dur=None, settle_after_s=60, stall_detect=True):
    """One job. Returns result dict. law 'EXISTING' = replica; 'PROPOSED' adds doc-13 C.6
    behaviours (no SETTLING-stage zero, floor pulse, settled TRIM with fallback schedule,
    NEAR_TARGET_NO_PROGRESS) = FUTURE feature, NOT in firmware."""
    rnd = random.Random(seed)
    max_dur = p['max_dur'] if max_dur is None else max_dur
    proposed = p['law'] == 'PROPOSED'
    kp, ki, kd = f32(p['kp']), f32(p['ki']), f32(p['kd'])
    lat_ticks = int(round(pl.lat * 1000 / TICK_MS))
    hist = []
    state, stage = 'WAIT_WEIGHT', 'NONE'
    integral, have_last, last_err = 0.0, False, 0.0
    last_pid = window_since = settled_since = 0
    weight, have_w = 0, False
    relay = False
    on_edges = 0
    on_start = None
    min_on_obs = min_off_obs = None
    off_start = None
    n_short_on = n_short_off = 0
    on_time_ms = 0
    peak_over = 0.0
    outcome, t_done = None, None
    trim_n, trim_until, prev_settled_E, noprog = 0, None, None, 0
    idle_cycles = 0
    dropped_since = None
    cut_pending = False
    w_ck = 0.0
    now = 0
    smallest_micro_on = min(p['micro_on'], p['fine_on'], p['coarse_on'])
    while True:
        pl.advance(now)
        hist.append(pl.W)
        if pl.W - target > peak_over:
            peak_over = pl.W - target
        if now % SAMPLE_MS == 0:
            idx = len(hist) - 1 - lat_ticks
            wm = hist[idx] if idx >= 0 else 0.0
            if pl.sigma > 0:
                wm += rnd.gauss(0.0, pl.sigma)
            weight = int(math.floor(wm + 0.5))
            have_w = True
        cmd = relay
        # ---------------- control_channel ----------------
        if (now - 0) > max_dur:
            outcome, t_done = 'TIMEOUT', now
            cmd = False
        elif have_w:
            E = target - weight
            if state == 'WAIT_WEIGHT':
                state = 'DISPENSING'
                nxt = compute_stage(E, p)
                if proposed and nxt == 'SETTLING':
                    nxt = 'MICRO'
                if nxt != stage:
                    stage, integral, have_last = nxt, 0.0, False
            if state == 'SETTLING':
                cmd = False
                if (now - settled_since) >= p['settle']:
                    over = weight - target
                    if over > p['os']:
                        outcome, t_done = 'OVERWEIGHT', now
                    elif E <= p['tol']:
                        outcome, t_done = 'COMPLETE', now
                    elif proposed:
                        # PROPOSED C.6.4/5: sized TRIM (fallback schedule), progress guard
                        if prev_settled_E is not None and E >= prev_settled_E - 1:
                            noprog += 1
                        else:
                            noprog = 0
                        prev_settled_E = E
                        if noprog >= 2 or trim_n >= 6:
                            outcome, t_done = 'NEAR_TARGET_NO_PROGRESS', now
                        else:
                            ton = min(p.get('trim0', p['micro_on']) * (2 ** trim_n),
                                      p.get('trim_max', p['win'] - p['min_off']))
                            trim_n += 1
                            trim_until = now + ton
                            state = 'TRIM'
                    else:
                        state = 'DISPENSING'
                        nxt = compute_stage(E, p)
                        if nxt != stage:
                            stage, integral, have_last = nxt, 0.0, False
            elif state == 'TRIM':
                cmd = now < trim_until
                if not cmd:
                    state, settled_since = 'SETTLING', now
            elif state == 'DISPENSING':
                eff = E - p['comp']
                if eff <= 0:
                    cmd = False
                    cut_pending = True
                    settled_since = now
                    stage = 'SETTLING'
                    state = 'SETTLING'
                    idle_cycles += 1
                else:
                    want = compute_stage(E, p)
                    if proposed and want == 'SETTLING':
                        want = 'MICRO'
                    if want != stage:
                        stage, integral, have_last = want, 0.0, False
                    dt = now - last_pid
                    if dt == 0:
                        dt = 1
                    if dt > 1000:
                        dt = 1000
                    last_pid = now
                    e = float(eff)
                    # fast path: unsaturated >1 in COARSE: integral frozen (anti-windup), duty 1
                    if stage == 'COARSE' and kp * e > 1.2 and e > 0:
                        out = 1.0
                        last_err, have_last = e, True
                    else:
                        dt_s = f32(dt / 1000.0)
                        d = f32(f32(e - last_err) / dt_s) if have_last else 0.0
                        cand = f32(integral + f32(e * dt_s))
                        cand = max(-200.0, min(200.0, cand))
                        pt = f32(kp * e)
                        it = f32(ki * cand)
                        dtm = f32(kd * d)
                        uns = f32(f32(pt + it) + dtm)
                        if (0.0 <= uns <= 1.0) or (uns > 1.0 and e < 0.0) or (uns < 0.0 and e > 0.0):
                            integral = cand
                        it = f32(ki * integral)
                        out = f32(f32(pt + it) + dtm)
                        if out < 0.0:
                            out = 0.0
                        if out > 1.0:
                            out = 1.0
                        out = stage_clamp(stage, out)
                        last_err, have_last = e, True
                    if now - window_since >= p['win']:
                        window_since = now
                    cmd = window_out(now - window_since, out, p['win'],
                                     stage_min_on(stage, p), p['min_off'],
                                     floor_pulse=(proposed and stage == 'MICRO'))
                    # stall detection (replica bookkeeping only, no effect on the law)
                    if (not cmd) and pl.quiet() and (stage == 'SETTLING' or p['ki'] == 0.0 or
                                                    abs(integral) >= 199.9):
                        if dropped_since is None:
                            dropped_since = now
                        elif now - dropped_since > 10_000:
                            outcome, t_done = 'STALL', now
                    else:
                        dropped_since = None
                    if cmd:
                        idle_cycles = 0
        # replica bookkeeping: no true-weight progress for 120 s => stall (would end as TIMEOUT)
        if stall_detect and outcome is None and now > 0 and now % 120_000 == 0:
            if abs(pl.W - w_ck) < 0.05:
                outcome, t_done = 'STALL', now
            w_ck = pl.W
        # livelock B (C > T): SETTLING<->DISPENSING cycles with no ON, plant quiet
        if outcome is None and idle_cycles >= 4 and pl.quiet() and not cmd:
            outcome, t_done = 'STALL', now
        if outcome is not None:
            cmd = False
        if cmd != relay:
            if cmd:
                on_edges += 1
                on_start = now
                if off_start is not None:
                    gap = now - off_start
                    min_off_obs = gap if min_off_obs is None else min(min_off_obs, gap)
                    if gap < p['min_off'] and state == 'DISPENSING':
                        n_short_off += 1
            else:
                ln = now - on_start
                if not cut_pending and outcome is None:
                    min_on_obs = ln if min_on_obs is None else min(min_on_obs, ln)
                    if ln < smallest_micro_on:
                        n_short_on += 1
                off_start = now
            relay = cmd
            pl.cmd(now, cmd)
        if relay:
            on_time_ms += TICK_MS
        cut_pending = False if state != 'SETTLING' else cut_pending
        if outcome is not None:
            break
        now += TICK_MS
    # let the plant settle with the relay OFF to read the TRUE final weight
    end = now
    steps = int(settle_after_s * 1000 / TICK_MS)
    for _ in range(steps):
        end += TICK_MS
        pl.advance(end)
        if pl.W - target > peak_over:
            peak_over = pl.W - target
    wt = pl.W
    ferr = target - wt
    in_band = (-p['os'] <= ferr <= p['tol']) if outcome == 'COMPLETE' else False
    # judge guard: the firmware decides on ONE noisy integer reading (res/2 + 3 sigma = 1.7 g here)
    in_band_g = (-p['os'] <= ferr <= p['tol'] + 0.5 + 3 * pl.sigma) if outcome == 'COMPLETE' else False
    return dict(outcome=outcome, t_s=t_done / 1000.0, final_err=ferr, in_band=in_band,
                in_band_g=in_band_g,
                over=max(0.0, -ferr), peak_over=max(0.0, peak_over), edges=on_edges,
                min_on=min_on_obs, min_off=min_off_obs, short_on=n_short_on,
                short_off=n_short_off, on_s=on_time_ms / 1000.0, trims=trim_n,
                w_true=wt)


# --------------------------------------------------------------------------------------
# Self-check: replica numbers vs values derived BY HAND from the C source
# --------------------------------------------------------------------------------------
# Hand derivation (kp=0.0025, W=500, stage min_on 80/30/15, comp 15, tol 20), e = E-15:
#   E=500 COARSE e=485 P=1.2125->1.0                      -> ON all window (500)
#   E=101 COARSE e=86  P=0.215 -> floor 0.4 -> 200.5->200
#   E=100 FINE   e=85  P=0.2125 -> 106.25+0.5 -> 106
#   E=30  MICRO  e=15  P=0.0375 -> 18.75+0.5 -> 19
#   E=27  MICRO  e=12  P=0.03   -> 15.0+0.5  -> 15 (>=15: first pulse)
#   E=26  MICRO  e=11  P=0.0275 -> 14.25     -> 14 < 15 -> DROPPED
#   E=21  MICRO  e=6   P=0.015  -> 8         -> DROPPED
#   E=20  stage SETTLING -> duty 0 (dead zone), E=16 same, E=15 -> cut
# kp=0.47: E=101 -> 1.0 ON all; E=100 -> 0.5 -> 250; E=30/26/21 -> 0.15 -> 75; E=20 -> 0
SELFCHECK = [
    (0.0025, 500, 'COARSE', 1.0, 500), (0.0025, 101, 'COARSE', 0.4, 200),
    (0.0025, 100, 'FINE', 0.2125, 106), (0.0025, 30, 'MICRO', 0.0375, 19),
    (0.0025, 27, 'MICRO', 0.03, 15), (0.0025, 26, 'MICRO', 0.0275, 0),
    (0.0025, 21, 'MICRO', 0.015, 0), (0.0025, 20, 'SETTLING', 0.0, 0),
    (0.0025, 16, 'SETTLING', 0.0, 0), (0.0025, 15, 'CUT', 0.0, 0),
    (0.47, 101, 'COARSE', 1.0, 500), (0.47, 100, 'FINE', 0.5, 250),
    (0.47, 30, 'MICRO', 0.15, 75), (0.47, 26, 'MICRO', 0.15, 75),
    (0.47, 21, 'MICRO', 0.15, 75), (0.47, 20, 'SETTLING', 0.0, 0),
]


def selfcheck():
    bad = 0
    print('SELF-CHECK replica vs hand-derived (C source) values')
    print('  kp      E   stage     duty    on_ms | expected stage duty on_ms | ok')
    for kp, E, es, ed, eo in SELFCHECK:
        p = dict(FW_DEFAULT, kp=kp)
        s, d, o = first_tick_duty(E, p)
        ok = (s == es) and abs(d - ed) < 1e-6 and o == eo
        bad += (not ok)
        print('  %-6g %4d %-9s %-7.4f %5d | %-8s %-7.4f %5d | %s' %
              (kp, E, s, d, o, es, ed, eo, 'OK' if ok else 'DIVERGENCE'))
    # window rule: 495 ms on of 500 -> gap 5 < min_off 40 -> whole window ON
    ok = window_out(300, 0.95, 500, 15, 40) is True
    bad += (not ok)
    print('  window rule duty 0.95 (475/500, gap 25<40) -> continuous ON: %s' % ('OK' if ok else 'DIVERGENCE'))
    # e_min: smallest integer e with on_ms>=15 at kp .0025 -> 12 g -> E = 27
    em = e_min_exact(0.0025, 500, 15)
    ok = abs(em - 11.6) < 1e-9
    bad += (not ok)
    print('  e_min exact (min_on-0.5)/(kp*W) = %.3f g (expect 11.600), first pulsing integer e = %d: %s' %
          (em, math.ceil(em), 'OK' if ok else 'DIVERGENCE'))
    ok = abs(0.15 * 13.33 - 2.0) < 0.01
    print('  MICRO ceiling rate 0.15*Q at Q=13.33: %.3f g/s (expect ~2.0): %s' % (0.15 * 13.33, 'OK' if ok else 'DIVERGENCE'))
    print('SELF-CHECK RESULT: %s' % ('PASS' if bad == 0 else 'FAIL (%d divergences)' % bad))
    return bad == 0


# --------------------------------------------------------------------------------------
# Characterisation stand-in and candidate generator (OFFLINE, deterministic)
# --------------------------------------------------------------------------------------
def stat(mean, sd, n):
    k = k_n(n)
    return dict(mean=mean, sd=sd, n=n, upper=mean + k * sd, lower=mean - k * sd)


def synth_char(P, n=6, sigma=0.4, res=1, conf='OK', tau_valid=True):
    """autotune_char_result_t EQUIVALENT built from a HYPOTHETICAL plant with ASSUMED
    estimator scatter/bias (Q sd 5 %, C mean 10 % LOW per doc 14 bias note, sd 15 %+0.3 g,
    startup +0.1 s detect lag, sd 10 %, tau sd 15 %). This is NOT measured data."""
    Ct = P['Q'] * (P['dd'] + P['tau'])
    return dict(
        conf=conf, n_valid=n, res=res, sigma=sigma, edge_excl_ms=300,
        q=stat(P['Q'], 0.05 * P['Q'], n),
        c=stat(0.9 * Ct, 0.15 * Ct + 0.3, n),
        startup=stat(P['du'] + 0.1, 0.1 * P['du'] + 0.02, n),
        latency_mean=P['lat'],
        tau=stat(P['tau'], 0.15 * P['tau'], n) if tau_valid else None)


def ceil_to(x, m):
    return int(math.ceil(x / m - 1e-9) * m)


def generate(ch, mode, alpha=3.0, T_max=20, os_max=100, kC=1.0, lat_margin_s=0.1,
             min_on_floor_ms=600, min_off_ms=40, alpha_min=2.0, n_min=5, se_max=0.25,
             Ts=0.1, tail_k=3.0):
    """mode 'A' = EXISTING control law (C must equal T; no firmware change),
    mode 'B' = law with the PROPOSED doc-13 C.6 fix (future feature, NOT in firmware).
    Returns dict(status, reasons, params, derived). Status CANDIDATE_UNVERIFIED / NO_CANDIDATE
    / REJECTED. Nothing is applied anywhere."""
    out = dict(mode=mode, reasons=[], params=None, d={})
    R = out['reasons']
    # ---- confidence gates (NO_CANDIDATE) ----
    if ch['conf'] != 'OK':
        R.append('NO_CAND:CONFIDENCE_NOT_OK')
    if ch['n_valid'] < n_min:
        R.append('NO_CAND:LOW_N(%d<%d)' % (ch['n_valid'], n_min))
    if ch['q']['mean'] <= 0 or ch['q']['sd'] / ch['q']['mean'] > se_max:
        R.append('NO_CAND:Q_SCATTER_HIGH')
    if ch['tau'] is None:
        R.append('NO_CAND:TAU_INVALID')
    if ch['c']['mean'] <= 0:
        R.append('NO_CAND:C_NOT_POSITIVE')
    if R:
        out['status'] = 'NO_CANDIDATE'
        return out
    Qu, Ql = ch['q']['upper'], max(ch['q']['lower'], 1e-6)
    Du = ch['startup']['upper']
    Lu = ch['latency_mean'] + lat_margin_s
    tu = ch['tau']['upper']
    # ---- minimum ON pulse, window ----
    min_on = ceil_to(max(min_on_floor_ms, (Du + Lu) * 1000.0 + 1.0), TICK_MS)
    W = ceil_to(max(min_on / 0.15, min_on + min_off_ms, 500), 100)
    th0 = Du + Lu + tu + Ts / 2.0
    if mode == 'A':
        # existing law needs Kp(W) >= (min_on-0.5)/W with Kp = 1/(Qu*alpha*(th0+W/2)):
        # widen the window (theta grows by W/2) until it holds; feasible iff Qu*min_on < 2/alpha
        Wa = W
        while Wa <= 10000 and (1.0 / (Qu * alpha * (th0 + Wa / 2000.0))) < (min_on - 0.5) / Wa:
            Wa += 100
        W = Wa if Wa <= 10000 else W
    if W > 10000:
        R.append('REJECT:WINDOW_RANGE(window %d ms > 10000 store limit)' % W)
    # ---- dead time, gain ----
    theta = th0 + W / 2000.0
    Tc = alpha * theta
    if alpha < alpha_min:
        R.append('REJECT:ALPHA<%g (KpQtheta=%.2f)' % (alpha_min, 1.0 / alpha))
    kp = 1.0 / (Qu * Tc)
    x = kp * Qu * theta
    out['d'].update(Qu=Qu, Ql=Ql, Du=Du, Lu=Lu, tau_u=tu, theta=theta, Tc=Tc, kp=kp, x=x,
                    pm_deg=math.degrees(math.pi / 2 - x), gm=math.pi / (2 * x))
    # ---- compensation (what is in C and what is added) ----
    # C_char = W_settled - W_off(latency-corrected OFF time): valve closing + tail, EXCLUDES the
    # known measurement latency and the start-up delay. The controller compares a reading that
    # LAGS the true weight by L, so Q*L is added separately (never inside C_char).
    c_hat = ch['c']['mean'] + kC * ch['c']['sd']
    c_cut_f = c_hat + Qu * Lu
    c_up_tot = ch['c']['upper'] + Qu * Lu
    C_cut = min(10000, max(0, int(math.ceil(c_cut_f))))
    d_s = Qu * min(SAMPLE_MS / 1000.0, 0.15 * W / 1000.0)         # grams between two samples
    q_pulse = Qu * (min_on / 1000.0)                                # grams of one minimum pulse (upper)
    # floor: judge guard (one noisy integer reading: res/2 + 3 sigma) + noise gate (3 sigma + res)
    # + compensation uncertainty (C upper total - C used)
    T_floor = int(math.ceil(6.0 * ch['sigma'] + 1.5 * ch['res'] + (c_up_tot - c_cut_f)))
    out['d'].update(C_cut=C_cut, c_up_tot=c_up_tot, d_s=d_s, q_pulse=q_pulse, T_floor=T_floor,
                    min_on=min_on, W=W)
    # ---- stall conditions ----
    kp_min = (min_on - 0.5) / W
    e_min = e_min_exact(kp, W, min_on)
    out['d'].update(kp_min=kp_min, e_min=e_min)
    if mode == 'A':
        T = max(T_floor, C_cut)
        if T > T_max:
            T = T_max                                  # C=T pinned to the operator limit
            if T < T_floor:
                R.append('REJECT:TOLERANCE_FLOOR(T_max %d < floor %d)' % (T_max, T_floor))
        C = T                                          # K1: only C==T has no dead zone/livelock
        if kp < kp_min:
            R.append('REJECT:MICRO_DROPOUT_STALL(kp %.4f < %.4f; e_min=%.1f g > 1 g; Ki=0)' % (kp, kp_min, e_min))
        need_os = int(math.ceil(max(c_up_tot + d_s - C, q_pulse + 1 - C, 1)))
        if need_os > os_max:
            R.append('REJECT:OVERSHOOT_RISK(need os %d g > operator %d g)' % (need_os, os_max))
        os_set = max(need_os, 10)
    else:
        T = T_max
        C = C_cut
        if T < T_floor:
            R.append('REJECT:TOLERANCE_FLOOR(T %d < floor %d)' % (T, T_floor))
        need_os = int(math.ceil(max(c_up_tot + d_s - C, q_pulse + 1 - T, 1)))
        if need_os > os_max:
            R.append('REJECT:OVERSHOOT_RISK(need os %d g > operator %d g)' % (need_os, os_max))
        os_set = max(need_os, 10)
        trim0 = ceil_to(ch['res'] * 1.5 / Ql * 1000.0 + Du * 1000.0, TICK_MS)
        trim0 = max(trim0, min_on)
        # a trim must never be able to push a settled error E_s>=T+1 below -os:
        # mass of one pulse <= Qu*p  =>  p <= (T+1+os)/Qu   (mass bound, not a time promise)
        trim_max = min(W - min_off_ms, int((T + 1 + os_set) / Qu * 1000.0) // TICK_MS * TICK_MS)
        out['d'].update(trim0=trim0, trim_max=trim_max)
        if trim0 > trim_max:
            R.append('REJECT:TRIM_BELOW_RESOLUTION(first trim %d ms > trim max %d ms)' % (trim0, trim_max))
    if 0.15 * W < min_on:
        R.append('REJECT:MICRO_CEILING(0.15*W<min_on)')
    # ---- settle, advisories ----
    settle = ceil_to(max(1500.0, 1000.0 * (Lu + ch['c']['upper'] / Ql + tail_k * tu)), 100)
    n_res = 5
    np_win = max(int(math.ceil(n_res * ch['res'] / Ql * 1000.0)), int(3 * (tu + Du) * 1000))
    out['d'].update(settle=settle, min_expected_flow_g_min=Ql * 60.0, noprog_window_on_ms=np_win,
                    os_need=need_os)
    out['params'] = dict(FW_DEFAULT, kp=f32(kp), ki=0.0, kd=0.0, tol=T, os=os_set, win=W,
                         min_on=min_on, min_off=min_off_ms, micro_on=min_on, settle=settle,
                         comp=C, law='EXISTING' if mode == 'A' else 'PROPOSED',
                         trim0=out['d'].get('trim0', min_on),
                         trim_max=out['d'].get('trim_max', W - min_off_ms))
    out['status'] = 'REJECTED' if R else 'CANDIDATE_UNVERIFIED'
    return out


# --------------------------------------------------------------------------------------
# HYPOTHETICAL plant sweep (NOT measurements)
# --------------------------------------------------------------------------------------
PLANTS = [   # name: Q g/s, D_up s, D_dn s, tau s, latency s  (D_up - D_dn = 20 ms: a pulse
             # shorter than D_up-D_dn never opens the valve; stress-tested separately in 'robust')
    dict(name='Q13.33', Q=13.33, du=0.15, dd=0.13, tau=0.20, lat=0.30),
    dict(name='Q5', Q=5.0, du=0.20, dd=0.18, tau=0.30, lat=0.30),
    dict(name='Q2', Q=2.0, du=0.25, dd=0.23, tau=0.50, lat=0.30),
    dict(name='Q1', Q=1.0, du=0.40, dd=0.38, tau=1.00, lat=0.30),
    dict(name='Q0.5', Q=0.5, du=0.50, dd=0.48, tau=2.00, lat=0.30),
    dict(name='Q0.3', Q=0.3, du=0.55, dd=0.53, tau=2.50, lat=0.30),
    dict(name='Q0.2', Q=0.2, du=0.60, dd=0.58, tau=3.00, lat=0.30),
]
SIGMA = 0.4                      # g, hypothetical scale noise
TARGETS = [100, 250, 500, 2500, 5000, 7250, 10000, 12000, 15000, 18500, 20000]
SIM_BUDGET_S = 3300.0            # keep inside the 3600 s firmware cap


def mkplant(P, scen='none', target=0, mult=1.0, qmult=1.0):
    return Plant(P['Q'] * qmult, P['du'] * mult, P['dd'] * mult, P['tau'] * mult, P['lat'] * mult,
                 SIGMA, scen, target)


def feasible(P, target, p):
    """Formula pre-check (NOT a promise): predicted approach time of THIS parameter set plus
    its settle time must fit inside the 3600 s firmware cap with 8 % margin."""
    tt = predict_time(target, P['Q'], p['kp'], p['comp'], p)
    return (tt * 1.08 + p['settle'] / 1000.0 + 30.0) <= SIM_BUDGET_S


def sweep_set(P, p, targets=TARGETS, seeds=(1, 2), scen='none', max_dur=None, big_seeds=1):
    rows = []
    for tg in targets:
        if tg < 3 * (p['tol'] + p['comp']) + 10 or not feasible(P, tg, p):
            rows.append((tg, None))
            continue
        res = []
        for sd in seeds[:big_seeds] if tg >= 10000 else seeds:
            res.append(run(dict(p), mkplant(P, scen, tg), tg, sd * 7919 + tg, max_dur=max_dur))
        rows.append((tg, res))
    return rows


def fmt_rows(rows, p):
    print('    target  n  complete/inband  outcomes                 err_mean  err_worst  over_worst(os %d) '
          ' t_mean_s  edges  minON/minOFF ms  shortON  trims' % p['os'])
    for tg, res in rows:
        if res is None:
            print('    %6d  NOT RUN: predicted time exceeds the %d s budget (3600 s cap) or target too small for T/C' % (tg, SIM_BUDGET_S))
            continue
        n = len(res)
        comp = sum(1 for r in res if r['outcome'] == 'COMPLETE')
        inb = sum(1 for r in res if r['in_band'])
        inbg = sum(1 for r in res if r['in_band_g'])
        hist = {}
        for r in res:
            hist[r['outcome']] = hist.get(r['outcome'], 0) + 1
        errs = [r['final_err'] for r in res]
        worst = max(errs, key=lambda v: abs(v))
        ovw = max(r['over'] for r in res)
        tm = sum(r['t_s'] for r in res) / n
        ed = sum(r['edges'] for r in res) / n
        mo = [r['min_on'] for r in res if r['min_on'] is not None]
        mf = [r['min_off'] for r in res if r['min_off'] is not None]
        so = sum(r['short_on'] + r['short_off'] for r in res)
        print('    %6d  %d  %d/%d (band %d, +guard %d)  %-24s %8.1f  %9.1f  %8.1f  %9.0f %6.0f  %s/%s  %d  %.1f' % (
            tg, n, comp, n, inb, inbg, ','.join('%s:%d' % kv for kv in sorted(hist.items())),
            sum(errs) / n, worst, ovw, tm, ed, min(mo) if mo else '-', min(mf) if mf else '-', so,
            sum(r['trims'] for r in res) / n))


def show_candidate(g, label):
    d = g['d']
    print('  %s: status=%s' % (label, g['status']))
    for r in g['reasons']:
        print('      reason: %s' % r)
    if d:
        print('      Qu=%.3f Ql=%.3f Du=%.3fs Lu=%.3fs tau_u=%.3fs theta_u=%.3fs Tc=%.2fs  Kp=%.5f  Kp*Qu*theta=%.3f (PM %.1f deg, GM %.1f)' % (
            d['Qu'], d['Ql'], d['Du'], d['Lu'], d['tau_u'], d['theta'], d['Tc'], d['kp'], d['x'], d['pm_deg'], d['gm']))
        print('      min_on=%d ms window=%d ms C_cut=%d g (c_up_tot %.2f) T_floor=%d  kp_min(e=1 g)=%.4f e_min=%.1f g  settle=%d ms  '
              'min_expected_flow=%.2f g/min  noprog_window_on=%d ms' % (
                  d['min_on'], d['W'], d['C_cut'], d['c_up_tot'], d['T_floor'], d['kp_min'], d['e_min'],
                  d['settle'], d['min_expected_flow_g_min'], d['noprog_window_on_ms']))
        print('      quantum Qu*min_on=%.2f g (mode A feasible only if < 2/alpha=0.67 g)  trim0=%s trim_max=%s ms' % (
            d['q_pulse'], d.get('trim0', '-'), d.get('trim_max', '-')))
    if g.get('params'):
        p = g['params']
        print('      PARAMS(unverified): kp=%.5f ki=0 kd=0 tol=%d os=%d window=%d min_on=%d min_off=%d settle=%d comp=%d law=%s' % (
            p['kp'], p['tol'], p['os'], p['win'], p['min_on'], p['min_off'], p['settle'], p['comp'], p['law']))


def cmd_gen():
    print('== CANDIDATE GENERATOR (synthetic characterisation from HYPOTHETICAL plants; UNVERIFIED) ==')
    for P in PLANTS:
        ch = synth_char(P)
        print('\nPlant %s (Q=%.2f g/s)' % (P['name'], P['Q']))
        show_candidate(generate(ch, 'A'), 'mode A existing law (C==T)')
        show_candidate(generate(ch, 'B'), 'mode B PROPOSED C.6 law')
    print('\n-- NO_CANDIDATE gates --')
    P = PLANTS[2]
    for lab, kw in (('low n=3', dict(n=3)), ('tau invalid', dict(tau_valid=False)),
                    ('confidence LOW', dict(conf='LOW')), ('high Q scatter', None)):
        ch = synth_char(P, **(kw or {}))
        if kw is None:
            ch['q'] = stat(P['Q'], 0.4 * P['Q'], 6)
        g = generate(ch, 'A')
        print('  %-16s -> %s %s' % (lab, g['status'], g['reasons']))
    print('\n-- Tc rule: alpha below 2 rejected (plant Q2) --')
    ch = synth_char(PLANTS[2])
    for a in (0.8, 1.0, 1.5, 2.0, 3.0, 4.0):
        g = generate(ch, 'B', alpha=a)
        print('  alpha=%.1f  Kp=%.5f  Kp*Qu*theta=%.2f  PM=%.1f deg  GM=%.1f  -> %s %s' % (
            a, g['d']['kp'], g['d']['x'], g['d']['pm_deg'], g['d']['gm'], g['status'],
            [r for r in g['reasons'] if 'ALPHA' in r]))
    print('\n-- pulse floor sensitivity (Q13.33 mode B): min_on_floor 600 vs physics-only --')
    ch = synth_char(PLANTS[0])
    for fl in (600, 0):
        g = generate(ch, 'B', min_on_floor_ms=fl)
        d = g['d']
        print('  floor=%4d: min_on=%d window=%d theta=%.2f Kp=%.5f kp_min=%.4f status=%s' % (
            fl, d['min_on'], d['W'], d['theta'], d['kp'], d['kp_min'], g['status']))


def cmd_stallcase():
    print('== BASELINES vs STALL (current firmware defaults; replica; hypothetical plant Q2/Q13.33) ==')
    print('  rest-error scan: which rest errors E_rest are dead-zone stalls for C=15, T=20')
    for E in range(14, 23):
        s, d, o = first_tick_duty(E, FW_DEFAULT)
        print('    E_rest=%d -> stage %s duty %.3f on_ms %d  %s' % (E, s, d, o,
              'STALL (no output, state stays DISPENSING)' if (o == 0 and s != 'CUT') else ''))
    print('  closed-loop, 500 g target, FW defaults (kp .0025 ki .0003 kd .0001 C15 T20), 6 seeds:')
    for P in PLANTS[:4]:
        res = [run(dict(FW_DEFAULT), mkplant(P, 'none', 500), 500, 100 + s) for s in range(6)]
        h = {}
        for r in res:
            h[r['outcome']] = h.get(r['outcome'], 0) + 1
        print('    %-7s %s  final_err(g) %s' % (P['name'], h, [round(r['final_err'], 1) for r in res]))
    print('  (outcome STALL = relay idle, plant quiet, ki*I cannot lift; would end as CHANNEL_TIMEOUT)')
    print('  C>T livelock (C=25, T=20) and C==T, 500 g, plant Q2:')
    for lab, c, t in (('C=25,T=20', 25, 20), ('C=20,T=20', 20, 20), ('C=15,T=20', 15, 20), ('C=0,T=20', 0, 20)):
        res = [run(dict(FW_DEFAULT, comp=c, tol=t), mkplant(PLANTS[2], 'none', 500), 500, 300 + s) for s in range(6)]
        h = {}
        for r in res:
            h[r['outcome']] = h.get(r['outcome'], 0) + 1
        print('    %-10s %s final_err %s' % (lab, h, [round(r['final_err'], 1) for r in res]))


def cmd_sweep(quick=False):
    print('== CLOSED-LOOP SWEEPS: REPLICA SIMULATION, hypothetical plants, not hardware evidence ==')
    print('   noise sigma %.1f g, integer-gram scale, 100 ms samples, 10 ms ticks, latency per plant.' % SIGMA)
    plants = PLANTS[:3] if quick else PLANTS
    for P in plants:
        ch = synth_char(P)
        print('\n### Plant %s: Q=%.2f g/s D_up=%.2f D_dn=%.2f tau=%.2f L=%.2f (HYPOTHETICAL); true C_full=%.2f g' % (
            P['name'], P['Q'], P['du'], P['dd'], P['tau'], P['lat'], P['Q'] * (P['dd'] + P['tau'])))
        sets = [('BASE_FW_defaults', dict(FW_DEFAULT)), ('BASE_SERVER_placeholder', dict(SRV_PLACEHOLDER))]
        for mode in ('A', 'B'):
            g = generate(ch, mode)
            if g['status'] == 'CANDIDATE_UNVERIFIED':
                sets.append(('GEN_%s' % mode, g['params']))
            else:
                print('  GEN_%s not run as a candidate: %s' % (mode, g['reasons']))
                if g.get('params'):
                    sets.append(('GEN_%s_FORCED_(REJECTED)' % mode, g['params']))
        for name, p in sets:
            print('  [%s] kp=%.5g ki=%.4g kd=%.4g win=%d tol=%d os=%d comp=%d micro_on=%d settle=%d law=%s' % (
                name, p['kp'], p['ki'], p['kd'], p['win'], p['tol'], p['os'], p['comp'], p['micro_on'],
                p['settle'], p['law']))
            tgs = [500, 2500, 5000] if quick else TARGETS
            fmt_rows(sweep_set(P, p, targets=tgs), p)


def cmd_robust():
    print('== ROBUSTNESS: true plant vs characterisation bound (REPLICA, hypothetical) ==')
    cases = [(PLANTS[6], 'A', 100), (PLANTS[2], 'B', 500), (PLANTS[0], 'B', 2500)]
    for P, mode, tg in cases:
        ch = synth_char(P)
        g = generate(ch, mode)
        if g['status'] != 'CANDIDATE_UNVERIFIED':
            print('  %s mode %s not a candidate: %s' % (P['name'], mode, g['reasons']))
            continue
        p, d = g['params'], g['d']
        print('\n  %s mode %s target %d: Kp=%.5f assumes Qu=%.3f theta_u=%.2f (x=%.2f)' % (P['name'], mode, tg, p['kp'], d['Qu'], d['theta'], d['x']))
        print('     Qtrue/Q  dyn_mult | x=Kp*Q*theta_true  PM_deg  GM  | outcome  inband  inband+guard  final_err  peak_over  t_s  edges')
        for qm in (0.5, 1.0, d['Qu'] / P['Q'], 1.5):
            for mult in (0.5, 1.0, 1.5, 2.0, 3.0):
                pl = mkplant(P, 'none', tg, mult=mult, qmult=qm)
                th = P['du'] * mult + P['lat'] * mult + P['tau'] * mult + p['win'] / 2000.0 + 0.05
                x = p['kp'] * P['Q'] * qm * th
                r = run(dict(p), pl, tg, 4242 + tg)
                print('     %5.2f    %4.1f   | %6.2f   %6.1f %5.1f | %-9s %-5s %-5s %8.1f %8.1f %7.1f %5d' % (
                    qm, mult, x, math.degrees(math.pi / 2 - x), (math.pi / (2 * x)), r['outcome'], r['in_band'],
                    r['in_band_g'], r['final_err'], r['peak_over'], r['t_s'], r['edges']))


def cmd_pi():
    print('== P vs PI on the REJECTED mode-A set (plant Q2, 500 g and 2500 g), REPLICA, hypothetical ==')
    P = PLANTS[2]
    g = generate(synth_char(P), 'A')
    base = g['params']
    kp, Tc = base['kp'], g['d']['Tc']
    print('   Kp=%.5f Tc=%.1fs window=%d min_on=%d : duty needed for a pulse at e=1 g = %.4f; '
          'integral authority = ki*200' % (kp, Tc, base['win'], base['micro_on'], (base['micro_on'] - 0.5) / base['win']))
    for lab, ki in (('Ki=0', 0.0), ('Ki=Kp/(10Tc)', kp / (10 * Tc)), ('Ki=Kp/Tc', kp / Tc), ('Ki=3Kp/Tc', 3 * kp / Tc)):
        p = dict(base, ki=f32(ki))
        for tg in (500, 2500):
            res = [run(dict(p), mkplant(P, 'none', tg), tg, 900 + s + tg, max_dur=3_600_000) for s in range(3)]
            h = {}
            for r in res:
                h[r['outcome']] = h.get(r['outcome'], 0) + 1
            print('   %-13s ki=%.2e (max I-term %.3f) tgt %5d: %s final_err %s over %s t_s %s' % (
                lab, ki, ki * 200, tg, h, [round(r['final_err'], 1) for r in res],
                [round(r['peak_over'], 1) for r in res], [round(r['t_s']) for r in res]))


def cmd_gain():
    print('== Kp SWEEP on the mode-B set (plant Q2, 500 g): does a large Kp oscillate or overshoot? REPLICA ==')
    P = PLANTS[2]
    g = generate(synth_char(P), 'B')
    base, d = g['params'], g['d']
    print('   rule Kp=%.5f (x=Kp*Qu*theta=%.2f). x_true = Kp*Q*theta_true with Q=2.0, theta=%.2f s' % (base['kp'], d['x'], d['theta']))
    for mult in (0.25, 1, 3, 10, 30, 100, 300):
        p = dict(base, kp=f32(base['kp'] * mult))
        xt = p['kp'] * P['Q'] * d['theta']
        res = [run(dict(p), mkplant(P, 'none', 500), 500, 1500 + s) for s in range(3)]
        print('   Kp x%-5g = %.4f  x_true=%7.2f (pi/2=1.57) -> %s final_err %s peak_over %s t_s %s edges %s' % (
            mult, p['kp'], xt, sorted(set(r['outcome'] for r in res)), [round(r['final_err'], 1) for r in res],
            [round(r['peak_over'], 1) for r in res], [round(r['t_s']) for r in res], [r['edges'] for r in res]))


def cmd_stress():
    print('== STRESS PLANTS (REPLICA, hypothetical): large in-flight, slow-opening valve ==')
    big = dict(name='Q13.33-bigC', Q=13.33, du=0.60, dd=0.58, tau=2.0, lat=0.50)   # C_full ~34 g + Q*L
    slowv = dict(name='Q2-slowopen', Q=2.0, du=0.35, dd=0.25, tau=0.5, lat=0.30)   # D_up-D_dn = 100 ms
    for P, tgs in ((big, (500, 5000)), (slowv, (500,))):
        ch = synth_char(P)
        g = generate(ch, 'B')
        print('\n  %s: Q=%.2f du=%.2f dd=%.2f tau=%.2f L=%.2f ; true full-flow C=%.1f g, + Q*L=%.1f g' % (
            P['name'], P['Q'], P['du'], P['dd'], P['tau'], P['lat'], P['Q'] * (P['dd'] + P['tau']), P['Q'] * P['lat']))
        show_candidate(g, 'mode B')
        sets = [('BASE_FW', dict(FW_DEFAULT)), ('BASE_FW_C=T=20', dict(FW_DEFAULT, comp=20))]
        if g['status'] == 'CANDIDATE_UNVERIFIED':
            sets.append(('GEN_B', g['params']))
            sets.append(('GEN_B_C=0(under-comp)', dict(g['params'], comp=0)))
        for name, p in sets:
            print('   [%s] tol=%d os=%d comp=%d settle=%d micro_on=%d' % (name, p['tol'], p['os'], p['comp'], p['settle'], p['micro_on']))
            fmt_rows(sweep_set(P, p, targets=list(tgs), seeds=(1, 2, 3)), p)


def cmd_faults():
    print('== FAULT SCENARIOS (max_duration 300 s blocked/zero, 900 s halve/surge), REPLICA, hypothetical plants ==')
    cases = [(PLANTS[0], 2500), (PLANTS[2], 500)]
    for P, tg in cases:
        ch = synth_char(P)
        gB = generate(ch, 'B')
        sets = [('BASE_FW', dict(FW_DEFAULT)), ('BASE_SRV', dict(SRV_PLACEHOLDER))]
        if gB['status'] == 'CANDIDATE_UNVERIFIED':
            sets.append(('GEN_B', gB['params']))
        for scen in ('halve', 'surge', 'blocked', 'zero'):
            for name, p in sets:
                md = 300_000 if scen in ('blocked', 'zero') else 900_000
                r = run(dict(p), mkplant(P, scen, tg), tg, 77, max_dur=md, settle_after_s=30,
                        stall_detect=False)
                print('  %-7s tgt %5d %-8s %-8s -> %-14s inband=%-5s final_err=%8.1f peak_over=%7.1f ON_time=%6.1fs t=%.0fs' % (
                    P['name'], tg, scen, name, r['outcome'], r['in_band'], r['final_err'], r['peak_over'], r['on_s'], r['t_s']))


def predict_time(target, Q, kp, comp, p=FW_DEFAULT):
    """Illustrative time to reach the cut (formula, no promise): integral dE/(Q*duty(E)),
    duty = stage-clamped kp*(E-C). Ignores pulse quantisation, delays and trims."""
    t, E = 0.0, float(target)
    while E - comp > 1.0 and E > p['tol']:       # stops at the tolerance edge (dead zone below)
        st = compute_stage(int(math.ceil(E)), p)
        d = stage_clamp(st, min(1.0, kp * (E - comp)))
        if d <= 0:
            return float('inf')
        step = max(1.0, (E - comp) * 0.001)
        t += step / (Q * d)
        E -= step
    return t


def cmd_times():
    print('== ILLUSTRATIVE bulk-fill / approach time formulas (NOT a promise; Q is a sweep value) ==')
    print('   t ~ integral dE / (Q*duty(E)), duty=clamp_stage(Kp*(E-C)); MICRO ceiling => approach >= dist/(0.15*Q)')
    qs = [0.2, 0.5, 1, 2, 5, 13.33]
    for tg in (500, 2500, 5000, 10000, 20000):
        row = []
        for q in qs:
            tt = predict_time(tg, q, 0.0025, 15)
            row.append('%9.0f' % tt)
        print('   target %5d g, FW kp .0025 C15, time s for Q = %s: %s' % (tg, qs, ' '.join(row)))
    print('   MICRO approach 30 g -> 15 g needs >= %s s at Q=%s (=15/(0.15*Q))' % (
        [round(15 / (0.15 * q), 1) for q in qs], qs))
    print('   Cap check: firmware PROFILE cap 3600 s => at duty 1 the largest reachable target = 3600*Q g:',
          [round(3600 * q) for q in qs])


def cmd_edges():
    print('== e_min table: smallest error that produces a pulse, e_min=(min_on-0.5)/(kp*W) ==')
    print('   stage floors: FINE 0.1*W, COARSE 0.4*W (pulse always >= min_on if floor*W>=min_on); MICRO floor 0')
    for kp, W, mo in ((0.0025, 500, 15), (0.0025, 500, 40), (0.47, 1000, 15), (0.0068, 4600, 680),
                      (0.167, 8100, 1210), (0.01, 500, 15), (0.03, 500, 15)):
        em = e_min_exact(kp, W, mo)
        print('   kp=%-7g W=%-5d min_on=%-5d e_min=%8.2f g -> first pulsing rest error E>= C+%d g;  0.15*W=%d ms %s min_on' % (
            kp, W, mo, em, math.ceil(em), 0.15 * W, '>=' if 0.15 * W >= mo else '< (MICRO CAN NEVER PULSE)'))


def main(argv):
    cmd = argv[1] if len(argv) > 1 else 'all'
    quick = '--quick' in argv
    ok = selfcheck()
    if not ok:
        print('ABORT: replica diverges from the hand-derived C values')
        return 2
    if cmd in ('edges', 'all'):
        cmd_edges()
    if cmd in ('times', 'all'):
        cmd_times()
    if cmd in ('gen', 'all'):
        cmd_gen()
    if cmd in ('stallcase', 'all'):
        cmd_stallcase()
    if cmd in ('sweep', 'all'):
        cmd_sweep(quick)
    if cmd in ('robust', 'all'):
        cmd_robust()
    if cmd in ('faults', 'all'):
        cmd_faults()
    if cmd in ('pi', 'all'):
        cmd_pi()
    if cmd in ('gain', 'all'):
        cmd_gain()
    if cmd in ('stress', 'all'):
        cmd_stress()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
