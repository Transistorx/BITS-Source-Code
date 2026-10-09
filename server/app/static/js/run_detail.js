/* ============================================================
   Run detail page (/runs/{run_id}) — full run view.
   run_id is parsed from window.location.pathname (no Jinja).
   Requires: common.js (window.Telemetry), Chart.js.
   ============================================================ */
(function () {
  'use strict';

  var T = window.Telemetry;
  var materialNames = {M1:'Material A', M2:'Material B'};
  var charts = [];
  var root = null;

  function currentRunId() {
    var parts = window.location.pathname.split('/').filter(Boolean);
    var i = parts.indexOf('runs');
    if (i >= 0 && parts[i + 1]) { return decodeURIComponent(parts[i + 1]); }
    return null;
  }

  function destroyCharts() {
    charts.forEach(function (c) { T.destroyChart(c); });
    charts = [];
  }

  /* ---------- small render helpers ---------- */

  function tile(label, value, cls) {
    return '<div class="stat-tile">' +
      '<span class="stat-label">' + T.escapeHtml(label) + '</span>' +
      '<span class="stat-value' + (cls ? ' ' + cls : '') + '">' + value + '</span>' +
      '</div>';
  }

  function eventCategory(name) {
    var n = String(name || '').toUpperCase();
    if (n === 'JOB_FAILED' || n === 'EMERGENCY_STOP' || n === 'WEIGHT_LINK_LOST' || n === 'OVERWEIGHT') {
      return 'fault';
    }
    if (n === 'JOB_COMPLETE' || n === 'TARGET_REACHED') { return 'success'; }
    if (n === 'JOB_CANCELLED' || n === 'CANCEL' || n === 'PAUSE') { return 'warn'; }
    return 'info';
  }

  function eventBadgeClass(name) {
    var cat = eventCategory(name);
    if (cat === 'fault') { return 'failed'; }
    if (cat === 'success') { return 'complete'; }
    if (cat === 'warn') { return 'cancelled'; }
    return 'running';
  }

  /* ---------- charts ---------- */

  function seriesPoints(samples, field, scale) {
    var pts = [];
    for (var i = 0; i < samples.length; i++) {
      var s = samples[i];
      var v = s ? s[field] : null;
      if (s == null || s.elapsed_ms == null || v == null) { continue; }
      pts.push({ x: s.elapsed_ms / 1000, y: scale ? v * scale : v });
    }
    return pts;
  }

  function lineDataset(label, pts, color, extra) {
    var ds = {
      label: label,
      data: pts,
      borderColor: color,
      backgroundColor: color,
      borderWidth: 2,
      pointRadius: pts.length > 200 ? 0 : 2,
      fill: false,
      tension: 0
    };
    if (extra) { Object.assign(ds, extra); }
    return ds;
  }

  function makeChart(canvasId, datasets, yTitle, yOpts) {
    var canvas = document.getElementById(canvasId);
    if (!canvas || typeof window.Chart === 'undefined') { return; }
    var scales = {
      x: { type: 'linear', title: { display: true, text: 'Elapsed (s)' }, beginAtZero: true },
      y: Object.assign({ title: { display: true, text: yTitle } }, yOpts || {})
    };
    charts.push(new window.Chart(canvas.getContext('2d'), {
      type: 'line',
      data: { datasets: datasets },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        interaction: { mode: 'nearest', axis: 'x', intersect: false },
        plugins: { legend: { display: datasets.length > 1 } },
        scales: scales
      }
    }));
  }

  function renderCharts(run, samples) {
    if (!samples || samples.length === 0) {
      var box = document.getElementById('charts-area');
      if (box) {
        box.innerHTML = '<div class="state-box">No weight samples stored for this run — charts unavailable.</div>';
      }
      return;
    }

    /* (a) Weight vs Time (kg) with dashed target line — shared builder */
    var weightCanvas = document.getElementById('chart-weight');
    if (weightCanvas) {
      var c = T.createWeightChart(weightCanvas, samples, run.target_g, { fill: true, zeroY: true });
      if (c) { charts.push(c); }
    }

    /* (b) Error vs Time (g) */
    makeChart('chart-error', [
      lineDataset('Error (g)', seriesPoints(samples, 'error_g'), T.cssVar('--series-error', '#a11d16'))
    ], 'Error (g)');

    /* (c) PID output vs time (duty 0..1) */
    makeChart('chart-pid', [
      lineDataset('PID output (duty)', seriesPoints(samples, 'pid_output'), T.cssVar('--series-pid', '#5b3ea8'))
    ], 'Duty (0–1)', { min: 0, max: 1 });

    /* (d) P / I / D terms */
    makeChart('chart-terms', [
      lineDataset('P term', seriesPoints(samples, 'p_term'), T.cssVar('--series-p', '#0b5cad')),
      lineDataset('I term', seriesPoints(samples, 'i_term'), T.cssVar('--series-i', '#14602c')),
      lineDataset('D term', seriesPoints(samples, 'd_term'), T.cssVar('--series-d', '#b35900'))
    ], 'Term value');

    /* (e) Relay 1 / Relay 2 — stepped binary traces (no interpolation) */
    var r1 = samples.map(function (s) {
      return { x: s.elapsed_ms / 1000, y: s.relay1 ? 1 : 0 };
    });
    var r2 = samples.map(function (s) {
      return { x: s.elapsed_ms / 1000, y: s.relay2 ? 1 : 0 };
    });
    makeChart('chart-relays', [
      lineDataset('Relay 1 · CH1', r1, T.cssVar('--series-r1', '#a11d16'), { stepped: true, borderWidth: 1.8 }),
      lineDataset('Relay 2 · CH2', r2, T.cssVar('--series-r2', '#0b4f94'), { stepped: true, borderWidth: 1.8 })
    ], 'Relay state', {
      min: -0.2,
      max: 1.2,
      ticks: {
        stepSize: 1,
        callback: function (v) { return v === 0 ? 'OFF' : (v === 1 ? 'ON' : ''); }
      }
    });
  }

  /* ---------- page sections ---------- */

  function renderHeader(run) {
    var csvHref = T.API_BASE + '/runs/' + encodeURIComponent(run.run_id) + '/csv';
    return '<div class="detail-header">' +
      '<div class="detail-title-group">' +
        '<h1 class="detail-title">' +
          T.escapeHtml((materialNames[run.material_id] || run.material_id) + ' · ' + run.channel_id + ' · Pump ' + (run.channel_id === 'CH1' ? '1' : '2') + ' · ' + T.fmtTargetKg(run.target_g)) + ' · Test ' +
          T.escapeHtml(run.test_number != null ? run.test_number : '?') + ' ' +
          T.statusBadge(run.status) +
        '</h1>' +
        '<div class="detail-sub">' +
          '<span class="mono" title="run_id">' + T.escapeHtml(run.run_id) + '</span>' +
          '<span>' + T.escapeHtml((materialNames[run.material_id] || run.material_id) + ' · Pump ' + (run.channel_id === 'CH1' ? '1' : '2') + ' · ' + run.channel_id + ' · ' + run.scale_id + ' · ' + run.relay_id) + '</span>' +
          '<span>Profile: <strong>' + T.escapeHtml(profileLabel(run)) + '</strong></span>' +
          '<span>Device: <strong>' + T.escapeHtml(run.device_id || '—') + '</strong></span>' +
          (run.firmware ? '<span>FW: ' + T.escapeHtml(run.firmware) + '</span>' : '') +
          '<span>Start: ' + T.escapeHtml(T.fmtDateTime(run.started_at)) + '</span>' +
          '<span>End: ' + T.escapeHtml(run.completed_at ? T.fmtDateTime(run.completed_at) : '—') + '</span>' +
        '</div>' +
        (run.error_text ? '<div class="detail-sub" style="color:var(--bad-fg)"><strong>Failure:</strong>&nbsp;' +
          T.escapeHtml(run.error_text) + '</div>' : '') +
      '</div>' +
      '<div>' +
        '<a class="btn" href="' + T.escapeHtml(csvHref) + '" download aria-label="Download CSV of samples for this run">Download CSV</a>' +
        ' <a class="btn secondary" href="/history">Back to History</a>' +
      '</div>' +
    '</div>';
  }

  /* "Material A · Pump 1 · default v8". profile_id + version is only the readable
   * snapshot (both pumps can own the same pair); the exact row is
   * run.profile_version_id. */
  function profileLabel(run) {
    var pump = run.pump_id || (run.channel_id === 'CH1' ? 'Pump 1' : 'Pump 2');
    return (materialNames[run.material_id] || run.material_id) + ' · ' + pump + ' · ' +
      (run.profile_id || 'device-default') + ' v' + (run.profile_version || 1);
  }

  function renderStats(run, sampleCount, eventCount) {
    var tiles = [
      tile('Material / target', T.escapeHtml((materialNames[run.material_id] || run.material_id) + ' · ' + T.fmtKg(run.target_g))),
      tile('Channel / relay', T.escapeHtml(run.channel_id + ' / Relay ' + (run.channel_id === 'CH1' ? '1 · P6' : '2 · P7'))),
      tile('Profile version', T.escapeHtml(profileLabel(run))),
      tile('Final weight', T.escapeHtml(T.fmtKg(run.final_weight_g))),
      tile('Final error', T.escapeHtml(T.fmtG(run.final_error_g)) +
        (run.final_error_pct != null ? ' (' + T.escapeHtml(T.fmtPct(run.final_error_pct)) + ')' : '')),
      tile('Max weight', T.escapeHtml(T.fmtKg(run.max_weight_g))),
      tile('Overshoot', T.escapeHtml(T.fmtGAbs(run.overshoot_g))),
      tile('Duration', T.escapeHtml(T.fmtDurationMs(run.duration_ms))),
      tile('Corrections', T.escapeHtml(run.corrections != null ? run.corrections : '—')),
      tile('Samples', T.escapeHtml(sampleCount != null ? sampleCount : (run.sample_count != null ? run.sample_count : '—'))),
      tile('Events', T.escapeHtml(eventCount != null ? eventCount : (run.event_count != null ? run.event_count : '—'))),
      tile('Kp', '<span class="mono">' + T.escapeHtml(T.fmtGain(run.kp)) + '</span>'),
      tile('Ki', '<span class="mono">' + T.escapeHtml(T.fmtGain(run.ki)) + '</span>'),
      tile('Kd', '<span class="mono">' + T.escapeHtml(T.fmtGain(run.kd)) + '</span>')
    ];

    /* config snapshot */
    var cfg = [
      tile('Tolerance', T.escapeHtml(T.fmtGAbs(run.tolerance_g))),
      tile('Coarse transition', T.escapeHtml(T.fmtGAbs(run.coarse_transition_g))),
      tile('Settle', T.escapeHtml(run.settle_ms != null ? T.fmtDurationMs(run.settle_ms) : '—')),
      tile('Window', T.escapeHtml(run.window_ms != null ? T.fmtDurationMs(run.window_ms) : '—')),
      tile('Min on', T.escapeHtml(run.min_on_ms != null ? run.min_on_ms + ' ms' : '—')),
      tile('Min off', T.escapeHtml(run.min_off_ms != null ? run.min_off_ms + ' ms' : '—')),
      tile('Max overshoot', T.escapeHtml(T.fmtGAbs(run.max_overshoot_g))),
      tile('Max duration', T.escapeHtml(run.max_duration_ms != null ? T.fmtDurationMs(run.max_duration_ms) : '—')),
      tile('Correction limit', T.escapeHtml(run.correction_limit != null ? run.correction_limit : '—')),
      tile('Integral max', T.escapeHtml(T.fmtNum(run.integral_max, 1))),
      tile('Completion mode', T.escapeHtml(run.completion_mode || '—')),
      tile('Priority', T.escapeHtml(run.priority === 1 ? 'HIGH' : 'normal'))
    ];

    return '<section class="section" aria-label="Run statistics">' +
        '<div class="section-header"><h2 class="section-title">Statistics</h2></div>' +
        '<div class="stats-grid">' + tiles.join('') + '</div>' +
        '<div class="section-header"><h2 class="section-title">Config snapshot</h2></div>' +
        '<div class="stats-grid">' + cfg.join('') + '</div>' +
      '</section>';
  }

  function renderChartsSkeleton(hasSamples) {
    if (!hasSamples) { return '<div id="charts-area"></div>'; }
    return '<div id="charts-area" class="detail-charts-grid">' +
      '<div class="chart-panel"><h3>Weight vs Time</h3>' +
        '<div class="chart-box tall"><canvas id="chart-weight" role="img" aria-label="Weight vs time chart"></canvas></div></div>' +
      '<div class="chart-panel"><h3>Error vs Time</h3>' +
        '<div class="chart-box"><canvas id="chart-error" role="img" aria-label="Error vs time chart, grams"></canvas></div></div>' +
      '<div class="chart-panel"><h3>PID Output vs Time (fine duty 0–1)</h3>' +
        '<div class="chart-box"><canvas id="chart-pid" role="img" aria-label="PID output vs time chart"></canvas></div></div>' +
      '<div class="chart-panel"><h3>P / I / D Terms vs Time</h3>' +
        '<div class="chart-box"><canvas id="chart-terms" role="img" aria-label="PID terms vs time chart"></canvas></div></div>' +
      '<div class="chart-panel"><h3>Relay Activity (stepped, ON/OFF)</h3>' +
        '<div class="chart-box"><canvas id="chart-relays" role="img" aria-label="Relay 1 and relay 2 stepped binary traces"></canvas></div></div>' +
      '</div>';
  }

  function renderEvents(events) {
    if (!events || events.length === 0) {
      return '<section class="section" aria-label="Event timeline">' +
        '<div class="section-header"><h2 class="section-title">Event Timeline</h2></div>' +
        '<div class="state-box">No events recorded for this run.</div></section>';
    }

    var items = events.map(function (ev) {
      var cat = eventCategory(ev.event);
      var when = ev.timestamp ? T.fmtDateTime(ev.timestamp) : '—';
      var elapsed = ev.elapsed_ms != null ? ('t +' + T.fmtElapsedMs(ev.elapsed_ms)) : '';
      var bits = [];
      if (ev.state) { bits.push('state: <strong>' + T.escapeHtml(ev.state) + '</strong>'); }
      if (ev.weight_g != null) { bits.push('weight: ' + T.escapeHtml(T.fmtKg(ev.weight_g))); }
      if (ev.detail) { bits.push(T.escapeHtml(ev.detail)); }
      return '<li class="timeline-item">' +
        '<span class="timeline-dot ' + cat + '" aria-hidden="true"></span>' +
        '<div class="timeline-head">' +
          '<span class="badge ' + eventBadgeClass(ev.event) + '">' + T.escapeHtml(ev.event) + '</span>' +
          '<span class="timeline-time">' + T.escapeHtml(when) + (elapsed ? ' · ' + T.escapeHtml(elapsed) : '') + '</span>' +
        '</div>' +
        (bits.length ? '<div class="timeline-detail">' + bits.join(' · ') + '</div>' : '') +
        '</li>';
    });

    return '<section class="section" aria-label="Event timeline">' +
      '<div class="section-header"><h2 class="section-title">Event Timeline</h2>' +
        '<span class="section-meta">' + events.length + ' events</span></div>' +
      '<ol class="timeline">' + items.join('') + '</ol>' +
      '</section>';
  }

  /* ---------- load ---------- */

  async function load(runId) {
    try {
      var loaded = await Promise.all([T.apiGet('/runs/' + encodeURIComponent(runId) + '/full?max_points=0'),
        T.apiGet('/materials').catch(function(){return [];})]);
      var data = loaded[0];
      loaded[1].forEach(function(m){materialNames[m.material_id]=m.name;});
      var run = data.run || {};
      var samples = data.samples || [];
      var events = data.events || [];

      destroyCharts();
      document.title = 'Run ' + runId + ' — BITS Dispense Telemetry';
      root.innerHTML =
        renderHeader(run) +
        renderStats(run, samples.length || run.sample_count, events.length || run.event_count) +
        '<section class="section" aria-label="Charts">' +
          '<div class="section-header"><h2 class="section-title">Charts</h2>' +
            '<span class="section-meta">' + samples.length + ' samples' +
            (samples.length && samples[0].elapsed_ms != null ? ', ' + T.fmtElapsedMs(samples[samples.length - 1].elapsed_ms) + ' span' : '') +
            '</span></div>' +
          renderChartsSkeleton(samples.length > 0) +
        '</section>' +
        renderEvents(events);

      renderCharts(run, samples);
    } catch (err) {
      root.innerHTML = '';
      if (err && err.status === 404) {
        T.showState(root, 'Run not found',
          'No run with id "' + runId + '" is stored on this server.', true);
      } else {
        T.showError(root, err);
      }
    }
  }

  function init() {
    root = T.el('detail-root');
    if (!root) { return; }
    T.applyChartDefaults();
    T.startHealthIndicator();

    var runId = currentRunId();
    if (!runId) {
      root.innerHTML = '';
      T.showState(root, 'Missing run id',
        'This page must be opened as /runs/<run_id>.', true);
      return;
    }
    load(runId);
    window.addEventListener('beforeunload', destroyCharts);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
