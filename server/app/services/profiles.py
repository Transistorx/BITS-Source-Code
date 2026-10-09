"""PID profile version rules, in one place.

Every (profile_id, material_id) pair has its own counter:
``next = max(version for that profile_id AND material_id, over tuning_profiles
AND the versions device_commands / dispense_runs still reference) + 1``, so a
number history points at is never reused, even after its row was deleted. Pumps never
synchronise: P1 at v7 and P2 at v4 save-both to P1 v8 and P2 v5.

``next_versions`` (read-only, used by the preview endpoint) and
``create_versions`` (used by single and save-both saves) share the same
``_max_versions`` query, so the UI prediction cannot differ from the server.
"""

import logging

from sqlalchemy import func, or_, select
from sqlalchemy.exc import IntegrityError, OperationalError
from sqlalchemy.orm import Session

from ..config import settings
from ..models import (DeviceCommand, DeviceStatus, DispenseRun, ProfileStatusLog,
                      TuningProfile)
from ..schemas import (BulkProfilesOut, CANONICAL_TARGETS_G, TuningProfileBulkIn,
                       TuningProfileIn, TuningProfileOut)
from . import profile_rules as rules
from .analytics import utcnow
from .errors import ServiceError

log = logging.getLogger(__name__)

MATERIAL_CHANNEL = {"M1": "CH1", "M2": "CH2"}
MAX_SAVE_ATTEMPTS = 5
# MySQL deadlock / lock wait timeout: another save of the same profile won.
_RETRYABLE_MYSQL = {1213, 1205}


def _max_versions(db: Session, profile_id: str, materials, lock: bool = False) -> dict[str, int]:
    """Highest number ever used per material for this profile_id.

    Counts tuning_profiles AND the version numbers history still references
    (device_commands / dispense_runs), so a number that a live or past job/run
    points at is never handed out again, even after its row was deleted.
    """
    materials = list(materials)
    stmt = (select(TuningProfile.material_id, func.max(TuningProfile.version))
            .where(TuningProfile.profile_id == profile_id,
                   TuningProfile.material_id.in_(materials))
            .group_by(TuningProfile.material_id))
    if lock:
        # MySQL: SELECT ... FOR UPDATE serialises concurrent saves of this
        # profile_id. SQLite ignores it (single writer). The unique key plus the
        # retry in create_versions covers the "no row to lock yet" case.
        stmt = stmt.with_for_update()
    found = {m: int(v or 0) for m, v in db.execute(stmt).all()}
    for model in (DeviceCommand, DispenseRun):
        hist = db.execute(
            select(model.material_id, func.max(model.profile_version))
            .where(model.profile_id == profile_id, model.material_id.in_(materials))
            .group_by(model.material_id)).all()
        for m, v in hist:
            found[m] = max(found.get(m, 0), int(v or 0))
    for payload in _profile_command_payloads(db):
        m, v = payload.get("material_id"), _as_int(payload.get("version"))
        if m in materials and payload.get("profile_id") == profile_id and v:
            found[m] = max(found.get(m, 0), v)
    return {m: found.get(m, 0) for m in materials}


def _as_int(value) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _profile_command_payloads(db: Session) -> list[dict]:
    """Payloads of PROFILE commands (versions staged to the device).

    Their profile_id/profile_version columns are NULL (the version lives only in
    payload_json, and the device wire must not change), so they are read here in
    Python, portable across MySQL and SQLite. PROFILE_CLEAR carries no version.
    """
    return [p for p in db.scalars(select(DeviceCommand.payload_json).where(
        DeviceCommand.command_type == "PROFILE")) if isinstance(p, dict)]


def lock_row(db: Session, profile_version_id: int) -> TuningProfile | None:
    """Re-read one profile row with a row lock (FOR UPDATE; no-op on SQLite).

    Held until commit/rollback, so a job pinning the row and a delete of it
    serialise. Returns None if the row is already gone.
    """
    return db.scalar(select(TuningProfile)
                     .where(TuningProfile.profile_version_id == profile_version_id)
                     .with_for_update().execution_options(populate_existing=True))


def row_for_job(job: DeviceCommand, row: TuningProfile | None) -> TuningProfile | None:
    """The row only if it is exactly what the job pinned, else None (fail safe).

    The row must agree with the job's own material, (profile_id, version) and
    target_g, sit on the channel fixed for that material (M1->CH1, M2->CH2), and
    have existed when the job was queued (created_at <= job.created_at, for
    id-linked jobs too: a job's id is set at creation, and a release keeps the
    old created_at), so a number created later can never feed an old job.
    """
    if row is None:
        return None
    if (row.material_id != job.material_id or row.profile_id != job.profile_id
            or row.version != job.profile_version):
        return None
    if (job.target_g is None or not rules.contains(row, job.target_g)
            or MATERIAL_CHANNEL.get(job.material_id) != row.channel_id):
        return None
    if row.created_at is None or job.created_at is None or row.created_at > job.created_at:
        return None
    return row


def pinned_row(db: Session, job: DeviceCommand, lock: bool = False) -> TuningProfile | None:
    """Resolve and verify the row a JOB is pinned to (see row_for_job)."""
    if job.profile_version_id is not None:
        row = lock_row(db, job.profile_version_id) if lock else db.get(
            TuningProfile, job.profile_version_id)
    else:
        row = resolve_version(db, job.profile_id, job.material_id, job.profile_version, lock=lock)
    return row_for_job(job, row)

def next_versions(db: Session, profile_id: str, materials) -> dict[str, int]:
    """Version each material would receive if saved now (read-only)."""
    return {m: v + 1 for m, v in _max_versions(db, profile_id, materials).items()}


def create_versions(db: Session, fields: dict, materials,
                    per_material: dict | None = None,
                    actor: str | None = None) -> list[TuningProfile]:
    """INSERT one new immutable row per material, all-or-nothing.

    ``fields`` are the TuningProfileFields values (no material/channel). Runs in
    one transaction; a unique-key collision or deadlock rolls everything back
    and retries with freshly computed versions, so concurrent saves can neither
    collide nor leave a half-saved pair.
    """
    materials = list(dict.fromkeys(materials))
    last_error = None
    for attempt in range(MAX_SAVE_ATTEMPTS):
        try:
            maxima = _max_versions(db, fields["profile_id"], materials, lock=True)
            now = utcnow()
            rows = []
            for material in materials:
                # Always a DRAFT: never active, never usable by a job, until it is
                # validated and then activated by a separate, explicit step.
                row = TuningProfile(**fields, **(per_material or {}).get(material, {}),
                                    material_id=material,
                                    channel_id=MATERIAL_CHANNEL[material],
                                    version=maxima[material] + 1,
                                    status="draft", active=False, created_at=now)
                db.add(row)
                rows.append(row)
            db.flush()
            for row in rows:
                _log_status(db, row, None, "draft", actor, "created")
            db.commit()
            return rows
        except IntegrityError as exc:
            last_error = exc
        except OperationalError as exc:
            code = exc.orig.args[0] if getattr(exc.orig, "args", None) else None
            if code not in _RETRYABLE_MYSQL:
                db.rollback()
                raise
            last_error = exc
        db.rollback()
        log.warning("profile save collided (attempt %d/%d): %s",
                    attempt + 1, MAX_SAVE_ATTEMPTS, last_error.__class__.__name__)
    raise ServiceError(
        "conflict",
        "another save of this profile_id was in progress; nothing was saved, please retry",
        409,
    )


def resolve_version(db: Session, profile_id: str, material_id: str,
                    version: int, lock: bool = False) -> TuningProfile | None:
    """The one row for (profile_id, material_id, version), or None."""
    stmt = select(TuningProfile).where(
        TuningProfile.profile_id == profile_id,
        TuningProfile.material_id == material_id,
        TuningProfile.version == version)
    if lock:
        stmt = stmt.with_for_update().execution_options(populate_existing=True)
    return db.scalar(stmt)


def _legacy_match(model, row: TuningProfile):
    """History rows written before the surrogate id existed (NULL id) still
    point at a row by its snapshot: profile_id + material_id + version."""
    return (model.profile_version_id.is_(None)
            & (model.profile_id == row.profile_id)
            & (model.material_id == row.material_id)
            & (model.profile_version == row.version))


def reference_counts(db: Session, row: TuningProfile) -> dict[str, int]:
    """How many queue commands (any state) and runs reference this exact row."""
    commands = db.scalar(select(func.count()).select_from(DeviceCommand).where(
        or_(DeviceCommand.profile_version_id == row.profile_version_id,
            _legacy_match(DeviceCommand, row)))) or 0
    # PROFILE commands staged this exact version to the device (payload only).
    commands += sum(
        1 for p in _profile_command_payloads(db)
        if p.get("profile_id") == row.profile_id and p.get("material_id") == row.material_id
        and _as_int(p.get("version")) == row.version)
    runs = db.scalar(select(func.count()).select_from(DispenseRun).where(
        or_(DispenseRun.profile_version_id == row.profile_version_id,
            _legacy_match(DispenseRun, row)))) or 0
    return {"device_commands": int(commands), "dispense_runs": int(runs)}


# --- operator-facing profile operations (list / create / activate / delete) ---

def _device_channel_report(db: Session, channel_id: str) -> dict | None:
    """Latest device status entry for a channel, if any device has reported."""
    device = db.scalar(select(DeviceStatus).order_by(DeviceStatus.updated_at.desc()).limit(1))
    if device is None or not isinstance(device.status_json, dict):
        return None
    channels = device.status_json.get("channels")
    if not isinstance(channels, list):
        return None
    for channel in channels:
        if isinstance(channel, dict) and channel.get("channel_id") == channel_id:
            return channel
    return None


def profile_out(db: Session, row: TuningProfile) -> TuningProfileOut:
    """Serialize a profile and attach device apply/sync state."""
    out = TuningProfileOut.model_validate(row, from_attributes=True)
    out.pump_id = "Pump 1" if row.channel_id == "CH1" else "Pump 2"
    report = _device_channel_report(db, row.channel_id)
    if report is None:
        out.applied_version = None
        out.sync_state = "UNKNOWN"
        return out
    raw_version = report.get("profile_version")
    try:
        out.applied_version = int(raw_version) if raw_version is not None else None
    except (TypeError, ValueError):
        out.applied_version = None
    applied_id = report.get("profile_id")
    if applied_id is not None and applied_id != row.profile_id:
        out.sync_state = "OUT-OF-SYNC"
    elif out.applied_version is None:
        out.sync_state = "UNKNOWN"
    elif out.applied_version == row.version:
        out.sync_state = "IN-SYNC"
    else:
        out.sync_state = "OUT-OF-SYNC"
    return out


def _log_status(db: Session, row: TuningProfile, old: str | None, new: str,
                actor: str | None, detail: str | None = None) -> None:
    db.add(ProfileStatusLog(profile_version_id=row.profile_version_id, ts=utcnow(),
                            from_status=old, to_status=new,
                            actor=(actor or "api")[:64], detail=(detail or None) and detail[:190]))


def _set_status(db: Session, row: TuningProfile, new: str, actor: str | None,
                detail: str | None = None) -> None:
    """The ONLY place a status changes: keeps ``active`` as its mirror, stamps
    who/when and appends to profile_status_log."""
    old = row.status
    legacy_direct = old == "draft" and new == "active" and row.profile_schema != 2
    if new not in rules.TRANSITIONS.get(old, ()) and not legacy_direct and not (
            old == new == "active" and row.profile_schema != 2):
        raise ServiceError("conflict", f"profile cannot go from {old} to {new}", 409)
    row.status = new
    row.active = new == "active"
    row.status_changed_at = utcnow()
    row.status_changed_by = (actor or "api")[:64]
    _log_status(db, row, old, new, actor, detail)


def _require_canonical_target(row: TuningProfile) -> None:
    """Reject legacy/NULL/non-canonical profile rows before they reach the device."""
    if row.target_g is None or row.target_g not in CANONICAL_TARGETS_G:
        raise ServiceError(
            "invalid",
            f"profile {row.profile_id} v{row.version} has invalid target_g="
            f"{row.target_g!r}; must be one of {CANONICAL_TARGETS_G} grams "
            "(0/NULL channel-wide fallback is not allowed)", 422)


def _queue_profile_command(db: Session, row: TuningProfile, clear: bool = False) -> None:
    """Legacy PROFILE / PROFILE_CLEAR device command (CONTRACT 4.1 "Device wire").

    Only for schema-1 canonical rows and only while LEGACY_PROFILE_COMMANDS is on
    (default ON until the firmware path that reads them is retired). A ranged
    profile is never staged this way: its job carries the pinned profile{} itself."""
    if not settings.legacy_profile_commands or row.profile_schema == 2:
        return
    if row.target_g is None or row.target_g not in CANONICAL_TARGETS_G:
        # Never send target_g 0 (the ESP-side bleed key) or a NULL fallback.
        log.error(
            "refusing to queue %s for profile %s v%s: target_g=%r is not canonical %s",
            "PROFILE_CLEAR" if clear else "PROFILE",
            row.profile_id, row.version, row.target_g, CANONICAL_TARGETS_G,
        )
        return
    device = db.scalar(select(DeviceStatus).order_by(DeviceStatus.updated_at.desc()).limit(1))
    if device is None:
        return  # The ESP32 will read the active profile before its next submitted job.
    payload = {"material_id": row.material_id, "channel_id": row.channel_id,
               "target_g": row.target_g}
    command_type = "PROFILE_CLEAR" if clear else "PROFILE"
    if not clear:
        # Device wire is unchanged: no profile_version_id is sent to firmware.
        payload.update({
            "profile_id": row.profile_id, "version": row.version,
            "kp": row.kp, "ki": row.ki, "kd": row.kd,
            "tolerance_g": row.tolerance_g,
            "max_overshoot_g": row.max_overshoot_g,
            "max_duration_ms": row.max_duration_ms,
            "window_ms": row.window_ms,
            "min_on_ms": row.min_on_ms,
            "min_off_ms": row.min_off_ms,
        })
    now = utcnow()
    db.add(DeviceCommand(device_id=device.device_id, command_type=command_type,
                         channel_id=row.channel_id, target_g=row.target_g,
                         payload_json=payload, state="PENDING",
                         created_at=now, updated_at=now))


def _check_target(material_id: str, target_g: int) -> None:
    top = settings.max_target_g(material_id)
    if not 1 <= target_g <= top:
        raise ServiceError(
            "invalid", f"target_g must be 1..{top} g for {material_id} (got {target_g})", 422)


def _candidate(row: TuningProfile) -> dict:
    lo, hi = rules.row_range(row)
    return {"profile_version_id": row.profile_version_id, "profile_id": row.profile_id,
            "version": row.version, "applicability": row.applicability,
            "target_min_g": lo, "target_max_g": hi,
            "validated_min_g": row.validated_min_g, "validated_max_g": row.validated_max_g}


def usable_error(row: TuningProfile, target_g: int) -> str | None:
    """Why ``row`` cannot run a job at ``target_g`` right now (None = it can).
    Re-checked at queue time: a stored row may predate the rules or be damaged."""
    if row.status != "active":
        return f"is {row.status}, not active"
    if not rules.contains(row, target_g):
        lo, hi = rules.row_range(row)
        return f"covers {lo}..{hi} g, not {target_g} g"
    if row.profile_schema == 2:
        return rules.consistency_error(row, target_g)
    return None


def active_rows(db: Session, material_id: str) -> list[TuningProfile]:
    stmt = select(TuningProfile).where(TuningProfile.material_id == material_id,
                                       TuningProfile.channel_id == MATERIAL_CHANNEL[material_id],
                                       TuningProfile.status == "active")
    return list(db.scalars(stmt.order_by(TuningProfile.profile_version_id)))


def compatible_active(db: Session, material_id: str, target_g: int) -> list[TuningProfile]:
    """Active rows of this pump whose range contains the target and that pass the
    consistency check at that target. Read-only."""
    return [r for r in active_rows(db, material_id) if usable_error(r, target_g) is None]


def nearest_ranges(db: Session, material_id: str, target_g: int, n: int = 3) -> list[dict]:
    """Nearest active ranges, FOR DISPLAY ONLY. Never applied to a job."""
    def gap(r):
        lo, hi = rules.row_range(r)
        return 0 if lo <= target_g <= hi else min(abs(lo - target_g), abs(target_g - hi))
    rows = [r for r in active_rows(db, material_id) if None not in rules.row_range(r)]
    return [_candidate(r) for r in sorted(rows, key=gap)[:n]]


def resolve_preview(db: Session, material_id: str, target_g: int) -> dict:
    """What queue.job.create would do for this target with NO explicit pin. Read-only."""
    _check_target(material_id, target_g)
    found = compatible_active(db, material_id, target_g)
    out = {"material_id": material_id, "target_g": target_g,
           "outcome": "ok" if len(found) == 1 else "ambiguous" if found else "none",
           "profile": profile_out(db, found[0]) if len(found) == 1 else None,
           "candidates": [_candidate(r) for r in found] if len(found) > 1 else [],
           "nearest": [] if found else nearest_ranges(db, material_id, target_g)}
    return out


def resolve_for_job(db: Session, material_id: str, target_g: int) -> TuningProfile:
    """The one active compatible profile for an unpinned job, else a typed refusal.
    NEVER falls back to the nearest or a canonical profile."""
    found = compatible_active(db, material_id, target_g)
    if len(found) == 1:
        return found[0]
    if not found:
        raise ServiceError(
            "no_compatible_profile",
            f"no active PID profile covers {target_g} g on {material_id}; nothing was queued",
            data={"nearest": nearest_ranges(db, material_id, target_g)})
    raise ServiceError(
        "ambiguous_profile",
        f"{len(found)} active PID profiles cover {target_g} g on {material_id}; "
        "pin one with profile_version_id",
        data={"candidates": [_candidate(r) for r in found]})


def coverage(db: Session, material_id: str | None = None) -> dict:
    """Per pump: the active ranges, the gaps inside 1..max and the overlaps. Read-only."""
    out = []
    for m in ([material_id] if material_id else ["M1", "M2"]):
        top = settings.max_target_g(m)
        spans = sorted(((*rules.row_range(r), r) for r in active_rows(db, m)
                        if None not in rules.row_range(r)), key=lambda s: (s[0], s[1]))
        gaps, reach = [], 0
        for lo, hi, _r in spans:
            if lo > reach + 1 and reach + 1 <= top:
                gaps.append({"from_g": reach + 1, "to_g": min(lo - 1, top)})
            reach = max(reach, hi)
        if reach < top:
            gaps.append({"from_g": reach + 1, "to_g": top})
        overlaps = []
        for i, (alo, ahi, a) in enumerate(spans):
            for blo, bhi, b in spans[i + 1:]:
                lo, hi = max(alo, blo), min(ahi, bhi)
                if lo <= hi and len(overlaps) < 100:
                    overlaps.append({"a": a.profile_version_id, "b": b.profile_version_id,
                                     "from_g": lo, "to_g": hi})
        out.append({"material_id": m, "max_target_g": top,
                    "range_count": len(spans),
                    "ranges": [_candidate(r) for _lo, _hi, r in spans][:100],   # 64 KB response cap
                    "gaps": gaps, "overlaps": overlaps})
    return {"pumps": out}


def list_profiles(db: Session, material_id: str | None = None, channel_id: str | None = None,
                  target_g: int | None = None, status: str | None = None) -> list[TuningProfileOut]:
    stmt = select(TuningProfile)
    if channel_id:
        stmt = stmt.where(TuningProfile.channel_id == channel_id)
    if material_id:
        stmt = stmt.where(TuningProfile.material_id == material_id)
    if status:
        stmt = stmt.where(TuningProfile.status == status)
    if target_g is not None:
        if target_g < 1:
            raise ServiceError("invalid", f"target_g must be a positive number of grams (got {target_g})", 422)
        low = func.coalesce(TuningProfile.target_min_g, TuningProfile.target_g)
        high = func.coalesce(TuningProfile.target_max_g, TuningProfile.target_g)
        stmt = stmt.where(low <= target_g, high >= target_g)
    else:
        # Rows with no usable target (the old NULL "channel-wide" key) stay hidden.
        stmt = stmt.where(TuningProfile.target_g.is_not(None))
    rows = db.scalars(stmt.order_by(TuningProfile.channel_id,
                                    TuningProfile.profile_id,
                                    TuningProfile.version.desc()))
    return [profile_out(db, row) for row in rows]


def active_profiles(db: Session, material_id: str, channel_id: str,
                    target_g: int) -> list[TuningProfileOut]:
    """Compatible ACTIVE profiles for a target (zero, one or several). Read-only."""
    if channel_id != MATERIAL_CHANNEL.get(material_id):
        raise ServiceError("invalid", "material_id does not match fixed channel mapping", 422)
    _check_target(material_id, target_g)
    return [profile_out(db, r) for r in compatible_active(db, material_id, target_g)]


def active_profile(db: Session, material_id: str, channel_id: str,
                   target_g: int) -> TuningProfileOut:
    """HTTP form: exactly one compatible active profile, else 404 / 409."""
    found = active_profiles(db, material_id, channel_id, target_g)
    if len(found) == 1:
        return found[0]
    if not found:
        raise ServiceError(
            "not_found",
            f"no active profile for material_id={material_id} "
            f"channel_id={channel_id} target_g={target_g}", 404)
    raise ServiceError(
        "ambiguous_profile", f"{len(found)} active profiles cover {target_g} g",
        data={"candidates": [_candidate(r) for r in compatible_active(db, material_id, target_g)]})


def _per_pump_max(db_fields: dict, material: str) -> None:
    if db_fields.get("profile_schema") == 2:
        top = settings.max_target_g(material)
        if db_fields["target_max_g"] > top:
            raise ServiceError(
                "invalid", f"target_max_g {db_fields['target_max_g']} exceeds the {material} "
                f"maximum of {top} g", 422)


def create_profile(db: Session, payload: TuningProfileIn,
                   actor: str | None = None) -> TuningProfileOut:
    # Always saved as a DRAFT (schema 1 canonical or schema 2 ranged, see schemas.RangedInput).
    # The version is max(version)+1 for THIS profile_id AND material only.
    fields = payload.model_dump(exclude={"material_id", "channel_id"})
    _per_pump_max(fields, payload.material_id)
    (row,) = create_versions(db, fields, [payload.material_id], actor=actor)
    return profile_out(db, row)


def create_profiles_bulk(db: Session, payload: TuningProfileBulkIn,
                         actor: str | None = None) -> BulkProfilesOut:
    """Save the same gains for several pumps in ONE transaction.

    Each material gets its own next version (P1 at v7 and P2 at v4 save to v8
    and v5). All rows are created or none are. Always drafts.
    """
    fields = payload.model_dump(exclude={"materials", "per_pump_validation"})
    for material in payload.materials:
        _per_pump_max(fields, material)
    extras = {m: {"validation_ref": v.validation_ref, "validated_min_g": v.validated_min_g,
                  "validated_max_g": v.validated_max_g}
              for m, v in (payload.per_pump_validation or {}).items()
              if m in payload.materials}
    rows = create_versions(db, fields, payload.materials, per_material=extras, actor=actor)
    return BulkProfilesOut(profiles=[profile_out(db, row) for row in rows])

def next_version_preview(db: Session, profile_id: str, materials: str = "M1,M2") -> dict:
    """Read-only preview computed by the same service the save uses."""
    wanted = [m.strip() for m in materials.split(",") if m.strip()]
    if not wanted or any(m not in ("M1", "M2") for m in wanted):
        raise ServiceError("invalid", "materials must be M1, M2 or M1,M2", 422)
    return {"profile_id": profile_id,
            "next_version": next_versions(db, profile_id, list(dict.fromkeys(wanted)))}


def row_by_pair(db: Session, profile_id: str, version: int,
                material_id: str | None) -> TuningProfile:
    """Legacy /{profile_id}/{version} lookup. The pair is no longer unique, so
    an ambiguous pair is refused instead of guessing which pump is meant."""
    stmt = select(TuningProfile).where(TuningProfile.profile_id == profile_id,
                                       TuningProfile.version == version)
    if material_id is not None:
        stmt = stmt.where(TuningProfile.material_id == material_id)
    rows = list(db.scalars(stmt.order_by(TuningProfile.material_id)))
    if not rows:
        raise ServiceError("not_found", "profile version not found", 404)
    if len(rows) > 1:
        raise ServiceError(
            "conflict",
            f"profile {profile_id} v{version} exists for "
            f"{' and '.join(r.material_id for r in rows)}; add "
            "?material_id=M1 or M2, or use /profile-versions/{profile_version_id}", 409)
    return rows[0]


def row_by_id(db: Session, profile_version_id: int) -> TuningProfile:
    row = db.get(TuningProfile, profile_version_id)
    if row is None:
        raise ServiceError("not_found", "profile version not found", 404)
    return row


def _activation_error(db: Session, row: TuningProfile) -> str | None:
    if row.status == "deprecated":
        return "a deprecated profile cannot be activated; save a new version"
    if row.profile_schema != 2:
        return None   # schema 1 (legacy canonical): activatable directly, as before
    if row.status == "active":
        return "already active"
    if row.status != "validated":
        return f"is {row.status}; it must be validated (admin, with a validation_ref) first"
    if not row.validation_ref:
        return "has no validation_ref"
    lo, hi = rules.row_range(row)
    if (row.validated_min_g is None or row.validated_max_g is None
            or lo < row.validated_min_g or hi > row.validated_max_g):
        return (f"declares {lo}..{hi} g but was only validated for "
                f"{row.validated_min_g}..{row.validated_max_g} g")
    if hi > settings.max_target_g(row.material_id):
        return f"range reaches {hi} g, above the {row.material_id} maximum"
    return rules.consistency_error(row, lo)


def _locked(db: Session, row: TuningProfile) -> TuningProfile:
    fresh = lock_row(db, row.profile_version_id)
    if fresh is None:
        db.rollback()
        raise ServiceError("not_found", "profile version not found", 404)
    return fresh


def activate(db: Session, row: TuningProfile, actor: str | None = None) -> dict:
    """validated -> active (schema 2) / draft|validated -> active (schema 1).

    The caller (RPC) makes this a dangerous-confirm action: the operator confirms.
    Never called by create, bulk, migration or restore."""
    row = _locked(db, row)
    why = _activation_error(db, row)
    if why:
        db.rollback()
        raise ServiceError("conflict", f"{row.profile_id} v{row.version} {why}", 409)
    if row.profile_schema != 2:
        _require_canonical_target(row)
    lo, hi = rules.row_range(row)
    twins = [r for r in active_rows(db, row.material_id)
             if r.profile_version_id != row.profile_version_id and rules.row_range(r) == (lo, hi)]
    if any(r.profile_schema == 2 for r in twins) or (row.profile_schema == 2 and twins):
        db.rollback()
        raise ServiceError(
            "conflict", f"another active profile already covers exactly {lo}..{hi} g on "
            f"{row.material_id}; deprecate it first", 409)
    for other in twins:   # schema-1 same-target rule, as before: the newest activation wins
        _set_status(db, other, "validated", actor, f"superseded by {row.profile_version_id}")
    _set_status(db, row, "active", actor)
    _queue_profile_command(db, row)
    db.commit()
    out = profile_out(db, row)
    return {"profile_version_id": row.profile_version_id,
            "material_id": row.material_id, "channel_id": row.channel_id, "profile_id": row.profile_id,
            "version": row.version, "active": True, "status": row.status,
            "applied_version": out.applied_version, "sync_state": out.sync_state,
            "apply_when": "channel_idle_between_jobs"}


def deactivate(db: Session, row: TuningProfile, actor: str | None = None) -> dict:
    """active -> validated (stops new jobs using it; reversible)."""
    if row.profile_schema != 2:
        _require_canonical_target(row)
    row = _locked(db, row)
    if row.status == "active":
        _set_status(db, row, "validated", actor, "deactivated")
        _queue_profile_command(db, row, clear=True)
    db.commit()
    out = profile_out(db, row)
    return {"profile_version_id": row.profile_version_id,
            "material_id": row.material_id, "channel_id": row.channel_id, "profile_id": row.profile_id,
            "version": row.version, "active": False, "status": row.status,
            "applied_version": out.applied_version, "sync_state": out.sync_state}


def validate(db: Session, row: TuningProfile, *, validation_ref: str, validated_min_g: int,
             validated_max_g: int, actor: str | None = None) -> TuningProfileOut:
    """draft -> validated. The caller (RPC) requires the admin role. Records the
    evidence reference and the range it actually covers."""
    row = _locked(db, row)
    if row.status != "draft":
        db.rollback()
        raise ServiceError("conflict", f"only a draft can be validated (this one is {row.status})", 409)
    if not (validation_ref or "").strip():
        db.rollback()
        raise ServiceError("invalid", "validation_ref is required", 422)
    top = settings.max_target_g(row.material_id)
    if not 1 <= validated_min_g <= validated_max_g <= top:
        db.rollback()
        raise ServiceError("invalid", f"validated range must be 1 <= min <= max <= {top} g", 422)
    if row.profile_schema == 2:
        why = rules.consistency_error(row, rules.row_range(row)[0])
        if why:
            db.rollback()
            raise ServiceError("invalid", why, 422)
    row.validation_ref = validation_ref.strip()[:190]
    row.validated_min_g, row.validated_max_g = validated_min_g, validated_max_g
    _set_status(db, row, "validated", actor, f"validated {validated_min_g}..{validated_max_g} g")
    db.commit()
    return profile_out(db, row)


def deprecate(db: Session, row: TuningProfile, actor: str | None = None) -> dict:
    """draft|validated|active -> deprecated (terminal). Queued jobs keep their pin."""
    row = _locked(db, row)
    was_active = row.status == "active"
    if row.status == "deprecated":
        db.rollback()
        raise ServiceError("conflict", "already deprecated", 409)
    _set_status(db, row, "deprecated", actor)
    if was_active:
        _queue_profile_command(db, row, clear=True)
    db.commit()
    return {"profile_version_id": row.profile_version_id, "material_id": row.material_id,
            "profile_id": row.profile_id, "version": row.version, "active": False,
            "status": row.status}

def delete(db: Session, row: TuningProfile) -> dict:
    """Remove one immutable profile version nobody has used.

    409 when the version is active (deactivate it first), and 409 when any queue
    command (any state) or run references this exact row: history is linked by
    profile_version_id (or by profile_id + material + version for legacy rows),
    so deleting it would let the number be reused and silently re-point history.
    """
    # Lock the row first, THEN read active/references: a job that pins this row
    # holds the same lock until it commits, so its reference is visible here
    # (rollback first so no earlier snapshot hides it). Never delete blind.
    pvid = row.profile_version_id
    db.rollback()
    row = lock_row(db, pvid)
    if row is None:
        raise ServiceError("not_found", "profile version not found", 404)
    if row.active:
        db.rollback()
        raise ServiceError("conflict", "deactivate this version before deleting it", 409)
    refs = reference_counts(db, row)
    if refs["device_commands"] or refs["dispense_runs"]:
        db.rollback()
        raise ServiceError(
            "conflict",
            f"{row.profile_id} v{row.version} ({row.material_id}) is referenced by "
            f"{refs['device_commands']} queue job(s) and {refs['dispense_runs']} run(s); "
            "a used version cannot be deleted so history keeps pointing at the exact "
            "settings it ran with", 409)
    db.delete(row)
    db.commit()
    return {"ok": True}
