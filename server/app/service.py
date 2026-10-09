"""Headless MQTT backend service: ``python -m app.service`` (CONTRACT 9.1, 9.2, 9.5, 9.8).

No FastAPI, Starlette or uvicorn on this import path. Observability only: it stores
what relays report and publishes ``run/ack``; it never drives a relay and is never
part of any safety loop.

Threads
  paho network   I/O only. on_message classifies, size-checks and enqueues; no JSON
                 parsing, no SQL.
  writer         the only thread that touches the DB for inbound traffic. Sample
                 batches are grouped per run_id (<= 200 rows or 250 ms per commit);
                 start / events / complete / acks / presence are written at once.
  scheduler      heartbeat, command expiry and registered hooks.

Two ingest lanes share one arrival order. The bulk lane (run samples, live,
weight) is bounded and drops + counts when full; a dropped sample batch is simply
never acked, so the relay resends it. The priority lane (run start / events /
complete, command ACKs, presence, RPC requests) is unbounded and never drops.

Extension points for the RPC agent (call before ``start()``):
  add_subscription(filter, qos, handler, lane="priority", max_bytes=65536, ...)
  register_hook(name, interval_s, fn)   scheduler thread; fn() must be quick
  publish(topic, payload, qos, retain)  False if the link is down (never queued)
  on_recovered(fn)                      runs once after startup_recovery
"""
import contextlib
import json
import logging
import os
import signal
import threading
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from typing import Callable

from sqlalchemy import select

from . import database, mqtt_bridge, recovery, run_ingest
from .config import settings
from .models import DispenseRun
from .services import telemetry_ingest
from .services.errors import ServiceError

log = logging.getLogger("service")

BUILD = "svc-1"
STATE_BACKEND_TOPIC = "bits/v1/ops/state/backend"
HEARTBEAT_S = 2.0
BULK_MAX = int(os.getenv("INGEST_BULK_MAX", "2000"))
SAMPLE_BATCH_ROWS = 200
SAMPLE_BATCH_S = 0.25
PRIORITY_WARN_DEPTH = 5000
DRAIN_TIMEOUT_S = 10.0
RUN_STALE_S = float(os.getenv("RUN_STALE_SECONDS", "30"))
DB_READY_TIMEOUT_S = float(os.getenv("DB_READY_TIMEOUT_S", "60"))


class StartupError(RuntimeError):
    pass


class Subscription:
    def __init__(self, name, filter_, qos, handler, lane, max_bytes, allow_retained):
        self.name, self.filter, self.qos, self.handler = name, filter_, qos, handler
        self.lane, self.max_bytes, self.allow_retained = lane, max_bytes, allow_retained


class Item:
    __slots__ = ("n", "t", "sub", "topic", "payload", "retain", "call")

    def __init__(self, n, sub, topic, payload, retain, call=None):
        self.n, self.t, self.sub, self.topic = n, time.monotonic(), sub, topic
        self.payload, self.retain, self.call = payload, retain, call


class Service:
    def __init__(self, client_factory: Callable | None = None, ops_plane: bool = True):
        self._client_factory = client_factory or self._paho_client
        self._client = None
        self.epoch = uuid.uuid4().hex[:12]
        self.run_stale_s = RUN_STALE_S  # STALLED flag threshold only (CONTRACT 9.8)
        self._boot_seen: dict[str, str] = {}
        self.started_mono = time.monotonic()
        self._subs: list[Subscription] = []
        self._hooks: list[dict] = []
        self._hooks_lock = threading.Lock()
        self._recovered_hooks: list[Callable] = []
        # ingest lanes (one arrival order)
        self._cv = threading.Condition()
        self._prio: deque[Item] = deque()
        self._bulk: deque[Item] = deque()
        self._counter = 0
        self._accepting = False
        self._stop = threading.Event()        # request_stop(): wakes wait()
        self._writer_stop = threading.Event()
        self._sched_stop = threading.Event()
        self._writer: threading.Thread | None = None
        self._scheduler: threading.Thread | None = None
        # sample batching (writer thread only)
        self._sbuf: dict[str, dict] = {}
        # connection state
        self._conn_lock = threading.Lock()
        self._connected = False
        self._granted = False
        self._pending_mids: set = set()
        self._recovery_requested = False
        self.recovered = False
        self.hb_seq = 0
        self._lag_ms = 0
        self._last_ingest_mono: float | None = None
        self._db_ok = False
        self._migration = {"state": "UNKNOWN", "t": 0.0}
        self.stats = dict(bulk_dropped=0, retained_ignored=0, malformed=0, oversize=0,
                          write_fail=0, handler_error=0, acks=0, ack_not_sent=0,
                          samples_committed=0, commits=0)
        self._log_times: dict[str, float] = {}
        self._sqlite_lock = threading.RLock()
        self._add_default_subscriptions()
        self.ops = None
        if ops_plane:  # CONTRACT 9.4/9.5 operator plane (bits/v1/ops/* only)
            from .rpc.ops_plane import OpsPlane
            self.ops = OpsPlane(self)

    # -- configuration -------------------------------------------------------

    def add_subscription(self, filter_: str, qos: int, handler: Callable,
                         lane: str = "priority", max_bytes: int = 65536,
                         allow_retained: bool = False, name: str | None = None) -> None:
        """handler(topic, payload: bytes, retain: bool) runs on the writer thread."""
        if self._client is not None:
            raise RuntimeError("add_subscription must be called before start()")
        self._subs.append(Subscription(name or filter_, filter_, qos, handler, lane,
                                       max_bytes, allow_retained))

    def register_hook(self, name: str, interval_s: float, fn: Callable) -> None:
        with self._hooks_lock:
            self._hooks.append({"name": name, "every": interval_s, "fn": fn, "next": 0.0})

    def _kick(self, name: str) -> None:
        with self._hooks_lock:
            for hook in self._hooks:
                if hook["name"] == name:
                    hook["next"] = 0.0

    def on_recovered(self, fn: Callable) -> None:
        self._recovered_hooks.append(fn)

    def _add_default_subscriptions(self) -> None:
        for kind, lane in (("start", "priority"), ("samples", "bulk"),
                           ("events", "priority"), ("complete", "priority")):
            self.add_subscription(f"cas/+/run/{kind}", 1, self._noop, lane,
                                  run_ingest.MAX_BYTES[kind], False, name="run")
        bridge = lambda topic, payload, retain: mqtt_bridge.handle_message(  # noqa: E731
            topic, payload, retain, enforce_pin=True)
        self.add_subscription("cas/+/status", 1, self._presence, "priority", 256, True, name="presence")
        self.add_subscription("cas/+/commands/ack", 1, bridge, "priority", 1024, False, name="ack")
        self.add_subscription("cas/+/telemetry/status", 0, self._tstatus, "bulk", 65536, False,
                              name="tstatus")
        self.add_subscription("cas/+/telemetry/live", 0, bridge, "bulk", 2048, False, name="live")
        self.add_subscription("cas/+/telemetry/weight", 0, bridge, "bulk", 512, False, name="weight")

    @staticmethod
    def _noop(*_a) -> None:
        return None

    def _presence(self, topic: str, payload: bytes, retain: bool) -> None:
        """Bridge presence handling, plus CONTRACT 9.2: a birth with a NEW boot_id ends
        every RUNNING run of that device from an older boot as INTERRUPTED (never resumed),
        even if the relay never starts another run."""
        mqtt_bridge.handle_message(topic, payload, retain, enforce_pin=True)
        if retain:
            return  # a retained birth is a replay, not a boot
        parsed = mqtt_bridge.parse_topic(topic)
        try:
            body = json.loads(payload)
        except ValueError:
            return
        boot = mqtt_bridge._valid_boot_id(body) if isinstance(body, dict) else None
        if parsed is None or boot is None or body.get("online") is not True:
            return
        self._interrupt_old_boot(parsed[0], boot)

    def _interrupt_old_boot(self, device_id: str, boot: str) -> None:
        with self._session() as db:
            for run in db.scalars(select(DispenseRun).where(
                    DispenseRun.device_id == device_id, DispenseRun.status == "RUNNING",
                    DispenseRun.boot_id.is_not(None), DispenseRun.boot_id != boot)).all():
                telemetry_ingest.interrupt_run(db, run, "DEVICE_REBOOTED")

    def _tstatus(self, topic: str, payload: bytes, retain: bool) -> None:
        """Bridge status handling, plus the boot_id check for a birth the backend missed
        while it was down (staleness alone never ends a run, CONTRACT 9.8)."""
        mqtt_bridge.handle_message(topic, payload, retain, enforce_pin=True)
        parsed = mqtt_bridge.parse_topic(topic)
        if retain or parsed is None:
            return
        try:
            body = json.loads(payload)
        except ValueError:
            return
        boot = mqtt_bridge._valid_boot_id(body) if isinstance(body, dict) else None
        if boot is None or self._boot_seen.get(parsed[0]) == boot:
            return
        if len(self._boot_seen) < 64:
            self._boot_seen[parsed[0]] = boot
        self._interrupt_old_boot(parsed[0], boot)

    # -- helpers -------------------------------------------------------------

    def _log_limited(self, key: str, message: str, *args, interval: float = 30.0) -> None:
        now = time.monotonic()
        if now - self._log_times.get(key, -interval) >= interval:
            self._log_times[key] = now
            log.warning(message, *args)

    @contextlib.contextmanager
    def _session(self):
        if not database._schema_initialized:
            database.init_db()
        engine = database.get_engine()
        # SQLite (tests/dev) uses ONE shared connection (StaticPool): another thread's
        # checkout/close would roll back an in-flight transaction. MySQL has a pool.
        guard = self._sqlite_lock if engine.dialect.name == "sqlite" else contextlib.nullcontext()
        with guard:
            db = database._SessionLocal()
            try:
                yield db
            except Exception:
                db.rollback()
                raise
            finally:
                db.close()

    # -- state ---------------------------------------------------------------

    def mqtt_state(self) -> str:
        if self._client is None:
            return "STOPPED"
        with self._conn_lock:
            return "CONNECTED" if self._connected and self._granted else "DISCONNECTED"

    def is_connected(self) -> bool:
        return self.mqtt_state() == "CONNECTED"

    def queue_depths(self) -> dict:
        with self._cv:
            return {"prio": len(self._prio), "bulk": len(self._bulk)}

    # -- lifecycle -----------------------------------------------------------

    def start(self) -> None:
        log.info("backend service starting (epoch %s)", self.epoch)
        self._wait_for_db()
        from .services.live_state import live_state
        live_state.clear()  # memory is empty until fresh non-retained telemetry
        self._accepting = True
        self._writer = threading.Thread(target=self._writer_loop, name="svc-writer", daemon=True)
        self._writer.start()
        self._scheduler = threading.Thread(target=self._scheduler_loop, name="svc-sched", daemon=True)
        self._scheduler.start()
        if self.ops is not None:
            self.ops.start()
        self.register_hook("heartbeat", HEARTBEAT_S, self.publish_heartbeat)
        self.register_hook("command_expiry", 5.0, self._expire_commands)
        self.register_hook("stats", 60.0, self._log_stats)
        client = self._client_factory()
        client.on_connect, client.on_disconnect = self._on_connect, self._on_disconnect
        client.on_subscribe, client.on_message = self._on_subscribe, self._on_message
        with contextlib.suppress(Exception):
            client.max_queued_messages_set(0)
            client.reconnect_delay_set(min_delay=1, max_delay=30)
        if settings.mqtt_username:
            client.username_pw_set(settings.mqtt_username, settings.mqtt_password or None)
        client.will_set(STATE_BACKEND_TOPIC, json.dumps({"online": False}), qos=1, retain=True)
        self._client = client
        client.connect_async(settings.mqtt_broker_host, settings.mqtt_broker_port, keepalive=30)
        client.loop_start()
        mqtt_bridge.set_publisher(mqtt_bridge.PahoPublisher(client))
        log.info("MQTT connecting to %s:%s", settings.mqtt_broker_host, settings.mqtt_broker_port)

    def request_stop(self) -> None:
        self._stop.set()

    def wait(self) -> None:
        while not self._stop.wait(0.5):
            pass

    def stop(self) -> None:
        if self._client is None and self._writer is None:
            return
        log.info("backend service stopping")
        self._accepting = False
        self._writer_stop.set()
        with self._cv:
            self._cv.notify_all()
        if self._writer is not None:
            self._writer.join(DRAIN_TIMEOUT_S + 5)
        self._sched_stop.set()
        if self._scheduler is not None:
            self._scheduler.join(5)
        if self.ops is not None:
            self.ops.stop()
        mqtt_bridge.set_publisher(None)
        client = self._client
        if client is not None:
            with contextlib.suppress(Exception):
                self._publish_state(online=False, graceful=True)
            with contextlib.suppress(Exception):
                client.disconnect()
            with contextlib.suppress(Exception):
                client.loop_stop()
        with self._conn_lock:
            self._connected = self._granted = False
        self._client = None
        self._writer = self._scheduler = None
        log.info("backend service stopped")

    def _wait_for_db(self) -> None:
        deadline = time.monotonic() + DB_READY_TIMEOUT_S
        delay = 0.5
        while True:
            try:
                database.init_db()
                if database.database_ok():
                    self._db_ok = True
                    log.info("database ready")
                    return
            except Exception as exc:
                self._log_limited("dbwait", "database not ready: %s", exc.__class__.__name__, interval=10)
            if self._stop.is_set() or time.monotonic() >= deadline:
                raise StartupError("database not ready; refusing to start")
            time.sleep(delay)
            delay = min(delay * 2, 5.0)

    # -- paho callbacks (network thread: no SQL, no parsing) ------------------

    @staticmethod
    def _paho_client():
        import paho.mqtt.client as mqtt
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                           client_id=os.getenv("MQTT_CLIENT_ID", "bits-backend"))

    def _on_connect(self, client, _userdata, _flags, reason_code, _props=None):
        if getattr(reason_code, "is_failure", False):
            self._log_limited("connect", "MQTT connect refused: %s", reason_code)
            return
        mqtt_bridge.drop_stale_on_connect(client)
        with self._conn_lock:
            self._connected, self._granted = True, False
            self._pending_mids = set()
            for sub in self._subs:
                result = client.subscribe(sub.filter, qos=sub.qos)
                mid = result[1] if isinstance(result, tuple) else None
                if mid is None or result[0] != 0:
                    self._pending_mids.add(("failed", sub.filter))
                else:
                    self._pending_mids.add(mid)
        log.info("MQTT connected; waiting for %d subscriptions", len(self._subs))

    def _on_subscribe(self, _client, _userdata, mid, reason_codes, _props=None):
        denied = any(getattr(rc, "is_failure", False) or (isinstance(rc, int) and rc >= 128)
                     for rc in (reason_codes or ()))
        with self._conn_lock:
            if denied:
                self._pending_mids.add(("denied", mid))
                self._log_limited("suback", "MQTT subscription refused (mid %s)", mid)
            self._pending_mids.discard(mid)
            granted = self._connected and not self._pending_mids
            self._granted = granted
            first = granted and not self._recovery_requested
            if first:
                self._recovery_requested = True
        if granted:
            log.info("MQTT subscribed; state CONNECTED")
            self._kick("heartbeat")  # scheduler publishes it; no SQL on the network thread
        if first:  # startup_recovery: once per process, only after SUBACK, never on reconnect
            self._put(Item(0, None, "", b"", False, call=self._run_recovery), "priority")

    def _on_disconnect(self, client, _userdata, _flags, reason_code, _props=None):
        with self._conn_lock:
            was = self._connected
            self._connected = self._granted = False
            self._pending_mids = set()
        mqtt_bridge.drop_queued_outgoing(client)
        if was:
            log.warning("MQTT disconnected (%s); paho will retry", reason_code)

    def _on_message(self, _client, _userdata, msg):
        if not self._accepting:
            return
        topic = msg.topic
        for sub in self._subs:
            if mqtt_topic_matches(sub.filter, topic):
                break
        else:
            return
        if msg.retain and not sub.allow_retained:
            self.stats["retained_ignored"] += 1  # never fresh telemetry or control
            return
        if len(msg.payload) > sub.max_bytes:
            self.stats["oversize"] += 1
            self._log_limited("oversize", "oversize payload dropped on %s", topic)
            return
        self._put(Item(0, sub, topic, msg.payload, bool(msg.retain)), sub.lane)

    def _put(self, item: Item, lane: str) -> bool:
        with self._cv:
            if lane == "bulk" and len(self._bulk) >= BULK_MAX:
                self.stats["bulk_dropped"] += 1
                self._log_limited("bulkfull", "ingest bulk queue full; dropped %d so far",
                                  self.stats["bulk_dropped"])
                return False
            self._counter += 1
            item.n = self._counter
            (self._bulk if lane == "bulk" else self._prio).append(item)
            if lane != "bulk" and len(self._prio) == PRIORITY_WARN_DEPTH:
                log.warning("ingest priority queue depth %d (writer behind)", len(self._prio))
            self._cv.notify()
        return True

    def _next(self, timeout: float) -> Item | None:
        with self._cv:
            if not self._prio and not self._bulk:
                self._cv.wait(timeout)
            if self._prio and (not self._bulk or self._prio[0].n < self._bulk[0].n):
                return self._prio.popleft()
            if self._bulk:
                return self._bulk.popleft()
            return None

    # -- writer thread -------------------------------------------------------

    def _writer_loop(self) -> None:
        drain_deadline = None
        while True:
            wait = 0.25
            if self._sbuf:
                oldest = min(e["t"] for e in self._sbuf.values())
                wait = max(0.0, min(wait, oldest + SAMPLE_BATCH_S - time.monotonic()))
            item = self._next(wait)
            if item is not None:
                self._lag_ms = int((time.monotonic() - item.t) * 1000)
                self._handle(item)
                self._last_ingest_mono = time.monotonic()
            self._flush_due()
            if self._writer_stop.is_set():
                drain_deadline = drain_deadline or time.monotonic() + DRAIN_TIMEOUT_S
                if item is None or time.monotonic() > drain_deadline:
                    break
        self._flush_all()

    def _handle(self, item: Item) -> None:
        try:
            if item.call is not None:
                item.call()
            elif item.sub.name == "run":
                self._handle_run(item)
            else:
                with self._db_probe_guard():  # no-op on MySQL (see _session)
                    item.sub.handler(item.topic, item.payload, item.retain)
        except Exception:
            self.stats["handler_error"] += 1
            self._log_limited(f"handler-{item.topic[:40]}", "handler failed for %s", item.topic)

    def _handle_run(self, item: Item) -> None:
        parsed = run_ingest.parse_topic(item.topic)
        if parsed is None:
            return
        device_id, kind = parsed
        body = run_ingest.decode(item.payload, kind)
        if body is None or body.get("device_id") not in (None, "", device_id):
            self.stats["malformed"] += 1  # unparseable: no run_id to ack, relay keeps its copy
            self._log_limited("malformed", "malformed run/%s payload from %s", kind, device_id)
            return
        if kind == "samples":
            self._buffer_samples(device_id, body)
            return
        self._flush_all()  # keep stored order: samples before events/complete
        self._run_single(device_id, kind, body)

    def _run_single(self, device_id: str, kind: str, body: dict) -> None:
        try:
            with self._session() as db:
                ack = run_ingest.process(db, device_id, kind, body)
            self.stats["commits"] += 1
        except Exception as exc:  # DB down etc.: no commit, therefore no ack
            self.stats["write_fail"] += 1
            self._log_limited("writefail", "run/%s write failed (%s); not acked", kind,
                              exc.__class__.__name__)
            return
        self._publish_ack(device_id, ack)

    def _buffer_samples(self, device_id: str, body: dict) -> None:
        run_id = body.get("run_id")
        if not isinstance(run_id, str):
            self.stats["malformed"] += 1
            return
        samples = body.get("samples")
        rows = len(samples) if isinstance(samples, list) else 0
        entry = self._sbuf.get(run_id)
        if entry is not None and (entry["dev"] != device_id
                                  or entry["rows"] + rows > SAMPLE_BATCH_ROWS):
            self._flush_run(run_id)
            entry = None
        if entry is None:
            entry = self._sbuf[run_id] = {"dev": device_id, "items": [], "rows": 0,
                                          "t": time.monotonic()}
        entry["items"].append(body)
        entry["rows"] += rows
        if entry["rows"] >= SAMPLE_BATCH_ROWS:
            self._flush_run(run_id)

    def _flush_due(self) -> None:
        now = time.monotonic()
        for run_id in [r for r, e in self._sbuf.items() if now - e["t"] >= SAMPLE_BATCH_S]:
            self._flush_run(run_id)

    def _flush_all(self) -> None:
        for run_id in list(self._sbuf):
            self._flush_run(run_id)

    def _flush_run(self, run_id: str) -> None:
        """One commit for the whole group; acks only after it. If the grouped commit
        fails, retry item by item so one bad batch cannot block the rest."""
        entry = self._sbuf.pop(run_id, None)
        if not entry:
            return
        device_id, items = entry["dev"], entry["items"]
        acks = None
        try:
            with self._session() as db:
                acks = [run_ingest.process_samples(db, device_id, body, commit=False)
                        for body in items]
                db.commit()
            self.stats["commits"] += 1
        except Exception as exc:
            log.debug("grouped sample commit failed (%s); retrying per item", exc.__class__.__name__)
            acks = None
        if acks is None:
            acks = []
            for body in items:
                try:
                    with self._session() as db:
                        acks.append(run_ingest.process_samples(db, device_id, body))
                    self.stats["commits"] += 1
                except Exception as exc:
                    self.stats["write_fail"] += 1
                    self._log_limited("writefail", "run/samples write failed (%s); not acked",
                                      exc.__class__.__name__)
        self.stats["samples_committed"] += entry["rows"]
        for ack in acks:
            self._publish_ack(device_id, ack)

    def _publish_ack(self, device_id: str, ack: dict | None) -> None:
        if ack is None:
            return
        if self.publish(run_ingest.ack_topic(device_id), run_ingest.encode_ack(ack), 1, False):
            self.stats["acks"] += 1
        else:
            self.stats["ack_not_sent"] += 1  # link down: relay resends, we ack again

    def _run_recovery(self) -> None:
        try:
            with self._session() as db:
                recovery.startup_recovery(db, RUN_STALE_S)
        except Exception:
            self._log_limited("recovery", "startup recovery failed")
            return
        self.recovered = True
        for fn in self._recovered_hooks:
            try:
                fn()
            except Exception:
                self._log_limited("recovered-hook", "on_recovered hook failed")

    # -- publishing ----------------------------------------------------------

    def publish(self, topic: str, payload: str, qos: int = 1, retain: bool = False) -> bool:
        client = self._client
        if topic.endswith("/weight/ctl"):  # sender -> relay only; the backend never writes it
            self._log_limited("ctl", "refusing to publish weight/ctl")
            return False
        if client is None or not self.is_connected():
            return False
        try:
            info = client.publish(topic, payload, qos=qos, retain=retain)
        except Exception:
            self._log_limited("publish", "publish failed on %s", topic)
            return False
        if getattr(info, "rc", 0) != 0:
            mqtt_bridge.discard_message(client, getattr(info, "mid", None))
            return False
        return True

    def _publish_state(self, online: bool = True, graceful: bool = False) -> bool:
        client = self._client
        if client is None:
            return False
        if not online:
            body = {"online": False, "graceful": graceful, "epoch": self.epoch}
        else:
            body = self.state_snapshot()
        info = client.publish(STATE_BACKEND_TOPIC, json.dumps(body, separators=(",", ":")),
                              qos=1, retain=True)
        return getattr(info, "rc", 0) == 0

    def state_snapshot(self) -> dict:
        now = time.monotonic()
        depths = self.queue_depths()
        with self._cv:
            head = min([q[0].t for q in (self._prio, self._bulk) if q], default=None)
        lag = int((now - head) * 1000) if head is not None else self._lag_ms
        age = (int((now - self._last_ingest_mono) * 1000)
               if self._last_ingest_mono is not None else None)
        self.hb_seq += 1
        return {
            "online": True, "seq": self.hb_seq, "epoch": self.epoch, "build": BUILD,
            "server_time": datetime.now(timezone.utc).isoformat(timespec="milliseconds")
            .replace("+00:00", "Z"),
            "uptime_s": int(now - self.started_mono), "recovered": self.recovered,
            "db": {"ok": self._db_ok, "migration": self._migration["state"]},
            "mqtt": self.mqtt_state(),
            "queues": depths,
            "drops": {"bulk": self.stats["bulk_dropped"], "retained": self.stats["retained_ignored"],
                      "malformed": self.stats["malformed"], "oversize": self.stats["oversize"],
                      "write_fail": self.stats["write_fail"]},
            "writer_lag_ms": lag, "last_ingest_age_ms": age,
        }

    def _db_probe_guard(self):
        return (self._sqlite_lock if database.get_engine().dialect.name == "sqlite"
                else contextlib.nullcontext())

    def publish_heartbeat(self) -> None:
        if not self.is_connected():
            return
        now = time.monotonic()
        with contextlib.suppress(Exception), self._db_probe_guard():
            self._db_ok = database.database_ok()
            if now - self._migration["t"] > 30:
                self._migration = {"state": database.current_migration_status().get("state", "UNKNOWN"),
                                   "t": now}
        with contextlib.suppress(Exception):
            self._publish_state(online=True)

    # -- scheduler thread ----------------------------------------------------

    def _scheduler_loop(self) -> None:
        while not self._sched_stop.wait(0.25):
            now = time.monotonic()
            with self._hooks_lock:
                hooks = list(self._hooks)
            for hook in hooks:
                if now < hook["next"]:
                    continue
                hook["next"] = now + hook["every"]
                try:
                    hook["fn"]()
                except Exception:
                    self._log_limited(f"hook-{hook['name']}", "scheduler hook %s failed", hook["name"])

    def _expire_commands(self) -> None:
        if not self.recovered:
            return
        with self._session() as db:
            n = recovery.expire_pending(db)
        if n:
            log.info("expired %d pending commands", n)

    def _log_stats(self) -> None:
        s = self.stats
        log.info("ingest: commits=%d samples=%d acks=%d dropped_bulk=%d write_fail=%d lag_ms=%d",
                 s["commits"], s["samples_committed"], s["acks"], s["bulk_dropped"],
                 s["write_fail"], self._lag_ms)


def mqtt_topic_matches(sub_filter: str, topic: str) -> bool:
    f, t = sub_filter.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t) or (part != "+" and part != t[i]):
            return False
    return len(f) == len(t)


def main() -> int:
    logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO").upper(),
                        format="%(asctime)s %(levelname)s %(name)s %(message)s")
    svc = Service()
    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), lambda *_: svc.request_stop())
    try:
        svc.start()
    except StartupError as exc:
        log.error("%s", exc)
        svc.stop()
        return 1
    try:
        svc.wait()
    finally:
        svc.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
