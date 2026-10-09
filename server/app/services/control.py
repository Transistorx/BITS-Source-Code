"""Operator control commands, device command delivery/ACK and device status.

Transport-neutral (db session + validated args in, dict out, ServiceError on
refusal). ZERO/TARE checks here are advisory: the firmware enforces the same
rules and is the safety authority. Nothing here drives a relay directly; every
action is a DeviceCommand row the device pulls (HTTP) or the bridge publishes.
"""

import logging
import math
from datetime import datetime, timedelta, timezone

from pydantic import BaseModel, Field
from sqlalchemy import case, func, or_, select, update
from sqlalchemy.orm import Session

from ..config import settings
from ..models import DeviceCommand, DeviceStatus, TuningProfile
from . import command_claim as claim
from . import profiles as profile_service
from . import queue as queue_service
from .analytics import utcnow
from .errors import ServiceError


class DeviceCommandAck(BaseModel):
    state: str = Field(pattern="^(QUEUED|APPLIED|FAILED)$")
    local_job_id: int | None = Field(default=None, ge=1)
    channel_id: str | None = Field(default=None, pattern="^(CH1|CH2)$")
    error: str | None = Field(default=None, max_length=120)
    # ZERO/TARE ACK extras (all optional; older ACKs omit them).
    result: str | None = Field(default=None, pattern="^(accepted|success|failed|timeout|rejected)$")
    reason: str | None = Field(default=None, max_length=120)
    weight_g: int | None = None  # integer grams; integral floats (0.0) coerce, 1.5 -> 422
    stable: bool | None = None


class DeviceStatusIn(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    status: dict


class ControlIn(BaseModel):
    action: str = Field(pattern="^(CANCEL|PAUSE|RESUME|ESTOP|CLEAR|HOLD|RELEASE|PROMOTE|PUMP_START|PUMP_STOP|READY|ZERO|TARE)$")
    local_job_id: int | None = Field(default=None, ge=1)
    command_id: int | None = Field(default=None, ge=1)
    channel_id: str | None = Field(default=None, pattern="^(CH1|CH2)$")
    device_id: str | None = Field(default=None, min_length=1, max_length=64)  # ZERO/TARE only


SCALE_STALE_SECONDS = 10  # same freshness the UI uses for "online"
_MISSING = object()
ZERO_TARE_APPLIED_REASON = "ACK_INVALID_ZERO_TARE_APPLIED"


def scale_command(db: Session, payload: ControlIn) -> dict:
    """ZERO/TARE go to the weight sender, never the relay. Advisory checks only:
    the firmware enforces the same rules and is the safety authority."""
    if payload.channel_id is None:
        raise ServiceError("invalid", "ZERO/TARE require channel_id", 422)
    rows = list(db.scalars(select(DeviceStatus).order_by(DeviceStatus.updated_at.desc())))
    senders = [r for r in rows if (r.status_json or {}).get("role") == "weight_sender"]
    if payload.device_id is not None:
        sender = next((r for r in rows if r.device_id == payload.device_id), None)
        if sender is None or (sender.status_json or {}).get("role") != "weight_sender":
            raise ServiceError("invalid", "device_id is not a weight_sender", 422)
    elif senders:
        sender = senders[0]
    else:
        raise ServiceError("unavailable", "no weight sender has reported status", 503)
    age = (utcnow() - sender.updated_at).total_seconds()
    if (sender.status_json or {}).get("online") is False or age > SCALE_STALE_SECONDS:
        raise ServiceError("conflict", "weight sender is offline", 409)
    for r in rows:
        status = r.status_json or {}
        if status.get("role") == "weight_sender":
            continue
        for ch in status.get("channels") or []:
            if not isinstance(ch, dict):
                continue
            busy = (ch.get("active_job_id") or ch.get("relay_on")
                    or str(ch.get("state", "")).upper() in ("DISPENSING", "RUNNING"))
            if busy:
                raise ServiceError("conflict",
                                   "cannot ZERO/TARE while a dispense is active", 409)
    cmd = DeviceCommand(device_id=sender.device_id, command_type=payload.action,
        channel_id=payload.channel_id, state="PENDING",
        created_at=utcnow(), updated_at=utcnow())
    db.add(cmd)
    db.commit()
    db.refresh(cmd)
    return {"command_id": cmd.id, "state": cmd.state, "device_id": cmd.device_id}


def read_control_command(db: Session, command_id: int) -> dict:
    row = db.get(DeviceCommand, command_id)
    if row is None:
        raise ServiceError("not_found", "command not found", 404)
    ack = row.ack_json or {}
    return {"command_id": row.id, "command_type": row.command_type, "state": row.state,
            "device_id": row.device_id, "channel_id": row.channel_id, "error": row.error_text,
            "result": ack.get("result"), "reason": ack.get("reason"),
            "weight_g": ack.get("weight_g"), "stable": ack.get("stable")}


def control_command(db: Session, payload: ControlIn) -> dict:
    if payload.action in ("ZERO", "TARE"):
        return scale_command(db, payload)
    if payload.action in ("ESTOP", "CLEAR"):
        cmd = DeviceCommand(device_id=queue_service.device_id_for_commands(db),
            command_type=payload.action,
            state="PENDING", created_at=utcnow(), updated_at=utcnow())
        db.add(cmd)
        db.commit()
        db.refresh(cmd)
        return {"command_id": cmd.id, "state": cmd.state}
    if payload.action == "CANCEL" and not (payload.local_job_id or payload.channel_id):
        raise ServiceError("invalid", "CANCEL requires local_job_id or channel_id", 422)
    if payload.action in ("PAUSE", "RESUME") and not payload.channel_id:
        raise ServiceError("invalid", "PAUSE/RESUME require channel_id", 422)
    if payload.action in ("PUMP_START", "PUMP_STOP", "READY"):
        if not payload.channel_id:
            raise ServiceError("invalid", "PUMP_START/PUMP_STOP/READY require channel_id", 422)
        if payload.action == "READY" and payload.channel_id not in ("CH1", "CH2"):
            raise ServiceError("invalid", "READY channel_id must be CH1 or CH2", 422)
        logging.getLogger("operations").info(
            "manual pump %s requested: channel=%s", payload.action, payload.channel_id)
        cmd = DeviceCommand(device_id=queue_service.device_id_for_commands(db),
            command_type=payload.action,
            channel_id=payload.channel_id,
            state="PENDING", created_at=utcnow(), updated_at=utcnow())
        db.add(cmd)
        db.commit()
        db.refresh(cmd)
        return {"command_id": cmd.id, "state": cmd.state}
    if payload.action in ("HOLD", "RELEASE", "PROMOTE"):
        return queue_service.queue_order(db, action=payload.action,
                                         command_id=payload.command_id,
                                         local_job_id=payload.local_job_id)
    if payload.action == "CANCEL":
        # Mark the job row CANCELLING now, not after the device acks. The UI
        # keys its optimistic drop on the row leaving the waiting states, and
        # only cancel_job() used to do this - so the channel-keyed
        # "Cancel active" path left the row visible until the next reconcile.
        queue_service.mark_job_cancelling(db, local_job_id=payload.local_job_id,
                                          channel_id=payload.channel_id)
    cmd = DeviceCommand(device_id=queue_service.device_id_for_commands(db),
        command_type=payload.action,
        local_job_id=payload.local_job_id, channel_id=payload.channel_id,
        state="PENDING", created_at=utcnow(), updated_at=utcnow())
    db.add(cmd)
    db.commit()
    db.refresh(cmd)
    return {"command_id": cmd.id, "state": cmd.state}


def poll_commands(db: Session, device_id: str) -> list[dict]:
    """HTTP poll: atomically claim and return up to 10 PENDING/DELIVERED rows."""
    # CONTRACT 8.4: a device on command_transport=MQTT is served by the bridge
    # only; the HTTP poll never hands out (or mutates) its rows.
    if claim.device_transport(db, device_id) == "MQTT":
        return []
    now = utcnow()
    # START/RESUME/CLEAR express recent operator intent. They must not start
    # machinery on reconnect minutes later. STOP/CANCEL/ESTOP never expire.
    db.execute(update(DeviceCommand).where(
        DeviceCommand.device_id == device_id,
        DeviceCommand.state.in_(("PENDING", "DELIVERED")),
        DeviceCommand.command_type.in_(("PUMP_START", "RESUME", "CLEAR", "READY", "ZERO", "TARE")),
        DeviceCommand.created_at < now - timedelta(seconds=settings.transient_command_ttl_seconds),
    ).values(state="FAILED", error_text="expired operator command", updated_at=now))
    # A STOP already acknowledged before a reboot must still supersede an
    # older START whose ACK was lost; the volatile firmware dedupe is gone.
    for channel_id in ("CH1", "CH2"):
        stop_id = db.scalar(select(func.max(DeviceCommand.id)).where(
            DeviceCommand.device_id == device_id,
            (DeviceCommand.command_type == "ESTOP") |
            ((DeviceCommand.command_type == "PUMP_STOP") &
             (DeviceCommand.channel_id == channel_id))))
        if stop_id is not None:
            db.execute(update(DeviceCommand).where(
                DeviceCommand.device_id == device_id,
                DeviceCommand.channel_id == channel_id,
                DeviceCommand.command_type == "PUMP_START",
                DeviceCommand.state.in_(("PENDING", "DELIVERED")),
                DeviceCommand.id < stop_id,
            ).values(state="FAILED", error_text="superseded by stop", updated_at=now))
    rows = list(db.scalars(select(DeviceCommand).where(
        DeviceCommand.device_id == device_id,
        DeviceCommand.state.in_(("PENDING", "DELIVERED")),
        # A parked JOB stays parked until the operator releases it. Control
        # commands (HOLD/RELEASE/PROMOTE/CANCEL/...) always have held=False and
        # so are never filtered out here.
        DeviceCommand.held.is_(False),
        # Rows the MQTT bridge already claimed are never handed out again here.
        or_(DeviceCommand.delivered_via.is_(None), DeviceCommand.delivered_via == "HTTP"),
    # Control commands first: this endpoint hands out at most 10 rows per poll,
    # so a HOLD that sorted purely by age could sit behind a full JOB backlog
    # for whole poll cycles - long enough for the job it is meant to park to
    # start. Among JOBs, promoted then FIFO, matching the device's own order.
    ).order_by(case((DeviceCommand.command_type == "ESTOP", 0),
                   (DeviceCommand.command_type.in_(("PUMP_STOP", "CANCEL")), 1),
                   (DeviceCommand.command_type != "JOB", 2), else_=3),
               DeviceCommand.promoted.desc(), DeviceCommand.created_at,
               DeviceCommand.id).limit(10)))
    boot_id = claim.device_boot_id(db, device_id)
    won = []
    for row in rows:
        # Atomic PENDING -> DELIVERED; a row lost to the MQTT bridge is skipped.
        if row.state == "PENDING" and not claim.claim_pending(db, row.id, "HTTP", boot_id):
            continue
        won.append(row)
    db.commit()
    for row in won:
        db.refresh(row)
    return [command_wire(row, db, gains) for row, gains in with_gains(won, db)]


def set_command_transport(db: Session, device_id: str, transport: str) -> dict:
    """Per-device command transport (CONTRACT 8.4). MQTT only when the birth caps
    contain cmd_mqtt. Switching back to HTTP leaves PENDING rows to the poll."""
    row = db.get(DeviceStatus, device_id)
    if row is None:
        raise ServiceError("not_found", "device not known", 404)
    if transport == "MQTT" and "cmd_mqtt" not in (row.caps_json or []):
        raise ServiceError("conflict", "device birth caps lack cmd_mqtt", 409)
    row.command_transport = transport
    db.commit()
    return {"device_id": device_id, "command_transport": row.command_transport}


def with_gains(rows: list[DeviceCommand], db: Session):
    """Pair each row with its pinned gains using one query for all of them.

    Replaces a db.get per row. A job is matched by its profile_version_id; a
    legacy job without one by (profile_id, its material, version). Both are
    loaded by one query and matched in Python so the query stays portable.
    """
    jobs = [r for r in rows
            if r.command_type == "JOB" and r.profile_id and r.profile_version]
    by_id, by_key = {}, {}
    version_ids = {r.profile_version_id for r in jobs if r.profile_version_id is not None}
    legacy_ids = {r.profile_id for r in jobs if r.profile_version_id is None}
    if version_ids or legacy_ids:
        cond = []
        if version_ids:
            cond.append(TuningProfile.profile_version_id.in_(version_ids))
        if legacy_ids:
            cond.append(TuningProfile.profile_id.in_(legacy_ids))
        for p in db.scalars(select(TuningProfile).where(or_(*cond))):
            by_id[p.profile_version_id] = p
            by_key[(p.profile_id, p.material_id, p.version)] = p

    def gains_for(r):
        if r.profile_version_id is not None:
            row = by_id.get(r.profile_version_id)
        else:
            row = by_key.get((r.profile_id, r.material_id, r.profile_version))
        # Same verification as profile_gains (material/id/version agree; a
        # legacy NULL-id job only matches a row that predates it).
        row = profile_service.row_for_job(r, row)
        return queue_service.safe_gains_wire(row)

    return [(r, gains_for(r)) for r in rows]


def command_wire(row: DeviceCommand, db: Session, gains=_MISSING) -> dict:
    """One command as the device receives it (HTTP poll item and MQTT body)."""
    item = {"command_id": row.id, "command_type": row.command_type,
            "target_g": row.target_g, "material_id": row.material_id,
            "priority": row.priority,
            "local_job_id": row.local_job_id, "channel_id": row.channel_id,
            "profile_id": row.profile_id, "profile_version": row.profile_version}
    if row.payload_json:
        item.update(row.payload_json)
    if row.command_type == "JOB" and row.profile_id and row.profile_version:
        # Ship the pinned gains with the job. The firmware must never
        # substitute "whatever is active on the channel" for the version the
        # operator chose: two jobs with the same target_g can pin different
        # versions, and the 4-slot device profile table cannot hold both.
        # Wire shape is unchanged (no profile_version_id): the server resolved
        # the exact row, the device only sees profile_id + version + gains.
        item["profile"] = queue_service.profile_gains(db, row) if gains is _MISSING else gains
    return item


def acknowledge_command(db: Session, command_id: int, payload: DeviceCommandAck) -> dict:
    row = db.get(DeviceCommand, command_id)
    if row is None:
        raise ServiceError("not_found", "command not found", 404)
    # An at-least-once JOB ACK can arrive after cancellation or completion.
    # Keep lifecycle state monotonic while still recording its local identity.
    terminal = row.state in claim.TERMINAL_STATES
    cancelling = row.state in ("CANCELLING", "RUNNING") and payload.state == "QUEUED"
    error, reason = payload.error, payload.reason
    if row.command_type in ("ZERO", "TARE") and payload.state == "APPLIED":
        # The CAS remote ZERO/TARE frame is unverified: APPLIED is never valid.
        logging.getLogger("operations").warning(
            "ACK APPLIED for %s command %s rejected", row.command_type, row.id)
        payload = payload.model_copy(update={
            "state": "FAILED", "result": "failed", "weight_g": None, "stable": None})
        error = reason = ZERO_TARE_APPLIED_REASON
    if not terminal and not cancelling:
        row.state = payload.state
    row.local_job_id = payload.local_job_id or row.local_job_id
    row.channel_id = payload.channel_id or row.channel_id
    row.error_text = error
    extra = {k: v for k, v in (("result", payload.result), ("reason", reason),
                               ("weight_g", payload.weight_g), ("stable", payload.stable))
             if v is not None}
    if extra:
        row.ack_json = {**(row.ack_json or {}), **extra}
    row.updated_at = utcnow()
    db.commit()
    return {"ok": True, "state": row.state}


def validate_device_status(payload: DeviceStatusIn) -> None:
    channels = payload.status.get("channels", [])
    if not isinstance(channels, list) or len(channels) > 2:
        raise ServiceError("invalid", "channels must be a list of at most two channels", 422)
    for channel in channels:
        if not isinstance(channel, dict):
            raise ServiceError("invalid", "invalid channel status", 422)
        age = channel.get("weight_age_ms")
        if age is not None and (type(age) not in (int, float) or not math.isfinite(age) or age < 0):
            raise ServiceError("invalid", "invalid weight acquisition age", 422)
        channel_id = channel.get("channel_id")
        if channel_id not in ("CH1", "CH2"):
            continue
        expected = 1 if channel_id == "CH1" else 2
        mapping = {"material_id": f"M{expected}", "pump_id": f"Pump {expected}",
                   "relay_id": f"Relay {expected}", "scale_id": f"Scale {expected}"}
        for key, value in mapping.items():
            if key in channel and channel[key] != value:
                raise ServiceError("invalid",
                    f"{channel_id} reports invalid fixed mapping for {key}", 422)
    for job in payload.status.get("queue", []):
        if not isinstance(job, dict) or not job.get("material_id"):
            continue
        material = job.get("material_id")
        local_channel = job.get("channel_id", 0)
        if material not in ("M1", "M2") or local_channel not in (0, None, "0", "") and str(local_channel) != material[-1]:
            raise ServiceError("invalid",
                "queued job material_id conflicts with its assigned channel", 422)


def update_live_status(payload: DeviceStatusIn) -> dict:
    """Latest state only: never queues a DB write or delays behind history."""
    from .live_state import live_state
    validate_device_status(payload)
    accepted = live_state.update_status(payload.device_id, payload.status)
    return {"ok": True, "accepted": accepted}


def update_device_status(db: Session, payload: DeviceStatusIn) -> dict:
    from .live_state import live_state
    validate_device_status(payload)
    live_state.update_status(payload.device_id, payload.status)
    row = db.get(DeviceStatus, payload.device_id)
    if row is None:
        row = DeviceStatus(device_id=payload.device_id, status_json=payload.status,
                          updated_at=utcnow())
        db.add(row)
    else:
        row.status_json = payload.status
        row.updated_at = utcnow()
    local_jobs = payload.status.get("queue", [])
    by_local_id = {int(item.get("id", 0)): item for item in local_jobs if isinstance(item, dict)}
    commands = list(db.scalars(select(DeviceCommand).where(
        DeviceCommand.device_id == payload.device_id,
        DeviceCommand.command_type == "JOB",
        DeviceCommand.local_job_id.is_not(None),
        DeviceCommand.state.in_(("QUEUED", "RUNNING")),
    )))
    active = {int(ch.get("active_job_id", 0)): ch.get("channel_id")
              for ch in payload.status.get("channels", []) if isinstance(ch, dict)}
    for command in commands:
        job = by_local_id.get(command.local_job_id)
        if job:
            local_state = str(job.get("state", "QUEUED"))
            local_channel = int(job.get("channel_id", 0) or 0) if str(job.get("channel_id", "")).isdigit() else 0
            command.channel_id = f"CH{local_channel}" if local_channel in (1, 2) else None
            command.pump_id = f"Pump {local_channel}" if local_channel in (1, 2) else None
            if local_state in ("COMPLETE", "FAILED", "CANCELLED"):
                command.state = local_state
            elif local_state == "RUNNING":
                command.state = "RUNNING"
                if command.assigned_at is None:
                    command.assigned_at = utcnow()
            else:
                command.state = "QUEUED"
        elif command.local_job_id in active:
            command.state = "RUNNING"
            command.channel_id = active[command.local_job_id]
            command.assigned_at = command.assigned_at or utcnow()
    db.commit()
    return {"ok": True, "updated_at": row.updated_at}


def read_device_status(db: Session) -> list[dict]:
    rows = list(db.scalars(select(DeviceStatus).order_by(DeviceStatus.device_id)))
    now = datetime.now(timezone.utc).replace(tzinfo=None)
    return [{"device_id": row.device_id, "updated_at": row.updated_at,
             "age_seconds": max(0, int((now - row.updated_at).total_seconds())),
             "status": row.status_json} for row in rows]
