/* ============================================================
   History page (/history) — run records, automatic filters, and
   a per-run Delete action.
   Requires: common.js (window.Telemetry). No charts here.

   Filters apply themselves: selects and dates refresh on change, free
   text debounces. Every request is abortable and carries a sequence
   token, so a slow older response can never overwrite a newer one.
   ============================================================ */
(function () {
  'use strict';

  var T = window.Telemetry;
  var PAGE_SIZE = 50;
  var DEBOUNCE_MS = 400;
  var materialNames = {M1:'Material A', M2:'Material B'};

  var form = null;
  var root = null;
  var pagination = null;
  var prevBtn = null;
  var nextBtn = null;
  var pageInfo = null;

  /* requestSeq is the authority on which response may be rendered; abort
   * merely stops work we no longer need. Both are required: abort alone
   * races on platforms that resolve an already-settled fetch. */
  var state = { offset: 0, total: 0, requestSeq: 0, inFlight: null, debounceTimer: null };

  /* ---------- filter helpers ---------- */

  function buildQuery(offset) {
    var params = new URLSearchParams();
    var channel = T.el('filter-channel').value;
    var material = T.el('filter-material').value;
    var target = T.el('filter-target').value;
    var status = T.el('filter-status').value;
    var from = T.localInputToIso(T.el('filter-from').value);
    var to = T.localInputToIso(T.el('filter-to').value);
    var runId = T.el('filter-run-id').value.trim();
    var profileId = T.el('filter-profile').value.trim();
    var profileVersion = T.el('filter-version').value;

    if (target) {
      var splitTarget = target.split(':');
      if (splitTarget.length === 2) {
        if (!channel) { channel = splitTarget[0]; }
        target = splitTarget[1];
      }
    }
    if (channel) { params.set('channel_id', channel); }
    if (material) { params.set('material_id', material); }
    if (target) { params.set('target_g', target); }
    if (status) { params.set('status', status); }
    if (from) { params.set('from', from); }
    if (to) { params.set('to', to); }
    if (runId) { params.set('run_id', runId); }
    if (profileId) { params.set('profile_id', profileId); }
    if (profileVersion) { params.set('profile_version', profileVersion); }
    params.set('limit', String(PAGE_SIZE));
    params.set('offset', String(offset));
    return params.toString();
  }

  async function populateTargets() {
    var select = T.el('filter-target');
    if (!select) { return; }
    try {
      var targets = await T.apiGet('/targets');
      targets.forEach(function (t) {
        var opt = document.createElement('option');
        opt.value = String(t.channel_id) + ':' + String(t.target_g);
        opt.textContent = (materialNames[t.material_id] || t.material_id) + ' · ' + t.channel_id + ' · ' + T.fmtTargetKg(t.target_g) + ' (' + t.run_count + ' runs)';
        select.appendChild(opt);
      });
    } catch (err) {
      /* keep the "Any target" option; filtering still works */
    }
  }

  /* ---------- results table ---------- */

  function rowHtml(run) {
    var detail = '/runs/' + encodeURIComponent(run.run_id);
    /* The row is addressed by run_id — the dispense_runs primary key — never
     * by the displayed test_number, which repeats across targets and materials.
     * data-label is what the mobile-cards layout renders as the cell heading. */
    return '<tr class="clickable" data-href="' + T.escapeHtml(detail) + '" tabindex="0" ' +
        'role="link" aria-label="Open run ' + T.escapeHtml(run.run_id) + '">' +
      '<td data-label="Start">' + T.escapeHtml(T.fmtDateTime(run.started_at)) + '</td>' +
      '<td data-label="Material">' + T.escapeHtml((materialNames[run.material_id] || run.material_id) + ' · ' + run.channel_id + ' · Pump ' + (run.channel_id === 'CH1' ? '1' : '2')) + '</td>' +
      '<td data-label="Target">' + T.escapeHtml(T.fmtTargetKg(run.target_g)) + '</td>' +
      '<td data-label="PID profile">' + (run.profile_id
        ? '<span title="' + T.escapeHtml((materialNames[run.material_id] || run.material_id) + ' · Pump ' + (run.channel_id === 'CH1' ? '1' : '2') +
            (run.profile_version_id ? ' · profile_version_id ' + run.profile_version_id : '')) + '">' +
          T.escapeHtml(run.profile_id) + ' v' + T.escapeHtml(run.profile_version) + '</span>'
        : '<span class="muted">—</span>') + '</td>' +
      '<td data-label="Test #">' + T.escapeHtml(run.test_number != null ? run.test_number : '—') + '</td>' +
      '<td class="mono" data-label="Run ID"><a href="' + T.escapeHtml(detail) + '" title="' + T.escapeHtml(run.run_id) + '">' +
        T.escapeHtml(T.shortId(run.run_id, 24)) + '</a></td>' +
      '<td data-label="Status">' + T.statusBadge(run.status) + '</td>' +
      '<td class="num" data-label="Final weight">' + T.escapeHtml(T.fmtKg(run.final_weight_g)) + '</td>' +
      '<td class="num" data-label="Error">' + T.escapeHtml(T.fmtG(run.final_error_g)) +
        (run.final_error_pct != null ? ' (' + T.escapeHtml(T.fmtPct(run.final_error_pct)) + ')' : '') + '</td>' +
      '<td class="num" data-label="Overshoot">' + T.escapeHtml(T.fmtGAbs(run.overshoot_g)) + '</td>' +
      '<td class="num" data-label="Duration">' + T.escapeHtml(T.fmtDurationMs(run.duration_ms)) + '</td>' +
      '<td class="actions" data-label="Actions">' +
        '<button type="button" class="btn danger small" data-delete="' + T.escapeHtml(run.run_id) + '" ' +
          'data-delete-label="' + T.escapeHtml(deleteLabel(run)) + '" ' +
          'aria-label="Delete run ' + T.escapeHtml(run.run_id) + '">Delete</button>' +
      '</td>' +
      '</tr>';
  }

  function deleteLabel(run) {
    return (materialNames[run.material_id] || run.material_id) +
      ' · ' + run.channel_id +
      ' · ' + T.fmtTargetKg(run.target_g) +
      (run.test_number != null ? ' · test #' + run.test_number : '') +
      ' · ' + T.fmtDateTime(run.started_at) +
      ' · ' + run.run_id;
  }

  function renderResults(items) {
    if (!items || items.length === 0) {
      root.innerHTML = '';
      T.showState(root, 'No matching runs',
        state.total === 0
          ? 'No stored runs match the current filters (or the database is empty yet).'
          : 'This page is beyond the last result — go back a page.');
      pagination.hidden = true;
      return;
    }

    root.innerHTML =
      '<div class="table-wrap mobile-cards">' +
      '<table class="data" id="results-table">' +
        '<thead><tr>' +
          '<th>Start (local)</th><th>Material / channel</th><th>Target</th><th>PID profile</th><th>Test #</th><th>Run ID</th>' +
          '<th>Status</th><th class="num">Final weight</th><th class="num">Error g (%)</th>' +
          '<th class="num">Overshoot</th><th class="num">Duration</th><th>Actions</th>' +
        '</tr></thead>' +
        '<tbody>' + items.map(rowHtml).join('') + '</tbody>' +
      '</table></div>';

    var table = T.el('results-table');
    table.addEventListener('click', function (e) {
      if (e.target.closest('a')) { return; } /* let the run_id link navigate */
      /* A Delete click is an action, not a navigation. */
      if (e.target.closest('[data-delete]')) { return; }
      var tr = e.target.closest('tr.clickable');
      if (tr && tr.dataset.href) { window.location.href = tr.dataset.href; }
    });
    table.addEventListener('keydown', function (e) {
      if (e.key !== 'Enter' && e.key !== ' ') { return; }
      if (e.target.closest('[data-delete]')) { return; }
      var tr = e.target.closest('tr.clickable');
      if (tr && tr.dataset.href) {
        e.preventDefault();
        window.location.href = tr.dataset.href;
      }
    });

    updatePagination();
  }

  function updatePagination() {
    var first = state.total === 0 ? 0 : state.offset + 1;
    var last = Math.min(state.offset + PAGE_SIZE, state.total);
    pageInfo.textContent = 'Showing ' + first + '–' + last + ' of ' + state.total;
    prevBtn.disabled = state.offset <= 0;
    nextBtn.disabled = state.offset + PAGE_SIZE >= state.total;
    pagination.hidden = false;
  }

  /* ---------- search (stale-safe) ---------- */

  async function search(offset) {
    /* Cancel whatever is still in flight and claim a new sequence number.
     * Only the response carrying the current sequence may render, so a slow
     * request that started before a filter change can never overwrite it. */
    if (state.inFlight) {
      try { state.inFlight.abort(); } catch (ignored) { /* already gone */ }
      state.inFlight = null;
    }
    var seq = ++state.requestSeq;
    var controller = new AbortController();
    state.inFlight = controller;

    T.showLoading(root, 'Searching runs…');
    pagination.hidden = true;
    try {
      var data = await T.apiGetWithSignal('/runs?' + buildQuery(offset), controller.signal);
      if (seq !== state.requestSeq) { return; } /* obsolete — a newer filter won */
      state.offset = data.offset != null ? data.offset : offset;
      state.total = data.total != null ? data.total : (data.items || []).length;
      renderResults(data.items || []);
    } catch (err) {
      if (seq !== state.requestSeq) { return; }
      if (err && err.name === 'AbortError') { return; }
      root.innerHTML = '';
      T.showError(root, err);
    } finally {
      if (state.inFlight === controller) { state.inFlight = null; }
    }
  }

  /* ---------- delete ---------- */

  function deleteRun(button) {
    var runId = button.getAttribute('data-delete');
    var label = button.getAttribute('data-delete-label') || runId;
    var message =
      'Delete this test run?\n\n' + label +
      '\n\nThis removes the run and its own weight samples and dispense events. ' +
      'It cannot be undone. Tuning profiles and every other run are left untouched.';
    if (!window.confirm(message)) { return; }

    T.setButtonBusy(button, true, 'Deleting…');
    T.apiWrite('/runs/' + encodeURIComponent(runId), { method: 'DELETE' })
      .then(function () {
        T.toast('Run deleted.', 'success');
        /* Re-query with the filters exactly as they stand. The brief requires
         * the list to update in place with no page reload and no loss of the
         * operator's active filters. If this was the last row on the last page,
         * step back one page so the view is not left on an empty leaf. */
        if (state.total > 0 && state.offset >= state.total - 1) {
          search(Math.max(0, state.offset - PAGE_SIZE));
        } else {
          search(state.offset);
        }
      })
      .catch(function (error) {
        T.toast(error.message, 'error', true);
      })
      .finally(function () {
        T.setButtonBusy(button, false);
      });
  }

  /* ---------- filter wiring ---------- */

  function applyNow() {
    if (state.debounceTimer) {
      clearTimeout(state.debounceTimer);
      state.debounceTimer = null;
    }
    search(0);
  }

  function applyDebounced() {
    if (state.debounceTimer) { clearTimeout(state.debounceTimer); }
    state.debounceTimer = window.setTimeout(function () {
      state.debounceTimer = null;
      search(0);
    }, DEBOUNCE_MS);
  }

  function resetFilters() {
    form.reset();
    applyNow();
  }

  function wireFilters() {
    /* Selects and dates are discrete choices: refresh on change. Free-text
     * fields refresh on a short debounce so typing does not flood the server,
     * while still feeling immediate. Everything is cumulative — changing one
     * filter preserves all the others, because buildQuery reads them all. */
    var discrete = ['filter-material', 'filter-channel', 'filter-target',
                    'filter-status', 'filter-from', 'filter-to', 'filter-version'];
    discrete.forEach(function (id) {
      var node = T.el(id);
      if (node) { node.addEventListener('change', applyNow); }
    });

    var freeText = ['filter-run-id', 'filter-profile'];
    freeText.forEach(function (id) {
      var node = T.el(id);
      if (node) {
        node.addEventListener('input', applyDebounced);
        node.addEventListener('change', applyNow);
      }
    });

    T.el('reset-filters').addEventListener('click', resetFilters);

    /* Enter in a free-text field must not submit the form and reload the page. */
    form.addEventListener('submit', function (e) {
      e.preventDefault();
      applyNow();
    });
  }

  /* ---------- init ---------- */

  function init() {
    form = T.el('filter-form');
    root = T.el('results-root');
    pagination = T.el('pagination');
    prevBtn = T.el('prev-btn');
    nextBtn = T.el('next-btn');
    pageInfo = T.el('page-info');
    if (!form || !root) { return; }

    T.startHealthIndicator();
    T.apiGet('/materials').then(function(rows){rows.forEach(function(m){materialNames[m.material_id]=m.name;});
      var select=T.el('filter-material');select.innerHTML='<option value="">All materials</option>'+rows.map(function(m){return '<option value="'+T.escapeHtml(m.material_id)+'">'+T.escapeHtml(m.name)+' · '+T.escapeHtml(m.pump_id)+'</option>';}).join('');
      populateTargets();
    }).catch(function(){populateTargets();});

    wireFilters();

    /* Delegated so the Delete buttons work however the rows are re-rendered. */
    document.addEventListener('click', function (e) {
      var button = e.target.closest('[data-delete]');
      if (button) {
        e.preventDefault();
        e.stopPropagation();
        deleteRun(button);
      }
    });

    prevBtn.addEventListener('click', function () {
      search(Math.max(0, state.offset - PAGE_SIZE));
    });
    nextBtn.addEventListener('click', function () {
      search(state.offset + PAGE_SIZE);
    });

    search(0);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
