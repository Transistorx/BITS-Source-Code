(function () {
  'use strict';

  const T = window.Telemetry;
  const TARGETS = [5000, 10000, 15000, 20000];
  const MATERIALS = {
    M1: { name: 'Material A', channel: 'CH1' },
    M2: { name: 'Material B', channel: 'CH2' },
  };
  const PALETTE = ['#32a6ff', '#2dd4bf', '#f59e0b', '#a78bfa', '#f87171', '#84cc16', '#22d3ee', '#fb7185', '#eab308', '#818cf8'];
  const states = new Map();
  let currentMaterial = 'M1';
  let refreshTimer = null;
  let compareChart = null;
  let refreshing = false;

  const $ = (id) => document.getElementById(id);
  const keyFor = (target) => `${currentMaterial}:${target}`;
  const groupState = (target) => {
    const key = keyFor(target);
    if (!states.has(key)) states.set(key, { runs: [], selected: 'latest', selectedRun: null, chart: null, total: 0 });
    return states.get(key);
  };
  const kg = (grams) => `${(Number(grams || 0) / 1000).toFixed(1)} kg`;
  const dateTime = (raw) => raw ? new Date(raw).toLocaleString() : '—';
  const selectedRun = (state) => {
    if (state.selected === 'latest') return state.runs.find((run) => run.status === 'COMPLETE' || run.status === 'COMPLETED') || state.runs[0] || null;
    return state.runs.find((run) => String(run.run_id) === String(state.selected)) || state.selectedRun;
  };

  function destroyChart(state) {
    if (state.chart) state.chart.destroy();
    state.chart = null;
  }

  function cardShell(target) {
    return `<article class="panel target-card" id="target-card-${target}" aria-labelledby="target-title-${target}">
      <div class="panel-head">
        <div><p class="eyebrow">Standard target</p><h2 id="target-title-${target}">${target / 1000} KG TESTS</h2></div>
        <span class="status-badge neutral">Loading</span>
      </div>
      <div class="loading-state"><span class="spinner" aria-hidden="true"></span><span>Loading recent tests…</span></div>
    </article>`;
  }

  function renderLoading() {
    $('target-grid').innerHTML = TARGETS.map(cardShell).join('');
  }

  function runLabel(run) {
    return `Test #${run.test_number || run.run_id} · ${dateTime(run.completed_at || run.started_at)}`;
  }

  function renderEmpty(target, total) {
    const state = groupState(target);
    destroyChart(state);
    $('target-card-' + target).outerHTML = `<article class="panel target-card target-empty" id="target-card-${target}" aria-labelledby="target-title-${target}">
      <div class="panel-head">
        <div><p class="eyebrow">Standard target</p><h2 id="target-title-${target}">${target / 1000} KG TESTS</h2></div>
        <span class="status-badge neutral">${total || 0} recorded</span>
      </div>
      <div class="empty-state compact"><strong>No ${kg(target)} tests recorded yet</strong><span>A graph will appear after the first completed ${kg(target)} dispense.</span></div>
    </article>`;
  }

  function renderError(target, message) {
    const state = groupState(target);
    destroyChart(state);
    $('target-card-' + target).outerHTML = `<article class="panel target-card" id="target-card-${target}" aria-labelledby="target-title-${target}">
      <div class="panel-head">
        <div><p class="eyebrow">Standard target</p><h2 id="target-title-${target}">${target / 1000} KG TESTS</h2></div>
        <span class="status-badge danger">Unavailable</span>
      </div>
      <div class="inline-error"><strong>Could not load this target</strong><span>${T.escapeHtml(message)}</span><button class="btn secondary small" data-retry-target="${target}">Retry</button></div>
    </article>`;
  }

  function renderRunCard(target) {
    /* One test entry = its graph, plus the minimal controls that choose which
     * run is drawn or overlaid. Run summary, metrics and detail links belong on
     * the History page — this workspace is for visualisation only. */
    const state = groupState(target);
    destroyChart(state);
    const run = selectedRun(state);
    state.selectedRun = run;
    if (!run) return renderEmpty(target, state.total);

    const choices = state.runs.map((item) => `<option value="${item.run_id}"${String(state.selected) === String(item.run_id) ? ' selected' : ''}>${T.escapeHtml(runLabel(item))}</option>`).join('');
    const pinned = state.selected !== 'latest' && !state.runs.some((item) => String(item.run_id) === String(state.selected))
      ? `<option value="${T.escapeHtml(state.selected)}" selected>Selected run #${T.escapeHtml(state.selected)}</option>` : '';

    $('target-card-' + target).outerHTML = `<article class="panel target-card" id="target-card-${target}" aria-labelledby="target-title-${target}">
      <div class="panel-head target-card-head">
        <div><p class="eyebrow">Standard target</p><h2 id="target-title-${target}">${target / 1000} KG TESTS</h2></div>
      </div>
      <div class="target-controls">
        <label class="form-field"><span>Displayed run</span><select data-run-select="${target}"><option value="latest"${state.selected === 'latest' ? ' selected' : ''}>Latest completed test</option>${pinned}${choices}</select></label>
        <button class="btn secondary small" data-compare-target="${target}" ${state.runs.length < 2 ? 'disabled' : ''}>Compare runs</button>
      </div>
      <div class="target-chart-host" id="target-chart-${target}"><div class="loading-state"><span class="spinner" aria-hidden="true"></span><span>Loading samples…</span></div></div>
    </article>`;
    loadSamples(target, run);
  }

  async function loadSamples(target, run) {
    const state = groupState(target);
    const host = $('target-chart-' + target);
    if (!host) return;
    try {
      const response = await T.apiGet(`/runs/${encodeURIComponent(run.run_id)}/samples?max_points=600`);
      if (state.selectedRun?.run_id !== run.run_id || currentMaterial !== run.material_id) return;
      const valid = (Array.isArray(response) ? response : response.samples || []).filter((sample) => Number(sample.target_g) === target || sample.target_g == null);
      if (!valid.length) {
        host.innerHTML = `<div class="empty-state compact"><strong>No sample points for this run</strong></div>`;
        return;
      }
      host.innerHTML = `<div class="target-chart-wrap"><canvas id="target-canvas-${target}" aria-label="Weight over time for ${kg(target)}"></canvas></div>`;
      state.chart = T.createWeightChart($('target-canvas-' + target), valid, target, {});
    } catch (error) {
      host.innerHTML = `<div class="inline-error"><strong>Samples unavailable</strong><span>${T.escapeHtml(error.message)}</span></div>`;
    }
  }

  async function loadTarget(target, total) {
    const requestedMaterial = currentMaterial;
    const state = groupState(target);
    try {
      const query = new URLSearchParams({ material_id: currentMaterial, channel_id: MATERIALS[currentMaterial].channel, target_g: String(target), limit: '10' });
      const result = await T.apiGet(`/runs/latest?${query}`);
      if (requestedMaterial !== currentMaterial) return;
      state.runs = Array.isArray(result) ? result : (result.items || []);
      state.total = Number.isFinite(Number(total)) ? Number(total) : state.runs.length;
      if (state.selected !== 'latest' && !state.runs.some((run) => String(run.run_id) === String(state.selected)) && !state.selectedRun) state.selected = 'latest';
      renderRunCard(target);
    } catch (error) {
      if (requestedMaterial === currentMaterial) renderError(target, error.message);
    }
  }

  async function refresh() {
    if (refreshing) return;
    refreshing = true;
    const materialAtStart = currentMaterial;
    try {
      let totals = {};
      try {
        const response = await T.apiGet('/targets');
        for (const item of (Array.isArray(response) ? response : response.items || [])) {
          if (item.material_id === currentMaterial && item.channel_id === MATERIALS[currentMaterial].channel) totals[Number(item.target_g)] = Number(item.run_count || item.count || 0);
        }
      } catch (_) { /* Latest lists still render if aggregate metadata is unavailable. */ }
      if (materialAtStart !== currentMaterial) return;
      await Promise.allSettled(TARGETS.map((target) => loadTarget(target, totals[target])));
      $('graphs-updated').textContent = `Updated ${new Date().toLocaleTimeString()}`;
    } finally {
      refreshing = false;
    }
  }

  function closeCompare() {
    const dialog = $('compare-dialog');
    if (compareChart) compareChart.destroy();
    compareChart = null;
    if (dialog?.open) dialog.close();
  }

  function openCompare(target) {
    const state = groupState(target);
    $('compare-title').textContent = `Compare ${kg(target)} runs`;
    $('compare-context').textContent = `${MATERIALS[currentMaterial].name} · ${MATERIALS[currentMaterial].channel} · choose two or more compatible tests`;
    $('compare-options').innerHTML = state.runs.map((run, index) => `<label class="compare-option"><input type="checkbox" value="${T.escapeHtml(run.run_id)}" data-compare-check="${target}" ${index < 2 ? 'checked' : ''}><span><strong>${T.escapeHtml(runLabel(run))}</strong></span></label>`).join('');
    $('comparison-chart').innerHTML = '<div class="empty-state compact"><strong>Ready to compare</strong><span>Select at least two runs, then build the overlay.</span></div>';
    $('compare-runs').dataset.target = String(target);
    $('compare-dialog').showModal();
  }

  async function buildComparison(button) {
    const target = Number(button.dataset.target);
    const state = groupState(target);
    const ids = [...document.querySelectorAll(`[data-compare-check="${target}"]:checked`)].map((input) => input.value);
    if (ids.length < 2) return T.toast('Select at least two runs to compare.', 'warning');
    T.setButtonBusy(button, true, 'Building chart…');
    try {
      const responses = await Promise.all(ids.map((id) => T.apiGet(`/runs/${encodeURIComponent(id)}/samples?max_points=600`)));
      const datasets = [];
      responses.forEach((response, index) => {
        const run = state.runs.find((item) => String(item.run_id) === String(ids[index]));
        const points = (Array.isArray(response) ? response : response.samples || []).filter((sample) => sample.target_g == null || Number(sample.target_g) === target);
        if (!run || run.material_id !== currentMaterial || run.channel_id !== MATERIALS[currentMaterial].channel || Number(run.target_g) !== target || !points.length) return;
        datasets.push({ label: `Test #${run.test_number || run.run_id}`, data: points.map((point) => ({ x: Number(point.elapsed_ms) / 1000, y: Number(point.weight_g) / 1000 })), borderColor: PALETTE[index % PALETTE.length], backgroundColor: 'transparent', pointRadius: 0, borderWidth: 2, tension: 0.15 });
      });
      if (datasets.length < 2) throw new Error('Two compatible runs with samples are required.');
      if (compareChart) compareChart.destroy();
      $('comparison-chart').innerHTML = '<div class="compare-chart-wrap"><canvas id="compare-canvas"></canvas></div>';
      datasets.push({ label: `Target ${kg(target)}`, data: [{ x: 0, y: target / 1000 }, { x: Math.max(...datasets.flatMap((set) => set.data.map((point) => point.x))), y: target / 1000 }], borderColor: '#94a3b8', borderDash: [6, 5], pointRadius: 0, borderWidth: 1.5 });
      compareChart = new Chart($('compare-canvas'), { type: 'line', data: { datasets }, options: { responsive: true, maintainAspectRatio: false, parsing: false, interaction: { mode: 'nearest', intersect: false }, scales: { x: { type: 'linear', title: { display: true, text: 'Elapsed time (s)' } }, y: { title: { display: true, text: 'Weight (kg)' }, suggestedMin: 0, suggestedMax: target / 1000 * 1.15 } } } });
    } catch (error) {
      T.toast(error.message, 'error', 6500);
      $('comparison-chart').innerHTML = `<div class="inline-error"><strong>Comparison unavailable</strong><span>${T.escapeHtml(error.message)}</span></div>`;
    } finally {
      T.setButtonBusy(button, false);
    }
  }

  function switchMaterial(material) {
    if (!MATERIALS[material] || currentMaterial === material) return;
    closeCompare();
    currentMaterial = material;
    document.querySelectorAll('[data-material]').forEach((button) => {
      const active = button.dataset.material === material;
      button.classList.toggle('active', active);
      button.setAttribute('aria-selected', String(active));
    });
    $('graphs-context').textContent = `${MATERIALS[material].name} · ${MATERIALS[material].channel}`;
    renderLoading();
    refresh();
  }

  document.addEventListener('click', (event) => {
    const material = event.target.closest('[data-material]');
    if (material) return switchMaterial(material.dataset.material);
    const retry = event.target.closest('[data-retry-target]');
    if (retry) return loadTarget(Number(retry.dataset.retryTarget));
    const compare = event.target.closest('[data-compare-target]');
    if (compare) return openCompare(Number(compare.dataset.compareTarget));
    if (event.target.closest('[data-close-compare]')) return closeCompare();
    const build = event.target.closest('#compare-runs');
    if (build) buildComparison(build);
  });

  document.addEventListener('change', (event) => {
    const select = event.target.closest('[data-run-select]');
    if (!select) return;
    const target = Number(select.dataset.runSelect);
    const state = groupState(target);
    state.selected = select.value;
    state.selectedRun = selectedRun(state);
    renderRunCard(target);
  });

  window.addEventListener('beforeunload', () => {
    if (refreshTimer) refreshTimer();
    states.forEach(destroyChart);
    if (compareChart) compareChart.destroy();
  });

  T.applyChartDefaults();
  T.startHealthIndicator();
  renderLoading();
  refresh();
  refreshTimer = T.visibleInterval(refresh, 15000);
  window.BitsGraphs = { targets: TARGETS.slice(), refresh };
})();
