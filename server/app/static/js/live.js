/* ============================================================
   Live monitor (/live) — sender weights and ZERO/TARE over the live WebSocket (live_ws.js);
   controller panel from the live SSE stream with a poll fallback; DB poll for run samples.
   Requires: common.js (window.Telemetry), live_ws.js, Chart.js.
   ============================================================ */
(function () {
  'use strict';

  var T = window.Telemetry;
  var POLL_MS = 1000;

  var root = null;
  var chart = null;
  var chartRunId = null;
  var layoutBuilt = false;
  var emptyShown = false;
  var inflight = false;
  var pollTimer = null;
  var failCount = 0;
  var liveSnapshot = null;
  var stopLive = null;

  var STALE_MS = 1000;
  var OFFLINE_MS = 3000;
  var link = 'connecting';
  var broker = 'UNKNOWN';
  var senders = {};
  var rows = {};
  var inflightCmd = {};
  var cmdMsg = {};
  var wsClient = null;
  var wsRaf = null;
  var wsTimer = null;
  var rowsHost = null;
  var wsFailures = 0;
  var wsNote = '';

  var LAYOUT =
    '<div class="live-grid">' +
      '<div class="live-panel">' +
        '<div id="live-mode" class="live-mode not-live" role="status" aria-live="polite">—</div>' +
        '<div class="live-weight-big" aria-label="Current weight">' +
          '<span id="live-weight">—</span> <span class="live-weight-unit">kg</span>' +
        '</div>' +
        '<div class="live-rows">' +
          '<div><span class="stat-label">Target</span><br><span class="stat-value" id="live-target">—</span></div>' +
          '<div><span class="stat-label">Error</span><br><span class="stat-value" id="live-error">—</span></div>' +
          '<div><span class="stat-label">PID output</span><br><span class="stat-value" id="live-pid">—</span></div>' +
          '<div><span class="stat-label">Controller state</span><br><span id="live-state">—</span></div>' +
          '<div><span class="stat-label">Run</span><br><span class="stat-value mono" id="live-run">—</span></div>' +
          '<div><span class="stat-label">Status</span><br><span id="live-status">—</span></div>' +
          '<div><span class="stat-label">Test #</span><br><span class="stat-value" id="live-test">—</span></div>' +
          '<div><span class="stat-label">Last sample (local)</span><br><span class="stat-value" id="live-time">—</span></div>' +
        '</div>' +
        '<div class="relay-lamps" role="group" aria-label="Relay states">' +
          '<div class="lamp" id="lamp-r1">' +
            '<div class="lamp-light" aria-hidden="true"></div>' +
            '<div class="lamp-label">Relay 1 · Coarse</div>' +
            '<div class="lamp-state" id="lamp-r1-state">—</div>' +
          '</div>' +
          '<div class="lamp" id="lamp-r2">' +
            '<div class="lamp-light" aria-hidden="true"></div>' +
            '<div class="lamp-label">Relay 2 · Fine</div>' +
            '<div class="lamp-state" id="lamp-r2-state">—</div>' +
          '</div>' +
        '</div>' +
        '<div id="live-conn" class="small muted mt-3"></div>' +
      '</div>' +
      '<div class="live-panel">' +
        '<h3 id="chart-title">Weight vs Time</h3>' +
        '<div class="live-chart-box">' +
          '<canvas id="live-chart" role="img" aria-label="Live weight vs time chart"></canvas>' +
          '<div class="chart-placeholder" id="live-chart-ph">Waiting for samples…</div>' +
        '</div>' +
      '</div>' +
    '</div>' +
    '<div class="live-panel mt-3" id="ws-panel">' +
      '<h3>Sender channels</h3>' +
      '<div id="ws-status" class="small muted" role="status" aria-live="polite"></div>' +
      '<div id="ws-senders"></div>' +
    '</div>';

  function ensureLayout() {
    if (layoutBuilt) { return true; }
    if (!root) { return false; }
    root.innerHTML = LAYOUT;
    layoutBuilt = true;
    emptyShown = false; /* leaving the empty state */
    return true;
  }

  function setLamp(id, stateId, on) {
    var lamp = T.el(id);
    var txt = T.el(stateId);
    if (lamp) { lamp.classList.toggle('on', !!on); }
    if (txt) { txt.textContent = on ? 'ON' : 'OFF'; }
  }

  /* ---------- chart ---------- */

  function updateChart(run, samples) {
    var canvas = T.el('live-chart');
    var ph = T.el('live-chart-ph');
    if (!canvas || typeof window.Chart === 'undefined') { return; }

    if (!samples || samples.length === 0) {
      chart = T.destroyChart(chart);
      chartRunId = null;
      if (ph) { ph.style.display = ''; ph.textContent = 'No samples yet for this run.'; }
      return;
    }
    if (ph) { ph.style.display = 'none'; }

    var runId = run ? run.run_id : null;
    if (chart && chartRunId !== runId) {
      chart = T.destroyChart(chart); /* run switched: rebuild cleanly */
    }

    if (!chart) {
      chart = T.createWeightChart(canvas, samples, run ? run.target_g : null,
        { fill: true, legend: true });
      chartRunId = runId;
    } else {
      /* replace whole dataset in place — no leaks, no re-create */
      var pts = T.weightPoints(samples);
      var targetKg = T.gToKg(run ? run.target_g : null);
      chart.data.datasets[0].data = pts;
      chart.data.datasets[0].pointRadius = pts.length > 150 ? 0 : 2;
      if (targetKg !== null && chart.data.datasets.length > 1) {
        chart.data.datasets[1].data = [
          { x: pts.length ? pts[0].x : 0, y: targetKg },
          { x: pts.length ? pts[pts.length - 1].x : 1, y: targetKg }
        ];
      }
      chart.update('none');
    }
  }

  /* ---------- render one poll result ---------- */

  function render(data) {
    if (!ensureLayout()) { return; }

    var run = data.active_run || null;
    var last = data.last_sample || null;

    /* LIVE vs last completed run */
    var mode = T.el('live-mode');
    if (data.is_live) {
      mode.className = 'live-mode is-live';
      mode.textContent = 'LIVE — run in progress';
    } else if (run) {
      mode.className = 'live-mode not-live';
      mode.textContent = 'Not live — showing last completed run';
    } else {
      mode.className = 'live-mode not-live';
      mode.textContent = 'Idle — no run recorded yet';
    }

    /* big weight + numbers from the newest sample (fall back to run card) */
    var weightG = last ? last.weight_g : (run ? run.final_weight_g : null);
    var targetG = last ? last.target_g : (run ? run.target_g : null);
    var errorG = last ? last.error_g : (run ? run.final_error_g : null);
    var pid = last ? last.pid_output : null;

    /* the DB poll must not overwrite the streamed controller weight */
    if (!(liveSnapshot && liveSnapshot.controller)) {
      T.el('live-weight').textContent = weightG != null ? T.gToKg(weightG).toFixed(3) : '—';
    }
    T.el('live-target').textContent = targetG != null ? T.fmtKg(targetG) : '—';
    T.el('live-error').textContent = errorG != null ? T.fmtG(errorG) : '—';
    T.el('live-pid').textContent = pid != null ? Number(pid).toFixed(3) : '—';

    var stateEl = T.el('live-state');
    var stateName = last ? last.state : null;
    stateEl.innerHTML = stateName
      ? '<span class="badge running">' + T.escapeHtml(stateName) + '</span>'
      : '—';

    if (run) {
      T.el('live-run').innerHTML =
        '<span title="' + T.escapeHtml(run.run_id) + '">' + T.escapeHtml(T.shortId(run.run_id, 20)) + '</span>' +
        ' <a href="/runs/' + encodeURIComponent(run.run_id) + '">details</a>';
      T.el('live-status').innerHTML = T.statusBadge(run.status);
      T.el('live-test').textContent = run.test_number != null ? ('#' + run.test_number) : '—';
    } else {
      T.el('live-run').textContent = '—';
      T.el('live-status').textContent = '—';
      T.el('live-test').textContent = '—';
    }

    var ts = last ? last.timestamp : (data.last_telemetry_at || null);
    T.el('live-time').textContent = ts ? T.fmtTime(ts) : '—';

    setLamp('lamp-r1', 'lamp-r1-state', last ? last.relay1 : false);
    setLamp('lamp-r2', 'lamp-r2-state', last ? last.relay2 : false);

    var title = T.el('chart-title');
    if (title) {
      title.textContent = 'Weight vs Time' + (run ? (' — ' + T.fmtTargetKg(run.target_g)) : '');
    }

    updateChart(run, data.samples || []);
    renderLiveWeight(); /* history responses cannot overwrite live readings */
  }

  function renderLiveWeight() {
    if (!liveSnapshot || !liveSnapshot.controller) return;
    if (!ensureLayout()) return;
    var controller = liveSnapshot.controller;
    var channels = controller.status.channels || [];
    var channel = channels.find(function (c) { return c.active_job_id; }) ||
      channels.find(function (c) { return c.weight_valid; }) || channels[0];
    if (!channel) return;
    var valid = controller.online && channel.weight_valid;
    T.el('live-weight').textContent = valid ? (Number(channel.weight_g) / 1000).toFixed(3) : 'Unavailable';
    T.el('live-mode').textContent = controller.online ? 'LIVE — controller telemetry' : 'Controller offline';
    T.el('live-target').textContent = channel.target_g ? T.fmtKg(channel.target_g) : '—';
    T.el('live-error').textContent = valid ? T.fmtG(channel.error_g) : '—';
    T.el('live-pid').textContent = channel.output != null ? Number(channel.output).toFixed(3) : '—';
    T.el('live-state').textContent = channel.state || '—';
    T.el('live-time').textContent = T.fmtTime(controller.updated_at);
    channels.forEach(function (c) {
      var n = c.channel_id === 'CH1' ? 1 : 2;
      setLamp('lamp-r' + n, 'lamp-r' + n + '-state', controller.online && c.relay_on);
    });
  }

  /* ---------- sender channels over WebSocket ---------- */

  function hasSenders() {
    return Object.keys(senders).length > 0;
  }

  function effectiveAge(s, c, now) {
    var base = c && c.weight_age_ms != null ? Number(c.weight_age_ms) : Number(s.age_ms);
    return base + (now - s.rxAt);
  }

  function rowFor(host, deviceId, ch) {
    var key = deviceId + '|' + ch;
    if (rows[key]) { return rows[key]; }
    var el = document.createElement('div');
    el.className = 'live-rows ws-row';
    var name = document.createElement('div');
    name.textContent = deviceId + ' / ' + ch;
    var weight = document.createElement('div');
    weight.className = 'stat-value';
    var meta = document.createElement('div');
    meta.className = 'small';
    var btns = document.createElement('div');
    var zero = document.createElement('button');
    zero.type = 'button'; zero.className = 'btn'; zero.textContent = 'ZERO';
    var tare = document.createElement('button');
    tare.type = 'button'; tare.className = 'btn'; tare.textContent = 'TARE';
    btns.appendChild(zero);
    btns.appendChild(document.createTextNode(' '));
    btns.appendChild(tare);
    var msg = document.createElement('div');
    msg.className = 'small';
    msg.setAttribute('role', 'status');
    [name, weight, meta, btns, msg].forEach(function (n) { el.appendChild(n); });
    zero.addEventListener('click', function () { sendCommand(deviceId, ch, 'ZERO'); });
    tare.addEventListener('click', function () { sendCommand(deviceId, ch, 'TARE'); });
    host.appendChild(el);
    rows[key] = { el: el, weight: weight, meta: meta, zero: zero, tare: tare, msg: msg };
    return rows[key];
  }

  function renderWs() {
    wsRaf = null;
    if (wsTimer !== null) { clearTimeout(wsTimer); wsTimer = null; }
    var host = T.el('ws-senders');
    var status = T.el('ws-status');
    if (!host) { return; }
    if (host !== rowsHost) { rows = {}; rowsHost = host; }
    var linkText = link === 'open' ? 'WebSocket connected' :
      (link === 'connecting' ? 'WebSocket connecting...' : 'WebSocket disconnected - reconnecting');
    if (status) {
      status.textContent = linkText + ' | broker: ' + broker +
        (wsFailures >= 3 && link !== 'open' ? ' | controller panel on SSE' : '') +
        (wsNote ? ' | ' + wsNote : '');
    }
    var now = performance.now();
    var next = Infinity;
    Object.keys(senders).sort().forEach(function (id) {
      var s = senders[id];
      var devAge = Number(s.age_ms) + (now - s.rxAt);
      var offline = link !== 'open' || s.online === false || devAge >= OFFLINE_MS;
      (s.channels || []).forEach(function (c) {
        var key = id + '|' + c.channel_id;
        var r = rowFor(host, id, c.channel_id);
        var age = effectiveAge(s, c, now);
        var stale = !offline && (age > STALE_MS || !c.weight_valid);
        var showValue = !offline && c.weight_valid && c.weight_g != null && age <= OFFLINE_MS;
        r.weight.textContent = showValue ? (Number(c.weight_g) / 1000).toFixed(3) + ' kg' : 'Unavailable';
        r.meta.textContent = offline ? 'OFFLINE' :
          (stale ? 'STALE ' : 'LIVE ') + Math.round(age) + ' ms' + (c.stable ? ' stable' : '');
        r.el.classList.toggle('ws-stale', stale);
        r.el.classList.toggle('ws-offline', offline);
        var busy = !!inflightCmd[key];
        r.zero.disabled = busy || link !== 'open';
        r.tare.disabled = busy || link !== 'open';
        r.msg.textContent = cmdMsg[key] || '';
        if (!offline) {
          if (age <= STALE_MS) { next = Math.min(next, STALE_MS - age + 5); }
          if (devAge < OFFLINE_MS) { next = Math.min(next, OFFLINE_MS - devAge + 5); }
        }
      });
    });
    if (next !== Infinity) { wsTimer = setTimeout(scheduleWs, Math.max(next, 20)); }
  }

  function scheduleWs() {
    if (wsRaf !== null) { return; }
    wsRaf = requestAnimationFrame(renderWs);
  }

  function setCmd(key, text, busy) {
    cmdMsg[key] = text;
    if (busy) { inflightCmd[key] = true; } else { delete inflightCmd[key]; }
    scheduleWs();
  }

  function sendCommand(deviceId, ch, cmd) {
    var key = deviceId + '|' + ch;
    if (inflightCmd[key] || !wsClient) { return; }
    if (!wsClient.sendCmd(deviceId, ch, cmd)) {
      setCmd(key, cmd + ' not sent: WebSocket not connected', false);
      return;
    }
    setCmd(key, cmd + ' sent - waiting for device', true);
  }

  function describeAck(m) {
    var text = String(m.state);
    if (m.result) { text += ' (' + m.result + ')'; }
    if (m.reason) { text += ': ' + m.reason; }
    if (m.weight_g != null) { text += ' @ ' + m.weight_g + ' g'; }
    if (m.late) { text += ' [late ack]'; }
    return text;
  }

  function promptKey() {
    if (!wsClient || typeof window.prompt !== 'function') { return; }
    var key = window.prompt('API key required to send commands (kept for this browser tab only):');
    if (key) { wsClient.sendAuth(key.trim()); }
  }

  function onCmdState(m) {
    var key = m.device_id + '|' + m.channel;
    var reason = m.reason ? ': ' + m.reason : '';
    if (m.status === 'accepted') {
      setCmd(key, 'accepted, waiting for device ack', true);
    } else if (m.status === 'unknown') {
      setCmd(key, 'OUTCOME UNKNOWN' + reason, true);
    } else if (m.status === 'refused') {
      setCmd(key, 'refused' + reason, false);
      if (m.reason === 'not authenticated') { promptKey(); }
    } else {
      setCmd(key, String(m.status) + reason, false);
    }
  }

  function onSender(m) {
    if (!m.device_id) { return; }
    var cur = senders[m.device_id];
    if (cur && typeof cur.revision === 'number' && typeof m.revision === 'number' &&
        m.revision < cur.revision) { return; }
    senders[m.device_id] = {
      channels: m.channels || [], age_ms: Number(m.age_ms) || 0, online: m.online,
      revision: m.revision, rxAt: performance.now()
    };
    ensureLayout();
    scheduleWs();
  }

  function onSnapshot(m) {
    var snap = m.senders || {};
    var fresh = {};
    var now = performance.now();
    Object.keys(snap).forEach(function (id) {
      var v = snap[id] || {};
      fresh[id] = { channels: v.channels || [], age_ms: Number(v.age_ms) || 0, online: v.online,
        revision: m.revision, rxAt: now };
    });
    senders = fresh;
    if (hasSenders()) { ensureLayout(); }
    scheduleWs();
  }

  function startWs() {
    if (typeof window.connectLiveWs !== 'function' || typeof WebSocket === 'undefined') { return; }
    wsClient = window.connectLiveWs({
      onSnapshot: onSnapshot,
      onSender: onSender,
      onBroker: function (m) { broker = String(m.state); scheduleWs(); },
      onCmdState: onCmdState,
      onCmdAck: function (m) { setCmd(m.device_id + '|' + m.channel, describeAck(m), false); },
      onAuth: function () {
        wsNote = 'authenticated for commands';
        scheduleWs();
      },
      onError: function (m) {
        wsNote = 'server error ' + m.code + ': ' + m.message;
        scheduleWs();
      },
      onLink: function (state, failures) {
        link = state;
        wsFailures = failures || 0;
        if (state !== 'open') {
          Object.keys(inflightCmd).forEach(function (k) {
            cmdMsg[k] = 'connection lost; outcome unknown';
            delete inflightCmd[k];
          });
        }
        scheduleWs();
      }
    });
  }

  function showNoRuns() {
    if (!root) { return; }
    if (emptyShown) { return; } /* don't re-render the same empty state every tick */
    emptyShown = true;
    layoutBuilt = false;
    chart = T.destroyChart(chart);
    chartRunId = null;
    root.innerHTML = '';
    T.showState(root, 'No runs recorded yet',
      'When the controller starts a dispense run (or once any run is stored), it will appear here.');
  }

  /* ---------- polling ---------- */

  async function poll() {
    if (inflight) { return; } /* skip tick if the previous one is still running */
    inflight = true;
    try {
      var data = await T.apiGet('/live');
      failCount = 0;
      if (!data.active_run && (!data.samples || data.samples.length === 0) && !data.last_sample) {
        if ((liveSnapshot && liveSnapshot.controller) || hasSenders()) { ensureLayout(); renderLiveWeight(); scheduleWs(); } else { showNoRuns(); }
      } else {
        if (!layoutBuilt) { ensureLayout(); }
        render(data);
        var conn = T.el('live-conn');
        if (conn) { conn.textContent = ''; }
      }
    } catch (err) {
      failCount++;
      if (!layoutBuilt) {
        root.innerHTML = '';
        T.showError(root, err);
      } else {
        var conn = T.el('live-conn');
        if (conn) {
          conn.textContent = failCount >= 2
            ? ('Connection problem: ' + err.message + ' — retrying…')
            : '';
        }
      }
    } finally {
      inflight = false;
    }
  }

  function init() {
    root = T.el('live-root');
    if (!root) { return; }
    T.applyChartDefaults();
    T.startHealthIndicator();
    stopLive = T.subscribeLive(function (data) { liveSnapshot = data; renderLiveWeight(); });
    startWs();
    poll();
    pollTimer = T.visibleInterval(poll, POLL_MS);
    window.addEventListener('beforeunload', function () {
      if (pollTimer) { pollTimer(); }
      chart = T.destroyChart(chart);
      if (stopLive) stopLive();
      if (wsClient) wsClient.close();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
