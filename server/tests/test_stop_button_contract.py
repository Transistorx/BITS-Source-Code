"""Static contract checks for the per-channel STOP button (queue.js)."""
from pathlib import Path

JS_DIR = Path(__file__).resolve().parents[1] / "app" / "static" / "js"


def _queue_js() -> str:
    return (JS_DIR / "queue.js").read_text(encoding="utf-8")


def test_stop_not_disabled_when_offline():
    js = _queue_js()
    assert "var stopDisabled = isInflight(id, 'PUMP_STOP');" in js
    assert 'stopBtn.disabled = false;' in js
    assert "stopDisabled = !online" not in js
    assert 'PUMP_STOP"]\').disabled = !online' not in js


def test_offline_note_text_present():
    js = _queue_js()
    assert ("Controller offline - STOP is queued, not delivered. "
            "Use the hardwired emergency stop if a pump is running.") in js
    assert "not the emergency stop" in js


def test_stop_command_tracked_and_pending_cleared_on_failure():
    js = _queue_js()
    assert "trackPumpCommand" in js
    assert "'/queue/control/' + encodeURIComponent(commandId)" in js
    assert "delete stopStatus[channel];" in js
    assert "API key required" in js
    assert "error.status" in js
    assert "failure.status = res.status" in (JS_DIR / "common.js").read_text(encoding="utf-8")


def test_start_interlocks_unchanged():
    js = _queue_js()
    # L4: only ever more restrictive - FAULT/ERROR state words now also disable START.
    assert ("var startDisabled = !online || hasJob || Boolean(fault) || isEstop || otherRunning || faultState ||\n"
            "      isInflight(id, 'PUMP_START');") in js
    assert "var faultState = word === 'FAULT';" in js
    assert ("startBtn.disabled = !online || !!channel.active_job_id ||\n"
            "        !!(channel.fault || channel.error) || state === 'EMERGENCY STOP' || state === 'ESTOP' || otherRunning ||\n"
            "        state === 'FAULT' || state === 'ERROR';") in js


def _pump_note_body() -> str:
    return _queue_js().split("function pumpNote", 1)[1].split("async function trackPumpCommand", 1)[0]


def test_stop_status_unknown_checked_before_in_progress_states():
    body = _pump_note_body()
    unknown = body.index("p.cmdTimedOut")
    assert "STOP status unknown - use the hardwired emergency stop if a pump is running." in body
    assert body.index("'FAILED'") < unknown
    assert body.index("'APPLIED'") < unknown
    for later in ("'DELIVERED'", "'PENDING'", "STOP_OFFLINE"):
        assert unknown < body.index(later), later
    assert "STOP STATUS UNKNOWN" in _queue_js()


def test_pump_state_keeps_pending_stop_entry():
    pump = _queue_js().split("function pumpState", 1)[1].split("var STOP_OFFLINE_NOTE", 1)[0]
    stop_branch = pump.split("if (stop) {", 1)[1]
    assert "delete " not in stop_branch
    assert "stopUnknown(stop)" in stop_branch
    assert "Date.now() - p.ts > STOP_UNKNOWN_MS" in _queue_js().split("function stopUnknown", 1)[1].split("}", 1)[0]
    assert "STOP_UNKNOWN_MS = 30000" in _queue_js()


def test_relay_wording_is_commanded_not_physical():
    js = _queue_js()
    assert "RELAY OFF (commanded)" in js and "RELAY ON (commanded)" in js
    assert "PUMP STOPPED" not in js and "PUMP RUNNING" not in js
    assert ("Controller accepted STOP and commanded the relay off. "
            "Confirm the pump has physically stopped.") in js


def test_offline_delivered_note_and_no_premature_not_delivered():
    body = _pump_note_body()
    assert ("STOP delivered before the controller went offline; outcome unknown. "
            "Use the hardwired emergency stop if a pump is running.") in _queue_js()
    assert "!online && p.cmdState === 'PENDING'" in body
    assert "!online && delivered" in body
    assert body.index("!online && delivered") < body.index("!online && p.cmdState === 'PENDING'")


def test_live_update_keeps_in_flight_buttons_disabled():
    js = _queue_js()
    assert "startBtn.classList.contains('is-loading') || isInflight(id, 'PUMP_START')) startBtn.disabled = true" in js
    assert "stopBtn.classList.contains('is-loading') || isInflight(id, 'PUMP_STOP')) stopBtn.disabled = true" in js
    assert "!!(channel.fault || channel.error)" in js


# ---- second fix loop: time-based unknown, timeouts, in-flight, separate STOP status ----

def test_time_based_unknown_in_note_and_state():
    js = _queue_js()
    assert "STOP_UNKNOWN_MS = 30000" in js
    assert "Date.now() - p.ts > STOP_UNKNOWN_MS" in _pump_note_body()
    assert "STOP request timed out - use the hardwired emergency stop" in _pump_note_body()
    state = js.split("function pumpState", 1)[1].split("function stopUnknown", 1)[0]
    assert "stopUnknown(stop)" in state


def test_request_timeouts_present():
    js = _queue_js()
    common = (JS_DIR / "common.js").read_text(encoding="utf-8")
    assert "REQUEST_TIMEOUT_MS" in js
    assert "'/queue/control/' + encodeURIComponent(commandId), REQUEST_TIMEOUT_MS" in js
    assert "new AbortController()" in common
    assert "async function apiGet(path, timeoutMs)" in common
    assert "options.timeoutMs" in common
    assert "e.timeout = true" in common and "timeout.timeout = true" in common


def test_interlocks_reapplied_after_busy_released():
    js = _queue_js()
    assert "function reapplyInterlocks" in js
    assert "if (liveController) applyLiveController(liveController);\n    else if (lastRender) renderChannels(lastRender.channels, lastRender.online);" in js
    assert js.count("T.setButtonBusy(button, false);\n      reapplyInterlocks();") == 3


def test_inflight_survives_redraw_and_stop_status_is_separate():
    js = _queue_js()
    render = js.split("function renderChannels", 1)[1].split("function applyLiveController", 1)[0]
    assert "isInflight(id, 'PUMP_START')" in render and "isInflight(id, 'PUMP_STOP')" in render
    card = js.split("function channelCard", 1)[1].split("function metric", 1)[0]
    assert "isInflight(id, 'PUMP_START')" in card and "isInflight(id, 'PUMP_STOP')" in card
    assert "var stopStatus = {};" in js and "var inflight = {};" in js
    assert "stopStatus[channel] = entry" in js and "pendingPump[channel] = entry" in js


def test_start_disabled_for_fault_state_word():
    js = _queue_js()
    assert "state === 'FAULT' || state === 'ERROR'" in js
    assert "var faultState = word === 'FAULT';" in js


# ---- behavior tests (node vm); skipped when node is unavailable ----

import json
import shutil
import subprocess

import pytest

_NODE = shutil.which("node")

_HARNESS = r"""
const fs = require('fs'), vm = require('vm');
const dir = process.argv[2];
function load(file, sandbox, patch) {
  let src = fs.readFileSync(dir + '/' + file, 'utf8');
  if (patch) src = patch(src);
  vm.runInContext(src, sandbox, { filename: file });
}
const out = {};
const clock = { now: 1000000 };
const RealDate = Date;
function FakeDate() {}
FakeDate.now = () => clock.now;
const sandbox = vm.createContext({
  console, setTimeout, clearTimeout, AbortController, Date: FakeDate, JSON, Promise, Error, Object, Array, Number, String, Boolean, Math,
  document: { addEventListener(type, fn) { if (type === 'click') sandbox.__click = fn; }, getElementById() { return null; }, hidden: false },
});
const toasts = [];
sandbox.window = sandbox;
sandbox.window.addEventListener = () => {};
// common.js: apiWrite / apiGet timeouts with a fetch that hangs until aborted.
sandbox.fetch = (url, init) => new Promise((_, reject) => {
  if (init && init.signal) init.signal.addEventListener('abort', () => { const e = new Error('aborted'); e.name = 'AbortError'; reject(e); });
});
load('common.js', sandbox);
const T = sandbox.Telemetry;
(async () => {
  try { await T.apiWrite('/x', { method: 'POST', timeoutMs: 20 }); out.write = 'resolved'; }
  catch (e) { out.write = e.timeout === true; }
  try { await T.apiGet('/x', 20); out.get = 'resolved'; }
  catch (e) { out.get = e.timeout === true; }
  // queue.js: expose internals.
  const stub = Object.assign({}, T, { stateWord: s => String(s).toUpperCase(), badge: s => s, fmtWeightDisplay: () => ({ valid: true, value: '1', note: '' }), fmtDurationMs: () => '0', toast: (m) => { toasts.push(String(m)); } });
  sandbox.Telemetry = stub; sandbox.window.Telemetry = stub;
  load('queue.js', sandbox, s => s.replace(/\}\)\(\);\s*$/, 'window.__t = { pumpNote, pumpState, channelCard, stopStatus, pendingPump, inflight };\n})();'));
  const t = sandbox.__t;
  const ch = (extra) => Object.assign({ channel_id: 'CH1', state: 'IDLE', relay_on: false }, extra || {});
  const start = t.channelCard; 
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now };
  out.fresh = t.pumpNote('CH1', true);
  clock.now += 31000;
  out.aged_online = t.pumpNote('CH1', true);
  out.aged_offline = t.pumpNote('CH1', false);
  out.aged_relay_on = t.pumpState(ch({ relay_on: true }), true).word;
  out.aged_relay_off = t.pumpState(ch(), true).word;
  t.stopStatus.CH1.cmdState = 'APPLIED';
  out.applied = t.pumpNote('CH1', true).slice(0, 24);
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now, requestTimedOut: true };
  out.req_timeout = t.pumpNote('CH1', true);
  // separate START does not hide STOP status
  t.pendingPump.CH1 = { action: 'PUMP_START', ts: clock.now };
  out.stop_kept = t.pumpNote('CH1', true);
  delete t.pendingPump.CH1;
  // card disabled flags
  const startDis = (html) => /data-control="PUMP_START"[^>]*disabled/.test(html);
  const stopDis = (html) => /data-control="PUMP_STOP"[^>]*disabled/.test(html);
  out.fault_state = startDis(t.channelCard(ch({ state: 'FAULT' }), true, []));
  out.error_state = startDis(t.channelCard(ch({ state: 'ERROR' }), true, []));
  out.idle_ok = startDis(t.channelCard(ch(), true, []));
  t.inflight.CH1 = { PUMP_START: true, PUMP_STOP: true };
  out.inflight_start = startDis(t.channelCard(ch(), true, []));
  out.inflight_stop = stopDis(t.channelCard(ch(), true, []));
  delete t.inflight.CH1;
  out.stop_offline_enabled = stopDis(t.channelCard(ch(), false, []));
  // F3: offline + STOP POST still in flight (no cmdState) shows the e-stop hint.
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now };
  out.offline_inflight_note = t.pumpNote('CH1', false);
  out.online_inflight_note = t.pumpNote('CH1', true);
  // F4: unknown/failed STOP wins the badge over a pending START.
  t.pendingPump.CH1 = { action: 'PUMP_START', ts: clock.now };
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now, requestTimedOut: true };
  out.badge_unknown = t.pumpState(ch(), true).word;
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now, cmdState: 'FAILED' };
  out.badge_failed = t.pumpState(ch(), true).word;
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now, cmdState: 'PENDING' };
  out.badge_fresh_stop = t.pumpState(ch(), true).word;
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now, cmdState: 'APPLIED' };
  out.badge_applied_stop = t.pumpState(ch(), true).word;
  delete t.pendingPump.CH1; delete t.stopStatus.CH1;

  // ---- drive real click -> runAction -> apiWrite with a fake clock and a controllable fetch ----
  const timers = []; let tid = 0;
  sandbox.setTimeout = (fn, ms) => { const id = ++tid; timers.push({ id, at: clock.now + ms, fn }); return id; };
  sandbox.clearTimeout = (id) => { const i = timers.findIndex(x => x.id === id); if (i >= 0) timers.splice(i, 1); };
  const advance = (ms) => { clock.now += ms; timers.filter(x => x.at <= clock.now).forEach(x => { sandbox.clearTimeout(x.id); x.fn(); }); };
  const tick = () => new Promise(r => setImmediate(r));
  const calls = []; let mode = 'ok';
  const abortable = (init) => new Promise((_, reject) => {
    if (init && init.signal) init.signal.addEventListener('abort', () => { const e = new Error('aborted'); e.name = 'AbortError'; reject(e); });
  });
  sandbox.fetch = (url, init) => {
    const method = (init && init.method) || 'GET';
    calls.push({ url, method, signal: Boolean(init && init.signal) });
    if (method !== 'POST') return abortable(init);   // every GET hangs until aborted
    if (mode === 'hang') return abortable(init);
    if (mode === 'neterr') return Promise.reject(new TypeError('failed to fetch'));
    if (mode === 'http500') return Promise.resolve({ ok: false, status: 500, json: async () => ({ detail: 'boom' }) });
    return Promise.resolve({ ok: true, status: 200, json: async () => ({ command_id: 7, state: 'PENDING' }) });
  };
  const click = (action) => {
    const button = { dataset: { control: action, channel: 'CH1' }, classList: { add() {}, remove() {} }, textContent: action, disabled: false };
    const promise = sandbox.__click({ target: { closest: (sel) => (sel === '[data-control]' ? button : null) } });
    return { button, promise };
  };
  const reset = () => { calls.length = 0; toasts.length = 0; delete t.stopStatus.CH1; delete t.pendingPump.CH1; delete t.inflight.CH1; };

  // F1: STOP POST succeeds, follow-up GETs hang: inflight and busy are released at once.
  reset(); mode = 'ok';
  let r = click('PUMP_STOP');
  await r.promise;
  out.hung_refresh_inflight = Boolean(t.inflight.CH1 && t.inflight.CH1.PUMP_STOP);
  out.hung_refresh_button_disabled = r.button.disabled;
  out.refresh_gets = calls.filter(c => c.method === 'GET' && /\/queue$/.test(c.url));
  out.refresh_gets_all_timed = out.refresh_gets.length > 0 && out.refresh_gets.every(c => c.signal);
  out.stop_post_signal = calls.find(c => c.method === 'POST').signal;
  out.stop_cmd_id = t.stopStatus.CH1 && t.stopStatus.CH1.commandId;

  // requestTimedOut set by the real apiWrite timeout path (hung STOP POST, fake clock).
  reset(); mode = 'hang';
  r = click('PUMP_STOP');
  await tick();
  out.timeout_inflight_before = Boolean(t.inflight.CH1 && t.inflight.CH1.PUMP_STOP);
  advance(10000);
  await r.promise;
  out.req_timed_out_flag = Boolean(t.stopStatus.CH1 && t.stopStatus.CH1.requestTimedOut);
  out.req_timed_out_note = t.pumpNote('CH1', true);
  out.req_timed_out_badge = t.pumpState(ch(), true).word;
  out.req_timed_out_inflight = Boolean(t.inflight.CH1 && t.inflight.CH1.PUMP_STOP);
  out.req_timed_out_toast = toasts.join('|');

  // F2: START has no abort timeout; a hung START stays pending however long we wait.
  reset(); mode = 'hang';
  r = click('PUMP_START');
  await tick();
  advance(120000);
  await tick();
  out.start_signal = calls.find(c => c.method === 'POST').signal;
  out.start_still_inflight = Boolean(t.inflight.CH1 && t.inflight.CH1.PUMP_START);
  out.start_no_toast = toasts.length === 0;
  out.start_hang_badge = t.pumpState(ch(), true).word;
  out.start_hang_note = t.pumpNote('CH1', true);

  // F2: START with no HTTP answer reports unknown, never "failed", and keeps a marker.
  reset(); mode = 'neterr';
  r = click('PUMP_START');
  await r.promise;
  out.start_net_toast = toasts.join('|');
  out.start_net_marker = Boolean(t.pendingPump.CH1 && t.pendingPump.CH1.outcomeUnknown);
  advance(10000);
  out.start_net_badge = t.pumpState(ch(), true).word;
  out.start_net_badge_relay_on = t.pumpState(ch({ relay_on: true }), true).word;

  // L1: unknown START expires after START_UNKNOWN_MS, falls back to relay state + note.
  reset(); mode = 'neterr';
  r = click('PUMP_START');
  await r.promise;
  advance(16000);
  out.unk_expired_badge = t.pumpState(ch(), true).word;
  out.unk_expired_note = t.pumpNote('CH1', true);
  out.unk_expired_marker = Boolean(t.pendingPump.CH1);
  // L1: a STOP that reaches APPLIED clears it.
  reset(); mode = 'neterr';
  r = click('PUMP_START');
  await r.promise;
  t.stopStatus.CH1 = { action: 'PUMP_STOP', ts: clock.now + 1, cmdState: 'APPLIED' };
  out.unk_stop_badge = t.pumpState(ch(), true).word;
  out.unk_stop_marker = Boolean(t.pendingPump.CH1);

  // L2: STOP network error (no HTTP status) -> unknown status with the e-stop hint.
  reset(); mode = 'neterr';
  r = click('PUMP_STOP');
  await r.promise;
  out.stop_net_entry = Boolean(t.stopStatus.CH1);
  out.stop_net_note = t.pumpNote('CH1', true);
  out.stop_net_badge = t.pumpState(ch(), true).word;
  reset(); mode = 'http500';
  r = click('PUMP_STOP');
  await r.promise;
  out.stop_500_entry = Boolean(t.stopStatus.CH1);
  out.stop_500_toast = toasts.join('|');

  // server errors stay failures with the HTTP status
  reset(); mode = 'http500';
  r = click('PUMP_START');
  await r.promise;
  out.start_500_toast = toasts.join('|');
  out.start_500_marker = Boolean(t.pendingPump.CH1);
  console.log(JSON.stringify(out));
})();
"""


@pytest.mark.skipif(_NODE is None, reason="node not available")
def test_behavior_under_node(tmp_path):
    script = tmp_path / "harness.js"
    script.write_text(_HARNESS, encoding="utf-8")
    run = subprocess.run([_NODE, str(script), str(JS_DIR)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    out = json.loads(run.stdout.strip().splitlines()[-1])
    assert out["write"] is True and out["get"] is True
    assert out["fresh"] == "Sending STOP..."
    unknown = "STOP status unknown - use the hardwired emergency stop if a pump is running."
    assert out["aged_online"] == unknown and out["aged_offline"] == unknown
    assert out["aged_relay_on"] == "STOP STATUS UNKNOWN" and out["aged_relay_off"] == "STOP STATUS UNKNOWN"
    assert out["applied"].startswith("Controller accepted STOP")
    assert out["req_timeout"].startswith("STOP request timed out")
    assert out["stop_kept"].startswith("STOP request timed out")
    assert out["fault_state"] and out["error_state"] and not out["idle_ok"]
    assert out["inflight_start"] and out["inflight_stop"] and not out["stop_offline_enabled"]
    # F3
    assert out["offline_inflight_note"].startswith("Controller offline - STOP is queued, not delivered.")
    assert out["online_inflight_note"] == "Sending STOP..."
    # F4
    assert out["badge_unknown"] == "STOP STATUS UNKNOWN" and out["badge_failed"] == "STOP FAILED"
    assert out["badge_fresh_stop"] == "COMMAND PENDING" and out["badge_applied_stop"] == "COMMAND PENDING"
    # F1: hung follow-up refresh does not keep STOP disabled; GETs are time-limited
    assert out["hung_refresh_inflight"] is False and out["hung_refresh_button_disabled"] is False
    assert out["refresh_gets_all_timed"] is True and out["stop_post_signal"] is True
    assert out["stop_cmd_id"] == 7
    # STOP timeout driven through the real apiWrite + fake clock (flag not set by hand)
    assert out["timeout_inflight_before"] is True
    assert out["req_timed_out_flag"] is True and out["req_timed_out_inflight"] is False
    assert out["req_timed_out_note"].startswith("STOP request timed out")
    assert out["req_timed_out_badge"] == "STOP STATUS UNKNOWN"
    assert "request timed out" in out["req_timed_out_toast"]
    # F2
    assert out["start_signal"] is False and out["start_still_inflight"] is True and out["start_no_toast"] is True
    assert "START outcome unknown - it may still be delivered; check the controller before retrying" in out["start_net_toast"]
    assert "failed" not in out["start_net_toast"]
    assert out["start_net_marker"] is True
    assert out["start_net_badge"] == "START OUTCOME UNKNOWN" and out["start_net_badge_relay_on"] == "RELAY ON (commanded)"
    assert "START failed (HTTP 500)" in out["start_500_toast"] and out["start_500_marker"] is False
    # M1: hung START never shows COMMAND FAILED
    assert out["start_hang_badge"] == "SENDING START... / OUTCOME UNKNOWN"
    assert "FAILED" not in out["start_hang_badge"]
    # L1
    assert out["unk_expired_badge"] == "RELAY OFF (commanded)" and out["unk_expired_marker"] is False
    assert out["unk_expired_note"] == "Earlier START outcome expired; verify the pump state at the machine."
    assert out["unk_stop_badge"] == "RELAY OFF (commanded)" and out["unk_stop_marker"] is False
    # L2
    assert out["stop_net_entry"] is True and out["stop_net_badge"] == "STOP STATUS UNKNOWN"
    assert out["stop_net_note"] == "STOP request failed (network) - the server may still have queued it - use the hardwired emergency stop if a pump is running."
    assert out["stop_500_entry"] is False and "STOP failed (HTTP 500)" in out["stop_500_toast"]


def test_start_resend_confirm_guard():
    js = _queue_js()
    assert "pendingPump[channel].outcomeUnknown" in js
    assert "typeof window.confirm === 'function'" in js
    assert "A previous START may still be delivered. Send another START?" in js
    assert "START_UNKNOWN_MS = 15000" in js


def test_third_fix_loop_string_contracts():
    js = _queue_js()
    run = js.split("async function runAction", 1)[1].split("function jobTarget", 1)[0]
    # F1: settle before the (non-awaited) refresh; refresh GETs time-limited
    assert "await refresh" not in run
    assert run.index("settle();\n      refresh(!!row).catch(") > run.index("T.toast(success")
    refresh = js.split("async function refresh", 1)[1].split("function markPosted", 1)[0]
    for path in ("'/queue'", "'/device/status'", "'/materials'"):
        assert "T.apiGet(" + path + ", REQUEST_TIMEOUT_MS)" in refresh
    # F2: only STOP carries a timeout
    assert "flight && flight.action === 'PUMP_STOP' ? REQUEST_TIMEOUT_MS : 0" in run
    assert "flight ? REQUEST_TIMEOUT_MS" not in js
    assert "START outcome unknown - it may still be delivered; check the controller before retrying" in js
    assert "outcomeUnknown" in js and "START OUTCOME UNKNOWN" in js
    # F3: offline check precedes the sending text
    note = _pump_note_body()
    assert note.index("if (!online) return STOP_OFFLINE_NOTE;") < note.index("'Sending STOP...'")
    # F4: failed/unknown STOP evaluated before the pending START branch
    state = js.split("function pumpState", 1)[1].split("var STOP_OFFLINE_NOTE", 1)[0]
    assert state.index("stopUnknown(stop)") < state.index("if (pending) {")