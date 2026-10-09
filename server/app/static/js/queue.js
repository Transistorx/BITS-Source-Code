(function () {
  'use strict';

  var T = window.Telemetry;
  var materialNames = { M1: 'Material 1', M2: 'Material 2' };
  /* A refresh must never be silently dropped. The 2 s poll and the post-action
   * refresh used to collide on `if (refreshing) return`, so a cancel could wait
   * a full extra cycle before the row disappeared. Coalesce instead. */
  var refreshing = false;
  var refreshQueued = false;
  var refreshQueuedFull = false;
  /* An older response must never overwrite a newer one. */
  var refreshGeneration = 0;
  /* Row-level pending feedback for HOLD / PROMOTE / CANCEL. Key kind:id.
   * { label, action, posted, ts }. Shown as a badge and keeps only that row's
   * controls disabled. It is NOT a device ACK: it clears when the refreshed
   * queue shows the new state, on POST failure, or after PENDING_JOB_TIMEOUT_MS. */
  var pendingJobs = {};
  var PENDING_JOB_TIMEOUT_MS = 12000;
  var PENDING_PROMOTE = 'PROMOTE';
  /* Last rendered queue markup; an unchanged poll leaves the DOM untouched. */
  var lastQueueHtml = null;
  var profilesCache = {};
  /* Pending manual pump commands per channel. Set on POST, cleared when the
   * controller-confirmed relay state matches the expectation, or marked failed
   * after PUMP_CONFIRM_TIMEOUT_MS. Never optimistically flips relay state. */
  var pendingPump = {};
  /* Last STOP result per channel, kept apart from a pending START so a new START
   * click never replaces or hides it. { action, ts, commandId, cmdState, ... } */
  var stopStatus = {};
  /* Requests in flight per channel: { PUMP_START: true, PUMP_STOP: true }. Survives
   * card redraws so a redrawn button stays disabled until its POST settles. */
  var inflight = {};
  var PUMP_CONFIRM_TIMEOUT_MS = 12000;
  /* STOP outcome is "unknown" this long after the click unless APPLIED/FAILED,
   * regardless of whether any request or poll ever returned. */
  var STOP_UNKNOWN_MS = 30000;
  var REQUEST_TIMEOUT_MS = 10000;
  /* An unknown START outcome stops being shown after this. The server expires
   * transient commands such as PUMP_START after 10 s (operations.py), so 15 s. */
  var START_UNKNOWN_MS = 15000;
  var START_EXPIRED_NOTE = 'Earlier START outcome expired; verify the pump state at the machine.';
  var startExpired = {};
  var liveController = null;
  var channelLayouts = {};
  var channelMetadata = {};

  function post(path, payload, timeoutMs) {
    return T.apiWrite(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload || {}),
      timeoutMs: timeoutMs || 0
    });
  }

  function esc(value, fallback) { return T.escapeHtml(value === null || value === undefined || value === '' ? (fallback || '—') : value); }
  function kg(grams, decimals) {
    var g = (grams === null || grams === undefined) ? NaN : Number(grams);
    return Number.isFinite(g) ? (g / 1000).toFixed(decimals === undefined ? 3 : decimals) + ' kg' : '—';
  }
  function pinLabel(job) {
    return job.profile_id ? esc(job.profile_id) + ' v' + esc(job.profile_version) : '—';
  }
  /* "Material A · Pump 1 · default v8" - material label + fixed pump + version. */
  function profileContext(row) {
    var pump = row.pump_id || (row.material_id === 'M1' ? 'Pump 1' : 'Pump 2');
    return (materialNames[row.material_id] || row.material_id) + ' · ' + pump + ' · ' + row.profile_id + ' v' + row.version;
  }
  function profileById(rows, versionId) {
    return rows.find(function (row) { return String(row.profile_version_id) === String(versionId); }) || null;
  }

  function placeholderChannel(channelId) {
    var number = channelId === 'CH1' ? 1 : 2;
    return { channel_id: channelId, material_id: 'M' + number, pump_id: 'Pump ' + number, state: 'OFFLINE', weight_valid: false };
  }

  var CHANNEL_STATE_WORDS = {
    'IDLE': 'IDLE', 'READY': 'READY', 'DISPENSING': 'DISPENSING',
    'RUNNING': 'DISPENSING', 'COMPLETE': 'COMPLETED', 'COMPLETED': 'COMPLETED',
    'OFFLINE': 'OFFLINE', 'FAULT': 'FAULT', 'ERROR': 'FAULT',
    'EMERGENCY STOP': 'EMERGENCY STOP', 'ESTOP': 'EMERGENCY STOP',
    'WAITING_FOR_SCALE': 'WAITING FOR SCALE', 'WAITING FOR SCALE': 'WAITING FOR SCALE',
    'WAITING_FOR_PROFILE': 'WAITING FOR PROFILE', 'WAITING FOR PROFILE': 'WAITING FOR PROFILE',
    'HOLD': 'PARKED', 'HELD': 'PARKED', 'PAUSE': 'PARKED'
  };

  function channelStateWord(state) {
    var key = T.stateWord(state);
    return CHANNEL_STATE_WORDS[key] || key;
  }

  /* Derive the confirmed pump-control state from the device telemetry and any
   * locally-tracked pending command. The state is NEVER set optimistically from
   * a button click — it always reflects what the controller has ACKed. */
  function pumpState(channel, online) {
    var id = channel.channel_id;
    var pending = pendingPump[id];
    var stop = stopStatus[id];
    var rawState = String(channel.state || '').toUpperCase();
    /* Safety states override everything else. */
    if (rawState === 'EMERGENCY STOP' || rawState === 'ESTOP') {
      return { word: 'E-STOP ACTIVE', cls: 'is-estop' };
    }
    if (rawState === 'FAULT' || rawState === 'ERROR' || channel.fault) {
      return { word: 'FAULT', cls: 'is-fault' };
    }
    if (!online) return { word: 'CONTROLLER OFFLINE', cls: 'is-offline' };
    /* A STOP that failed or whose outcome is unknown always wins the badge over a
     * newer START: it is the safety-relevant fact (a pump may still be running). */
    if (stop && (stop.cmdState === 'FAILED' || (stop.cmdState !== 'APPLIED' && stopUnknown(stop)))) {
      return stop.cmdState === 'FAILED'
        ? { word: 'STOP FAILED', cls: 'is-failed' }
        : { word: 'STOP STATUS UNKNOWN', cls: 'is-failed' };
    }
    /* A STOP clicked after the unknown START and since APPLIED supersedes it. */
    if (pending && pending.outcomeUnknown && stop && stop.cmdState === 'APPLIED' && stop.ts >= pending.ts) {
      delete pendingPump[id];
      pending = null;
    }
    if (pending && pending.outcomeUnknown && Date.now() - pending.unknownAt > START_UNKNOWN_MS) {
      delete pendingPump[id];
      startExpired[id] = true;
      pending = null;
    }
    if (pending) {
      /* START POST ended without an HTTP answer: it may still have been accepted.
       * Stays until relay on, a STOP APPLIED, START_UNKNOWN_MS, or a new START. */
      if (pending.outcomeUnknown) {
        if (Boolean(channel.relay_on)) {
          delete pendingPump[id];
          return { word: 'RELAY ON (commanded)', cls: 'is-running' };
        }
        return { word: 'START OUTCOME UNKNOWN', cls: 'is-failed' };
      }
      /* START has no request timeout, so while its POST is in flight the 12 s
       * window must not run: the outcome is unknown, not failed. */
      if (isInflight(id, 'PUMP_START')) {
        if (Boolean(channel.relay_on)) return { word: 'RELAY ON (commanded)', cls: 'is-running' };
        return { word: 'SENDING START... / OUTCOME UNKNOWN', cls: 'is-pending' };
      }
      /* START keeps the 12 s confirm window (counted from when the POST settled). */
      if (Date.now() - pending.ts > PUMP_CONFIRM_TIMEOUT_MS) {
        delete pendingPump[id];
        return { word: 'COMMAND FAILED', cls: 'is-failed' };
      }
      /* Check if the controller has confirmed the expected state. */
      if (Boolean(channel.relay_on)) {
        delete pendingPump[id];
        return { word: 'RELAY ON (commanded)', cls: 'is-running' };
      }
      return { word: 'COMMAND PENDING', cls: 'is-pending' };
    }
    if (stop) {
      /* The STOP entry is kept (never deleted here) until the command is
       * APPLIED/FAILED, independent of relay_on. Unknown is TIME based. */
      if (stop.cmdState === 'FAILED') return { word: 'STOP FAILED', cls: 'is-failed' };
      if (stop.cmdState !== 'APPLIED') {
        if (stopUnknown(stop)) return { word: 'STOP STATUS UNKNOWN', cls: 'is-failed' };
        return { word: 'COMMAND PENDING', cls: 'is-pending' };
      }
      return channel.relay_on
        ? { word: 'RELAY ON (commanded)', cls: 'is-running' }
        : { word: 'RELAY OFF (commanded)', cls: 'is-stopped' };
    }
    return channel.relay_on
      ? { word: 'RELAY ON (commanded)', cls: 'is-running' }
      : { word: 'RELAY OFF (commanded)', cls: 'is-stopped' };
  }

  var STOP_OFFLINE_NOTE = 'Controller offline - STOP is queued, not delivered. Use the hardwired emergency stop if a pump is running.';
  var STOP_OFFLINE_DELIVERED_NOTE = 'STOP delivered before the controller went offline; outcome unknown. Use the hardwired emergency stop if a pump is running.';
  var STOP_DISCLAIMER = 'Software STOP is not the emergency stop. Use the hardwired e-stop in an emergency.';

  function stopUnknown(p) {
    return Boolean(p.cmdTimedOut || p.requestTimedOut || Date.now() - p.ts > STOP_UNKNOWN_MS);
  }

  /* Real state of the last STOP command for a channel (queued -> delivered ->
   * applied/failed with the device's reason). Observability only. */
  function pumpNote(channelId, online) {
    var p = stopStatus[channelId];
    if (!p) return startExpired[channelId] ? START_EXPIRED_NOTE : STOP_DISCLAIMER;
    if (p.cmdState === 'FAILED') return 'STOP failed' + (p.cmdReason ? ': ' + p.cmdReason : '') + '. Use the hardwired emergency stop if a pump is running.';
    if (p.cmdState === 'APPLIED') return 'Controller accepted STOP and commanded the relay off. Confirm the pump has physically stopped.';
    if (p.requestNetworkError) return 'STOP request failed (network) - the server may still have queued it - use the hardwired emergency stop if a pump is running.';
    if (p.requestTimedOut) return 'STOP request timed out - use the hardwired emergency stop if a pump is running.';
    if (p.cmdTimedOut || Date.now() - p.ts > STOP_UNKNOWN_MS) return 'STOP status unknown - use the hardwired emergency stop if a pump is running.';
    var delivered = p.cmdState === 'DELIVERED' || p.cmdState === 'QUEUED';
    if (!online && delivered) return STOP_OFFLINE_DELIVERED_NOTE;
    if (!online && p.cmdState === 'PENDING') return STOP_OFFLINE_NOTE;
    if (delivered) return 'STOP delivered to controller. Waiting for acknowledgement.';
    if (p.cmdState === 'PENDING') return 'STOP queued on server. Waiting for controller to fetch it.';
    if (!online) return STOP_OFFLINE_NOTE;
    return 'Sending STOP...';
  }

  /* Poll the STOP command until a terminal state; updates stopStatus only. Each
   * GET is time-limited; the unknown state itself is time based (STOP_UNKNOWN_MS). */
  async function trackPumpCommand(channelId, commandId) {
    for (var i = 0; i < 90; i++) {
      var p = stopStatus[channelId];
      if (!p || p.commandId !== commandId) return;
      try {
        var c = await T.apiGet('/queue/control/' + encodeURIComponent(commandId), REQUEST_TIMEOUT_MS);
        p = stopStatus[channelId];
        if (!p || p.commandId !== commandId) return;
        p.cmdState = c.state;
        p.cmdReason = c.reason || c.error || '';
        if (c.state === 'APPLIED' || c.state === 'FAILED') return;
      } catch (ignored) { /* keep polling; status is advisory */ }
      await new Promise(function (resolve) { setTimeout(resolve, 1000); });
    }
    var last = stopStatus[channelId];
    if (last && last.commandId === commandId) last.cmdTimedOut = true;
  }

  function isInflight(channelId, action) {
    return Boolean(inflight[channelId] && inflight[channelId][action]);
  }

  /* Re-apply START/ZERO/TARE interlocks after a busy button is released, so a
   * button is never left enabled against an active interlock. */
  function reapplyInterlocks() {
    if (liveController) applyLiveController(liveController);
    else if (lastRender) renderChannels(lastRender.channels, lastRender.online);
  }

  function channelCard(channel, online, allChannels) {
    var id = channel.channel_id;
    var number = id === 'CH1' ? 1 : 2;
    var material = channel.material_id || 'M' + number;
    var target = (channel.target_g === null || channel.target_g === undefined) ? NaN : Number(channel.target_g);
    var weight = (channel.weight_g === null || channel.weight_g === undefined) ? NaN : Number(channel.weight_g);
    var weightValid = Boolean(channel.weight_valid);
    var err = (channel.error_g === null || channel.error_g === undefined) ? NaN : Number(channel.error_g);
    var output = (channel.output === null || channel.output === undefined) ? NaN : Number(channel.output);
    var remaining = Number.isFinite(err)
      ? err
      : (Number.isFinite(target) && Number.isFinite(weight) ? target - weight : NaN);
    var targetOk = Number.isFinite(target) && target > 0;
    var progress = targetOk && Number.isFinite(weight)
      ? Math.max(0, Math.min(100, weight * 100 / target)) : null;
    var rawState = online ? (channel.state || 'IDLE') : 'OFFLINE';
    var word = channelStateWord(rawState);
    var hasJob = Boolean(channel.active_job_id);
    var fault = channel.fault || channel.error;
    var faultCode = channel.fault_code || channel.error_code || null;
    var display = T.fmtWeightDisplay(channel.weight_g, weightValid, channel.weight_age_s);
    if (display.valid && channel.stable !== true) { display.note = 'Reading active'; }
    var pump = pumpState(channel, online);

    /* Sequential relay rule: refuse START when the other pump is energized.
     * Firmware enforces mutual-exclusion as well — this is a UI guard only. */
    var other = (allChannels || []).find(function (c) {
      return c && c.channel_id !== id;
    });
    var otherRunning = Boolean(other && other.relay_on);
    var isEstop = String(channel.state || '').toUpperCase() === 'EMERGENCY STOP' ||
                  String(channel.state || '').toUpperCase() === 'ESTOP';
    var faultState = word === 'FAULT';
    var startDisabled = !online || hasJob || Boolean(fault) || isEstop || otherRunning || faultState ||
      isInflight(id, 'PUMP_START');
    var stopDisabled = isInflight(id, 'PUMP_STOP'); /* never disabled by offline/state; only while its own request is in flight */

    return '<article class="panel channel-card' + (fault ? ' is-fault' : '') +
             (online && hasJob ? ' is-running' : '') + '" data-channel-card="' + esc(id) + '">' +
      /* --- Header: channel identity + state badge --- */
      '<div class="channel-card-head">' +
        '<div><p class="eyebrow">' + esc(id) + ' · RELAY ' + number + ' · SCALE ' + number + '</p>' +
        '<h3 class="channel-title">' + esc(materialNames[material] || material) + ' · ' +
        esc(channel.pump_id, 'Pump ' + number) + '</h3></div>' +
        '<span class="channel-badge">' + T.badge(word) + '</span>' +
      '</div>' +
      /* --- Current Weight: dominant live value --- */
      '<div class="channel-weight">' +
        '<span>CURRENT WEIGHT</span>' +
        '<strong class="channel-weight-value' + (display.valid ? '' : ' is-unavailable') + '">' +
          esc(display.value) + '</strong>' +
        '<small class="channel-weight-note">' + esc(display.note) + '</small>' +
      '</div>' +
      /* --- PUMP CONTROL: upper portion, immediately reachable --- */
      '<div class="pump-control">' +
        '<div class="pump-control-head">' +
          '<span class="pump-control-label">PUMP CONTROL</span>' +
          '<span class="pump-state ' + pump.cls + '">' + esc(pump.word) + '</span>' +
        '</div>' +
        '<div class="pump-control-actions">' +
          '<button class="btn sm" data-control="PUMP_START" data-channel="' + esc(id) + '"' +
            (startDisabled ? ' disabled' : '') + '>START</button>' +
          '<button class="btn danger sm" data-control="PUMP_STOP" data-channel="' + esc(id) + '"' +
            (stopDisabled ? ' disabled' : '') + '>STOP</button>' +
        '</div>' +
        '<small class="pump-stop-note" data-pump-note>' + esc(pumpNote(id, online)) + '</small>' +
      '</div>' +
      /* --- Scale control: operator ZERO/TARE to the weight sender (never the relay) --- */
      '<div class="pump-control-actions">' +
        '<button class="btn secondary sm" data-scale="ZERO" data-channel="' + esc(id) + '">ZERO</button>' +
        '<button class="btn secondary sm" data-scale="TARE" data-channel="' + esc(id) + '">TARE</button>' +
      '</div>' +
      /* --- Progress bar --- */
      (progress !== null
        ? '<div class="progress channel-progress" role="progressbar" aria-valuenow="' +
            progress.toFixed(0) + '" aria-valuemin="0" aria-valuemax="100" aria-label="Dispense progress">' +
            '<span style="width:' + progress.toFixed(1) + '%"></span></div>'
        : '') +
      /* --- Primary metric grid: live process values --- */
      '<div class="channel-metrics metrics-grid">' +
        metric('Target', targetOk ? kg(target) : '—') +
        metric('Remaining', Number.isFinite(remaining) ? kg(remaining) : '—') +
        metric('PID output', Number.isFinite(output)
          ? (output * 100).toFixed(1) + '%' : '—') +
        metric('Relay', channel.relay_on ? 'ON' : 'OFF') +
        metric('Progress', progress === null ? '—' : progress.toFixed(1) + '%') +
        metric('Job', channel.active_job_id || '—') +
        metric('Elapsed', Number.isFinite(Number(channel.elapsed_ms))
          ? T.fmtDurationMs(channel.elapsed_ms) : '—') +
      '</div>' +
      /* --- Secondary: Profile / Kp-Ki-Kd (lower priority) --- */
      '<div class="channel-secondary">' +
        '<span>Profile: <strong>' +
          (channel.profile_id ? esc(channel.profile_id) + ' v' + esc(channel.profile_version) : '—') +
        '</strong></span>' +
        '<span>Kp / Ki / Kd: <strong>' +
          esc(channel.kp) + ' / ' + esc(channel.ki) + ' / ' + esc(channel.kd) +
        '</strong></span>' +
      '</div>' +
      /* --- Fault alert --- */
      (fault
        ? '<div class="alert" role="alert"><strong>' + esc(fault) + '</strong>' +
          (faultCode ? '<span class="alert-code">' + esc(faultCode) + '</span>' : '') +
          '</div>'
        : '') +
      /* --- Job actions (when a job owns this channel) --- */
      (hasJob
        ? '<div class="channel-actions">' +
            '<button class="btn secondary sm" data-control="PAUSE" data-channel="' + esc(id) + '">Pause</button>' +
            '<button class="btn secondary sm" data-control="RESUME" data-channel="' + esc(id) + '">Resume</button>' +
            '<button class="btn danger sm" data-control="CANCEL" data-channel="' + esc(id) + '">Cancel active</button>' +
          '</div>'
        : '') +
    '</article>';
  }

  function metric(label, value) {
    return '<div class="metric"><span>' + esc(label) + '</span><strong>' +
           esc(value) + '</strong></div>';
  }

  function renderChannels(channels, online) {
    var host = T.el('channel-cards');
    if (!host) return;
    channels.forEach(function (channel) {
      var id = channel.channel_id;
      var layout = JSON.stringify([channel.active_job_id, channel.profile_id, channel.profile_version,
        channel.kp, channel.ki, channel.kd, materialNames[channel.material_id]]);
      var card = host.querySelector('[data-channel-card="' + id + '"]');
      if (!card || channelLayouts[id] !== layout) {
        if (card) card.outerHTML = channelCard(channel, online, channels);
        else host.insertAdjacentHTML('beforeend', channelCard(channel, online, channels));
        channelLayouts[id] = layout;
        card = host.querySelector('[data-channel-card="' + id + '"]');
      }
      var display = T.fmtWeightDisplay(channel.weight_g, online && channel.weight_valid, channel.weight_age_s);
      if (display.valid && channel.stable !== true) display.note = 'Reading active';
      card.querySelector('.channel-weight-value').textContent = display.value;
      card.querySelector('.channel-weight-value').classList.toggle('is-unavailable', !display.valid);
      card.querySelector('.channel-weight-note').textContent = display.note;
      card.querySelector('.channel-badge').innerHTML = T.badge(channelStateWord(online ? channel.state : 'OFFLINE'));
      var pump = pumpState(channel, online);
      var pumpNode = card.querySelector('.pump-state');
      pumpNode.textContent = pump.word;
      pumpNode.className = 'pump-state ' + pump.cls;
      var otherRunning = channels.some(function (c) { return c.channel_id !== id && c.relay_on; });
      var state = String(channel.state || '').toUpperCase();
      var startBtn = card.querySelector('[data-control="PUMP_START"]');
      var stopBtn = card.querySelector('[data-control="PUMP_STOP"]');
      startBtn.disabled = !online || !!channel.active_job_id ||
        !!(channel.fault || channel.error) || state === 'EMERGENCY STOP' || state === 'ESTOP' || otherRunning ||
        state === 'FAULT' || state === 'ERROR';
      stopBtn.disabled = false;
      /* A button whose request is in flight stays disabled, also after a card redraw. */
      if (startBtn.classList.contains('is-loading') || isInflight(id, 'PUMP_START')) startBtn.disabled = true;
      if (stopBtn.classList.contains('is-loading') || isInflight(id, 'PUMP_STOP')) stopBtn.disabled = true;
      var noteNode = card.querySelector('[data-pump-note]');
      if (noteNode) noteNode.textContent = pumpNote(id, online);
      card.querySelectorAll('[data-scale]').forEach(function (b) {
        b.disabled = !online || channels.some(function (c) {
          return c.active_job_id || c.relay_on;
        });
      });
      var target = Number(channel.target_g), weight = Number(channel.weight_g);
      var progress = target > 0 && channel.weight_valid ? Math.max(0, Math.min(100, weight * 100 / target)) : null;
      var bar = card.querySelector('.channel-progress');
      if (bar) {
        bar.setAttribute('aria-valuenow', progress === null ? '0' : progress.toFixed(0));
        bar.querySelector('span').style.width = progress === null ? '0%' : progress.toFixed(1) + '%';
      }
      var values = [target > 0 ? kg(target) : '—', channel.weight_valid ? kg(channel.error_g) : '—',
        channel.output != null ? (Number(channel.output) * 100).toFixed(1) + '%' : '—',
        online ? (channel.relay_on ? 'ON' : 'OFF') : 'UNKNOWN', progress === null ? '—' : progress.toFixed(1) + '%',
        channel.active_job_id || '—', T.fmtDurationMs(channel.elapsed_ms)];
      card.querySelectorAll('.channel-metrics .metric strong').forEach(function (node, i) { node.textContent = values[i]; });
      card.classList.toggle('is-fault', !!channel.fault);
      card.classList.toggle('is-running', online && !!channel.active_job_id);
    });
  }

  function applyLiveController(controller) {
    if (!controller || !controller.status) return;
    var channels = (controller.status.channels || []).map(function (channel) {
      return Object.assign({}, channelMetadata[channel.channel_id] || {}, channel);
    });
    renderChannels(channels, controller.online);
    var status = T.el('queue-live-status');
    if (status) {
      status.textContent = controller.online ? 'Controller online' : 'Controller offline';
      status.className = 'status-badge ' + (controller.online ? 'success' : 'danger');
    }
  }

  function localQueuedAt(job, command, device) {
    if (command && command.queued_at) return command.queued_at;
    var status = device && device.status;
    var queuedMs = (job.queued_ms === null || job.queued_ms === undefined) ? NaN : Number(job.queued_ms);
    var uptimeMs = (status && status.uptime_ms !== null && status.uptime_ms !== undefined)
      ? Number(status.uptime_ms) : NaN;
    if (status && Number.isFinite(uptimeMs) && Number.isFinite(queuedMs)) {
      var age = (uptimeMs - queuedMs) >>> 0;
      var received = Date.parse(device.updated_at || '');
      if (Number.isFinite(received)) return new Date(received - age).toISOString();
    }
    return null;
  }

  function collectQueue(queueData, device) {
    var status = device && device.status;
    var commands = queueData.waiting_commands || [];
    var local = status && Array.isArray(status.queue) ? status.queue : [];
    var usedCommands = {};
    var jobs = [];
    local.forEach(function (job) {
      if (String(job.state).toUpperCase() !== 'QUEUED') return;
      var command = commands.find(function (item) { return Number(item.local_job_id) === Number(job.id); });
      if (command) usedCommands[command.command_id] = true;
      jobs.push({
        id: job.id, commandId: command && command.command_id, kind: 'local', material_id: job.material_id || (command && command.material_id),
        target_g: job.target_g, priority: job.priority, state: job.state,
        held: Boolean(job.held), promoted: Boolean(job.promoted),
        /* The pin lives on the server-side job row; the device queue mirror
         * does not carry it. Take it from the matching command when present. */
        profile_id: (command && command.profile_id) || job.profile_id,
        profile_version: (command && command.profile_version) || job.profile_version,
        profile_version_id: command && command.profile_version_id,
        queued_at: localQueuedAt(job, command, device)
      });
    });
    commands.forEach(function (command) {
      if (usedCommands[command.command_id] || !['PENDING', 'DELIVERED', 'QUEUED'].includes(String(command.state).toUpperCase())) return;
      jobs.push({ id: command.command_id, kind: 'command', material_id: command.material_id, target_g: command.target_g, priority: command.priority, state: command.state,
        held: Boolean(command.held), promoted: Boolean(command.promoted),
        profile_id: command.profile_id, profile_version: command.profile_version,
        profile_version_id: command.profile_version_id,
        queued_at: command.queued_at });
    });
    jobs.sort(function (a, b) {
      // Mirrors job_outweighs() in the firmware: a promoted job jumps every
      // non-promoted one regardless of priority, then priority, then FIFO.
      return (Number(b.promoted) - Number(a.promoted)) ||
             (Number(b.priority || 0) - Number(a.priority || 0)) ||
             (Date.parse(a.queued_at || 0) - Date.parse(b.queued_at || 0));
    });
    return jobs;
  }

  function pendingKey(kind, id) { return kind + ':' + id; }

  /* Replace the queue markup only when it changed, and keep keyboard focus on
   * the same row button across a repaint. */
  function setQueueHtml(root, html) {
    if (html === lastQueueHtml) return;
    var active = document.activeElement;
    var focusSelector = null;
    if (active && root.contains(active)) {
      ['data-start', 'data-stop', 'data-cancel', 'data-control'].some(function (attr) {
        if (active.hasAttribute(attr)) {
          focusSelector = 'button[' + attr + '="' + String(active.getAttribute(attr)).replace(/"/g, '\\"') + '"]' +
            (active.hasAttribute('data-channel') ? '[data-channel="' + active.getAttribute('data-channel') + '"]' : '');
          return true;
        }
        return false;
      });
    }
    root.innerHTML = html;
    lastQueueHtml = html;
    if (focusSelector) {
      var again = root.querySelector(focusSelector);
      if (again && !again.disabled) again.focus();
    }
  }

  /* Read-only "why is this job not running" hint. Pure: first match wins, and
   * a missing field yields no reason for that rule (never an invented one).
   * ctx = { online, channels, jobs, index }. Returns {text, tone} or null. */
  function toChannelId(value) {
    var s = String(value === null || value === undefined ? '' : value).toUpperCase();
    if (s === 'M1' || s === 'CH1' || s === '1') return 'CH1';
    if (s === 'M2' || s === 'CH2' || s === '2') return 'CH2';
    return '';
  }
  function waitReason(job, ctx) {
    if (!job || !ctx) return null;
    var channelId = toChannelId(job.material_id || 'M1');
    var mName = channelId === 'CH2' ? 'M2' : 'M1';
    var channels = ctx.channels || [];
    var channel = channels.find(function (c) { return c && c.channel_id === channelId; }) || null;
    var state = String(job.state || '').toUpperCase();
    var chState = channel ? String(channel.state || '').toUpperCase() : '';
    if (!ctx.online) return { text: 'Controller offline', tone: 'danger' };
    if (state === 'PENDING' || state === 'DELIVERED') {
      return { text: 'Waiting for controller to accept the job', tone: 'warning' };
    }
    if (!channel) return null;
    var fault = channel.fault || channel.error || null;
    var safety = channel.safety_fault || null;
    var estop = chState === 'EMERGENCY STOP' || chState === 'ESTOP' || chState === 'EMERGENCY_STOP';
    if (fault || safety || estop || chState === 'FAULT' || chState === 'ERROR') {
      var parts = [];
      if (fault) parts.push(String(fault));
      if (safety && String(safety) !== String(fault)) parts.push(String(safety));
      if (!parts.length) parts.push(estop ? 'EMERGENCY STOP' : 'FAULT');
      return { text: 'Channel ' + mName + ' faulted: ' + parts.join(', ') + '. Operator must clear it at the machine.', tone: 'danger' };
    }
    if (channel.weight_valid === false) return { text: 'No valid weight from scale', tone: 'warning' };
    if (chState === 'PAUSED' || chState === 'PAUSE') return { text: 'Channel ' + mName + ' is paused', tone: 'warning' };
    if (channel.relay_on === true && !channel.active_job_id) {
      return { text: 'Manual pump running on ' + mName, tone: 'warning' };
    }
    var owner = '';
    if (channel.scale_owner) owner = toChannelId(channel.scale_owner);
    else {
      var holder = channels.find(function (c) { return c && c.channel_id !== channelId && c.owns_scale === true; });
      if (holder) owner = holder.channel_id;
    }
    if (owner && owner !== channelId) {
      return { text: 'Scale in use by ' + (owner === 'CH2' ? 'M2' : 'M1'), tone: 'warning' };
    }
    var jobs = ctx.jobs || [];
    var ahead = 0;
    for (var i = 0; i < ctx.index && i < jobs.length; i++) {
      if (toChannelId(jobs[i].material_id || 'M1') === channelId) ahead++;
    }
    if (ahead === 0 && channel.awaiting_operator_ready === true) {
      return { text: 'Awaiting operator READY', tone: 'warning' };
    }
    if (job.held) return { text: 'Job on hold', tone: 'neutral' };
    if (ahead > 0) return { text: 'Waiting behind ' + ahead + ' job' + (ahead === 1 ? '' : 's') + ' (position ' + (ahead + 1) + ')', tone: 'neutral' };
    if (channel.active_job_id) return { text: 'Waiting for running job ' + channel.active_job_id + ' to finish', tone: 'neutral' };
    if (chState === 'IDLE' || chState === 'READY') {
      return { text: 'Ready - waiting for the controller to start it', tone: 'success' };
    }
    return null;
  }

  function renderQueue(jobs, channels, online) {
    var root = T.el('queue-root');
    /* A cancel the server accepted (or is still sending) hides its row at once. */
    jobs = jobs.filter(function (job) {
      var p = pendingJobs[pendingKey(job.kind, job.id)];
      return !(p && p.action === 'CANCEL');
    });
    if (!jobs.length) {
      setQueueHtml(root, '<div class="empty-state compact"><strong>No jobs waiting</strong><span>New jobs will appear here in priority and FIFO order.</span></div>');
      return;
    }
    var headSeen = {};
    setQueueHtml(root, '<div class="table-wrap"><table class="data mobile-cards"><thead><tr><th>Position</th><th>Job ID</th><th>Material</th><th>Target</th><th>PID profile</th><th>Priority</th><th>Created</th><th>Status</th><th>Action</th></tr></thead><tbody>' + jobs.map(function (job, index) {
      var material = job.material_id || 'M1';
      var channelId = material === 'M1' ? 'CH1' : 'CH2';
      var channel = channels.find(function (item) { return item.channel_id === channelId; });
      /* READY is an operator intent the firmware validates. Use its
       * awaiting_operator_ready flag when reported; otherwise offer it on the
       * head waiting job of the channel. */
      var isHead = !headSeen[channelId];
      headSeen[channelId] = true;
      var awaiting = channel && channel.awaiting_operator_ready !== undefined && channel.awaiting_operator_ready !== null
        ? (!!channel.awaiting_operator_ready && isHead) : isHead;
      var readyBtn = awaiting && !job.held
        ? '<button class="btn sm" data-control="READY" data-channel="' + channelId + '">Ready</button>' : '';
      var waiting = channel && ['FAULT', 'OFFLINE'].includes(String(channel.state).toUpperCase()) ? 'Waiting for channel · ' + channel.state : job.state;
      var why = waitReason(job, { online: online, channels: channels, jobs: jobs, index: index });
      var whyHtml = why ? '<div class="wait-reason"><span class="status-badge ' + why.tone + '">' + esc(why.text) + '</span></div>' : '';
      var rowPending = pendingJobs[pendingKey(job.kind, job.id)];
      var pendingHtml = rowPending ? ' <span class="status-badge warning" data-pending="' + esc(rowPending.action) + '">' + esc(rowPending.label) + '</span>' : '';
      var lock = rowPending ? ' disabled' : '';
      return '<tr class="' + (job.priority ? 'priority' : '') + (job.held ? ' held' : '') + '" data-row-id="' + esc(job.id) + '" data-row-kind="' + job.kind + '">' +
        '<td data-label="Position"><strong>' + (index + 1) + '</strong></td>' +
        '<td data-label="Job ID">' + esc(job.id) + '</td>' +
        '<td data-label="Material">' + esc(materialNames[material] || material) + '</td>' +
        '<td data-label="Target">' + kg(job.target_g) + '</td>' +
        '<td data-label="PID profile">' + pinLabel(job) + '</td>' +
        '<td data-label="Priority"><span class="status-badge ' + (job.priority ? 'warning' : 'neutral') + '">' + (job.priority ? 'Priority' : 'Normal') + '</span></td>' +
        '<td data-label="Created">' + esc(T.fmtDateTime(job.queued_at)) + '</td>' +
        '<td data-label="Status">' + esc(waiting) + (job.held ? ' <span class="status-badge neutral">Parked</span>' : '') + pendingHtml + whyHtml + '</td>' +
        '<td data-label="Action">' + readyBtn +
          '<button class="btn sm" data-start="' + esc(job.id) + '" data-kind="' + job.kind + '" data-held="' + (job.held ? 'true' : 'false') + '"' + lock + '>Start</button>' +
          '<button class="btn secondary sm" data-stop="' + esc(job.id) + '" data-kind="' + job.kind + '"' + lock + '>Stop</button>' +
          '<button class="btn danger sm" data-cancel="' + esc(job.id) + '" data-kind="' + job.kind + '"' + lock + '>Cancel</button>' +
        '</td></tr>';
    }).join('') + '</tbody></table></div>');
  }

  /* Jobs the controller refused (e.g. stale command id) must not vanish: show
   * the reason and let the operator resubmit under a new id. */
  function renderFailedJobs(failed) {
    var root = T.el('queue-failed');
    if (!root) {
      var queueRoot = T.el('queue-root');
      root = document.createElement('div');
      root.id = 'queue-failed';
      queueRoot.parentNode.insertBefore(root, queueRoot.nextSibling);
    }
    if (!failed.length) { root.innerHTML = ''; return; }
    root.innerHTML = '<div class="table-wrap"><table class="data mobile-cards"><thead><tr><th>Job ID</th><th>Material</th><th>Target</th><th>Refused</th><th>Action</th></tr></thead><tbody>' + failed.map(function (job) {
      return '<tr class="failed"><td data-label="Job ID">' + esc(job.command_id) + '</td>' +
        '<td data-label="Material">' + esc(materialNames[job.material_id] || job.material_id) + '</td>' +
        '<td data-label="Target">' + kg(job.target_g) + '</td>' +
        '<td data-label="Refused"><span class="status-badge danger">Failed</span> ' + esc(job.error || 'refused by controller') +
          (job.outcome_unknown ? ' <span class="status-badge warn">Outcome unknown - may already have run</span>' : '') + '</td>' +
        '<td data-label="Action"><button class="btn sm" data-resubmit="' + esc(job.command_id) + '"' +
          (job.outcome_unknown ? ' data-unknown="1"' : '') + '>Resubmit</button></td></tr>';
    }).join('') + '</tbody></table></div>';
  }

  function renderKpiStrip(info) {
    var host = T.el('queue-kpis');
    if (!host) { return; }
    var c = T.el('kpi-controller');
    var w = T.el('kpi-waiting');
    var a = T.el('kpi-active');
    var t = T.el('kpi-telemetry');
    if (!c) { return; }
    var controllerAge = (info.controllerAge === null || info.controllerAge === undefined) ? NaN : Number(info.controllerAge);
    var waiting = (info.waitingCount === null || info.waitingCount === undefined) ? NaN : Number(info.waitingCount);
    var telemetryAge = (info.telemetryAge === null || info.telemetryAge === undefined) ? NaN : Number(info.telemetryAge);
    var f = T.freshnessState(controllerAge);
    c.textContent = f.word;
    c.className = f.className;
    w.textContent = Number.isFinite(waiting) ? String(waiting) : '—';
    a.textContent = info.activeJob ? ('Job ' + info.activeJob) : 'None';
    t.textContent = Number.isFinite(telemetryAge)
      ? (Math.round(telemetryAge) + 's ago') : '—';
    t.className = T.freshnessState(telemetryAge).className;
  }

  function renderProfilePreview(row) {
    var host = T.el('job-profile-preview');
    if (!host) { return; }
    if (!row) {
      host.innerHTML = '<span class="muted small">' +
        'Select a PID profile to preview its exact values.</span>';
      return;
    }
    host.innerHTML =
      '<strong>' + esc(row.profile_id) + ' v' + esc(row.version) + '</strong>' +
      '<span class="muted small">Kp ' + esc(row.kp) + ' · Ki ' + esc(row.ki) + ' · Kd ' + esc(row.kd) + '</span>' +
      '<span class="muted small">window ' + esc(row.window_ms) + ' ms · min ON ' + esc(row.min_on_ms) +
        ' ms · min OFF ' + esc(row.min_off_ms) + ' ms</span>' +
      '<span class="muted small">tolerance ' + esc(row.tolerance_g) + ' g · max overshoot ' +
        esc(row.max_overshoot_g) + ' g · max time ' + esc(row.max_duration_ms) + ' ms</span>' +
      '<span class="badge ' + (row.active ? 'ok' : 'neutral') + '">' +
        (row.active ? 'ACTIVE' : 'HISTORICAL') + '</span>';
  }

  /* Drop one row from the DOM the moment the server accepts the cancel. The
   * poll will repaint within 2 s either way; waiting for it made cancellation
   * feel stuck on a live board. */
  function dropRow(id, kind) {
    var root = T.el('queue-root');
    var row = root.querySelector('tr[data-row-id="' + String(id).replace(/"/g, '\\"') + '"][data-row-kind="' + kind + '"]');
    if (row) row.remove();
    lastQueueHtml = null;
  }

  /* Mark a row pending and repaint from the last data without a network trip. */
  var lastRender = null;
  function setRowPending(kind, id, action, label) {
    pendingJobs[pendingKey(kind, id)] = { action: action, label: label, posted: false, ts: Date.now() };
    if (action === 'CANCEL') dropRow(id, kind);
    repaintQueue();
  }
  function clearRowPending(kind, id) {
    delete pendingJobs[pendingKey(kind, id)];
    lastQueueHtml = null;
    repaintQueue();
  }
  function repaintQueue() {
    if (lastRender) renderQueue(lastRender.jobs, lastRender.channels, lastRender.online);
  }
  /* Drop pending marks the refreshed queue now confirms, or that timed out. */
  function settlePending(jobs) {
    var now = Date.now();
    Object.keys(pendingJobs).forEach(function (key) {
      var p = pendingJobs[key];
      if (!p.posted) return;
      var job = jobs.find(function (j) { return pendingKey(j.kind, j.id) === key; });
      var done = now - p.ts > PENDING_JOB_TIMEOUT_MS ||
        (p.action === 'CANCEL' && !job) ||
        (p.action === 'HOLD' && job && job.held) ||
        (p.action === 'PROMOTE' && job && job.promoted && !job.held);
      if (done) delete pendingJobs[key];
    });
  }

  /* The PID profile dropdown is filtered to the chosen material and target.
   * Only exact-version options appear: the job stores whichever one is picked. */
  async function refreshProfileOptions() {
    var select = T.el('job-profile');
    var materialId = T.el('job-material').value;
    var targetG = Number(T.el('job-target').value);
    var cacheKey = materialId + ':' + targetG;
    if (!select) return;
    var previous = select.value;
    select.disabled = true;
    try {
      var rows = profilesCache[cacheKey];
      if (!rows) {
        rows = await T.apiGet('/profiles?material_id=' + encodeURIComponent(materialId) + '&target_g=' + encodeURIComponent(targetG));
        profilesCache[cacheKey] = rows;
      }
      if (!rows.length) {
        select.innerHTML = '<option value="">No PID profile for this material and target</option>';
        T.el('job-message').textContent = 'Save a PID profile for ' + materialId + ' at ' + kg(targetG, 1) + ' in PID Tuning before queueing this job.';
        renderProfilePreview(null);
        return;
      }
      // Newest version first so the operator sees the latest tune at a glance.
      rows = rows.slice().sort(function (a, b) {
        return (a.profile_id === b.profile_id) ? (b.version - a.version) : a.profile_id.localeCompare(b.profile_id);
      });
      /* The option value is the immutable profile_version_id: "default v8" can
       * exist for both pumps, so id + version alone is not a key. */
      select.innerHTML = rows.map(function (row) {
        return '<option value="' + esc(row.profile_version_id) + '">' +
          esc(profileContext(row)) + ' · Kp ' + row.kp + ' / Ki ' + row.ki + ' / Kd ' + row.kd +
          ' · tol ' + row.tolerance_g + ' g</option>';
      }).join('');
      if ([].some.call(select.options, function (option) { return option.value === previous; })) select.value = previous;
      T.el('job-message').textContent = '';
      renderProfilePreview(profileById(rows, select.value));
    } catch (error) {
      select.innerHTML = '<option value="">PID profiles unavailable</option>';
      T.el('job-message').textContent = error.message;
      renderProfilePreview(null);
    } finally {
      select.disabled = false;
    }
  }

  /* Is the shared live snapshot good enough to stand in for /device/status? */
  function liveDevice() {
    return liveController && liveController.status && Array.isArray(liveController.status.channels)
      ? liveController : null;
  }

  /* queueOnly: after a row action only /queue is refetched; the controller
   * snapshot comes from the already-running live subscription. */
  async function refresh(queueOnly) {
    queueOnly = queueOnly === true;
    if (refreshing) {
      refreshQueued = true;
      if (!queueOnly) refreshQueuedFull = true;
      return;
    }
    refreshing = true;
    var generation = ++refreshGeneration;
    try {
      /* /device/status failing must not blank the queue list — the operator is
       * usually here to manage the queue, and the board is still useful with a
       * stale channel pane. Only a failed /queue blanks the list. The live
       * subscription already carries the controller status, so reuse it rather
       * than polling the same data twice. */
      var live = liveDevice();
      var useLive = live !== null;
      var results = await Promise.all([
        T.apiGet('/queue', REQUEST_TIMEOUT_MS),
        useLive ? [live] : T.apiGet('/device/status', REQUEST_TIMEOUT_MS).catch(function () { return null; }),
        queueOnly ? null : T.apiGet('/materials', REQUEST_TIMEOUT_MS).catch(function () { return []; })
      ]);
      if (generation !== refreshGeneration) return;
      if (useLive && liveController) results[1] = [liveController];
      var devices = Array.isArray(results[1]) ? results[1] : [];
      var device = devices.find(function (d) {
        return d && d.status && d.status.role === 'relay_controller';
      }) || devices.find(function (d) {
        return d && d.status && Array.isArray(d.status.channels) &&
               d.status.channels.some(function (c) { return c && c.channel_id; });
      }) || devices[0] || null;
      var online = Boolean(device && Number(device.age_seconds) <= 10);
      var channels = online && device.status && Array.isArray(device.status.channels) ? device.status.channels.slice() : [];
      ['CH1', 'CH2'].forEach(function (id) { if (!channels.some(function (item) { return item.channel_id === id; })) channels.push(placeholderChannel(id)); });
      channels.sort(function (a, b) { return a.channel_id.localeCompare(b.channel_id); });
      channels.forEach(function (ch) {
        var available = Boolean(ch.weight_valid) &&
          ch.weight_g !== null && ch.weight_g !== undefined;
        ch.weight_age_s = (online && device && available)
          ? Number(device.age_seconds) : NaN;
      });
      if (results[2]) {
        var namesBefore = JSON.stringify(materialNames);
        results[2].forEach(function (material) { materialNames[material.material_id] = material.name; });
        /* Dropdown labels carry the material name; redraw from the cache once it changes. */
        if (JSON.stringify(materialNames) !== namesBefore) refreshProfileOptions();
        var select = T.el('job-material');
        var selected = select.value;
        var enabled = results[2].filter(function (material) { return material.enabled; });
        if (enabled.length) select.innerHTML = enabled.map(function (material) { return '<option value="' + esc(material.material_id) + '">' + esc(material.name) + ' · ' + esc(material.pump_id) + '</option>'; }).join('');
        if ([].some.call(select.options, function (option) { return option.value === selected; })) select.value = selected;
      }
      channels.forEach(function (channel) { channelMetadata[channel.channel_id] = channel; });
      renderChannels(channels, online);
      var jobs = collectQueue(results[0], device);
      settlePending(jobs);
      lastRender = { jobs: jobs, channels: channels, online: online };
      renderQueue(jobs, channels, online);
      renderFailedJobs(results[0].failed_commands || []);
      renderKpiStrip({
        controllerAge: online ? device.age_seconds : NaN,
        waitingCount: jobs.length,
        activeJob: channels.map(function (ch) { return ch.active_job_id; })
                           .find(function (id) { return !!id; }) || null,
        telemetryAge: device && device.updated_at
          ? (Date.now() - Date.parse(device.updated_at)) / 1000 : NaN
      });
      T.el('queue-live-status').className = 'status-badge ' + (online ? 'success' : 'danger');
      T.el('queue-live-status').textContent = online ? 'Controller online' : 'Controller offline';
      T.el('connection-detail').textContent = online ? 'Latest controller status received ' + T.timeAgo(device.updated_at) : 'Waiting for fresh controller telemetry';
      if (liveController) applyLiveController(liveController);
    } catch (error) {
      if (generation === refreshGeneration) {
        lastQueueHtml = null;
        T.el('queue-root').innerHTML = '<div class="inline-error"><strong>Queue unavailable</strong><span>' + esc(error.message) + '</span></div>';
        T.el('queue-live-status').className = 'status-badge danger';
        T.el('queue-live-status').textContent = 'Server unavailable';
      }
    } finally {
      refreshing = false;
      if (refreshQueued) {
        var full = refreshQueuedFull;
        refreshQueued = false;
        refreshQueuedFull = false;
        refresh(!full);
      }
    }
  }

  function markPosted(row) {
    var p = row && pendingJobs[pendingKey(row.kind, row.id)];
    if (p) { p.posted = true; p.ts = Date.now(); }
  }

  /* row = { kind, id, action, label } shows pending feedback on that row only. */
  async function runAction(button, path, payload, success, onAccepted, row, onFailed, flight) {
    T.setButtonBusy(button, true, 'Sending…');
    if (row) setRowPending(row.kind, row.id, row.action, row.label);
    if (flight) (inflight[flight.channel] = inflight[flight.channel] || {})[flight.action] = true;
    /* Release the in-flight flag and busy state the moment the POST settles, never
     * after the follow-up refresh (a hung GET must not keep STOP disabled). */
    var settled = false;
    function settle() {
      if (settled) return;
      settled = true;
      if (flight && inflight[flight.channel]) delete inflight[flight.channel][flight.action];
      T.setButtonBusy(button, false);
      reapplyInterlocks();
    }
    try {
      /* Only STOP is time-limited. START has no abort timeout: an aborted START
       * could still have been accepted by the server. */
      var result = await post(path, payload, flight && flight.action === 'PUMP_STOP' ? REQUEST_TIMEOUT_MS : 0);
      markPosted(row);
      if (onAccepted) onAccepted(result);
      T.toast(success, 'success');
      settle();
      refresh(!!row).catch(function () { /* refresh renders its own error */ });
    } catch (error) {
      if (row) clearRowPending(row.kind, row.id);
      if (onFailed) onFailed(error);
      else T.toast(error.message, 'error', true);
    } finally {
      settle();
    }
  }

  /* Row actions name the job by whichever id that row actually carries: a job
   * already on the device is addressed by its local id, one the server still
   * owns by its command id. */
  function jobTarget(button, attr) {
    var id = Number(button.getAttribute(attr));
    return button.dataset.kind === 'command' ? { command_id: id } : { local_job_id: id };
  }

  function stopJob(button) {
    return runAction(button, '/queue/control',
      Object.assign({ action: 'HOLD' }, jobTarget(button, 'data-stop')),
      'Job parked. It will not start until you press Start.', null,
      { kind: button.dataset.kind, id: button.getAttribute('data-stop'), action: 'HOLD', label: 'Holding…' });
  }

  async function startJob(button) {
    /* Start means "dispense this one next". On a parked row that must also clear
     * the hold — promote alone only moves a job up a line it is still barred
     * from. RELEASE goes first so a failure leaves the job parked rather than
     * promoted-but-ineligible. Neither call touches a running job: the queue is
     * non-preemptive and Start never preempts. */
    var target = jobTarget(button, 'data-start');
    var row = { kind: button.dataset.kind, id: button.getAttribute('data-start'), action: PENDING_PROMOTE, label: 'Promoting…' };
    T.setButtonBusy(button, true, 'Sending…');
    setRowPending(row.kind, row.id, row.action, row.label);
    try {
      if (button.dataset.held === 'true') {
        await post('/queue/control', Object.assign({ action: 'RELEASE' }, target));
      }
      await post('/queue/control', Object.assign({ action: 'PROMOTE' }, target));
      markPosted(row);
      T.toast('Job moved to the front of the queue.', 'success');
      await refresh(true);
    } catch (error) {
      clearRowPending(row.kind, row.id);
      T.toast(error.message, 'error', true);
    } finally {
      T.setButtonBusy(button, false);
      reapplyInterlocks();
    }
  }

  /* Cancel is addressed by the row's own id and nothing else. Sibling jobs —
   * including ones with a duplicate target weight — are never touched. */
  function cancelJob(button) {
    var id = button.getAttribute('data-cancel');
    var kind = button.dataset.kind;
    var path = kind === 'command' ? '/queue/jobs/' + encodeURIComponent(id) + '/cancel' : '/queue/control';
    var payload = kind === 'command' ? {} : { action: 'CANCEL', local_job_id: Number(id) };
    /* The row disappears at once (pending CANCEL hides it); it is restored if
     * the POST fails. */
    return runAction(button, path, payload, 'Job cancelled.', null,
      { kind: kind, id: id, action: 'CANCEL', label: 'Cancelling…' });
  }

  /* ZERO/TARE: confirm, send, then poll the command for the sender's ACK
   * result. Advisory only; the firmware decides and may refuse. */
  async function scaleCommand(button) {
    var action = button.dataset.scale, channel = button.dataset.channel;
    if (!window.confirm(action + ' the scale for ' + channel + '? Only do this with the scale empty and no dispense running.')) return;
    T.setButtonBusy(button, true, 'Sending…');
    try {
      var sent = await post('/queue/control', { action: action, channel_id: channel });
      T.toast(action + ' sent to scale. Waiting for result…', 'success');
      for (var i = 0; i < 15; i++) {
        await new Promise(function (resolve) { setTimeout(resolve, 1000); });
        var c = await T.apiGet('/queue/control/' + encodeURIComponent(sent.command_id));
        if (c.result || c.state === 'APPLIED' || c.state === 'FAILED') {
          var ok = c.result === 'success' || (!c.result && c.state === 'APPLIED');
          var text = action + ' ' + (c.result || (ok ? 'success' : 'failed')) +
            (c.reason || c.error ? ': ' + (c.reason || c.error) : '') +
            (ok && c.weight_g != null ? ' (' + c.weight_g + ' g' + (c.stable ? ', stable' : '') + ')' : '');
          return T.toast(text, ok ? 'success' : 'error', !ok);
        }
      }
      T.toast(action + ' result not received yet.', 'error', true);
    } catch (error) {
      T.toast(error.message, 'error', true);
    } finally {
      T.setButtonBusy(button, false);
      reapplyInterlocks();
    }
  }

  document.addEventListener('click', function (event) {
    var scale = event.target.closest('[data-scale]');
    if (scale) return scaleCommand(scale);
    var control = event.target.closest('[data-control]');
    if (control) {
      var action = control.dataset.control;
      var channel = control.dataset.channel;
      /* Track pending pump commands so the UI shows COMMAND PENDING until
       * the controller confirms the relay state. Never optimistically flips. */
      var isPump = action === 'PUMP_START' || action === 'PUMP_STOP';
      var entry = null;
      if (action === 'PUMP_START' && pendingPump[channel] && pendingPump[channel].outcomeUnknown &&
          typeof window.confirm === 'function' &&
          !window.confirm('A previous START may still be delivered. Send another START?')) {
        return;
      }
      if (isPump) {
        entry = { action: action, ts: Date.now() };
        if (action === 'PUMP_STOP') stopStatus[channel] = entry;
        else { pendingPump[channel] = entry; delete startExpired[channel]; }
      }
      return runAction(control, '/queue/control', { action: action, channel_id: channel },
        action + ' command queued.',
        isPump ? function (sent) {
          var p = action === 'PUMP_STOP' ? stopStatus[channel] : pendingPump[channel];
          if (p === entry && sent && sent.command_id != null) {
            p.commandId = sent.command_id;
            p.cmdState = sent.state || 'PENDING';
            if (action === 'PUMP_START') p.ts = Date.now(); /* confirmation window starts when the POST settled */
            if (action === 'PUMP_STOP') trackPumpCommand(channel, sent.command_id);
          }
        } : null,
        null,
        isPump ? function (error) {
          var msg = error.status === 401 ? 'API key required' : error.message;
          if (action === 'PUMP_STOP' && (error.timeout || !error.status)) {
            /* Hung POST or network error: delivery is unknown. Keep the entry so the note and badge say so. */
            if (stopStatus[channel] === entry) {
              entry.requestTimedOut = true;
              if (!error.timeout) entry.requestNetworkError = true;
            }
            msg = (error.timeout ? 'request timed out' : 'request failed (network)') + ' - use the hardwired emergency stop';
          } else if (action === 'PUMP_STOP') {
            if (stopStatus[channel] === entry) delete stopStatus[channel];
          } else if (!error.status) {
            /* START got no HTTP answer (network/timeout): it may still be delivered. */
            if (pendingPump[channel] === entry) { entry.outcomeUnknown = true; entry.unknownAt = Date.now(); }
            T.toast('START outcome unknown - it may still be delivered; check the controller before retrying', 'error', true);
            return;
          } else if (pendingPump[channel] === entry) {
            delete pendingPump[channel];
          }
          T.toast(action.replace('PUMP_', '') + ' failed' + (error.status ? ' (HTTP ' + error.status + ')' : '') + ': ' + msg, 'error', true);
        } : null,
        isPump ? { channel: channel, action: action } : null);
    }
    var start = event.target.closest('[data-start]');
    if (start) return startJob(start);
    var stop = event.target.closest('[data-stop]');
    if (stop) return stopJob(stop);
    var cancel = event.target.closest('[data-cancel]');
    if (cancel) return cancelJob(cancel);
    var resubmit = event.target.closest('[data-resubmit]');
    if (resubmit) {
      var unknown = resubmit.getAttribute('data-unknown') === '1';
      if (unknown && !window.confirm('The controller could not tell whether this job already ran. The material may already have been dispensed. Resubmit anyway?')) {
        return;
      }
      return runAction(resubmit, '/queue/jobs/' + encodeURIComponent(resubmit.getAttribute('data-resubmit')) + '/resubmit', unknown ? { confirm_unknown: true } : {}, 'Job resubmitted.');
    }
  });

  document.addEventListener('DOMContentLoaded', function () {
    T.startHealthIndicator();
    T.el('job-material').addEventListener('change', refreshProfileOptions);
    T.el('job-target').addEventListener('change', refreshProfileOptions);
    T.el('job-profile').addEventListener('change', function () {
      var rows = profilesCache[T.el('job-material').value + ':' + Number(T.el('job-target').value)] || [];
      renderProfilePreview(profileById(rows, T.el('job-profile').value));
    });
    T.el('job-form').addEventListener('submit', async function (event) {
      event.preventDefault();
      var button = T.el('add-job');
      var targetG = Number(T.el('job-target').value);
      if (!Number.isFinite(targetG) || targetG <= 0) return T.toast('Select a target weight.', 'error');
      var pinRows = profilesCache[T.el('job-material').value + ':' + targetG] || [];
      var pin = profileById(pinRows, T.el('job-profile').value);
      if (!pin || !Number(pin.profile_version_id)) {
        return T.toast('Select the exact PID profile version this job must run with.', 'error');
      }
      T.setButtonBusy(button, true, 'Adding…');
      try {
        /* The server checks the id against the job's own material and target;
         * id + version are sent too so a mismatch is rejected, not guessed. */
        await post('/queue/jobs', {
          material_id: T.el('job-material').value,
          target_g: targetG,
          priority: Number(T.el('job-priority').value),
          profile_version_id: Number(pin.profile_version_id),
          profile_id: pin.profile_id,
          profile_version: Number(pin.version)
        });
        T.el('job-message').textContent = 'Job accepted with ' + profileContext(pin) + '.';
        T.toast('Job added to the dispensing queue.', 'success');
        await refresh();
      } catch (error) {
        T.el('job-message').textContent = error.message;
        T.toast(error.message, 'error', true);
      } finally { T.setButtonBusy(button, false); }
    });
    refreshProfileOptions();
    refresh();
    T.subscribeLive(function (data) {
      liveController = data.controller;
      applyLiveController(liveController);
    });
    /* Paused while the tab is hidden; one immediate refresh on return. */
    T.visibleInterval(function () { refresh(); }, 2000);
  });
})();
