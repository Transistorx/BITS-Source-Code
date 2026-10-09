(function () {
  'use strict';

  var BACKOFF_START_MS = 1000;
  var BACKOFF_MAX_MS = 10000;
  var PING_MS = 10000;
  var SILENT_LIMIT_MS = 25000;
  var KEY_STORE = 'bits_live_ws_key';

  function wsUrl() {
    var scheme = window.location.protocol === 'https:' ? 'wss://' : 'ws://';
    return scheme + window.location.host + '/api/v1/live/ws';
  }

  function storedKey() {
    try { return window.sessionStorage.getItem(KEY_STORE) || ''; } catch (ignored) { return ''; }
  }

  function storeKey(key) {
    try {
      if (key) { window.sessionStorage.setItem(KEY_STORE, key); }
      else { window.sessionStorage.removeItem(KEY_STORE); }
    } catch (ignored) { }
  }

  function connectLiveWs(handlers) {
    var h = handlers || {};
    var sock = null;
    var closed = false;
    var backoff = BACKOFF_START_MS;
    var retryTimer = null;
    var pingTimer = null;
    var lastRx = 0;
    var failures = 0;

    function call(name, a, b) {
      var fn = h[name];
      if (typeof fn !== 'function') { return; }
      try { fn(a, b); } catch (err) { if (window.console) { window.console.error(err); } }
    }

    function stopPing() {
      if (pingTimer !== null) { clearTimeout(pingTimer); pingTimer = null; }
    }

    function schedulePing() {
      stopPing();
      pingTimer = setTimeout(function () {
        pingTimer = null;
        if (!sock || sock.readyState !== 1) { return; }
        if (performance.now() - lastRx > SILENT_LIMIT_MS) { sock.close(); return; }
        try { sock.send('{"type":"ping"}'); } catch (ignored) { }
        schedulePing();
      }, PING_MS);
    }

    function sendObj(obj) {
      if (!sock || sock.readyState !== 1) { return false; }
      try { sock.send(JSON.stringify(obj)); return true; } catch (ignored) { return false; }
    }

    function sendAuth(key) {
      if (!key) { return false; }
      return sendObj({ type: 'auth', api_key: key });
    }

    function onMessage(event) {
      var msg;
      lastRx = performance.now();
      try { msg = JSON.parse(event.data); } catch (ignored) { return; }
      if (!msg || typeof msg !== 'object') { return; }
      switch (msg.type) {
        case 'snapshot': call('onSnapshot', msg); break;
        case 'sender': call('onSender', msg); break;
        case 'broker': call('onBroker', msg); break;
        case 'cmd_state': call('onCmdState', msg); break;
        case 'cmd_ack': call('onCmdAck', msg); break;
        case 'auth_ok': call('onAuth', true); break;
        case 'error':
          if (msg.code === 'unauthorized') { storeKey(''); }
          call('onError', msg);
          break;
        default: break;
      }
    }

    function scheduleRetry() {
      if (closed || retryTimer !== null) { return; }
      var delay = backoff;
      backoff = Math.min(backoff * 2, BACKOFF_MAX_MS);
      retryTimer = setTimeout(function () { retryTimer = null; open(); }, delay);
    }

    function open() {
      if (closed) { return; }
      var opened = false;
      var ws;
      try {
        ws = new WebSocket(wsUrl());
      } catch (ignored) {
        failures++;
        call('onLink', 'down', failures);
        scheduleRetry();
        return;
      }
      sock = ws;
      call('onLink', 'connecting', failures);
      ws.onopen = function () {
        if (sock !== ws) { return; }
        opened = true;
        failures = 0;
        backoff = BACKOFF_START_MS;
        lastRx = performance.now();
        var key = storedKey();
        if (key) { sendAuth(key); }
        schedulePing();
        call('onLink', 'open', 0);
      };
      ws.onmessage = function (event) { if (sock === ws) { onMessage(event); } };
      ws.onerror = function () { };
      ws.onclose = function () {
        if (sock !== ws) { return; }
        sock = null;
        stopPing();
        if (!opened) { failures++; }
        call('onLink', 'down', failures);
        scheduleRetry();
      };
    }

    open();

    return {
      sendCmd: function (deviceId, channel, cmd) {
        return sendObj({ type: 'cmd', device_id: deviceId, channel: channel, cmd: cmd });
      },
      sendAuth: function (key) {
        var ok = sendAuth(key);
        if (ok) { storeKey(key); }
        return ok;
      },
      hasKey: function () { return !!storedKey(); },
      close: function () {
        closed = true;
        stopPing();
        if (retryTimer !== null) { clearTimeout(retryTimer); retryTimer = null; }
        if (sock) { try { sock.close(); } catch (ignored) { } }
        sock = null;
      }
    };
  }

  window.connectLiveWs = connectLiveWs;
})();
