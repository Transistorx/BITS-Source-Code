"""Operator-app simulator for the bits/v1/ops plane (CONTRACT 9.4, 9.5).

``call`` publishes a request envelope QoS 1 and waits for the response on
res/{client_id}/{corr_id}; a timeout on a WRITE is reported as outcome UNKNOWN (never
a fake success) and the caller must ``command.get`` before retrying. ``LiveView``
implements the freshness rules: LIVE only while a NON-retained live message arrived
< 3 s ago by the LOCAL clock; any retained delivery is "last known, UNVERIFIED"; a
seq gap or new epoch on the evt/live stream triggers ``live.resync``; backend heartbeat
absent for 6 s means OFFLINE.
"""
import json
import threading
import time
import uuid
from datetime import datetime, timezone

from .mqttx import BrokerInfo, Link

PREFIX = "bits/v1/ops/"
LIVE_FRESH_S = 3.0
BACKEND_OFFLINE_S = 6.0


def iso_ms(dt: datetime | None = None) -> str:
    dt = dt or datetime.now(timezone.utc)
    return dt.strftime("%Y-%m-%dT%H:%M:%S.") + f"{dt.microsecond // 1000:03d}Z"


class Unknown(Exception):
    """A write got no answer in time: the outcome is UNKNOWN (call command.get)."""


class LiveView:
    """State machine behind the LIVE / RECONNECTING / OFFLINE / STALE / FAULT badge."""

    def __init__(self, clock=time.monotonic):
        self._clock = clock
        self.lock = threading.Lock()
        self.connected = False
        self.backend: dict | None = None
        self.backend_seen: float | None = None        # local time of last NON-retained heartbeat
        self.backend_retained_only = True
        self.live: dict | None = None
        self.live_seen: float | None = None           # local time of last NON-retained live data
        self.live_retained = False
        self.last_evt_seq: int | None = None
        self.epoch: str | None = None
        self.resyncs = 0
        self.gaps = 0
        self.events: list[dict] = []                  # evt/command bodies

    def on_connection(self, up: bool) -> None:
        with self.lock:
            self.connected = up

    def on_message(self, topic: str, body: dict, retained: bool) -> bool:
        """True when the caller should send live.resync."""
        now = self._clock()
        resync = False
        with self.lock:
            if topic == PREFIX + "state/backend":
                if not retained:
                    self.backend_seen, self.backend_retained_only = now, False
                self.backend = body
            elif topic == PREFIX + "state/live":
                self.live = body
                self.live_retained = retained
                if not retained:
                    self.live_seen = now
                resync |= self._epoch(body)
            elif topic == PREFIX + "evt/live":
                self.live, self.live_retained, self.live_seen = body, False, now
                resync |= self._epoch(body)
                seq = body.get("seq")
                if isinstance(seq, int):
                    if self.last_evt_seq is not None and seq != self.last_evt_seq + 1:
                        self.gaps += 1
                        resync = True
                    self.last_evt_seq = seq
            elif topic == PREFIX + "evt/command":
                self.events.append(body)
            if resync:
                self.resyncs += 1
        return resync

    def _epoch(self, body: dict) -> bool:
        ep = body.get("epoch")
        if ep is None:
            return False
        changed = self.epoch is not None and ep != self.epoch
        self.epoch = ep
        if changed:
            self.last_evt_seq = None
        return changed

    def adopt_snapshot(self, live: dict) -> None:
        """live.resync answer: a non-retained full snapshot."""
        now = self._clock()
        with self.lock:
            self.live, self.live_retained, self.live_seen = live, False, now
            self.last_evt_seq = None        # evt/live has its own counter: resume at the next one
            self.epoch = live.get("epoch", self.epoch)

    def status(self) -> str:
        now = self._clock()
        with self.lock:
            if not self.connected:
                return "RECONNECTING"
            if self.backend and self.backend.get("online") is False and self.backend_seen is not None \
                    and now - self.backend_seen < BACKEND_OFFLINE_S:
                return "OFFLINE"                     # LWT or graceful stop honoured at once
            if self.backend_seen is None or now - self.backend_seen > BACKEND_OFFLINE_S:
                return "OFFLINE"
            if self.backend and self.backend.get("db", {}).get("ok") is False:
                return "FAULT"
            if self.live_seen is None or now - self.live_seen >= LIVE_FRESH_S:
                return "STALE"
            return "LIVE"

    def last_known_unverified(self) -> bool:
        with self.lock:
            return self.live is not None and (self.live_seen is None or self.live_retained)

    def device(self, device_id: str) -> dict | None:
        with self.lock:
            for d in (self.live or {}).get("devices", []):
                if d.get("device_id") == device_id:
                    return d
        return None

    def weight(self, device_id: str, channel: str):
        """Displayed weight: None unless the view is LIVE and the device not stale."""
        if self.status() != "LIVE":
            return None
        dev = self.device(device_id)
        if not dev or dev.get("stale"):
            return None
        for c in dev.get("channels", []):
            if c.get("channel_id") == channel and c.get("weight_valid"):
                return c.get("weight_g")
        return None


class OperatorSim:
    def __init__(self, station: str, broker: BrokerInfo):
        self.client_id = f"app_{station}"
        self.token: str | None = None
        self.view = LiveView()
        self.responses: dict[str, dict] = {}
        self._waiters: dict[str, threading.Event] = {}
        self._lock = threading.Lock()
        self.retained_seen: list[str] = []
        self.link = Link(broker, self.client_id, subs=[
            (f"{PREFIX}res/{self.client_id}/#", 1), (PREFIX + "state/#", 1), (PREFIX + "evt/#", 0)],
            on_message=self._on_msg, on_ready=lambda: self.view.on_connection(True),
            on_lost=lambda: self.view.on_connection(False))
        self.seq = 0

    def start(self) -> "OperatorSim":
        self.link.start()
        assert self.link.wait_ready(10), "operator sim could not connect"
        return self

    def stop(self) -> None:
        self.link.close()

    def crash(self) -> None:
        self.link.crash()

    def _on_msg(self, topic: str, payload: bytes, retained: bool) -> None:
        try:
            body = json.loads(payload)
        except ValueError:
            return
        if not isinstance(body, dict):
            return
        if topic.startswith(f"{PREFIX}res/{self.client_id}/"):
            corr = topic.rsplit("/", 1)[1]
            with self._lock:
                self.responses[corr] = body
                ev = self._waiters.get(corr)
            if ev:
                ev.set()
            return
        if retained:
            self.retained_seen.append(topic)
        if self.view.on_message(topic, body, retained):
            threading.Thread(target=self._resync, daemon=True).start()

    def _resync(self) -> None:
        if not self.token:
            return
        try:
            r = self.call("live.resync", timeout=5)
        except (Unknown, TimeoutError):
            return
        if r.get("ok") and r.get("data"):
            self.view.adopt_snapshot(r["data"]["live"])

    # -- rpc ----------------------------------------------------------------------------
    def request(self, method: str, args: dict | None = None, *, corr: str | None = None,
                ttl_ms: int = 10000, token: str | None = ..., raw: bytes | str | None = None) -> str:
        corr = corr or f"c{uuid.uuid4().hex[:12]}"
        self.seq += 1
        body = {"v": 1, "corr_id": corr, "method": method, "args": args or {},
                "session_token": self.token if token is ... else token,
                "issued_at": iso_ms(), "ttl_ms": ttl_ms, "client_seq": self.seq}
        with self._lock:
            self._waiters.setdefault(corr, threading.Event())
        self.link.publish(f"{PREFIX}req/{self.client_id}/{method}",
                          raw if raw is not None else json.dumps(body, separators=(",", ":")), 1)
        return corr

    def wait(self, corr: str, timeout: float) -> dict | None:
        ev = self._waiters.get(corr)
        if ev and ev.wait(timeout):
            return self.responses.get(corr)
        return self.responses.get(corr)

    def call(self, method: str, args: dict | None = None, *, timeout: float | None = None,
             corr: str | None = None, write: bool | None = None, **kw) -> dict:
        """Response envelope. Timeouts: 5 s reads, 10 s writes; a timed-out write raises
        Unknown (outcome unknown: command.get before any retry)."""
        is_write = write if write is not None else method.split(".")[0] in (
            "queue", "control", "run", "profiles", "materials") and method not in (
            "queue.list", "run.get", "run.events", "run.samples", "profiles.list",
            "profiles.active", "profiles.nextversion", "materials.list")
        corr = self.request(method, args, corr=corr, **kw)
        resp = self.wait(corr, timeout if timeout is not None else (10.0 if is_write else 5.0))
        if resp is None:
            if is_write:
                raise Unknown(f"{method} corr={corr}: outcome unknown")
            raise TimeoutError(f"{method} corr={corr}")
        return resp

    def login(self, username: str, password: str) -> dict:
        r = self.call("auth.login", {"username": username, "password": password}, token=None)
        if r["ok"]:
            self.token = r["data"]["session_token"]
        return r

    def confirm_call(self, method: str, args: dict) -> dict:
        """Two-step dangerous action: confirm.begin then the real request carrying the token."""
        b = self.call("confirm.begin", {"method": method, "args": args})
        if not b["ok"]:
            return b
        return self.call(method, dict(args, confirm_token=b["data"]["confirm_token"]))
