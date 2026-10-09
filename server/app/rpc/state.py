"""Pure builders for the operator-plane state/evt payloads (CONTRACT 9.5).

Inputs are plain dicts: ``live_state.snapshot()``, ``queue_service.read_queue()``
and ``control_service.read_device_status()``. Nothing here touches the broker, the
database or weight/ctl. Weight comes from the 1-2 Hz telemetry copy held in
live_state; it is ``None`` (never 0) whenever it is invalid or stale. Retained
delivery is a transport property: these payloads never claim to be live on their own.
"""

import json
import threading
from datetime import datetime
from uuid import uuid4

from .envelope import iso_ms

WEIGHT_STALE_MS = 3000
STATUS_STALE_MS = 10000
LIVE_MAX_BYTES = 4096
EVT_LIVE_MAX_BYTES = 2048
QUEUE_MAX_BYTES = 16384
DEVICES_MAX_BYTES = 8192
EVT_MIN_INTERVAL_S = 0.5   # 2 Hz per dispenser
HEARTBEAT_S = 1.0          # 1 Hz heartbeat


class StateClock:
    """epoch (backend start id) + monotonically increasing seq shared by all payloads."""

    def __init__(self):
        self.epoch = uuid4().hex[:8]
        self._seq = 0
        self._lock = threading.Lock()

    def next(self) -> int:
        with self._lock:
            self._seq += 1
            return self._seq


def _channel(ch: dict) -> dict:
    age = ch.get("weight_age_ms")
    age = age if isinstance(age, (int, float)) and not isinstance(age, bool) and age >= 0 else None
    valid = bool(ch.get("weight_valid")) and age is not None and age <= WEIGHT_STALE_MS \
        and isinstance(ch.get("weight_g"), (int, float))
    return {
        "channel_id": ch.get("channel_id"),
        "material_id": ch.get("material_id"),
        "state": ch.get("state"),
        "weight_g": ch.get("weight_g") if valid else None,
        "weight_valid": valid,
        "weight_age_ms": int(age) if age is not None else None,
        "weight_stale": not valid,
        "stable": bool(ch.get("stable")) and valid,
        "relay_on": ch.get("relay_on"),
        "active_job_id": ch.get("active_job_id"),
        "target_g": ch.get("target_g"),
        "fault": ch.get("fault") or ch.get("error"),
    }


def _envelope(kind: str, server_time: datetime, seq: int, epoch: str) -> dict:
    return {"kind": kind, "server_time": iso_ms(server_time), "seq": seq, "epoch": epoch}


def _fit(payload: dict, key: str, max_bytes: int) -> dict:
    """Drop trailing list items until the compact JSON fits; flag truncation."""
    while len(json.dumps(payload, separators=(",", ":"), default=str)) > max_bytes \
            and payload.get(key):
        payload[key] = payload[key][:-1]
        payload["truncated"] = True
    return payload


def build_live(snapshot: dict, *, server_time: datetime, seq: int, epoch: str,
               kind: str = "state", max_bytes: int = LIVE_MAX_BYTES) -> dict:
    """state/live (kind 'state', retained snapshot) or evt/live (kind 'evt')."""
    out = _envelope(kind, server_time, seq, epoch)
    devices = []
    ctrl = snapshot.get("controller")
    if ctrl:
        age = int(max(0, ctrl.get("age_seconds", 0)) * 1000)
        status = ctrl.get("status") or {}
        stale = age > STATUS_STALE_MS or not ctrl.get("online", False)
        devices.append({
            "device_id": ctrl.get("device_id"), "role": "relay_controller",
            "age_ms": age, "stale": stale, "online": not stale,
            "estop": status.get("estop"),
            # A dead status stream must not leave a plausible-looking weight.
            "channels": [dict(_channel(c), **({"weight_g": None, "weight_valid": False,
                                                "weight_stale": True, "stable": False}
                                               if stale else {}))
                         for c in (status.get("channels") or []) if isinstance(c, dict)],
        })
    for sender_id, sender in sorted((snapshot.get("senders") or {}).items()):
        # cas/{sender}/telemetry/weight copy (never weight/ctl): per-channel, same rules.
        chans = [_channel(c) for c in sender.get("channels", [])]
        age = int(sender.get("age_ms", 0))
        stale = age > WEIGHT_STALE_MS or not any(c["weight_valid"] for c in chans)
        devices.append({"device_id": sender_id, "role": "weight_sender", "age_ms": age,
                        "stale": stale, "online": not stale,
                        "channels": [{k: c[k] for k in ("channel_id", "weight_g", "weight_valid",
                                                        "weight_age_ms", "weight_stale", "stable")}
                                     for c in chans]})
    weight = snapshot.get("weight")
    if weight:
        age = weight.get("age_ms")
        ok = isinstance(age, (int, float)) and age <= WEIGHT_STALE_MS and bool(weight.get("weight_valid"))
        devices.append({"device_id": weight.get("device_id"), "role": "weight_sender",
                        "age_ms": int(age) if isinstance(age, (int, float)) else None,
                        "stale": not ok, "online": ok,
                        "weight_g": weight.get("weight_g") if ok else None})
    out["devices"] = devices
    return _fit(out, "devices", max_bytes)


def build_queue(queue: dict, *, server_time: datetime, seq: int, epoch: str,
                max_bytes: int = QUEUE_MAX_BYTES) -> dict:
    out = _envelope("state", server_time, seq, epoch)
    keys = ("command_id", "local_job_id", "material_id", "target_g", "priority", "state",
            "held", "promoted", "profile_id", "profile_version", "channel_id")
    out["waiting"] = [{k: j.get(k) for k in keys} for j in queue.get("waiting_commands", [])]
    out["failed"] = [{k: j.get(k) for k in ("command_id", "material_id", "target_g", "state",
                                            "error", "outcome_unknown")}
                     for j in queue.get("failed_commands", [])]
    out["device_id"] = queue.get("device_id")
    out = _fit(out, "failed", max_bytes)
    return _fit(out, "waiting", max_bytes)


def build_devices(rows: list[dict], *, server_time: datetime, seq: int, epoch: str,
                  max_bytes: int = DEVICES_MAX_BYTES, stalled: list[dict] | None = None) -> dict:
    """rows = control_service.read_device_status(): device_id, age_seconds, status.
    ``stalled`` = recovery.stalled_runs(): RUNNING runs with no data for RUN_STALE_SECONDS
    (a flag only, CONTRACT 9.8; the run status is untouched)."""
    out = _envelope("state", server_time, seq, epoch)
    if stalled is not None:
        out["stalled_runs"] = stalled
    devices = []
    for r in rows:
        st = r.get("status") or {}
        age = int(max(0, r.get("age_seconds", 0)) * 1000)
        stale = age > STATUS_STALE_MS or st.get("online") is False
        devices.append({
            "device_id": r.get("device_id"), "role": st.get("role"), "age_ms": age,
            "stale": stale, "online": not stale, "boot_id": r.get("boot_id"),
            "channels": [{"channel_id": c.get("channel_id"), "state": c.get("state"),
                          "profile_id": c.get("profile_id"),
                          "profile_version": c.get("profile_version"),
                          "applied_version": c.get("applied_version")}
                         for c in (st.get("channels") or []) if isinstance(c, dict)],
        })
    out["devices"] = devices
    return _fit(out, "devices", max_bytes)


class LiveCoalescer:
    """2 Hz per dispenser plus a 1 Hz heartbeat for evt/live.

    mark(key) on every inbound telemetry update; due(now) returns the keys that
    should emit now. The caller rebuilds the payload from live_state at emit time,
    so a heartbeat never replays stale numbers."""

    def __init__(self, min_interval_s: float = EVT_MIN_INTERVAL_S,
                 heartbeat_s: float = HEARTBEAT_S):
        self._min, self._hb = min_interval_s, heartbeat_s
        self._last: dict[str, float] = {}
        self._dirty: set[str] = set()
        self._lock = threading.Lock()

    def mark(self, key: str, now: float) -> bool:
        """Record an update; True when it may be emitted immediately."""
        with self._lock:
            self._last.setdefault(key, now - self._hb - 1)
            if now - self._last[key] >= self._min:
                self._last[key] = now
                self._dirty.discard(key)
                return True
            self._dirty.add(key)
            return False

    def due(self, now: float) -> list[str]:
        with self._lock:
            out = [k for k, t in self._last.items()
                   if (k in self._dirty and now - t >= self._min) or now - t >= self._hb]
            for k in out:
                self._last[k] = now
                self._dirty.discard(k)
            return out
