"""Glue between the headless Service and the operator-plane dispatcher (CONTRACT 9.4, 9.5).

Observability and operator intent only. This module publishes ONLY under
``bits/v1/ops/``; it never touches ``cas/+/weight/ctl`` and never drives a relay
(control.cmd creates the same DeviceCommand row the HTTP route does).

Threads
  writer      the request handler only enqueues (cheap, never SQL).
  rpc worker  RpcDispatcher.handle() (SQL, argon2); then the reply is published.
  scheduler   state/live (5 s), state/queue + state/devices (change driven), evt/live.
"""
import contextlib
import json
import logging
import queue
import threading
import time
from datetime import datetime, timezone

from .. import recovery
from ..services import control as control_service
from ..services import queue as queue_service
from ..services.live_state import live_state
from .dispatch import RpcDispatcher
from .envelope import MAX_REQUEST_BYTES, TOPIC_PREFIX
from .state import (EVT_LIVE_MAX_BYTES, LiveCoalescer, StateClock, build_devices, build_live,
                    build_queue)

log = logging.getLogger("rpc.glue")

REQ_FILTER = TOPIC_PREFIX + "req/+/+"
STATE_LIVE_S = 5.0
STATE_SYNC_S = 2.0          # how often queue/devices are re-read for changes
STATE_REPUBLISH_S = 30.0    # unchanged state is still re-published this often
RPC_QUEUE_MAX = 256
RPC_WORKERS = 2


def _compact(body: dict) -> str:
    return json.dumps(body, separators=(",", ":"), default=str)


class OpsPlane:
    def __init__(self, svc, dispatcher: RpcDispatcher | None = None,
                 workers: int = RPC_WORKERS, queue_max: int = RPC_QUEUE_MAX):
        self.svc = svc
        clock = StateClock()
        clock.epoch = svc.epoch              # state/backend and every ops payload agree
        self.dispatcher = dispatcher or RpcDispatcher(state_clock=clock)
        self.state = self.dispatcher.state
        # evt/live is the only stream a client can check for seq gaps, so it gets its own
        # contiguous counter (same epoch). The shared dispatcher.state also numbers other
        # clients' responses and the state/* snapshots, which no single client sees in full.
        self.live_seq = StateClock()
        self.live_seq.epoch = self.state.epoch
        self.coalescer = LiveCoalescer()
        self.stats = dict(requests=0, dropped=0, errors=0, reply_not_sent=0)
        self._q: queue.Queue = queue.Queue(maxsize=queue_max)
        self._workers_n = workers
        self._threads: list[threading.Thread] = []
        self._rev = None
        self._last_state: dict[str, tuple[str, float]] = {}
        svc.add_subscription(REQ_FILTER, 1, self._on_request, "priority",
                             MAX_REQUEST_BYTES, False, name="ops-req")
        svc.register_hook("ops_state_live", STATE_LIVE_S, self.publish_state_live)
        svc.register_hook("ops_state_sync", STATE_SYNC_S, self.sync_state)
        svc.register_hook("ops_evt_live", 0.25, self.tick_evt_live)
        svc.on_recovered(self.sync_state)
        self.coalescer.mark("heartbeat", time.monotonic())

    # -- lifecycle -----------------------------------------------------------

    def start(self) -> None:
        for n in range(self._workers_n):
            t = threading.Thread(target=self._work, name=f"rpc-{n}", daemon=True)
            t.start()
            self._threads.append(t)

    def stop(self) -> None:
        for _ in self._threads:
            with contextlib.suppress(queue.Full):
                self._q.put_nowait(None)
        for t in self._threads:
            t.join(5)
        self._threads = []

    # -- requests ------------------------------------------------------------

    def _on_request(self, topic: str, payload: bytes, retain: bool) -> None:
        """Writer thread: enqueue only. A full queue drops (counted); the client's
        corr_id retry after its timeout is answered from the dedupe store."""
        try:
            self._q.put_nowait((topic, payload))
        except queue.Full:
            self.stats["dropped"] += 1
            self.svc._log_limited("rpc-full", "rpc request queue full; dropped %d so far",
                                  self.stats["dropped"])

    def _work(self) -> None:
        while True:
            item = self._q.get()
            if item is None:
                return
            topic, payload = item
            self.stats["requests"] += 1
            try:
                self.handle(topic, payload)
            except Exception:
                self.stats["errors"] += 1
                self.svc._log_limited("rpc-err", "rpc request failed for %s", topic[:60])

    def handle(self, topic: str, payload: bytes) -> None:
        parts = topic.split("/")
        client_id = parts[4] if len(parts) == 6 else None
        with self.svc._db_probe_guard():       # SQLite shares one connection; MySQL: no-op
            reply = self.dispatcher.handle(topic, payload, client_id)
        if reply.topic:
            self._pub(reply.topic, reply.payload, reply.qos, reply.retain, reply=True)
        for ev_topic, body, qos, retain in reply.events:
            self._pub(ev_topic, _compact(body), qos, retain)
        for name in reply.refresh:
            with contextlib.suppress(Exception):
                self.publish_snapshot(name)

    # -- publishing ----------------------------------------------------------

    def _pub(self, topic: str, payload, qos: int, retain: bool, reply: bool = False) -> bool:
        if not topic.startswith(TOPIC_PREFIX):       # hard guard: never cas/#
            log.error("refusing to publish outside %s: %s", TOPIC_PREFIX, topic[:60])
            return False
        ok = self.svc.publish(topic, payload, qos, retain)
        if not ok and reply:
            self.stats["reply_not_sent"] += 1   # client retries the same corr_id
        return ok

    def _now(self) -> datetime:
        return datetime.now(timezone.utc)

    def publish_state_live(self) -> None:
        if not self.svc.is_connected():
            return
        body = build_live(live_state.snapshot(), server_time=self._now(), seq=self.state.next(),
                          epoch=self.state.epoch)
        self._pub(TOPIC_PREFIX + "state/live", _compact(body), 0, True)

    def tick_evt_live(self) -> None:
        if not self.svc.is_connected():
            return
        snap = live_state.snapshot()
        now = time.monotonic()
        emit = False
        if snap["revision"] != self._rev:
            self._rev = snap["revision"]
            ctrl = snap.get("controller") or {}
            emit = self.coalescer.mark(ctrl.get("device_id") or "live", now)
        if self.coalescer.due(now) or emit:
            body = build_live(snap, server_time=self._now(), seq=self.live_seq.next(),
                              epoch=self.state.epoch, kind="evt", max_bytes=EVT_LIVE_MAX_BYTES)
            self._pub(TOPIC_PREFIX + "evt/live", _compact(body), 0, False)

    def sync_state(self) -> None:
        """Re-read queue and devices; publish when changed (or periodically)."""
        if not self.svc.is_connected() or not self.svc.recovered:
            return
        for name in ("queue", "devices"):
            self.publish_snapshot(name, only_if_changed=True)

    def publish_snapshot(self, name: str, only_if_changed: bool = False) -> bool:
        """state/queue or state/devices: QoS 1, retained. The seq is only consumed
        when something is really published, so quiet periods leave no seq gaps."""
        if not self.svc.is_connected():
            return False
        now = self._now()
        with self.svc._session() as db:
            if name == "queue":
                body = build_queue(queue_service.read_queue(db), server_time=now, seq=0,
                                   epoch=self.state.epoch)
            else:
                stale_s = getattr(self.svc, "run_stale_s", None)
                body = build_devices(control_service.read_device_status(db), server_time=now,
                                     seq=0, epoch=self.state.epoch,
                                     stalled=(recovery.stalled_runs(db, stale_s)
                                              if stale_s is not None else None))
        sig = _compact({k: v for k, v in body.items() if k not in ("server_time", "seq")})
        last = self._last_state.get(name)
        mono = time.monotonic()
        if only_if_changed and last and last[0] == sig and mono - last[1] < STATE_REPUBLISH_S:
            return False
        body["seq"] = self.state.next()
        ok = self._pub(TOPIC_PREFIX + "state/" + name, _compact(body), 1, True)
        if ok:
            self._last_state[name] = (sig, mono)
        return ok
