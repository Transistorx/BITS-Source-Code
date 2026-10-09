"""Shared helpers for the REQ-WMQ server suites (no broker, no MySQL)."""
import itertools
import json
import queue
import threading
import time
from datetime import timedelta
from types import SimpleNamespace

from sqlalchemy.orm import Session

from app import database, mqtt_bridge
from app.models import DeviceCommand, DeviceStatus
from app.services.analytics import utcnow

RELAY_STATUS = {"role": "relay_controller", "channels": [
    {"channel_id": "CH1", "state": "IDLE", "relay_on": False, "active_job_id": 0},
    {"channel_id": "CH2", "state": "IDLE", "relay_on": False, "active_job_id": 0}], "queue": []}

_counter = itertools.count(1)


def weight_body(**overrides):
    body = {"schema_version": 1, "boot_id": "8a7b6c5d", "message_id": "8a7b6c5d-1",
            "seq": 1, "uptime_ms": 10000, "scale_id": "SCALE1", "channel": "CH1",
            "src_uart": "UART1", "weight_g": 1234, "stable": True, "age_ms": 15,
            "cas_seq": 7, "source": "CAS_RS485"}
    body.update(overrides)
    for key in [k for k, v in body.items() if v is DROP]:
        del body[key]
    return body


DROP = object()


def weight_topic(device):
    return f"cas/{device}/telemetry/weight"


def send_weight(device, body, retain=False):
    raw = body if isinstance(body, bytes) else json.dumps(body).encode()
    mqtt_bridge.handle_message(weight_topic(device), raw, retain)


def unique_sender():
    return f"bits-sender-{next(_counter):04d}"


def seed_devices(sender, relay_id="bits-relay-0001", boot_id="aaaaaaaa"):
    with Session(database.get_engine()) as db:
        if db.get(DeviceStatus, relay_id) is None:
            db.add(DeviceStatus(device_id=relay_id, status_json=RELAY_STATUS,
                                updated_at=utcnow()))
        db.add(DeviceStatus(device_id=sender, status_json={"role": "weight_sender",
               "cas_link": "ONLINE"}, boot_id=boot_id, command_transport="MQTT",
               updated_at=utcnow()))
        db.commit()


def set_link(monkeypatch, connected=True):
    monkeypatch.setattr(mqtt_bridge, "_client", object())
    monkeypatch.setitem(mqtt_bridge._conn, "connected", connected)
    monkeypatch.setitem(mqtt_bridge._conn, "granted", connected)


def ack(device, command_id, state="FAILED", **extra):
    body = {"command_id": command_id, "state": state, "result": "failed",
            "reason": "UNVERIFIED", "channel_id": "CH1"}
    body.update(extra)
    mqtt_bridge.handle_message(f"cas/{device}/commands/ack", json.dumps(body).encode(), False)


def command_rows(device):
    with Session(database.get_engine()) as db:
        return list(db.query(DeviceCommand).filter(DeviceCommand.device_id == device))


def sent_commands(pub, device):
    return [m for m in pub.sent if m[0] == f"cas/{device}/commands"]


def wait_for(predicate, timeout=1.5, step=0.02):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(step)
    return predicate()


class Probe:
    """Reads frames from a TestClient websocket on a thread so tests can time out."""

    def __init__(self, ws):
        self.ws = ws
        self.q = queue.Queue()
        self.log = []
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        try:
            while True:
                self.q.put(self.ws.receive_json())
        except BaseException as exc:
            self.q.put({"type": "_closed", "exc": repr(exc)})

    def send(self, obj):
        self.ws.send_json(obj)

    def next(self, timeout=2.0):
        try:
            frame = self.q.get(timeout=timeout)
        except queue.Empty:
            return None
        self.log.append(frame)
        return frame

    def until(self, pred, timeout=2.0):
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return None
            frame = self.next(left)
            if frame is None:
                return None
            if pred(frame):
                return frame
            if frame.get("type") == "_closed":
                return None

    def of_type(self, type_, timeout=2.0, **match):
        return self.until(lambda f: f.get("type") == type_
                          and all(f.get(k) == v for k, v in match.items()), timeout)

    def drain(self, window=0.4):
        frames, end = [], time.monotonic() + window
        while time.monotonic() < end:
            frame = self.next(max(0.0, end - time.monotonic()))
            if frame is not None:
                frames.append(frame)
        return frames

    def verdict(self, timeout=1.5):
        """First of: cmd_state, error, or closed. None when nothing came."""
        return self.until(lambda f: f.get("type") in ("cmd_state", "error", "_closed"), timeout)


def is_refusal(frame):
    if frame is None:
        return False
    if frame["type"] in ("error", "_closed"):
        return True
    return frame["type"] == "cmd_state" and frame.get("status") == "refused"


def cmd(device, channel="CH1", name="ZERO"):
    return {"type": "cmd", "device_id": device, "channel": channel, "cmd": name}
