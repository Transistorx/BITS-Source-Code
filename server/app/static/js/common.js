/* ============================================================
   BITS Dispense Telemetry — shared helpers.
   Exposes a single global: window.Telemetry
   No page-specific logic lives here.
   ============================================================ */
(function () {
  'use strict';

  var API_BASE = '/api/v1';
  var HEALTH_PATH = '/health';
  var HEALTH_POLL_MS = 5000;

  /* ---------- fetch helpers (same-origin, relative URLs) ---------- */

  /* Optional timeoutMs aborts a hung request (error.timeout === true). */
  async function apiGet(path, timeoutMs) {
    if (!timeoutMs) { return apiGetWithSignal(path); }
    var controller = new AbortController();
    var timer = setTimeout(function () { controller.abort(); }, timeoutMs);
    try {
      return await apiGetWithSignal(path, controller.signal);
    } catch (err) {
      if (err && err.name === 'AbortError') {
        var timeout = new Error('Request timed out.');
        timeout.timeout = true;
        throw timeout;
      }
      throw err;
    } finally {
      clearTimeout(timer);
    }
  }

  /* Same as apiGet, but the caller can abort the request. An aborted fetch
   * rejects with a DOMException named 'AbortError'; callers that cancel
   * superseded requests check for that name and drop the result. */
  async function apiGetWithSignal(path, signal) {
    var res;
    try {
      res = await fetch(API_BASE + path, {
        headers: { 'Accept': 'application/json' },
        signal: signal
      });
    } catch (err) {
      if (err && err.name === 'AbortError') { throw err; }
      throw new Error('Cannot reach the telemetry server (network error).');
    }
    if (!res.ok) {
      var detail = '';
      try {
        var body = await res.json();
        if (body && body.detail) { detail = errorText(body.detail, res.status); }
      } catch (ignored) { /* non-JSON error body */ }
      var err404 = new Error(detail || ('Server returned HTTP ' + res.status));
      err404.status = res.status;
      throw err404;
    }
    return res.json();
  }

  async function getHealth() {
    var res;
    try {
      res = await fetch(HEALTH_PATH, { headers: { 'Accept': 'application/json' } });
    } catch (err) {
      throw new Error('Cannot reach the telemetry server (network error).');
    }
    if (!res.ok) {
      throw new Error('Health endpoint returned HTTP ' + res.status);
    }
    return res.json();
  }

  /* One bounded stream per page. The fallback reads memory, never history.
   * A response started before a newer SSE delivery cannot overwrite it. */
  /* setInterval that stops while the tab is hidden and, on return, runs fn once
   * immediately then resumes. Returns a stop function. Read-only polling only. */
  function visibleInterval(fn, ms) {
    var id = null, stopped = false;
    function start() { if (id === null) id = setInterval(fn, ms); }
    function halt() { if (id !== null) { clearInterval(id); id = null; } }
    function onVisibility() {
      if (stopped) return;
      if (document.hidden) { halt(); } else { fn(); start(); }
    }
    document.addEventListener('visibilitychange', onVisibility);
    if (!document.hidden) start();
    return function () {
      stopped = true;
      halt();
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }

  function subscribeLive(callback) {
    var source = null, closed = false, streaming = false, inflight = false;
    var generation = 0, revision = -1, instance = null, pending = null, raf = null;
    var lastData = null, lastReceipt = 0;
    var diag = { messages: 0, rendered: 0, coalesced: 0, rejected: 0, lastRenderDelayMs: 0, maxRenderDelayMs: 0 };
    window.Telemetry.liveDiagnostics = diag;
    function deliver(data) {
      if (closed) return;
      if (instance === data.instance && data.revision < revision) { diag.rejected++; return; }
      instance = data.instance;
      revision = data.revision;
      lastData = data;
      lastReceipt = performance.now();
      generation++;
      diag.messages++;
      if (pending) diag.coalesced++;
      pending = { data: data, received: performance.now() };
      if (raf !== null) return;
      raf = requestAnimationFrame(function () {
        raf = null;
        var latest = pending;
        pending = null;
        diag.lastRenderDelayMs = performance.now() - latest.received;
        diag.maxRenderDelayMs = Math.max(diag.maxRenderDelayMs, diag.lastRenderDelayMs);
        diag.rendered++;
        callback(latest.data);
      });
    }
    async function pollLive() {
      if (closed || streaming || inflight) return;
      inflight = true;
      var started = generation;
      var abort = new AbortController();
      var deadline = setTimeout(function () { abort.abort(); }, 2000);
      try {
        var data = await apiGetWithSignal('/live/telemetry', abort.signal);
        if (started === generation) deliver(data); else diag.rejected++;
      } catch (ignored) { /* retain last data; stream/fallback retries */ }
      finally { clearTimeout(deadline); inflight = false; }
    }
    if (typeof EventSource !== 'undefined') {
      source = new EventSource(API_BASE + '/live/events');
      source.onopen = function () { streaming = true; };
      source.onmessage = function (event) {
        try { deliver(JSON.parse(event.data)); } catch (ignored) { /* malformed event */ }
      };
      source.onerror = function () { streaming = false; pollLive(); };
    }
    pollLive();
    var timer = visibleInterval(pollLive, 250);
    // Expire locally even if both network paths fail. Do not manufacture zero.
    var expiry = visibleInterval(function () {
      if (closed || !lastData) return;
      var age = performance.now() - lastReceipt;
      if (age < 1000) return;
      var data = JSON.parse(JSON.stringify(lastData));
      if (data.weight) {
        data.weight.age_ms += age;
        data.weight.weight_valid = data.weight.weight_valid && data.weight.age_ms <= 3000;
      }
      if (data.controller) {
        data.controller.age_seconds += age / 1000;
        data.controller.online = data.controller.online && data.controller.age_seconds <= 10;
        (data.controller.status.channels || []).forEach(function (channel) {
          if (channel.weight_age_ms !== null && channel.weight_age_ms !== undefined &&
              Number(channel.weight_age_ms) < 4294967295) {
            channel.weight_age_ms += age;
            channel.weight_age_s = channel.weight_age_ms / 1000;
          }
          channel.weight_valid = channel.weight_valid && channel.weight_age_ms <= 3000 && data.controller.online;
          if (!data.controller.online) channel.state = 'OFFLINE';
        });
      }
      callback(data);
    }, 500);
    var stop = function () {
      closed = true;
      timer();
      expiry();
      if (source) source.close();
      if (raf !== null) cancelAnimationFrame(raf);
      window.removeEventListener('beforeunload', stop);
    };
    window.addEventListener('beforeunload', stop);
    return stop;
  }

  /* ---------- timestamp formatting (API = UTC ISO, UI = local) ---------- */

  function isValidDate(iso) {
    if (!iso) { return false; }
    var d = new Date(iso);
    return !isNaN(d.getTime());
  }

  function fmtDateTime(iso) {
    if (!isValidDate(iso)) { return '—'; }
    return new Date(iso).toLocaleString();
  }

  function fmtTime(iso) {
    if (!isValidDate(iso)) { return '—'; }
    return new Date(iso).toLocaleTimeString();
  }

  function timeAgo(iso) {
    if (!isValidDate(iso)) { return null; }
    var secs = Math.max(0, Math.round((Date.now() - new Date(iso).getTime()) / 1000));
    if (secs < 60) { return secs + 's ago'; }
    if (secs < 3600) { return Math.floor(secs / 60) + 'm ago'; }
    if (secs < 86400) { return Math.floor(secs / 3600) + 'h ago'; }
    return Math.floor(secs / 86400) + 'd ago';
  }

  /* datetime-local input value (browser-local) -> UTC ISO for the API */
  function localInputToIso(value) {
    if (!value) { return null; }
    var d = new Date(value);
    if (isNaN(d.getTime())) { return null; }
    return d.toISOString();
  }

  /* ---------- unit / number formatting ---------- */

  function gToKg(g) {
    return (g === null || g === undefined || isNaN(g)) ? null : g / 1000;
  }

  function fmtKg(g, decimals) {
    var kg = gToKg(g);
    if (kg === null) { return '—'; }
    return kg.toFixed(decimals === undefined ? 3 : decimals) + ' kg';
  }

  function fmtG(g) {
    if (g === null || g === undefined || isNaN(g)) { return '—'; }
    var n = Number(g);
    return (n > 0 ? '+' + n : String(n)) + ' g';
  }

  function fmtGAbs(g) {
    if (g === null || g === undefined || isNaN(g)) { return '—'; }
    return Number(g) + ' g';
  }

  function fmtPct(pct) {
    if (pct === null || pct === undefined || isNaN(pct)) { return '—'; }
    var n = Number(pct);
    return (n > 0 ? '+' + n.toFixed(2) : n.toFixed(2)) + '%';
  }

  function fmtDurationMs(ms) {
    if (ms === null || ms === undefined || isNaN(ms)) { return '—'; }
    return (Number(ms) / 1000).toFixed(1) + ' s';
  }

  function fmtElapsedMs(ms) {
    if (ms === null || ms === undefined || isNaN(ms)) { return '—'; }
    return (Number(ms) / 1000).toFixed(1) + ' s';
  }

  function fmtNum(v, decimals) {
    if (v === null || v === undefined || isNaN(v)) { return '—'; }
    return Number(v).toFixed(decimals === undefined ? 2 : decimals);
  }

  function fmtGain(v) {
    if (v === null || v === undefined || isNaN(v)) { return '—'; }
    return Number(v).toPrecision(3).replace(/\.?0+$/, '');
  }

  function fmtTargetKg(targetG) {
    var kg = gToKg(targetG);
    if (kg === null) { return '? kg'; }
    return (Math.round(kg * 1000) / 1000) + ' kg';
  }

  /* ---------- strings / DOM ---------- */

  function escapeHtml(s) {
    if (s === null || s === undefined) { return ''; }
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function shortId(id, maxLen) {
    if (!id) { return '—'; }
    var n = maxLen || 16;
    return String(id).length > n ? String(id).slice(0, n - 1) + '…' : String(id);
  }

  function el(id) { return document.getElementById(id); }

  function statusClass(status) { return toneFor(status); }
  function statusBadge(status) { return badge(status || 'UNKNOWN', toneFor(status)); }

  /* State boxes: empty / error / loading messages inside a container. */
  function showState(container, title, message, isError) {
    if (!container) { return; }
    container.innerHTML =
      '<div class="state-box' + (isError ? ' error' : '') + '" role="' + (isError ? 'alert' : 'status') + '">' +
      '<div class="state-title">' + escapeHtml(title) + '</div>' +
      '<div>' + escapeHtml(message) + '</div>' +
      '</div>';
  }

  function showError(container, err) {
    var msg = (err && err.message) ? err.message : String(err);
    showState(container, 'Could not load data', msg, true);
  }

  function showLoading(container, message) {
    if (!container) { return; }
    container.innerHTML =
      '<div class="state-box" role="status">' + escapeHtml(message || 'Loading…') + '</div>';
  }

  function errorText(detail, status) {
    if (Array.isArray(detail)) {
      detail = detail.map(function (item) {
        if (!item || typeof item !== 'object') { return String(item); }
        var location = Array.isArray(item.loc) ? item.loc.filter(function (part) { return part !== 'body'; }).join('.') : '';
        return (location ? location + ': ' : '') + (item.msg || JSON.stringify(item));
      }).join('; ');
    } else if (detail && typeof detail === 'object') {
      detail = detail.message || JSON.stringify(detail);
    }
    return detail ? String(detail) : 'HTTP ' + status;
  }

  async function apiWrite(path, options) {
    options = options || {};
    var headers = Object.assign({ 'Accept': 'application/json' }, options.headers || {});
    var timeoutMs = options.timeoutMs;
    var init = Object.assign({}, options, { headers: headers });
    delete init.timeoutMs;
    var controller = null, timer = null, timedOut = false;
    if (timeoutMs) {
      controller = new AbortController();
      init.signal = controller.signal;
      timer = setTimeout(function () { timedOut = true; controller.abort(); }, timeoutMs);
    }
    var timeoutError = function () {
      var e = new Error('Request timed out.');
      e.timeout = true;
      return e;
    };
    try {
      var res;
      try {
        res = await fetch(API_BASE + path, init);
      } catch (ignored) {
        if (timedOut) { throw timeoutError(); }
        throw new Error('Cannot reach the telemetry server (network error).');
      }
      var body;
      try {
        body = await res.json();
      } catch (ignored) {
        if (timedOut) { throw timeoutError(); }
        body = {};
      }
      if (!res.ok) {
        var failure = new Error(errorText(body.detail || body.error, res.status));
        failure.status = res.status;
        throw failure;
      }
      return body;
    } finally {
      if (timer) { clearTimeout(timer); }
    }
  }

  function setButtonBusy(button, busy, label) {
    if (!button) { return; }
    if (busy) {
      if (!button.dataset.idleLabel) { button.dataset.idleLabel = button.textContent; }
      button.disabled = true;
      button.classList.add('is-loading');
      button.textContent = label || 'Working…';
    } else {
      button.disabled = false;
      button.classList.remove('is-loading');
      button.textContent = button.dataset.idleLabel || button.textContent;
      delete button.dataset.idleLabel;
    }
  }

  function toast(message, type, persistent) {
    var region = el('toast-region');
    if (!region) { return; }
    var node = document.createElement('div');
    node.className = 'toast ' + (type || 'info');
    node.setAttribute('role', type === 'error' ? 'alert' : 'status');
    node.innerHTML = '<span>' + escapeHtml(message) + '</span><button type="button" aria-label="Dismiss notification">×</button>';
    node.querySelector('button').addEventListener('click', function () { node.remove(); });
    region.appendChild(node);
    requestAnimationFrame(function () { node.classList.add('show'); });
    if (!persistent) {
      window.setTimeout(function () { node.classList.remove('show'); window.setTimeout(function () { node.remove(); }, 180); }, 4200);
    }
  }

  /* ---------- system health rows + status strip (all pages) ---------- */

  /* Writes one System Health row. `info` is {word, className, ageText}. */
  function setHealth(name, info) {
    var row = document.querySelector('[data-health="' + name + '"]');
    if (!row) { return; }
    var stateEl = row.querySelector('.health-state');
    var ageEl = row.querySelector('.health-age');
    if (stateEl) {
      stateEl.textContent = info.word;
      stateEl.className = 'health-state ' + info.className;
    }
    if (ageEl) { ageEl.textContent = info.ageText || ''; }
  }

  function startHealthIndicator() {
    if (!el('health-text') && !document.querySelector('[data-health="server"]')) { return; }
    initSidebar();

    /* Nested helpers use function(...) with no space after the keyword so the
       contract test can capture the whole poller body. */
    var poll = async function() {
      try {
        var results = await Promise.all([
          getHealth(),
          apiGet('/device/status').catch(function() { return []; })
        ]);
        var h = results[0];
        var devices = results[1] || [];
        var dbUp = String(h.database).toLowerCase() === 'up';
        setHealth('server', { word: 'ONLINE', className: 'ok', ageText: '' });
        setHealth('database', dbUp
          ? { word: 'ONLINE', className: 'ok', ageText: '' }
          : { word: 'OFFLINE', className: 'bad', ageText: '' });

        /* Two separate ESP32s, each reporting for itself under its own
         * device_id. Identify them by the explicit role they publish — never
         * by arrival order, which would swap the rows after a reconnect. */
        var isRelay = function(d) {
          return d && d.status && d.status.role === 'relay_controller';
        };
        var isSender = function(d) {
          return d && d.status && d.status.role === 'weight_sender';
        };
        var relay = devices.find(isRelay);
        var sender = devices.find(isSender);
        /* Pre-role firmware fallback: the first row that carries dispense
         * channels is the relay controller. */
        if (!relay) {
          relay = devices.find(function(d) {
            return d && d.status && Array.isArray(d.status.channels) &&
                   d.status.channels.some(function(c) { return c && c.channel_id; });
          });
        }

        var ageOf = function(device) {
          return device ? Number(device.age_seconds) : NaN;
        };
        var describe = function(device) {
          var age = ageOf(device);
          var f = freshnessState(age);
          return {
            word: f.word,
            className: f.className,
            ageText: isFinite(age) ? (Math.round(age) + 's') : 'no data'
          };
        };

        setHealth('relay', describe(relay));
        setHealth('sender', describe(sender));

        /* Scale rows come from the relay's own channel reports. A missing
         * channel is never ONLINE — the row must say OFFLINE / no data.
         * A stale relay is the same story: its channels cannot be trusted,
         * so the scale rows must not claim ONLINE off old numbers. */
        var relayFresh = freshnessState(ageOf(relay)).state === 'fresh';
        var channels = relay && relay.status && Array.isArray(relay.status.channels)
          ? relay.status.channels : [];
        [['scale1', 0], ['scale2', 1]].forEach(function(pair) {
          var ch = channels[pair[1]];
          if (!relayFresh || !ch) {
            setHealth(pair[0], { word: 'OFFLINE', className: 'bad', ageText: 'no data' });
            return;
          }
          if (!ch.weight_valid) {
            setHealth(pair[0], {
              word: 'STALE',
              className: 'warn',
              ageText: ch.weight_age_ms !== null && ch.weight_age_ms !== undefined &&
                       isFinite(Number(ch.weight_age_ms)) && Number(ch.weight_age_ms) < 4294967295
                ? (Math.round(Number(ch.weight_age_ms) / 1000) + 's') : 'no data'
            });
            return;
          }
          setHealth(pair[0], { word: 'ONLINE', className: 'ok', ageText: 'live' });
        });

        var text = el('health-text');
        if (text) {
          var ago = timeAgo(h.last_telemetry_at);
          text.textContent = ago ? 'Telemetry ' + ago : 'No telemetry yet';
        }
        renderSystemStatus('system-status', {
          controllerAge: ageOf(relay),
          transport: 'HTTP',
          telemetryAge: h.last_telemetry_at
            ? (Date.now() - Date.parse(h.last_telemetry_at)) / 1000 : NaN
        });
      } catch (err) {
        ['server', 'database', 'relay', 'sender', 'scale1', 'scale2'].forEach(function(name) {
          setHealth(name, { word: 'OFFLINE', className: 'bad', ageText: 'no data' });
        });
        var text = el('health-text');
        if (text) { text.textContent = 'Server unreachable'; }
      }
    };

    poll();
    visibleInterval(poll, HEALTH_POLL_MS);
  }

  /* Bottom status strip: controller age, transport, telemetry age. */
  function renderSystemStatus(elementId, info) {
    var host = el(elementId);
    if (!host) { return; }
    var controller = el('status-controller');
    var transport = el('status-transport');
    var telemetry = el('status-telemetry');
    var f = freshnessState(info.controllerAge);
    if (controller) {
      controller.textContent = 'CONTROLLER ' + f.word;
      controller.className = 'status-chip ' + f.className;
    }
    if (transport) {
      transport.textContent = info.transport || 'HTTP';
      transport.className = 'status-chip neutral';
    }
    var t = freshnessState(info.telemetryAge);
    if (telemetry) {
      telemetry.textContent = 'TELEMETRY ' + t.word +
        (isFinite(info.telemetryAge) ? (' · ' + Math.round(info.telemetryAge) + 's') : '');
      telemetry.className = 'status-chip ' + t.className;
    }
  }

  /* Drawer toggle for the sidebar on small screens. */
  function initSidebar() {
    var toggle = el('menu-toggle');
    var sidebar = document.querySelector('.site-header');
    if (!toggle || !sidebar) { return; }
    toggle.addEventListener('click', function () {
      var open = sidebar.classList.toggle('open');
      toggle.setAttribute('aria-expanded', open ? 'true' : 'false');
      toggle.setAttribute('aria-label', open ? 'Close navigation menu' : 'Open navigation menu');
    });
    sidebar.addEventListener('click', function (event) {
      if (event.target.closest('.nav-link') && sidebar.classList.contains('open')) {
        sidebar.classList.remove('open');
        toggle.setAttribute('aria-expanded', 'false');
      }
    });
  }

  /* ---------- Chart.js shared setup ---------- */

  function applyChartDefaults() {
    if (typeof window.Chart === 'undefined') { return; }
    var C = window.Chart;
    C.defaults.font.family = '-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif';
    C.defaults.font.size = 11;
    C.defaults.color = cssVar('--text-muted', '#9aaabd');
    C.defaults.borderColor = cssVar('--border', '#2a3745');
    C.defaults.scale.grid.color = cssVar('--chart-grid', 'rgba(154, 170, 189, 0.16)');
    C.defaults.animation.duration = 250;
    C.defaults.plugins.legend.labels.boxWidth = 14;
    C.defaults.maintainAspectRatio = false;
  }

  function cssVar(name, fallback) {
    var v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
    return v || fallback;
  }

  /* Destroy a Chart bound to a canvas (leak-free re-create helper). */
  function destroyChart(chart) {
    if (chart && typeof chart.destroy === 'function') {
      try { chart.destroy(); } catch (ignored) { /* already gone */ }
    }
    return null;
  }

  /* Build sample -> {x: elapsed seconds, y: kg} points. */
  function weightPoints(samples) {
    var pts = [];
    for (var i = 0; i < samples.length; i++) {
      var s = samples[i];
      if (s == null || s.elapsed_ms == null || s.weight_g == null) { continue; }
      pts.push({ x: s.elapsed_ms / 1000, y: s.weight_g / 1000 });
    }
    return pts;
  }

  /* Weight-vs-Time chart: kg vs elapsed seconds + dashed flat target line.
     Shared by dashboard cards, detail page and live page. */
  function createWeightChart(canvas, samples, targetG, opts) {
    if (typeof window.Chart === 'undefined') { return null; }
    opts = opts || {};
    var pts = weightPoints(samples || []);
    var targetKg = gToKg(targetG);
    var maxWeight = pts.reduce(function (max, point) { return Math.max(max, point.y); }, 0);
    var suggestedMax = Math.max(maxWeight * 1.06, targetKg !== null ? targetKg * 1.08 : 1);

    var datasets = [{
      label: 'Weight (kg)',
      data: pts,
      borderColor: cssVar('--series-weight', '#0b5cad'),
      backgroundColor: 'rgba(11, 92, 173, 0.10)',
      borderWidth: 2,
      pointRadius: pts.length > 150 ? 0 : 2,
      pointHoverRadius: 3,
      fill: opts.fill !== false,
      tension: 0
    }];

    if (targetKg !== null && pts.length > 0) {
      datasets.push({
        label: 'Target (' + targetKg + ' kg)',
        data: [
          { x: pts[0].x, y: targetKg },
          { x: pts[pts.length - 1].x, y: targetKg }
        ],
        borderColor: cssVar('--series-target', '#b35900'),
        borderWidth: 1.5,
        borderDash: [6, 4],
        pointRadius: 0,
        fill: false
      });
    }

    return new window.Chart(canvas.getContext('2d'), {
      type: 'line',
      data: { datasets: datasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'nearest', axis: 'x', intersect: false },
        plugins: {
          legend: { display: opts.legend !== false },
          tooltip: {
            backgroundColor: '#111923',
            borderColor: cssVar('--border-strong', '#405264'),
            borderWidth: 1,
            titleColor: cssVar('--text', '#e6edf5'),
            bodyColor: cssVar('--text-muted', '#9aaabd'),
            callbacks: {
              title: function (items) {
                return items.length ? ('t = ' + Number(items[0].parsed.x).toFixed(1) + ' s') : '';
              },
              label: function (ctx) {
                return ctx.dataset.label + ': ' + Number(ctx.parsed.y).toFixed(3) + ' kg';
              }
            }
          }
        },
        scales: {
          x: {
            type: 'linear',
            title: { display: true, text: 'Elapsed (s)' },
            beginAtZero: true
          },
          y: {
            title: { display: true, text: 'Weight (kg)' },
            beginAtZero: opts.zeroY !== false,
            suggestedMax: suggestedMax
          }
        }
      }
    });
  }

  /* ---------- shared presentation helpers ---------- */

  var STATE_TONES = {
    'OFFLINE': 'bad', 'FAULT': 'bad', 'FAILED': 'bad', 'EMERGENCY STOP': 'bad',
    'ERROR': 'bad', 'WEIGHT LINK LOST': 'bad', 'OVERWEIGHT': 'bad',
    'STALE': 'warn', 'PENDING': 'warn', 'PENDING DEVICE': 'warn',
    'PARKED': 'warn', 'HELD': 'warn', 'PRIORITY': 'warn', 'WAITING': 'warn',
    'WAITING FOR SCALE': 'warn', 'WAITING FOR PROFILE': 'warn',
    'CANCELLED': 'warn', 'DEACTIVATED': 'warn',
    'DISPENSING': 'info', 'RUNNING': 'info', 'ACTIVE': 'info', 'COMPLETED': 'info',
    'COMPLETE': 'ok', 'READY': 'ok', 'IDLE': 'ok', 'ONLINE': 'ok', 'APPLIED': 'ok',
    'QUEUED': 'neutral', 'HISTORICAL': 'neutral', 'NO ACTIVE PROFILE': 'neutral',
    'INACTIVE': 'neutral'
  };

  function stateWord(state) {
    var s = String(state === null || state === undefined ? '' : state).toUpperCase().trim();
    return s || 'UNKNOWN';
  }

  function toneFor(state) {
    return STATE_TONES[stateWord(state)] || 'neutral';
  }

  function badge(label, tone) {
    var t = tone || toneFor(label);
    return '<span class="badge ' + t + '">' + escapeHtml(String(label).toUpperCase()) + '</span>';
  }

  /* Inline SVG sprite reference. The sprite lives in nav.html as
     <svg hidden><symbol id="i-NAME">…</symbol></svg>. */
  function icon(name, cls) {
    return '<svg class="icon' + (cls ? ' ' + cls : '') + '" aria-hidden="true">' +
           '<use href="#i-' + escapeHtml(name) + '"></use></svg>';
  }

  /* FRESH <= 10s is the pre-existing rule (queue.js / common.js).
     OFFLINE > 30s = 6x the 5s health poll. 11..30 is STALE. */
  function freshnessState(ageSeconds) {
    if (ageSeconds === null || ageSeconds === undefined || isNaN(ageSeconds)) {
      return { state: 'offline', word: 'OFFLINE', className: 'bad' };
    }
    var age = Number(ageSeconds);
    if (age <= 10) { return { state: 'fresh', word: 'ONLINE', className: 'ok' }; }
    if (age <= 30) { return { state: 'stale', word: 'STALE', className: 'warn' }; }
    return { state: 'offline', word: 'OFFLINE', className: 'bad' };
  }

  /* Unavailable is never 0 and never a bare dash. Valid zero stays 0.000 kg. */
  function fmtWeightDisplay(weightG, weightValid, ageSeconds) {
    var kg = (weightG === null || weightG === undefined) ? NaN : Number(weightG);
    if (!weightValid || !Number.isFinite(kg)) {
      // UINT32_MAX ms (4294967.295 s) is the firmware "no reading yet" sentinel.
      var stale = ageSeconds !== null && ageSeconds !== undefined && !isNaN(ageSeconds) &&
                  Number(ageSeconds) < 4294967;
      var noReading = ageSeconds !== null && ageSeconds !== undefined && !isNaN(ageSeconds) && !stale;
      return {
        value: '—.— kg',
        note: stale ? ('Stale — last reading ' + Math.round(Number(ageSeconds)) + 's ago')
                    : (noReading ? 'No reading yet' : 'Scale reading unavailable'),
        valid: false
      };
    }
    return {
      value: (kg / 1000).toFixed(3) + ' kg',
      note: 'Stable reading',
      valid: true
    };
  }

  window.Telemetry = {
    API_BASE: API_BASE,
    apiGet: apiGet,
    apiGetWithSignal: apiGetWithSignal,
    subscribeLive: subscribeLive,
    visibleInterval: visibleInterval,
    getHealth: getHealth,
    fmtDateTime: fmtDateTime,
    fmtTime: fmtTime,
    timeAgo: timeAgo,
    localInputToIso: localInputToIso,
    gToKg: gToKg,
    fmtKg: fmtKg,
    fmtG: fmtG,
    fmtGAbs: fmtGAbs,
    fmtPct: fmtPct,
    fmtDurationMs: fmtDurationMs,
    fmtElapsedMs: fmtElapsedMs,
    fmtNum: fmtNum,
    fmtGain: fmtGain,
    fmtTargetKg: fmtTargetKg,
    escapeHtml: escapeHtml,
    shortId: shortId,
    el: el,
    statusClass: statusClass,
    statusBadge: statusBadge,
    stateWord: stateWord,
    toneFor: toneFor,
    badge: badge,
    icon: icon,
    freshnessState: freshnessState,
    fmtWeightDisplay: fmtWeightDisplay,
    showState: showState,
    showError: showError,
    showLoading: showLoading,
    errorText: errorText,
    apiWrite: apiWrite,
    setButtonBusy: setButtonBusy,
    toast: toast,
    setHealth: setHealth,
    startHealthIndicator: startHealthIndicator,
    renderSystemStatus: renderSystemStatus,
    initSidebar: initSidebar,
    applyChartDefaults: applyChartDefaults,
    cssVar: cssVar,
    destroyChart: destroyChart,
    weightPoints: weightPoints,
    createWeightChart: createWeightChart
  };
})();
