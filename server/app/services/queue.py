"""Queue job lifecycle: create / list / cancel / hold-release-promote / resubmit.

Transport-neutral: every function takes (db, validated args) and returns a plain
dict or raises ServiceError. The HTTP routes and a future MQTT dispatcher both
call these. Telemetry is observability only: nothing here drives a relay; jobs
are rows the device pulls (or the bridge publishes) and the firmware decides.
"""

from pydantic import (BaseModel, ConfigDict, Field, ValidationError, field_validator,
                      model_validator)
from sqlalchemy import select, update
from sqlalchemy.orm import Session

from ..models import DeviceCommand, DeviceStatus, Material, TuningProfile
from ..config import settings
from ..schemas import TuningProfileIn
from . import profile_rules
from . import profiles as profile_service
from .analytics import utcnow
from .errors import ServiceError


class QueueJobIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    material_id: str = Field(pattern="^(M1|M2)$")
    # Any weight 1..per-pump maximum (config PUMPn_MAX_TARGET_G, default 20000 g,
    # mirrored by a firmware constant). Not limited to the four Test Graph
    # categories; the per-pump bound is enforced in create_job.
    target_g: int = Field(gt=0, le=100_000)
    priority: int = Field(default=0, ge=0, le=1)
    # Optional pin. A queued job records the exact immutable tuning version it
    # will run with; nothing is resolved from "the active profile" at start
    # time, so a version saved between enqueue and start cannot change what
    # runs. See docs/cas-audit/11-queue-cancel-and-profile-pin.md.
    # Either profile_version_id (the exact row) or profile_id + profile_version;
    # the pair is resolved against THIS job's material, never by id+version alone.
    # With no pin the server resolves among ACTIVE compatible profiles at enqueue
    # time and pins the single match, or refuses (NO_COMPATIBLE_PROFILE /
    # AMBIGUOUS_PROFILE). It never picks the nearest or a canonical one.
    profile_id: str | None = Field(default=None, min_length=1, max_length=64,
                                   pattern=r"^[A-Za-z0-9._-]+$")
    profile_version: int | None = Field(default=None, ge=1)
    profile_version_id: int | None = Field(default=None, ge=1)

    @model_validator(mode="after")
    def pin_is_whole(self):
        if self.profile_version_id is None and (
                (self.profile_id is None) != (self.profile_version is None)):
            raise ValueError("pin with profile_version_id, or profile_id and profile_version together")
        return self

class ResubmitIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    confirm_unknown: bool = False


STALE_UNKNOWN_PREFIX = "STALE_UNKNOWN:"
WAITING_STATES = ("PENDING", "DELIVERED", "QUEUED")


def device_id_for_commands(db: Session) -> str:
    """The device that receives operator commands (relay controller preferred)."""
    rows = list(db.scalars(select(DeviceStatus).order_by(DeviceStatus.updated_at.desc())))
    for row in rows:
        status = row.status_json or {}
        if status.get("role") == "relay_controller":
            return row.device_id
    for row in rows:
        status = row.status_json or {}
        if any(c.get("channel_id") for c in (status.get("channels") or [])):
            return row.device_id
    if rows:
        return rows[0].device_id
    raise ServiceError("unavailable", "no ESP32 device has reported status", 503)


def _range_text(row: TuningProfile) -> str:
    lo, hi = profile_rules.row_range(row)
    return f"{lo}..{hi} g"


def resolve_pinned_profile(db: Session, *, material_id: str, target_g: int,
                           profile_id: str | None = None,
                           profile_version: int | None = None,
                           profile_version_id: int | None = None) -> TuningProfile:
    """Look up and re-validate the exact immutable version a job is pinned to.

    Checks are repeated at queue time, not only at profile-create time, because
    a row written before the clamps existed (or corrupted afterwards) must
    never reach the device as a job's pin. The row must be ACTIVE, belong to this
    pump, and its declared range must contain the target (else
    TARGET_OUT_OF_PROFILE_RANGE). A ranged (schema 2) row must also carry all
    staged fields and pass the firmware consistency check AT THIS TARGET.

    profile_id + profile_version is only unique per material, so it is resolved
    against the JOB's material; a row that exists only for the other pump is
    rejected, never silently substituted.
    """
    if profile_version_id is not None:
        row = db.get(TuningProfile, profile_version_id)
        if row is None:
            raise ServiceError(
                "invalid", f"no PID profile with profile_version_id {profile_version_id}", 422)
        if ((profile_id is not None and profile_id != row.profile_id)
                or (profile_version is not None and profile_version != row.version)):
            raise ServiceError(
                "invalid",
                f"profile_version_id {profile_version_id} is {row.profile_id} "
                f"v{row.version}, not {profile_id} v{profile_version}", 422)
    else:
        row = profile_service.resolve_version(db, profile_id, material_id, profile_version)
        if row is None:
            other = db.scalar(select(TuningProfile).where(
                TuningProfile.profile_id == profile_id,
                TuningProfile.version == profile_version))
            if other is not None:
                raise ServiceError(
                    "invalid",
                    f"PID profile {profile_id} v{profile_version} is for "
                    f"{other.material_id}, not {material_id}", 422)
            raise ServiceError(
                "invalid", f"no PID profile {profile_id} version {profile_version}", 422)
    profile_id, profile_version = row.profile_id, row.version
    if row.material_id != material_id:
        raise ServiceError(
            "invalid",
            f"PID profile {profile_id} v{profile_version} is for "
            f"{row.material_id}, not {material_id}", 422)
    expected_channel = "CH1" if material_id == "M1" else "CH2"
    if row.channel_id != expected_channel:
        raise ServiceError(
            "invalid",
            f"PID profile {profile_id} v{profile_version} channel "
            f"{row.channel_id} conflicts with the fixed material mapping", 422)
    if row.status != "active":
        raise ServiceError(
            "invalid", f"PID profile {profile_id} v{profile_version} is {row.status}; "
            "only an active profile can be pinned", 422)
    if not profile_rules.contains(row, target_g):
        raise ServiceError(
            "target_out_of_profile_range",
            f"PID profile {profile_id} v{profile_version} covers {_range_text(row)}, "
            f"not {target_g} g",
            data={"target_g": target_g, "profile_version_id": row.profile_version_id,
                  "target_min_g": profile_rules.row_range(row)[0],
                  "target_max_g": profile_rules.row_range(row)[1]})
    if row.profile_schema == 2:
        why = profile_rules.consistency_error(row, target_g)
        if why:
            raise ServiceError(
                "invalid", f"PID profile {profile_id} v{profile_version} is unsafe at "
                f"{target_g} g and cannot be pinned: {why}", 422)
        return row
    try:
        TuningProfileIn.model_validate({
            "profile_id": row.profile_id,
            "material_id": row.material_id,
            "channel_id": row.channel_id,
            "target_g": row.target_g,
            "kp": row.kp, "ki": row.ki, "kd": row.kd,
            "tolerance_g": row.tolerance_g,
            "max_overshoot_g": row.max_overshoot_g,
            "max_duration_ms": row.max_duration_ms,
            "window_ms": row.window_ms,
            "min_on_ms": row.min_on_ms,
            "min_off_ms": row.min_off_ms,
        })
    except ValidationError as exc:
        raise ServiceError(
            "invalid",
            f"PID profile {profile_id} v{profile_version} has unsafe "
            f"bounds and cannot be pinned to a job: {exc.errors()[0]['msg']}", 422) from exc
    return row


def gains_wire(row: TuningProfile) -> dict:
    """The nested ``profile{}`` the device receives and ``profile_hash`` covers.

    A schema-1 row serialises EXACTLY as it always did (11 members), so every
    existing profile_hash is unchanged. A schema-2 row adds its declared range and
    the eight staged members. Raises ValueError for a schema-2 row with a missing
    member (callers treat that as an unresolvable pin, fail safe)."""
    wire = {
        "profile_id": row.profile_id, "version": row.version,
        "kp": row.kp, "ki": row.ki, "kd": row.kd,
        "tolerance_g": row.tolerance_g,
        "max_overshoot_g": row.max_overshoot_g,
        "max_duration_ms": row.max_duration_ms,
        "window_ms": row.window_ms,
        "min_on_ms": row.min_on_ms,
        "min_off_ms": row.min_off_ms,
    }
    if row.profile_schema == 2:
        wire["target_min_g"], wire["target_max_g"] = row.target_min_g, row.target_max_g
        for name in profile_rules.STAGED_FIELDS:
            wire[name] = getattr(row, name)
        if any(v is None for v in wire.values()):
            raise ValueError("ranged profile is missing a required member")
    return wire


def safe_gains_wire(row: TuningProfile | None) -> dict | None:
    if row is None:
        return None
    try:
        return gains_wire(row)
    except ValueError:
        return None


def profile_gains(db: Session, job: DeviceCommand) -> dict | None:
    """The immutable gains for one job's pinned version, as the device expects them.

    Taken from the exact row: profile_version_id when the job has one, else the
    legacy snapshot (profile_id + the JOB's material + version, and only a row
    that existed when the job was queued). The row must also agree with the
    job's material/profile_id/version. None (device refuses, fail safe) if it
    was deleted, mismatches, or a legacy job would match a newer row.
    """
    return safe_gains_wire(profile_service.pinned_row(db, job))


def create_job(db: Session, payload: QueueJobIn) -> dict:
    material = db.get(Material, payload.material_id)
    if material is None or not material.enabled:
        raise ServiceError("invalid", "unknown or disabled material_id", 422)
    top = settings.max_target_g(payload.material_id)
    if not 1 <= payload.target_g <= top:
        raise ServiceError(
            "invalid", f"target_g must be 1..{top} g for {payload.material_id} "
            f"(got {payload.target_g})", 422)
    pinned = (payload.profile_version_id is not None or payload.profile_id is not None)
    if pinned:
        profile = resolve_pinned_profile(
            db, material_id=payload.material_id, target_g=payload.target_g,
            profile_id=payload.profile_id, profile_version=payload.profile_version,
            profile_version_id=payload.profile_version_id,
        )
    else:
        profile = profile_service.resolve_for_job(db, payload.material_id, payload.target_g)
    device_id = device_id_for_commands(db)
    # Lock the pinned row until commit and re-check it is still there and still
    # valid for this target, so a concurrent delete, deprecate or deactivate
    # cannot leave a live job with a dangling or stale pin.
    locked = profile_service.lock_row(db, profile.profile_version_id)
    if locked is None:
        db.rollback()
        raise ServiceError("conflict", "PID profile version was deleted; pick another", 409)
    why = profile_service.usable_error(locked, payload.target_g)
    if why:
        db.rollback()
        raise ServiceError("conflict", f"PID profile {locked.profile_id} v{locked.version} "
                           f"changed while queueing: it {why}", 409)
    row = DeviceCommand(device_id=device_id, command_type="JOB", material_id=payload.material_id,
                        target_g=payload.target_g,
                        priority=payload.priority, state="PENDING",
                        profile_id=profile.profile_id, profile_version=profile.version,
                        profile_version_id=profile.profile_version_id,
                        created_at=utcnow(), updated_at=utcnow())
    db.add(row)
    db.commit()
    db.refresh(row)
    return {"command_id": row.id, "device_id": row.device_id, "material_id": row.material_id,
            "enqueue_sequence": row.id, "target_g": row.target_g,
            "priority": row.priority, "state": row.state, "status": row.state,
            "profile_id": row.profile_id, "profile_version": row.profile_version,
            "profile_version_id": row.profile_version_id,
            "channel_id": None, "pump_id": None, "created_at": row.created_at,
            "queued_at": row.created_at}

def read_queue(db: Session) -> dict:
    device = db.scalar(select(DeviceStatus).order_by(DeviceStatus.updated_at.desc()).limit(1))
    commands = list(db.scalars(select(DeviceCommand).where(
        DeviceCommand.command_type == "JOB",
        DeviceCommand.state.in_(WAITING_STATES),
    ).order_by(DeviceCommand.promoted.desc(), DeviceCommand.priority.desc(),
               DeviceCommand.created_at, DeviceCommand.id)))
    jobs = [{"command_id": c.id, "enqueue_sequence": c.id,
             "local_job_id": c.local_job_id, "material_id": c.material_id,
             "target_g": c.target_g,
             "priority": c.priority, "state": c.state, "status": c.state,
             "held": c.held, "promoted": c.promoted,
             "profile_id": c.profile_id, "profile_version": c.profile_version,
             "profile_version_id": c.profile_version_id,
             "created_at": c.created_at, "queued_at": c.created_at,
             "channel_id": c.channel_id} for c in commands]
    # Jobs the device refused outright (e.g. 'stale: older than ledger floor'):
    # never started, so no run history. Shown until resubmitted so they are not
    # silently lost.
    failed = [{"command_id": c.id, "material_id": c.material_id, "target_g": c.target_g,
               "priority": c.priority, "state": c.state, "error": c.error_text,
               "outcome_unknown": (c.error_text or "").startswith(STALE_UNKNOWN_PREFIX),
               "profile_id": c.profile_id, "profile_version": c.profile_version,
               "profile_version_id": c.profile_version_id,
               "created_at": c.created_at, "updated_at": c.updated_at}
              for c in db.scalars(select(DeviceCommand).where(
                  DeviceCommand.command_type == "JOB", DeviceCommand.state == "FAILED",
                  DeviceCommand.local_job_id.is_(None),
              ).order_by(DeviceCommand.id.desc()).limit(50))
              if resubmittable(c)]
    return {"device_id": device.device_id if device else None,
            "device_updated_at": device.updated_at if device else None,
            "device_status": device.status_json if device else None,
            "waiting_commands": jobs, "failed_commands": failed}


def cancel_job(db: Session, command_id: int) -> dict:
    job = db.get(DeviceCommand, command_id)
    if job is None or job.command_type != "JOB":
        raise ServiceError("not_found", "queue job not found", 404)
    job = follow_successor(db, job) or job
    if never_handed_out(job):
        job.state = "CANCELLED"
        job.ack_json = {**(job.ack_json or {}), "cancelled": True}
        job.updated_at = utcnow()
        db.commit()
        return {"ok": True, "state": job.state}
    if job.local_job_id is None and (job.state == "DELIVERED" or (
            job.state == "PENDING" and job.published_at is not None)):
        # Delivery may already be in flight (HTTP poll or MQTT publish). Send a
        # cancellation keyed by the remote command id so the ESP32 can cancel it
        # after it deduplicates and enqueues the delivered JOB command.
        cmd = DeviceCommand(device_id=job.device_id, command_type="CANCEL",
            payload_json={"remote_command_id": job.id}, state="PENDING",
            created_at=utcnow(), updated_at=utcnow())
        job.state = "CANCELLING"
        job.ack_json = {**(job.ack_json or {}), "cancelled": True}
        job.updated_at = utcnow()
        db.add(cmd)
        db.commit()
        return {"ok": True, "state": job.state}
    if job.state != "QUEUED" or job.local_job_id is None:
        raise ServiceError("conflict", "job is no longer waiting", 409)
    cmd = DeviceCommand(device_id=job.device_id, command_type="CANCEL",
                        local_job_id=job.local_job_id, state="PENDING",
                        created_at=utcnow(), updated_at=utcnow())
    job.state = "CANCELLING"
    job.ack_json = {**(job.ack_json or {}), "cancelled": True}
    job.updated_at = utcnow()
    db.add(cmd)
    db.commit()
    return {"ok": True, "state": job.state}


def mark_job_cancelling(db: Session, *, local_job_id: int | None,
                        channel_id: str | None) -> None:
    """Flip the matching waiting JOB row to CANCELLING so the UI drops it now.

    Strictly one row. A CANCEL named by local_job_id touches only that job; one
    named by channel_id touches only the job currently RUNNING on that channel.
    Queued siblings - including ones sharing a target_g - are never touched.
    """
    job = None
    if local_job_id is not None:
        job = db.scalar(select(DeviceCommand).where(
            DeviceCommand.command_type == "JOB",
            DeviceCommand.local_job_id == local_job_id,
            DeviceCommand.state.in_(WAITING_STATES),
        ).order_by(DeviceCommand.id.desc()).limit(1))
    elif channel_id:
        job = db.scalar(select(DeviceCommand).where(
            DeviceCommand.command_type == "JOB",
            DeviceCommand.channel_id == channel_id,
            DeviceCommand.state == "RUNNING",
        ).order_by(DeviceCommand.id.desc()).limit(1))
    if job is None:
        return
    job.state = "CANCELLING"
    job.ack_json = {**(job.ack_json or {}), "cancelled": True}
    job.updated_at = utcnow()


def queue_order(db: Session, *, action: str, command_id: int | None = None,
                local_job_id: int | None = None) -> dict:
    """Park (HOLD), release (RELEASE) or promote (PROMOTE) one waiting job.

    A job the server still owns - PENDING, never delivered - is changed here and
    never sent to the device at all. A job the device already owns is forwarded
    as its own command, because the ESP32 queue is what decides what starts, and
    the row is mirrored so the UI updates before the next status snapshot lands.

    None of these ever touch a running job: the queue is non-preemptive and
    these only change eligibility and order. Hold is reversible; only Cancel is
    irreversible.
    """
    if not (command_id or local_job_id):
        raise ServiceError("invalid", "HOLD/RELEASE/PROMOTE require command_id or local_job_id", 422)

    job = None
    if command_id is not None:
        job = db.get(DeviceCommand, command_id)
        if job is None or job.command_type != "JOB":
            raise ServiceError("not_found", "queue job not found", 404)
    else:
        job = db.scalar(select(DeviceCommand).where(
            DeviceCommand.command_type == "JOB",
            DeviceCommand.local_job_id == local_job_id,
            DeviceCommand.state.in_(WAITING_STATES),
        ).order_by(DeviceCommand.id.desc()).limit(1))

    # An old id the operator's UI still holds after RELEASE re-issued the job.
    job = follow_successor(db, job)

    if job is not None and job.state not in WAITING_STATES:
        raise ServiceError("conflict", "job is no longer waiting", 409)

    if (job is not None and action == "RELEASE" and job.held
            and never_handed_out(job)):
        # The device refuses any non-STOP id at or below its eviction floor, and
        # a parked job keeps its original id while newer commands advance that
        # floor. Release under a fresh monotonic id; the old row can never be
        # served again (SUPERSEDED is outside every poll/publish filter).
        new = reissue_job(db, job)
        db.commit()
        return {"command_id": new.id, "state": new.state,
                "held": new.held, "promoted": new.promoted}

    if job is not None and never_handed_out(job):
        # Conditional: if the MQTT hook published the row meanwhile, the flag
        # alone would not reach the device, so fall through and forward it.
        flags = {"held": job.held, "promoted": job.promoted}
        if action == "HOLD":
            flags["held"] = True
        elif action == "RELEASE":
            flags["held"] = False
        else:
            flags["promoted"] = True
        won = db.execute(update(DeviceCommand).where(
            DeviceCommand.id == job.id, DeviceCommand.state == "PENDING",
            DeviceCommand.local_job_id.is_(None), DeviceCommand.published_at.is_(None),
        ).values(updated_at=utcnow(), **flags))
        if won.rowcount == 1:
            db.commit()
            db.refresh(job)
            return {"command_id": job.id, "state": job.state,
                    "held": job.held, "promoted": job.promoted}
        db.refresh(job)
        if job.state not in WAITING_STATES:
            raise ServiceError("conflict", "job is no longer waiting", 409)

    local_id = local_job_id or (job.local_job_id if job else None)
    # DELIVERED but not yet acked: key the command by the remote id the same way
    # a racing cancel does, so the ESP32 can resolve it after it deduplicates
    # and enqueues the delivered JOB.
    remote_key = {"remote_command_id": job.id} if (job is not None and local_id is None) else None

    cmd = DeviceCommand(
        device_id=job.device_id if job is not None else device_id_for_commands(db),
        command_type=action, local_job_id=local_id, state="PENDING",
        payload_json=remote_key, created_at=utcnow(), updated_at=utcnow())
    db.add(cmd)
    if job is not None:
        # Mirror onto the row so the UI updates before the next status snapshot
        # lands. The device remains the source of truth for a job it owns.
        _apply_order_flag(action, job)
        job.updated_at = utcnow()
    db.commit()
    db.refresh(cmd)
    return {"command_id": cmd.id, "state": cmd.state,
            "held": job.held if job is not None else False,
            "promoted": job.promoted if job is not None else False}


def never_handed_out(job: DeviceCommand) -> bool:
    """True only if neither transport can have delivered this JOB: still
    PENDING, no ACK identity, and not published over MQTT."""
    return (job.state == "PENDING" and job.local_job_id is None
            and job.published_at is None)


def repin(db: Session, job: DeviceCommand) -> TuningProfile:
    """Re-resolve and re-validate a job's pin when it is copied (release or
    resubmit). Refuses with 409 instead of copying a dangling or ambiguous pin;
    a legacy NULL id is upgraded to the exact row id (rule: row must predate
    the job). The row is locked until commit so a delete cannot race the copy."""
    row = profile_service.pinned_row(db, job, lock=True)
    if row is None:
        db.rollback()
        raise ServiceError("conflict", (
            f"the PID profile {job.profile_id} v{job.profile_version} pinned by this job "
            "can no longer be resolved (deleted or ambiguous); cancel it and queue a new job"), 409)
    try:
        resolve_pinned_profile(db, material_id=job.material_id, target_g=job.target_g,
                               profile_version_id=row.profile_version_id)
    except ServiceError as exc:
        db.rollback()
        raise ServiceError("conflict", (
            f"the PID profile pinned by this job is no longer valid: {exc.message}"), 409) from exc
    return row


def reissue_job(db: Session, old: DeviceCommand, *, held: bool = False) -> DeviceCommand:
    """Copy a JOB row under a fresh id; link both ways and retire the old row.

    created_at is kept so queue position/FIFO and the UI age are unchanged.
    The old row is claimed with one conditional UPDATE (rowcount must be 1) so
    two concurrent RELEASEs can never both create a live successor.
    """
    pin = repin(db, old)
    claim = db.execute(update(DeviceCommand).where(
        DeviceCommand.id == old.id, DeviceCommand.state == "PENDING",
        DeviceCommand.held.is_(True), DeviceCommand.local_job_id.is_(None),
        DeviceCommand.published_at.is_(None),
    ).values(state="SUPERSEDED", held=False, updated_at=utcnow()))
    if claim.rowcount != 1:
        db.rollback()
        raise ServiceError("conflict", "job was already released or handed out", 409)
    new = DeviceCommand(
        device_id=old.device_id, command_type="JOB", material_id=old.material_id,
        target_g=old.target_g, priority=old.priority, channel_id=old.channel_id,
        payload_json=old.payload_json, profile_id=old.profile_id,
        profile_version=old.profile_version,
        profile_version_id=pin.profile_version_id, state="PENDING", held=held,
        promoted=old.promoted, ack_json={"supersedes": old.id},
        created_at=old.created_at, updated_at=utcnow())
    db.add(new)
    db.flush()
    db.refresh(old)
    old.error_text = f"superseded by command {new.id}"
    old.ack_json = {**(old.ack_json or {}), "superseded_by": new.id}
    old.updated_at = utcnow()
    return new


def follow_successor(db: Session, job: DeviceCommand | None) -> DeviceCommand | None:
    for _ in range(8):
        if job is None or job.state != "SUPERSEDED":
            break
        nxt = (job.ack_json or {}).get("superseded_by")
        job = db.get(DeviceCommand, nxt) if nxt else None
    return job


def resubmittable(job: DeviceCommand) -> bool:
    """Only an explicit device refusal: FAILED, never started, with an error and
    no operator-cancel marker."""
    return (job.state == "FAILED" and job.local_job_id is None and bool(job.error_text)
            and not (job.ack_json or {}).get("cancelled"))


def resubmit_job(db: Session, command_id: int, payload: ResubmitIn | None = None) -> dict:
    """Re-queue a JOB the device refused (e.g. stale id) under a new command id."""
    job = db.get(DeviceCommand, command_id)
    if job is None or job.command_type != "JOB":
        raise ServiceError("not_found", "queue job not found", 404)
    if not resubmittable(job):
        raise ServiceError("conflict", "only a refused, never-started job can be resubmitted", 409)
    if (job.error_text or "").startswith(STALE_UNKNOWN_PREFIX) and not (
            payload is not None and payload.confirm_unknown):
        raise ServiceError("conflict", (
            "the device could not tell whether this job already ran; the material may "
            "already have been dispensed. Resubmit again with confirm_unknown=true to run it anyway"), 409)
    pin = repin(db, job)
    # Atomic claim: only one caller moves FAILED -> RESUBMITTED.
    claim = db.execute(update(DeviceCommand).where(
        DeviceCommand.id == job.id, DeviceCommand.state == "FAILED",
        DeviceCommand.local_job_id.is_(None),
    ).values(state="RESUBMITTED", updated_at=utcnow()))
    if claim.rowcount != 1:
        db.rollback()
        raise ServiceError("conflict", "job was already resubmitted", 409)
    new = DeviceCommand(
        device_id=job.device_id, command_type="JOB", material_id=job.material_id,
        target_g=job.target_g, priority=job.priority, payload_json=job.payload_json,
        profile_id=job.profile_id, profile_version=job.profile_version,
        profile_version_id=pin.profile_version_id,
        state="PENDING", ack_json={"resubmits": job.id},
        created_at=utcnow(), updated_at=utcnow())
    db.add(new)
    db.flush()
    db.refresh(job)
    job.ack_json = {**(job.ack_json or {}), "resubmitted_as": new.id}
    job.updated_at = utcnow()
    db.commit()
    return {"command_id": new.id, "state": new.state, "resubmits": job.id}


def _apply_order_flag(action: str, job: DeviceCommand) -> None:
    if action == "HOLD":
        job.held = True
    elif action == "RELEASE":
        job.held = False
    else:  # PROMOTE
        job.promoted = True
