(function () {
  'use strict';

  var T = window.Telemetry;
  var STANDARD_TARGETS = [5000, 10000, 15000, 20000];
  var profiles = [];
  var deviceChannels = [];
  var selectedProfile = null;
  /* When set, the form is loading values out of an existing immutable version
   * and the save button becomes "Save as vN+1". Saving always INSERTs a new
   * row — there is no PUT — so a historical version is never rewritten and
   * jobs already pinned to it keep their exact numbers. */
  var editingSource = null;
  /* Operator labels for the two fixed pumps (GET /materials). Falls back to the
   * material id if that request fails. */
  var materialNames = {};
  /* Server-computed next versions, keyed "profile_id|M1,M2" -> { M1: n, M2: m }.
   * false = the preview request failed (the button then shows no number). The
   * generation counter drops answers that were requested before a save/reload. */
  var nextCache = {};
  var nextGen = 0;
  var previewTimer = null;

  function esc(value, fallback) { return T.escapeHtml(value === null || value === undefined || value === '' ? (fallback || '—') : value); }
  /* Number('') === 0 would render a missing target as "0.0 kg". Coerce to NaN
   * first so the non-finite path can say 'No target'; a genuine 0 stays 0.0 kg. */
  function kg(grams) {
    var g = (grams === null || grams === undefined || grams === '') ? NaN : Number(grams);
    return Number.isFinite(g) ? (g / 1000).toFixed(1) + ' kg' : 'No target';
  }
  function post(path, payload) {
    return T.apiWrite(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload || {}) });
  }
  function material() { return T.el('material').value; }
  function channel() { return material() === 'M1' ? 'CH1' : 'CH2'; }
  // Canonical targets only: the API rejects 0/NULL so the UI never offers them.
  function selectedTarget() {
    var value = Number(T.el('target').value);
    return STANDARD_TARGETS.indexOf(value) >= 0 ? value : null;
  }
  /* profile_id + version is NOT unique: both pumps may own "default v8". The
   * immutable surrogate profile_version_id is the row key everywhere. */
  function profileKey(profile) { return String(profile.profile_version_id); }
  function pumpOf(materialId) { return materialId === 'M1' ? 'Pump 1' : 'Pump 2'; }
  function materialContext(materialId) {
    return (materialNames[materialId] || materialId) + ' · ' + pumpOf(materialId);
  }
  /* "Material A · Pump 1 · default v8" */
  function contextLabel(profile) {
    return materialContext(profile.material_id) + ' · ' + profile.profile_id + ' v' + profile.version;
  }
  /* The device echoes only profile_id + version for its channel; the channel_id
   * match below is what makes this specific to one pump. */
  function deviceProfile(profile) {
    var live = deviceChannels.find(function (item) { return item.channel_id === profile.channel_id; });
    return Boolean(live && live.profile_id === profile.profile_id && Number(live.profile_version) === Number(profile.version));
  }
  /* Reachable row badges: APPLIED when the device is already running this
   * exact version, PENDING DEVICE when the version is staged as active but not
   * yet on the device, HISTORICAL for every other immutable version. Tone names
   * stay the live 'success'/'warning'/'neutral' set. deviceProfile is tested
   * first: a version the device has already adopted is applied regardless of
   * its local active flag. */
  function profileStatus(profile) {
    if (deviceProfile(profile)) return { label: 'APPLIED', tone: 'success' };
    if (profile.active) return { label: 'PENDING DEVICE', tone: 'warning' };
    return { label: 'HISTORICAL', tone: 'neutral' };
  }

  function updateTargetOptions() {
    var select = T.el('target');
    var current = select.value;
    // Only the four canonical targets; no "Material default"/NULL fallback.
    select.innerHTML = STANDARD_TARGETS.map(function (target) { return '<option value="' + target + '">' + kg(target) + '</option>'; }).join('');
    if ([].some.call(select.options, function (option) { return option.value === current; })) select.value = current;
    else select.value = String(STANDARD_TARGETS[0]);
  }

  function updateActiveSummary() {
    var target = selectedTarget();
    var candidates = profiles.filter(function (profile) { return profile.material_id === material() && profile.channel_id === channel() && profile.active && Number(profile.target_g) === target; });
    candidates.sort(function (a, b) {
      return Number(b.version) - Number(a.version);
    });
    var active = candidates[0];
    var root = T.el('active-profile-summary');
    if (!active) {
      root.innerHTML = '<span class="status-badge neutral">NO ACTIVE PROFILE</span>' +
        '<span>No active version for ' + esc(kg(target)) + '.</span>';
      return;
    }
    var state = profileStatus(active);
    root.innerHTML = '<span class="status-badge ' + state.tone + '">' + state.label + '</span><span><strong>' + esc(active.profile_id) + ' v' + active.version + '</strong> · ' + esc(kg(active.target_g)) + ' · Kp/Ki/Kd ' + active.kp + '/' + active.ki + '/' + active.kd + '</span>';
  }

  /* Kept as a named function so the contract test can capture it. renderProfiles'
   * map callback contains a nested `function `, which silently truncates a
   * split("function ", 1)[0] capture — do not inline this back.
   * Delete is available only on non-active rows: the API rejects deleting an
   * active version (409), so showing the button there would be a dead
   * affordance. Same pattern as queue.js hiding Pause/Resume when no job runs. */
  function profileActions(profile) {
    var id = esc(profile.profile_id);
    var v = profile.version;
    var vid = esc(profile.profile_version_id);
    return '<td data-label="Actions" class="actions">' +
      '<button class="btn secondary sm" data-select-profile="' + vid + '">Inspect</button> ' +
      '<button class="btn secondary sm" data-edit-profile="' + vid + '">Edit</button> ' +
      (profile.active
        ? '<button class="btn secondary sm" data-profile-action="deactivate" data-version-id="' + vid + '" data-profile-id="' + id + '" data-version="' + v + '">Deactivate</button>'
        : '<button class="btn sm" data-profile-action="activate" data-version-id="' + vid + '" data-profile-id="' + id + '" data-version="' + v + '">Apply next job</button>') +
      (profile.active
        ? ''
        : ' <span class="action-cluster">' +
            '<button class="btn ghost sm" data-profile-action="delete" data-version-id="' + vid + '" data-profile-id="' + id + '" data-version="' + v + '">Delete</button>' +
          '</span>') +
    '</td>';
  }

  function renderProfiles() {
    var root = T.el('profile-root');
    var rows = profiles.filter(function (profile) { return profile.material_id === material(); });
    if (!rows.length) {
      root.innerHTML = '<div class="empty-state compact"><strong>No profiles for this material</strong><span>Save the first immutable version with the form above.</span></div>';
      return;
    }
    root.innerHTML = '<div class="table-wrap"><table class="data mobile-cards"><thead><tr><th>Profile</th><th>Target</th><th>PID gains</th><th>Relay timing</th><th>Created</th><th>Status</th><th>Actions</th></tr></thead><tbody>' + rows.map(function (profile) {
      var state = profileStatus(profile);
      var selected = selectedProfile && profileKey(selectedProfile) === profileKey(profile);
      return '<tr class="' + (selected ? 'selected-row' : '') + '"><td data-label="Profile"><button class="text-button" data-select-profile="' + esc(profile.profile_version_id) + '"><strong>' + esc(profile.profile_id) + '</strong> v' + profile.version + '</button></td><td data-label="Target">' + esc(kg(profile.target_g)) + '</td><td data-label="PID gains">' + profile.kp + ' / ' + profile.ki + ' / ' + profile.kd + '</td><td data-label="Relay timing">' + profile.window_ms + ' ms · ' + profile.min_on_ms + '/' + profile.min_off_ms + ' ms</td><td data-label="Created">' + esc(T.fmtDateTime(profile.created_at)) + '</td><td data-label="Status"><span class="status-badge ' + state.tone + '">' + state.label + '</span></td>' + profileActions(profile) + '</tr>';
    }).join('') + '</tbody></table></div>';
  }

  function fillForm(profile) {
    T.el('material').value = profile.material_id;
    updateTargetOptions();
    T.el('target').value = profile.target_g === null ? '' : String(profile.target_g);
    T.el('profile-id').value = profile.profile_id;
    T.el('kp').value = profile.kp;
    T.el('ki').value = profile.ki;
    T.el('kd').value = profile.kd;
    T.el('window').value = profile.window_ms;
    T.el('min-on').value = profile.min_on_ms;
    T.el('min-off').value = profile.min_off_ms;
    T.el('tolerance').value = profile.tolerance_g;
    T.el('overshoot').value = profile.max_overshoot_g;
    T.el('timeout').value = profile.max_duration_ms;
    updateActiveSummary();
  }

  /* Single renderer for the save label AND the edit banner. Always derived from
   * live state (checkbox, edit source, typed profile_id, loaded versions);
   * call it after anything that changes one of those. */
  function updateEditBanner() {
    var message = T.el('tuning-message');
    var info = renderSaveButton();
    if (!editingSource) {
      /* Only clear our own edit banner; a "Saved ..." / error status set by the
       * submit handler must survive the post-save banner refresh. */
      if (message.className.indexOf('banner-edit') >= 0) message.textContent = '';
      message.className = 'small muted';
      return;
    }
    var dual = info.dual;
    var id = info.id;
    /* Numbers come from GET /profiles/next-version; '?' = not known (yet). */
    var next = info.next === null ? '?' : info.next;
    var nextM2 = info.nextM2 === null ? '?' : info.nextM2;
    message.className = 'small banner-edit';
    message.textContent = 'Editing ' + materialContext(editingSource.material_id) + ' · ' +
      editingSource.profile_id + ' v' +
      editingSource.version + ' — saving creates v' + next +
      (dual ? ' (M1) and v' + nextM2 + ' (M2)' : '') +
      (id !== editingSource.profile_id ? ' as ' + id : '') +
      ' and historical runs remain linked to v' + editingSource.version + '.';
  }

  function bothChecked() {
    return Boolean(T.el('apply-both') && T.el('apply-both').checked);
  }
  /* The pumps a save would write: both when "Apply to both" is ticked. */
  function targetMaterials() { return bothChecked() ? ['M1', 'M2'] : [material()]; }

  /* Button label only (never touches the status line, so an error or "Saved"
   * message survives). Derived from the checkbox, edit source, typed profile_id,
   * selected pump and the server's next-version preview. Each pump has its own
   * counter, so dual mode can read "Save as M1 v8 / M2 v5". With no preview
   * (not loaded or failed) it shows no number: a guess is never displayed. */
  function renderSaveButton() {
    var button = T.el('save-profile');
    var dual = bothChecked();
    var id = T.el('profile-id').value.trim() || (editingSource ? editingSource.profile_id : '');
    /* A busy button shows "Saving…"; the submit handler re-renders afterwards. */
    var busy = button.disabled;
    if (!/^[A-Za-z0-9._-]{1,64}$/.test(id)) {
      if (!busy) button.textContent = 'Save New Version';
      return { id: id, next: null, nextM2: null, dual: dual };
    }
    var mats = targetMaterials();
    var next = nextVersionFor(id, mats[0]);
    var nextM2 = dual ? nextVersionFor(id, 'M2') : null;
    if (next === null || (dual && nextM2 === null)) {
      if (!busy) button.textContent = 'Save New Version';
      schedulePreview(id, mats);
      return { id: id, next: null, nextM2: null, dual: dual };
    }
    if (!busy) button.textContent = 'Save as v' + next;
    if (dual && !busy) {
      button.textContent = next === nextM2
        ? 'Save as v' + next + ' (both pumps)'
        : 'Save as M1 v' + next + ' / M2 v' + nextM2;
    }
    return { id: id, next: next, nextM2: nextM2, dual: dual };
  }

  function previewKey(profileId, mats) { return profileId + '|' + mats.join(','); }

  /* The next version for one pump, from the server's own rule (the same service
   * that numbers the save), or null when unknown. */
  function nextVersionFor(profileId, materialId) {
    var found = nextCache[previewKey(profileId, targetMaterials())];
    return found && found[materialId] !== undefined ? Number(found[materialId]) : null;
  }

  /* Fetch GET /profiles/next-version once per (profile_id, pumps) state, debounced
   * so typing does not spam the server. The answer re-renders the button. */
  function schedulePreview(profileId, mats) {
    var key = previewKey(profileId, mats);
    if (Object.prototype.hasOwnProperty.call(nextCache, key)) return;
    window.clearTimeout(previewTimer);
    previewTimer = window.setTimeout(function () {
      var gen = nextGen;
      if (Object.prototype.hasOwnProperty.call(nextCache, key)) return;
      T.apiGet('/profiles/next-version?profile_id=' + encodeURIComponent(profileId) + '&materials=' + mats.join(',')).then(function (result) {
        if (gen === nextGen) nextCache[key] = result && result.next_version ? result.next_version : false;
      }).catch(function () {
        if (gen === nextGen) nextCache[key] = false;
      }).then(function () {
        if (gen === nextGen) renderSaveButton();
      });
    }, 200);
  }

  /* Edit loads an existing immutable version into the form. The save below is
   * still a plain INSERT of max(version)+1: there is no update endpoint, so a
   * historical version can never be rewritten and every job/run that pinned it
   * stays reproducible. */
  function startEdit(profile) {
    selectedProfile = profile;
    editingSource = profile;
    fillForm(profile);
    renderProfiles();
    updateEditBanner();
    T.el('profile-id').readOnly = false;
  }

  async function selectProfile(profile) {
    selectedProfile = profile;
    /* Inspect is not an edit: drop any stale edit source so the button and
     * banner do not keep describing a different version than the form shows. */
    editingSource = null;
    fillForm(profile);
    renderProfiles();
    updateEditBanner();
    var state = profileStatus(profile);
    var summary = T.el('selected-profile-summary');
    summary.hidden = false;
    summary.innerHTML = '<div class="panel-head"><div><p class="eyebrow">Selected immutable version</p><h2>' + esc(profile.profile_id) + ' v' + profile.version + '</h2></div><span class="status-badge ' + state.tone + '">' + state.label + '</span></div>' +
      '<div class="profile-summary-grid"><div class="metric"><span>Scope</span><strong>' + esc(materialContext(profile.material_id)) + ' · ' + esc(profile.channel_id) + ' · ' + esc(kg(profile.target_g)) + '</strong></div><div class="metric"><span>PID gains</span><strong>' + profile.kp + ' / ' + profile.ki + ' / ' + profile.kd + '</strong></div><div class="metric"><span>Tolerance</span><strong>±' + profile.tolerance_g + ' g</strong></div><div class="metric"><span>Overshoot limit</span><strong>' + profile.max_overshoot_g + ' g</strong></div><div class="metric"><span>Control window</span><strong>' + profile.window_ms + ' ms</strong></div><div class="metric"><span>Maximum time</span><strong>' + T.fmtDurationMs(profile.max_duration_ms) + '</strong></div><div id="selected-performance" class="metric wide"><span>Measured performance</span><strong>Loading…</strong></div></div>';
    await loadResults(profile);
  }

  function metric(label, value) {
    return '<div class="metric"><span>' + esc(label) + '</span><strong>' +
           esc(value) + '</strong></div>';
  }

  function resultCard(run) {
    var finalG = (run.final_weight_g === null || run.final_weight_g === undefined) ? NaN : Number(run.final_weight_g);
    var errG = (run.final_error_g === null || run.final_error_g === undefined) ? NaN : Number(run.final_error_g);
    return '<a class="result-card" href="/runs/' + encodeURIComponent(run.run_id) + '">' +
      '<span><strong>Test #' + esc(run.test_number) + '</strong><small>' +
        esc(T.fmtDateTime(run.completed_at || run.started_at)) + '</small></span>' +
      '<span><strong>' + (Number.isFinite(finalG) ? finalG.toFixed(1) + ' g' : '—') + '</strong>' +
        '<small>Error ' + (Number.isFinite(errG) ? errG.toFixed(1) + ' g' : '—') + '</small></span>' +
      '<span class="status-badge ' + ((run.status === 'COMPLETE' || run.status === 'COMPLETED') ? 'success' : 'warning') +
        '">' + esc(run.status) + '</span></a>';
  }

  /* Owns the #profile-results panel: empty states and the real-metric summary.
   * Defined before loadResults so the contract test can bound its capture by
   * "function loadResults" (renderResults contains nested `function ` tokens).
   * Averages only over runs whose field is finite — never Number(x || 0). */
  function renderResults(profile, runs) {
    var host = T.el('profile-results');
    if (!profile) {
      host.innerHTML = '<div class="empty-state compact"><strong>Select a profile version</strong>' +
        '<span>Its latest test results and tuning metrics will appear here. ' +
        'Run a test from the Queue page to record one.</span></div>';
      return;
    }
    if (!runs || !runs.length) {
      host.innerHTML = '<div class="empty-state compact"><strong>No tests yet for ' +
        esc(profile.profile_id) + ' v' + profile.version + '</strong>' +
        '<span>Queue a job with this exact profile to measure it.</span></div>';
      return;
    }
    function avgOf(rows, pick) {
      var vals = rows.map(pick).filter(Number.isFinite);
      return vals.length ? vals.reduce(function (s, v) { return s + v; }, 0) / vals.length : null;
    }
    var complete = runs.filter(function (r) { return r.status === 'COMPLETE' || r.status === 'COMPLETED'; });
    var avgErr = avgOf(complete, function (r) {
      return (r.final_error_g === null || r.final_error_g === undefined) ? NaN : Math.abs(Number(r.final_error_g));
    });
    var avgOver = avgOf(complete, function (r) {
      return (r.overshoot_g === null || r.overshoot_g === undefined) ? NaN : Number(r.overshoot_g);
    });
    var avgDur = avgOf(complete, function (r) {
      return (r.duration_ms === null || r.duration_ms === undefined) ? NaN : Number(r.duration_ms);
    });
    var rate = runs.length ? (complete.length * 100 / runs.length) : null;
    host.innerHTML =
      '<div class="results-summary metrics-grid">' +
        metric('Test count', String(runs.length)) +
        metric('Average error', avgErr === null ? '—' : avgErr.toFixed(1) + ' g') +
        metric('Average overshoot', avgOver === null ? '—' : avgOver.toFixed(1) + ' g') +
        metric('Average dispense time', avgDur === null ? '—' : (avgDur / 1000).toFixed(1) + ' s') +
        metric('Success rate', rate === null ? '—' : rate.toFixed(1) + '%') +
      '</div>' +
      '<div class="result-cards">' + runs.map(resultCard).join('') + '</div>';
  }

  async function loadResults(profile) {
    var root = T.el('profile-results');
    root.innerHTML = '<div class="loading-state"><span class="spinner"></span><span>Loading measured results…</span></div>';
    try {
      /* By the exact row: another pump's "default v8" must not leak in. */
      var query = new URLSearchParams({ profile_version_id: String(profile.profile_version_id), limit: '10' });
      var response = await T.apiGet('/runs?' + query);
      var runs = response.items || [];
      var completed = runs.filter(function (run) { return run.status === 'COMPLETE' || run.status === 'COMPLETED'; });
      var usable = completed.length ? completed : runs;
      /* Averages only over runs whose field is finite. Number(x || 0) would
       * crown a missing final_error_g as a perfect 0 g result. */
      var errVals = usable.map(function (run) {
        return (run.final_error_g === null || run.final_error_g === undefined) ? NaN : Math.abs(Number(run.final_error_g));
      }).filter(Number.isFinite);
      var avgError = errVals.length ? errVals.reduce(function (s, v) { return s + v; }, 0) / errVals.length : null;
      var overVals = usable.map(function (run) {
        return (run.overshoot_g === null || run.overshoot_g === undefined) ? NaN : Number(run.overshoot_g);
      }).filter(Number.isFinite);
      var avgOvershoot = overVals.length ? overVals.reduce(function (s, v) { return s + v; }, 0) / overVals.length : null;
      var performance = T.el('selected-performance');
      if (performance) {
        performance.innerHTML = '<span>Measured performance</span><strong>' + runs.length + ' tests · ' +
          (avgError === null ? '—' : avgError.toFixed(1) + ' g avg error') + ' · ' +
          (avgOvershoot === null ? '—' : avgOvershoot.toFixed(1) + ' g avg overshoot') + '</strong>';
      }
      renderResults(profile, runs);
    } catch (error) {
      root.innerHTML = '<div class="inline-error"><strong>Results unavailable</strong><span>' + esc(error.message) + '</span></div>';
    }
  }

  async function refresh() {
    try {
      var results = await Promise.all([T.apiGet('/profiles'), T.apiGet('/device/status').catch(function () { return []; }), T.apiGet('/materials').catch(function () { return []; })]);
      profiles = results[0] || [];
      (Array.isArray(results[2]) ? results[2] : []).forEach(function (m) { if (m && m.material_id && m.name) materialNames[m.material_id] = m.name; });
      /* Versions changed (save/delete/other operator): drop every prediction. */
      nextGen += 1;
      nextCache = {};
      var devices = Array.isArray(results[1]) ? results[1] : [];
      var device = devices.find(function (d) {
        return d && d.status && d.status.role === 'relay_controller';
      }) || devices.find(function (d) {
        return d && d.status && Array.isArray(d.status.channels) &&
               d.status.channels.some(function (c) { return c && c.channel_id; });
      }) || devices[0] || null;
      deviceChannels = device && Number(device.age_seconds) <= 10 && device.status && Array.isArray(device.status.channels) ? device.status.channels : [];
      updateTargetOptions();
      updateActiveSummary();
      renderProfiles();
      if (selectedProfile) {
        var replacement = profiles.find(function (profile) { return profileKey(profile) === profileKey(selectedProfile); });
        if (replacement) selectedProfile = replacement;
      }
      renderSaveButton();
    } catch (error) {
      T.el('profile-root').innerHTML = '<div class="inline-error"><strong>Profiles unavailable</strong><span>' + esc(error.message) + '</span></div>';
    }
  }

  async function deleteProfile(button) {
    var versionId = button.dataset.versionId;
    var target = profiles.find(function (item) { return profileKey(item) === String(versionId); });
    var label = target ? contextLabel(target) : button.dataset.profileId + ' v' + button.dataset.version;
    if (!window.confirm('Delete ' + label + '? This cannot be undone.')) return;
    T.setButtonBusy(button, true, 'Deleting…');
    try {
      await T.apiWrite('/profile-versions/' + encodeURIComponent(versionId), { method: 'DELETE' });
      T.toast('Profile version deleted.', 'success');
      if (selectedProfile && profileKey(selectedProfile) === String(versionId)) {
        selectedProfile = null;
        T.el('selected-profile-summary').hidden = true;
        renderResults(null, null);
      }
      await refresh();
    } catch (error) {
      var message = error && error.message ? error.message : '';
      if (message.indexOf('referenced') >= 0) {
        /* A version used by a queued job or a run is permanent history. */
        T.toast(message, 'error', true);
      } else if (message.indexOf('409') >= 0 || message.indexOf('deactivate') >= 0) {
        T.toast('Deactivate this version before deleting it.', 'error');
      } else if (message.indexOf('404') >= 0) {
        T.toast('Version not found (already deleted?).', 'error');
        await refresh();
      } else {
        T.toast(message || 'Delete failed.', 'error', true);
      }
    } finally { T.setButtonBusy(button, false); }
  }

  async function profileAction(button) {
    if (button.dataset.profileAction === 'delete') return deleteProfile(button);
    T.setButtonBusy(button, true, button.dataset.profileAction === 'activate' ? 'Activating…' : 'Deactivating…');
    try {
      await post('/profile-versions/' + encodeURIComponent(button.dataset.versionId) + '/' + button.dataset.profileAction, {});
      T.toast(button.dataset.profileAction === 'activate' ? 'Profile activation queued for the controller.' : 'Profile deactivation queued.', 'success');
      await refresh();
    } catch (error) {
      T.toast(error.message, 'error', true);
    } finally { T.setButtonBusy(button, false); }
  }

  document.addEventListener('click', function (event) {
    var action = event.target.closest('[data-profile-action]');
    if (action) return profileAction(action);
    var edit = event.target.closest('[data-edit-profile]');
    if (edit) {
      var source = profiles.find(function (item) { return profileKey(item) === edit.dataset.editProfile; });
      if (source) startEdit(source);
      return;
    }
    var select = event.target.closest('[data-select-profile]');
    if (select) {
      var profile = profiles.find(function (item) { return profileKey(item) === select.dataset.selectProfile; });
      if (profile) selectProfile(profile);
    }
  });

  document.addEventListener('DOMContentLoaded', function () {
    T.startHealthIndicator();

    T.el('material').addEventListener('change', function () {
      selectedProfile = null;
      editingSource = null;
      updateTargetOptions();
      updateActiveSummary();
      renderProfiles();
      updateEditBanner();
      T.el('selected-profile-summary').hidden = true;
      renderResults(null, null);
    });
    T.el('target').addEventListener('change', function () { updateActiveSummary(); updateEditBanner(); });
    T.el('profile-id').addEventListener('input', updateEditBanner);
    T.el('apply-both').addEventListener('change', updateEditBanner);
    T.el('tuning-form').addEventListener('submit', async function (event) {
      event.preventDefault();
      var button = T.el('save-profile');
      var target = selectedTarget();
      if (target === null) return T.toast('Select one of the canonical targets: 5/10/15/20 kg.', 'error');
      var payload = {
        profile_id: T.el('profile-id').value.trim(), material_id: material(), channel_id: channel(), target_g: target,
        kp: Number(T.el('kp').value), ki: Number(T.el('ki').value), kd: Number(T.el('kd').value),
        window_ms: Number(T.el('window').value), min_on_ms: Number(T.el('min-on').value), min_off_ms: Number(T.el('min-off').value),
        tolerance_g: Number(T.el('tolerance').value), max_overshoot_g: Number(T.el('overshoot').value), max_duration_ms: Number(T.el('timeout').value)
      };
      if (!/^[A-Za-z0-9._-]{1,64}$/.test(payload.profile_id)) {
        T.el('tuning-message').textContent = 'Profile ID may only contain letters, digits, dot, underscore and hyphen (max 64, no spaces).';
        return T.toast('Profile ID: use letters, digits, . _ - only (no spaces), e.g. m1-5kg.', 'error');
      }
      if (payload.min_on_ms > payload.window_ms || payload.min_off_ms > payload.window_ms) return T.toast('Minimum relay times must fit inside the control window.', 'error');
      /* "Apply to both pumps": ONE atomic POST /profiles/bulk. The server gives
       * each pump its own next version (P1 v8 / P2 v5 when their histories
       * differ) and writes both rows or neither. Nothing is activated here,
       * exactly like a single save. */
      var both = bothChecked();
      T.setButtonBusy(button, true, 'Saving…');
      var savedRows = [];
      try {
        if (both) {
          var fields = Object.assign({}, payload);
          delete fields.material_id;
          delete fields.channel_id;
          var bulk = await post('/profiles/bulk', Object.assign(fields, { materials: ['M1', 'M2'] }));
          savedRows = bulk.profiles || [];
        } else {
          savedRows = [await post('/profiles', payload)];
        }
        /* Everything below reads the server's response, not client-side guesses. */
        var created = savedRows.find(function (row) { return row.material_id === material(); }) || savedRows[0];
        var from = editingSource && editingSource.profile_id === created.profile_id ? editingSource : null;
        T.el('tuning-message').className = 'small muted';
        T.el('tuning-message').textContent = both
          ? 'Saved ' + created.profile_id + ': ' + savedRows.map(function (row) { return materialContext(row.material_id) + ' v' + row.version; }).join(', ') + '.'
          : from
          ? 'Saved ' + created.profile_id + ' version ' + created.version +
            ' (kept v' + from.version + ' intact).'
          : 'Saved ' + created.profile_id + ' version ' + created.version + '.';
        T.toast(both ? 'Immutable profile versions saved for both pumps.' : 'New immutable profile version saved.', 'success');
        editingSource = null;
        if (both) T.el('apply-both').checked = false;
        await refresh();
        updateEditBanner();
        var saved = profiles.find(function (profile) { return profileKey(profile) === profileKey(created); });
        if (saved) await selectProfile(saved);
      } catch (error) {
        /* The bulk save is all-or-nothing, so a failed request saved neither pump. */
        var failure = both && !savedRows.length ? 'Nothing was saved for either pump: ' + error.message : error.message;
        T.el('tuning-message').textContent = failure;
        T.toast(failure, 'error', true);
      } finally {
        T.setButtonBusy(button, false);
        /* setButtonBusy restores the label captured BEFORE the save (e.g. the
         * dual "Save as v8 / v9"); recompute from current state. */
        renderSaveButton();
      }
    });
    updateEditBanner();
    renderResults(null, null);
    refresh();
  });
})();
