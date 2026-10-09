"""MQTT bridge (docs/telemetry/CONTRACT.md section 7).

Transport only. Inbound telemetry and ACKs are routed into the SAME functions
the HTTP routes use (ingest_batch / ingest_events / update_device_status /
acknowledge_command), so validation and business rules live in one place. The
DB is the source of truth; MQTT is a delivery push on top of it. The HTTP
routes stay as the fallback and nothing here ever drives a relay: commands are
operator intent that firmware may accept or refuse.

Device uptime_ms is never compared with wall-clock. Ordering uses server
receive time (utcnow() in the shared code paths).

The network side sits behind a tiny Publisher interface so tests inject an
in-memory fake and need no broker or paho.
"""

import contextlib
import json
import logging
import os
import queue
import re
import ssl
import threading
import time
from datetime import timezone
from typing import Literal, Protocol

from pydantic import (BaseModel, ConfigDict, Field, StrictBool, StrictInt, ValidationError,
                      field_validator, model_validator)
from sqlalchemy import event
from sqlalchemy.orm import Session

from .config import settings
from .services import command_claim, profile_pin
from .services.live_hub import live_hub
from .services.errors import ServiceError  # no fastapi import: app.service runs headless

log = logging.getLogger(__name__)

DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
# Commands that express recent operator intent; the device must drop them once
# stale (same set the HTTP poll expires). Only STOP-class/PAUSE have ttl 0; JOB/PROFILE
# etc. carry COMMAND_TTL_MS (30000) over MQTT. The HTTP poll item has no ttl_ms.
TRANSIENT_TYPES = ("PUMP_START", "RESUME", "CLEAR", "READY", "ZERO", "TARE")
# STOP-class must always run: ttl_ms 0 (never expires). PAUSE is stop-like.
NO_EXPIRY_TYPES = ("ESTOP", "PUMP_STOP", "PAUSE")

SUBSCRIPTIONS = (("cas/+/telemetry/samples", 0), ("cas/+/telemetry/status", 0),
                 ("cas/+/telemetry/events", 0), ("cas/+/status", 1),
                 ("cas/+/commands/ack", 1), ("cas/+/telemetry/live", 0),
                 # CONTRACT 8.2: the reduced 1-2 Hz observability copy only. The
                 # server never subscribes to cas/+/weight/ctl (relay-only).
                 ("cas/+/telemetry/weight", 0))

# CONTRACT 8.2 max payload bytes per inbound kind (presence is 256 by contract;
# the telemetry bodies are bounded generously, anything larger is dropped unparsed).
MAX_PAYLOAD_BYTES = {"presence": 256, "ack": 1024, "live": 2048, "weight": 512,
                     "status": 65536, "samples": 65536, "events": 65536}
QUEUE_MAX = 1000
BOOT_ID_RE = re.compile(r"^[0-9a-fA-F]{8}$")
REBOOT_REASON = "DEVICE_REBOOTED_OUTCOME_UNKNOWN"


class Publisher(Protocol):
    def publish(self, topic: str, payload: str, qos: int, retain: bool) -> bool | None: ...


_publisher: Publisher | None = None
_log_times: dict[str, float] = {}


def set_publisher(publisher: Publisher | None) -> None:
    global _publisher
    _publisher = publisher


def _log_limited(key: str, message: str, *args, interval: float = 30.0) -> None:
    now = time.monotonic()
    if now - _log_times.get(key, -interval) >= interval:
        _log_times[key] = now
        log.warning(message, *args)


# --- topic mapping ---------------------------------------------------------

def commands_topic(device_id: str) -> str:
    return f"cas/{device_id}/commands"


def parse_topic(topic: str) -> tuple[str, str] | None:
    """Return (device_id, kind) for inbound topics we handle, else None.

    kind is one of samples / status / events / live / presence / ack. The device id
    always comes from the topic, never from the payload.
    """
    parts = topic.split("/")
    if parts[0] != "cas" or len(parts) < 3 or not DEVICE_ID_RE.match(parts[1]):
        return None
    device_id = parts[1]
    if len(parts) == 3 and parts[2] == "status":
        return device_id, "presence"
    if len(parts) == 4 and parts[2] == "telemetry" and parts[3] in (
            "samples", "status", "events", "live", "weight"):
        return device_id, parts[3]
    if len(parts) == 4 and parts[2] == "commands" and parts[3] == "ack":
        return device_id, "ack"
    return None


# --- command payloads ------------------------------------------------------

def _iso_ms(dt) -> str:
    if dt.tzinfo is not None:
        dt = dt.astimezone(timezone.utc).replace(tzinfo=None)
    return dt.isoformat(timespec="milliseconds") + "Z"


def build_command_payload(row, db: Session) -> dict:
    """MQTT command body: the HTTP poll item plus the envelope fields."""
    from .services.control import command_wire
    body = command_wire(row, db)
    body["type"] = row.command_type
    channel = body.get("channel_id")
    if channel is None and row.material_id in ("M1", "M2"):
        channel = "CH1" if row.material_id == "M1" else "CH2"
    body["channel_id"] = channel
    if row.command_type in ("ZERO", "TARE"):
        body["device_id"] = row.device_id
    body["issued_at"] = _iso_ms(row.created_at)
    body["ttl_ms"] = command_ttl_ms(row.command_type)
    if isinstance(body.get("profile"), dict):  # CONTRACT 9.3: pin hash for the ACK check
        body["profile_hash"] = profile_pin.profile_hash(body["profile"])
    return body


def command_ttl_ms(command_type: str) -> int:
    """STOP-class never expires (0); transient types use the transient ttl;
    everything else the finite COMMAND_TTL_MS default so a late flush is dropped."""
    if command_type in NO_EXPIRY_TYPES:
        return 0
    if command_type in ("ZERO", "TARE"):
        return settings.scale_cmd_ttl_ms
    if command_type in TRANSIENT_TYPES:
        return settings.transient_command_ttl_seconds * 1000
    return settings.command_ttl_ms


def _too_old(row) -> bool:
    ttl = command_ttl_ms(row.command_type)
    if ttl <= 0 or row.created_at is None:
        return False
    created = row.created_at
    if created.tzinfo is not None:
        created = created.astimezone(timezone.utc).replace(tzinfo=None)
    from .services.analytics import utcnow
    now = utcnow()
    if now.tzinfo is not None:
        now = now.astimezone(timezone.utc).replace(tzinfo=None)
    return (now - created).total_seconds() * 1000 > ttl


def on_reconnect() -> None:
    """Deliberately a no-op: PENDING rows are never republished after a broker
    reconnect; the HTTP poll (which expires/supersedes) covers them."""


def publish_command(device_id: str, body: dict) -> bool:
    """QoS 1, never retained. Never raises: a broker problem must not fail the
    API call; the row stays PENDING and GET /device/commands still delivers.
    Not published (and not queued in paho) while the broker link is down."""
    if _publisher is None:
        return False
    is_connected = getattr(_publisher, "is_connected", None)
    if is_connected is not None and not is_connected():
        return False
    try:
        sent = _publisher.publish(commands_topic(device_id),
                                  json.dumps(body, separators=(",", ":")), 1, False)
        return sent is not False  # None (legacy/fake publishers) counts as sent
    except Exception:
        _log_limited("publish", "MQTT command publish failed for %s", device_id)
        return False


@contextlib.contextmanager
def _db():
    from .database import get_db
    gen = get_db()
    db = next(gen)
    try:
        yield db
    finally:
        gen.close()


def _claim_and_publish(db: Session, row) -> None:
    """MQTT-transport device: win the atomic PENDING -> DELIVERED claim, then
    publish. HTTP can no longer hand this row out. If the broker did not take it
    (nothing on the wire) only our own claim is released back to PENDING; the
    row is then published only by the one-shot startup recovery, never on
    reconnect."""
    from sqlalchemy import update
    from .models import DeviceCommand
    command_id = row.id
    boot_id = command_claim.device_boot_id(db, row.device_id)
    won = command_claim.claim_pending(db, command_id, "MQTT", boot_id)
    db.commit()
    if not won:
        return
    db.refresh(row)
    if publish_command(row.device_id, build_command_payload(row, db)):
        return
    db.execute(update(DeviceCommand).where(
        DeviceCommand.id == command_id, DeviceCommand.state == "DELIVERED",
        DeviceCommand.delivered_via == "MQTT", DeviceCommand.local_job_id.is_(None),
    ).values(state="PENDING", delivered_via=None, delivered_boot_id=None, published_at=None))
    db.commit()


def _publish_rows(ids: list[int]) -> None:
    from sqlalchemy import update
    from .models import DeviceCommand
    from .services.analytics import utcnow
    try:
        with _db() as db:
            for command_id in ids:
                row = db.get(DeviceCommand, command_id)
                if (row is None or row.state != "PENDING" or row.held
                        or _too_old(row)):
                    continue
                if command_claim.device_transport(db, row.device_id) == "MQTT":
                    _claim_and_publish(db, row)
                    continue
                # HTTP-transport device (default): unchanged push-on-top behaviour.
                # Claim before publishing so a concurrent HOLD/RELEASE/CANCEL
                # either sees published_at (and forwards to the device) or wins
                # the claim (and the row is never published).
                claim = db.execute(update(DeviceCommand).where(
                    DeviceCommand.id == command_id, DeviceCommand.state == "PENDING",
                    DeviceCommand.held.is_(False), DeviceCommand.published_at.is_(None),
                ).values(published_at=utcnow()))
                db.commit()
                if claim.rowcount != 1:
                    continue
                db.refresh(row)
                if not publish_command(row.device_id, build_command_payload(row, db)):
                    # Not sent: the HTTP poll still owns delivery of this row.
                    db.execute(update(DeviceCommand).where(
                        DeviceCommand.id == command_id, DeviceCommand.state == "PENDING",
                    ).values(published_at=None))
                    db.commit()
    except Exception:
        _log_limited("publish_rows", "MQTT command publish hook failed", )


def _install_hooks() -> None:
    """Publish every DeviceCommand the moment its insert commits, wherever the
    route created it. One hook instead of editing each insertion site."""
    from .models import DeviceCommand

    @event.listens_for(Session, "after_flush")
    def _collect(session, _ctx):
        if _publisher is None:
            return
        new = [o for o in session.new if isinstance(o, DeviceCommand)]
        if new:
            session.info.setdefault("mqtt_new_cmds", []).extend(new)

    @event.listens_for(Session, "after_commit")
    def _after_commit(session):
        pending = session.info.pop("mqtt_new_cmds", None)
        if pending and _publisher is not None:
            _publish_rows([c.id for c in pending if c.id is not None])

    @event.listens_for(Session, "after_rollback")
    def _after_rollback(session):
        session.info.pop("mqtt_new_cmds", None)


_install_hooks()


# --- inbound ---------------------------------------------------------------

class MqttAck(BaseModel):
    model_config = ConfigDict(extra="ignore")
    command_id: int = Field(ge=1)
    state: str = Field(pattern="^(QUEUED|APPLIED|FAILED)$")
    local_job_id: int | None = Field(default=None, ge=1)
    channel_id: str | None = Field(default=None, pattern="^(CH1|CH2)$")
    error: str | None = Field(default=None, max_length=120)
    reason: str | None = None
    result: str | None = Field(default=None, pattern="^(accepted|success|failed|timeout|rejected)$")
    weight_g: int | None = None  # integer grams (CONTRACT section 1); 0.0 ok, 1.5 rejected
    stable: bool | None = None


U32_MAX = 4294967295


class MqttLiveChannel(BaseModel):
    model_config = ConfigDict(extra="ignore")
    channel_id: Literal["CH1", "CH2"]
    weight_g: StrictInt | None = None
    weight_valid: StrictBool
    weight_age_ms: StrictInt | None = None
    stable: StrictBool = False

    @field_validator("weight_age_ms")
    @classmethod
    def _age(cls, value):
        if value is None:
            return None
        if value < 0:
            raise ValueError("weight_age_ms must be >= 0")
        return None if value >= U32_MAX else value  # UINT32_MAX = no reading yet

    @model_validator(mode="after")
    def _invalid_means_no_weight(self):
        if not self.weight_valid or self.weight_g is None:
            self.weight_valid, self.weight_g, self.stable = False, None, False
        return self


class MqttLive(BaseModel):
    model_config = ConfigDict(extra="ignore")
    uptime_ms: StrictInt = Field(ge=0, le=U32_MAX)
    channels: list[MqttLiveChannel] = Field(min_length=1, max_length=2)

    @field_validator("channels")
    @classmethod
    def _unique(cls, value):
        if len({c.channel_id for c in value}) != len(value):
            raise ValueError("duplicate channel_id")
        return value


class MqttWeight(BaseModel):
    """cas/{sender}/telemetry/weight: reduced observability copy, one channel."""
    model_config = ConfigDict(extra="ignore")
    uptime_ms: StrictInt = Field(ge=0, le=U32_MAX)
    channel: Literal["CH1", "CH2"] | None = None
    src_uart: str | None = None
    weight_g: StrictInt
    stable: StrictBool = False
    age_ms: StrictInt = Field(ge=0, lt=U32_MAX)

    @model_validator(mode="after")
    def _derive_channel(self):
        if self.channel is None:
            derived = {"UART1": "CH1", "UART2": "CH2"}.get(self.src_uart)
            if derived is None:
                raise ValueError("channel missing and src_uart unknown")
            self.channel = derived
        return self


def _reject_constant(value):
    raise ValueError(f"non-finite number {value}")


def _finite_float(text: str) -> float:
    number = float(text)
    if number != number or number in (float("inf"), float("-inf")):
        raise ValueError("non-finite number")
    return number


def handle_message(topic: str, payload: bytes | str, retain: bool = False,
                   enforce_pin: bool = False) -> None:
    """Route one inbound message. Never raises. Used by the worker and by tests.
    ``enforce_pin`` (headless service only): CONTRACT 9.3 applied_profile check on
    JOB ACKs; the legacy bridge keeps accepting ACKs from firmware without it."""
    parsed = parse_topic(topic)
    if parsed is None:
        return
    device_id, kind = parsed
    if len(payload) > MAX_PAYLOAD_BYTES.get(kind, 65536):
        _log_limited(f"big-{kind}", "MQTT %s payload too large on %s", kind, topic)
        return
    try:
        body = json.loads(payload, parse_constant=_reject_constant, parse_float=_finite_float)
    except (ValueError, UnicodeDecodeError):
        _log_limited("badjson", "MQTT non-JSON or non-finite message on %s", topic)
        return
    if not isinstance(body, dict):
        return
    claimed = body.get("device_id")
    if claimed not in (None, "", device_id):
        _log_limited("crossdev", "MQTT device_id mismatch on %s", topic)
        return
    if retain and kind != "presence":
        return  # a replayed retained snapshot must never look like fresh telemetry
    if kind == "status" and set(body) <= {"online", "graceful"}:
        kind = "presence"
    try:
        if kind == "live":  # live memory only: no DB session, no commit
            _live(device_id, body)
            return
        if kind == "weight":  # same: memory only
            _weight(device_id, body)
            return
        with _db() as db:
            if kind == "presence":
                _presence(db, device_id, body, retain)
            elif kind == "status":
                _status(db, device_id, body)
            elif kind == "samples":
                _samples(db, device_id, body)
            elif kind == "events":
                _events(db, device_id, body)
            elif kind == "ack":
                _ack(db, device_id, body, enforce_pin)
    except (ValidationError, ServiceError) as exc:
        _log_limited(f"reject-{kind}", "MQTT %s rejected for %s: %s", kind, device_id,
                     getattr(exc, "message", None) or exc.__class__.__name__)
    except Exception:
        _log_limited(f"err-{kind}", "MQTT %s handling failed for %s", kind, device_id)


def _live(device_id: str, body: dict) -> None:
    from .services.live_state import live_state
    msg = MqttLive.model_validate(body)
    live_state.update_live(device_id, msg.uptime_ms, [c.model_dump() for c in msg.channels])


def _weight(device_id: str, body: dict) -> None:
    from .services.live_state import live_state
    try:
        msg = MqttWeight.model_validate(body)
    except ValidationError as exc:
        err = exc.errors()[0]
        live_state.note_reject(device_id, f"{'.'.join(str(x) for x in err['loc'])}: {err['type']}")
        return
    if live_state.update_sender_weight(device_id, msg.channel, msg.uptime_ms, msg.weight_g,
                                       msg.age_ms, msg.stable, boot_id=_valid_boot_id(body)):
        live_hub.on_weight_threadsafe(device_id)


def _valid_boot_id(body: dict) -> str | None:
    value = body.get("boot_id")
    return value.lower() if isinstance(value, str) and BOOT_ID_RE.match(value) else None


def note_boot_id(db: Session, device_id: str, boot_id: str | None) -> None:
    """CONTRACT 8.4: a boot_id change closes DELIVERED/QUEUED rows that have no
    terminal ACK as FAILED / DEVICE_REBOOTED_OUTCOME_UNKNOWN. Never resent (the
    operator re-issues with a new command_id); STOP-class is closed the same way
    and is not resent either. The first boot_id seen for a device is not a change."""
    from sqlalchemy import select
    from .models import DeviceCommand, DeviceStatus
    from .services.analytics import utcnow
    row = db.get(DeviceStatus, device_id)
    if boot_id is None or row is None or row.boot_id == boot_id:
        return
    previous, row.boot_id = row.boot_id, boot_id
    if previous is not None:
        now = utcnow()
        for cmd in db.scalars(select(DeviceCommand).where(
                DeviceCommand.device_id == device_id,
                DeviceCommand.state.in_(("DELIVERED", "QUEUED")),
                (DeviceCommand.delivered_boot_id.is_(None))
                | (DeviceCommand.delivered_boot_id != boot_id))):
            cmd.state, cmd.error_text, cmd.updated_at = "FAILED", REBOOT_REASON, now
            cmd.ack_json = {**(cmd.ack_json or {}), "reason": REBOOT_REASON}
        log.warning("MQTT boot_id changed for %s; closed unacked commands", device_id)
    db.commit()
    if previous is not None:
        live_hub.on_reboot_threadsafe(device_id)


def _status(db: Session, device_id: str, body: dict) -> None:
    from .services.control import DeviceStatusIn, update_device_status
    boot_id = _valid_boot_id(body)
    # Before the status reconciles rows by local job id: those ids reset on reboot.
    note_boot_id(db, device_id, boot_id)
    update_device_status(db, DeviceStatusIn(device_id=device_id, status=body))
    note_boot_id(db, device_id, boot_id)  # first status of a new device records it


def _samples(db: Session, device_id: str, body: dict) -> None:
    from .routes.telemetry import ingest_batch
    from .schemas import TelemetryBatchIn
    ingest_batch(TelemetryBatchIn.model_validate({**body, "device_id": device_id}), db)


def _events(db: Session, device_id: str, body: dict) -> None:
    from .routes.telemetry import ingest_events
    from .schemas import EventsIn
    ingest_events(EventsIn.model_validate({**body, "device_id": device_id}), db)


PROFILE_MISMATCH = "PROFILE_MISMATCH"


def _enforce_pin(db: Session, row, ack: "MqttAck", body: dict) -> bool:
    """CONTRACT 9.3. True if the ACK was consumed as a PROFILE_MISMATCH failure.
    A pinned JOB is dispensable only if the device reports exactly the pinned
    (profile_id, version, hash); anything else (absent, wrong, deleted pin) fails
    closed. Server-side bookkeeping only: the device already refused or accepted."""
    if row.command_type != "JOB" or not (row.profile_id and row.profile_version):
        return False
    if ack.state == "FAILED":
        return False  # already a failure; keep the device's own reason
    if profile_pin.matches(db, row, body.get("applied_profile")):
        return False
    from .services.analytics import utcnow
    row.state, row.error_text, row.updated_at = "FAILED", PROFILE_MISMATCH, utcnow()
    row.ack_json = {**(row.ack_json or {}), "reason": PROFILE_MISMATCH,
                    "applied_profile": body.get("applied_profile")
                    if isinstance(body.get("applied_profile"), dict) else None}
    if ack.local_job_id:
        row.local_job_id = ack.local_job_id
    db.commit()
    log.warning("MQTT JOB ack %s failed PROFILE_MISMATCH", row.id)
    return True


def _ack(db: Session, device_id: str, body: dict, enforce_pin: bool = False) -> None:
    from .models import DeviceCommand
    from .services.control import DeviceCommandAck, acknowledge_command
    ack = MqttAck.model_validate(body)
    row = db.get(DeviceCommand, ack.command_id)
    if row is None or row.device_id != device_id:
        return  # unknown or another device's command: ignore, never mutate
    note_boot_id(db, device_id, _valid_boot_id(body))
    db.refresh(row)
    if row.state in command_claim.TERMINAL_STATES:
        # Terminal states are immutable over MQTT: a late or duplicate ACK
        # (e.g. after DEVICE_REBOOTED_OUTCOME_UNKNOWN) is logged and dropped.
        log.info("MQTT ACK for terminal command %s (%s) ignored", row.id, row.state)
        return
    if enforce_pin and _enforce_pin(db, row, ack, body):
        return
    error = (ack.error or ack.reason or None)
    acknowledge_command(db, ack.command_id, DeviceCommandAck(
        state=ack.state, local_job_id=ack.local_job_id,
        channel_id=ack.channel_id, error=error[:120] if error else None,
        result=ack.result, reason=ack.reason[:120] if ack.reason else None,
        weight_g=ack.weight_g, stable=ack.stable))
    db.refresh(row)
    live_hub.on_ack_threadsafe(row.id, device_id, {"state": row.state, **(row.ack_json or {})})


def _presence(db: Session, device_id: str, body: dict, retained: bool) -> None:
    from datetime import datetime
    from .models import DeviceStatus
    from .services.analytics import utcnow
    online = body.get("online")
    if not isinstance(online, bool):
        return
    boot_id = _valid_boot_id(body)
    caps = body.get("caps")
    caps = [c for c in caps if isinstance(c, str) and len(c) <= 32][:16] \
        if isinstance(caps, list) else None
    row = db.get(DeviceStatus, device_id)
    if row is None:
        if not (online and (boot_id or caps is not None)):
            return
        # First birth of a device: remember boot_id/caps; epoch updated_at so a
        # birth alone never looks like fresh telemetry.
        row = DeviceStatus(device_id=device_id, status_json={}, boot_id=boot_id,
                           caps_json=caps, updated_at=datetime(1970, 1, 1))
        db.add(row)
        db.flush()
    elif online:
        note_boot_id(db, device_id, boot_id)
        if caps is not None:
            row.caps_json = caps
            if row.command_transport == "MQTT" and "cmd_mqtt" not in caps:
                row.command_transport = "HTTP"  # firmware lost the capability
    status = dict(row.status_json or {})
    if online and retained:
        status["online_hint"] = True  # a hint only: live needs fresh telemetry
    else:
        status["online"] = online  # LWT false is honoured at once: OFFLINE, not E-STOP
    row.status_json = status
    # online=false leaves updated_at alone so age_seconds keeps growing and the
    # UI health dot degrades. A live online=true refreshes it; a retained one is
    # only a replay and proves nothing about the device now.
    if online and not retained:
        row.updated_at = utcnow()
    db.commit()


# --- paho adapter and lifecycle -------------------------------------------

class PahoPublisher:
    def __init__(self, client):
        self._client = client

    def publish(self, topic: str, payload: str, qos: int, retain: bool) -> bool:
        """True only if paho accepted the message on a live link (rc == 0).
        Any other rc means NOT sent: paho may have parked the QoS1 message for
        resend on reconnect, so drop it now. A stale command must never replay."""
        if not self.is_connected():
            return False
        info = self._client.publish(topic, payload, qos=qos, retain=retain)
        if getattr(info, "rc", 0) != 0:
            discard_message(self._client, getattr(info, "mid", None))
            return False
        if qos > 0 and not _is_on_wire(self._client, getattr(info, "mid", None)):
            discard_message(self._client, getattr(info, "mid", None))
            return False
        return True

    def is_connected(self) -> bool:
        return bool(self._client.is_connected())


_WAIT_STATES = ("wait_for_puback", "wait_for_pubrec", "wait_for_pubcomp")


def _state_name(msg) -> str:
    state = getattr(msg, "state", None)
    return str(getattr(state, "name", state)).lower().replace("mqtt_ms_", "")


def _is_on_wire(client, mid) -> bool:
    """False only if paho still holds the message un-sent (parked/queued).
    A missing entry means it was already acked, i.e. sent."""
    with contextlib.suppress(Exception):
        with client._out_message_mutex:
            msg = client._out_messages.get(mid)
            return msg is None or _state_name(msg) in _WAIT_STATES
    return True


def discard_message(client, mid) -> None:
    """Remove ONLY this publish from paho's outgoing map (link may be up and
    other packets, e.g. an ESTOP awaiting PUBACK, must stay untouched)."""
    if mid is None:
        return
    with contextlib.suppress(Exception):
        with client._out_message_mutex:
            msg = client._out_messages.pop(mid, None)
            if msg is not None and _state_name(msg) in _WAIT_STATES:
                client._inflight_messages = max(0, client._inflight_messages - 1)


def drop_queued_outgoing(client) -> None:
    """Link is DOWN (on_disconnect): discard everything paho would resend."""
    with contextlib.suppress(Exception):
        with client._out_message_mutex:
            client._out_messages.clear()
            client._inflight_messages = 0
            if hasattr(client, "_out_packet"):
                client._out_packet.clear()


def drop_stale_on_connect(client) -> None:
    """Called from on_connect, before paho's resend loop. paho has already reset
    every pre-existing message to publish/queued; drop those. Messages already
    wait_for_puback were published after CONNACK and stay (e.g. an ESTOP)."""
    with contextlib.suppress(Exception):
        with client._out_message_mutex:
            for mid in [m for m, v in client._out_messages.items()
                        if _state_name(v) not in _WAIT_STATES]:
                del client._out_messages[mid]


_client = None

# --- connection state (health) ---------------------------------------------

_state_lock = threading.Lock()
_conn = {"connected": False, "pending": set(), "granted": False}
_recovered = False  # startup recovery runs once per process, never on reconnect


def handle_connect(c, reason_code) -> None:
    """CONNECTED is reported only after CONNACK success AND all SUBACKs granted."""
    if getattr(reason_code, "is_failure", False):
        _log_limited("connect", "MQTT connect refused: %s", reason_code)
        return
    drop_stale_on_connect(c)
    with _state_lock:
        _conn.update(connected=True, pending=set(), granted=False)
        for topic, qos in SUBSCRIPTIONS:
            result = c.subscribe(topic, qos=qos)
            mid = result[1] if isinstance(result, tuple) else None
            if mid is None or result[0] != 0:
                _log_limited("subscribe", "MQTT subscribe call failed for %s", topic)
                _conn["pending"].add(("failed", topic))
            else:
                _conn["pending"].add(mid)
    log.info("MQTT bridge connected; waiting for subscriptions")


def handle_subscribe(mid, reason_codes) -> None:
    denied = any(getattr(rc, "is_failure", False) or (isinstance(rc, int) and rc >= 128)
                 for rc in (reason_codes or ()))
    with _state_lock:
        if denied:
            _conn["pending"].add(("denied", mid))
            _log_limited("suback", "MQTT subscription refused (mid %s)", mid)
        _conn["pending"].discard(mid)
        granted = _conn["connected"] and not _conn["pending"]
        _conn["granted"] = granted
    if granted:
        log.info("MQTT bridge subscribed")
        _request_recovery()
        live_hub.on_link_up_threadsafe()


def handle_disconnect(c, reason_code) -> None:
    with _state_lock:
        _conn.update(connected=False, pending=set(), granted=False)
    drop_queued_outgoing(c)  # nothing unconfirmed may flush after reconnect
    live_hub.on_link_lost_threadsafe()
    _log_limited("disconnect", "MQTT bridge disconnected (%s); paho will retry", reason_code)


def mqtt_state() -> dict:
    """Read-only health view: CONNECTED only after connect success AND subscriptions
    granted; DISCONNECTED once started but not (or no longer) so; UNKNOWN when the
    bridge is disabled or has not started."""
    if _client is None:
        return {"enabled": bool(settings.mqtt_enabled), "state": "UNKNOWN"}
    with _state_lock:
        ok = _conn["connected"] and _conn["granted"]
    age = None if _last_inbound is None else max(0.0, time.monotonic() - _last_inbound)
    state = "CONNECTED" if ok else "DISCONNECTED"
    if ok and age is not None and age >= INBOUND_STALE_S:
        state = "DEGRADED"
    return {"enabled": True, "state": state, "last_inbound_age_s": age,
            "fanout_failures": live_hub.health()["fanout_failures"]}


# --- worker: no SQL on the paho callback thread -----------------------------

_queue: "queue.Queue" = queue.Queue(maxsize=QUEUE_MAX)
_worker: threading.Thread | None = None
_dropped = 0
_last_inbound: float | None = None
INBOUND_STALE_S = 30
_INLINE_KINDS = ("live", "weight")  # memory-only; cheap enough for the callback


def enqueue_message(topic: str, payload: bytes, retain: bool) -> None:
    """Called on the paho thread. Memory-only kinds run inline; everything that
    touches SQL goes through the bounded queue and is dropped (logged) if full."""
    global _dropped
    parsed = parse_topic(topic)
    if parsed is None:
        return
    global _last_inbound
    _last_inbound = time.monotonic()
    if parsed[1] in _INLINE_KINDS:
        handle_message(topic, payload, retain)
        return
    try:
        _queue.put_nowait((topic, payload, retain))
    except queue.Full:
        _dropped += 1
        _log_limited("qfull", "MQTT inbound queue full; dropped %d messages so far", _dropped)


def _request_recovery() -> None:
    global _recovered
    if _recovered:
        return
    _recovered = True
    with contextlib.suppress(queue.Full):
        _queue.put_nowait(None)  # sentinel: one-shot startup recovery


def _run_worker() -> None:
    while True:
        item = _queue.get()
        if item is False:
            return
        try:
            if item is None:
                startup_recovery()
            else:
                handle_message(*item)
        except Exception:
            _log_limited("worker", "MQTT worker error")


def _start_worker() -> None:
    global _worker
    if _worker is None or not _worker.is_alive():
        _worker = threading.Thread(target=_run_worker, name="mqtt-worker", daemon=True)
        _worker.start()


def startup_recovery() -> None:
    """Once per process (never on reconnect). For MQTT-transport devices only:
    expire PENDING rows past their TTL, close DELIVERED rows with no ACK that are
    older than the TTL (outcome unknown, never resent), then publish only the
    PENDING rows still within TTL. HTTP-transport devices are untouched."""
    from sqlalchemy import select
    from .models import DeviceCommand, DeviceStatus
    from .services.analytics import utcnow
    try:
        with _db() as db:
            devices = [d for d in db.scalars(select(DeviceStatus.device_id).where(
                DeviceStatus.command_transport == "MQTT"))]
            if not devices:
                return
            now = utcnow()
            live_ids = []
            for row in db.scalars(select(DeviceCommand).where(
                    DeviceCommand.device_id.in_(devices),
                    DeviceCommand.state.in_(("PENDING", "DELIVERED")))
                    .order_by(DeviceCommand.id)):
                if row.state == "PENDING":
                    if row.command_type in ("ZERO", "TARE"):
                        row.state, row.updated_at = "EXPIRED", now
                        row.error_text = "not republished after restart"
                    elif _too_old(row):
                        row.state, row.updated_at = "EXPIRED", now
                        row.error_text = "expired before delivery"
                    elif not row.held:
                        live_ids.append(row.id)
                elif row.delivered_via == "MQTT" and _stale(row.updated_at, now):
                    row.state, row.updated_at = "FAILED", now
                    row.error_text = "DELIVERED_NO_ACK_OUTCOME_UNKNOWN"
            db.commit()
        if live_ids:
            _publish_rows(live_ids)
    except Exception:
        _log_limited("recovery", "MQTT startup recovery failed")


def _stale(stamp, now) -> bool:
    if stamp is None:
        return True
    if stamp.tzinfo is not None:
        stamp = stamp.astimezone(timezone.utc).replace(tzinfo=None)
    return (now.replace(tzinfo=None) - stamp).total_seconds() * 1000 > settings.command_ttl_ms


def configure_client(client) -> None:
    """No artificial queue cap: max_queued_messages=1 made a second publish
    fail while an ESTOP awaited PUBACK. The guard is is_connected() before
    publish plus clearing paho's queues on disconnect and on connect."""
    client.max_queued_messages_set(0)
    client.reconnect_delay_set(min_delay=1, max_delay=30)  # exponential backoff


class MqttConfigError(RuntimeError):
    pass


def _check_ca_file(path: str) -> None:
    if not os.path.isfile(path):
        raise MqttConfigError(f"MQTT_TLS_CA_FILE not found or not a file: {path}")
    try:
        with open(path, "rb") as fh:
            fh.read(1)
    except OSError as exc:
        raise MqttConfigError(f"MQTT_TLS_CA_FILE unreadable: {path} ({type(exc).__name__})") from None


def apply_tls(client) -> None:
    ca_file = settings.mqtt_tls_ca_file
    if ca_file:
        _check_ca_file(ca_file)
    try:
        client.tls_set(ca_certs=ca_file or None, cert_reqs=ssl.CERT_REQUIRED,
                       tls_version=ssl.PROTOCOL_TLS_CLIENT)
    except (OSError, ValueError) as exc:
        raise MqttConfigError(f"MQTT_TLS setup failed (MQTT_TLS_CA_FILE={ca_file or 'system CAs'}): "
                              f"{type(exc).__name__}: {exc}") from None
    ctx = getattr(client, "_ssl_context", None)
    if not isinstance(ctx, ssl.SSLContext):
        raise MqttConfigError("MQTT_TLS setup failed: no SSL context to enforce TLS 1.2 minimum")
    if ctx.minimum_version != ssl.TLSVersion.TLSv1_3:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    if ctx.verify_mode != ssl.CERT_REQUIRED or not ctx.check_hostname:
        raise MqttConfigError("MQTT_TLS setup failed: certificate or hostname verification is off")


def _worker_count() -> int:
    try:
        return int(os.getenv("WEB_CONCURRENCY", "1"))
    except ValueError:
        return 1


def start() -> None:
    global _client
    if not settings.mqtt_enabled or _client is not None:
        return
    if _worker_count() > 1 and not os.getenv("MQTT_CLIENT_ID", "").strip():
        log.error("MQTT bridge refused: multiple workers need an explicit MQTT_CLIENT_ID")
        return
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        log.error("paho-mqtt not installed; MQTT bridge disabled")
        return
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=settings.mqtt_client_id)
    if settings.mqtt_tls:
        apply_tls(client)
        if settings.mqtt_broker_port == 1883:
            log.warning("MQTT_TLS on with MQTT_BROKER_PORT 1883; TLS brokers usually listen on 8883")

    def on_connect(c, _u, _f, reason_code, _p=None):
        handle_connect(c, reason_code)

    def on_subscribe(_c, _u, mid, reason_codes, _p=None):
        handle_subscribe(mid, reason_codes)

    def on_disconnect(_c, _u, _f, reason_code, _p=None):
        handle_disconnect(_c, reason_code)

    def on_message(_c, _u, msg):
        enqueue_message(msg.topic, msg.payload, bool(msg.retain))

    client.on_connect, client.on_disconnect, client.on_message = on_connect, on_disconnect, on_message
    client.on_subscribe = on_subscribe
    configure_client(client)
    _start_worker()
    if settings.mqtt_username:
        client.username_pw_set(settings.mqtt_username, settings.mqtt_password or None)
    try:
        client.connect_async(settings.mqtt_broker_host, settings.mqtt_broker_port, keepalive=30)
        client.loop_start()  # background thread; also drives reconnect
    except Exception:
        log.exception("MQTT bridge failed to start; HTTP fallback still works")
        return
    _client = client
    set_publisher(PahoPublisher(client))
    log.info("MQTT bridge started for %s:%s (tls=%s)", settings.mqtt_broker_host,
             settings.mqtt_broker_port, settings.mqtt_tls)


def stop() -> None:
    global _client, _worker, _recovered, _last_inbound
    set_publisher(None)
    _last_inbound = None
    with _state_lock:
        _conn.update(connected=False, pending=set(), granted=False)
    _recovered = False
    if _worker is not None:
        with contextlib.suppress(queue.Full):
            _queue.put_nowait(False)
        _worker = None
    if _client is not None:
        with contextlib.suppress(Exception):
            _client.loop_stop()
            _client.disconnect()
    _client = None
