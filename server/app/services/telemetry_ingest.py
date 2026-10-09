"""Telemetry write path (ESP32 -> server). Contract section 3.

Transport-neutral: (db session, validated payload) in, pydantic model out,
ServiceError on refusal. Every write is a single transaction: all rows of a
batch commit together or none do, retried batches are idempotent via the
(run_id, idx) unique keys, and nothing here ever deletes or mutates stored
history - a run's samples are append-only and completion only fills the run's
own result columns.
"""

import math
from datetime import datetime, timedelta

from sqlalchemy import func, select
from sqlalchemy.exc import IntegrityError, SQLAlchemyError
from sqlalchemy.orm import Session

from ..models import DispenseEvent, DispenseRun, RunBatch, ServerMeta, WeightSample
from ..schemas import (
    BatchOut,
    EventsIn,
    RunCompleteIn,
    RunDetail,
    RunStartIn,
    RunStartOut,
    TelemetryBatchIn,
)
from . import profiles as profile_service
from .analytics import next_test_number, to_detail, utcnow
from .errors import ServiceError


DROPPED_KIND = "dropped"  # run_batches.kind of a ring-overflow tombstone (CONTRACT 9.2)


def _touch_telemetry(db: Session) -> None:
    stamp = utcnow().isoformat(timespec="milliseconds")
    row = db.get(ServerMeta, "last_telemetry_at")
    if row is None:
        db.add(ServerMeta(key="last_telemetry_at", value=stamp))
    else:
        row.value = stamp


def _get_run_or_409(db: Session, run_id: str) -> DispenseRun:
    run = db.get(DispenseRun, run_id)
    if run is None:
        # The device must (re)send runs/start first - it keeps buffering and
        # retries, so nothing is lost by refusing early.
        raise ServiceError("conflict", f"unknown run_id: {run_id}", 409)
    return run


def _db_unavailable(exc: SQLAlchemyError) -> ServiceError:
    return ServiceError("unavailable", f"database unavailable: {exc.__class__.__name__}", 503)


_CFG_FIELDS = ("kp", "ki", "kd", "tolerance_g", "max_overshoot_g", "window_ms",
               "min_on_ms", "min_off_ms", "max_duration_ms")


def _row_matches_run(row, payload, started_at: datetime) -> bool:
    """True only if the resolved profile row is provably the one this run used."""
    if not profile_service.rules.contains(row, payload.target_g):
        return False
    if row.created_at is None or row.created_at > started_at:
        return False
    cfg = payload.config
    if cfg is None:
        return True
    for name in _CFG_FIELDS:
        reported = getattr(cfg, name, None)
        if reported is None:
            continue
        stored = getattr(row, name)
        if stored is None or not math.isclose(float(stored), float(reported),
                                              rel_tol=1e-5, abs_tol=1e-9):
            return False
    return True


def _start_out(run: DispenseRun) -> RunStartOut:
    return RunStartOut(run_id=run.run_id, test_number=run.test_number,
                       status=run.status, assigned_at=run.assigned_at,
                       started_at=run.started_at)


def record_batch(db: Session, run_id: str, batch_seq: int, kind: str, count: int) -> None:
    """MQTT only (CONTRACT 9.2): remember (run_id, batch_seq) in the caller's
    transaction. No-op if it is already stored (a replay). A real batch replaces a
    tombstone of the same number; a tombstone never replaces a real batch."""
    row = db.get(RunBatch, (run_id, batch_seq))
    if row is None:
        db.add(RunBatch(run_id=run_id, batch_seq=batch_seq, kind=kind,
                        item_count=count, received_at=utcnow()))
    elif row.kind == DROPPED_KIND and kind != DROPPED_KIND:
        row.kind, row.item_count = kind, count


def batch_seqs(db: Session, run_id: str) -> set[int]:
    return {r[0] for r in db.execute(
        select(RunBatch.batch_seq).where(RunBatch.run_id == run_id)).all()}


def watermark(seqs: set[int]) -> int:
    """Highest n such that every batch_seq 0..n is stored; -1 if not even the start."""
    n = -1
    while (n + 1) in seqs:
        n += 1
    return n


def _ranges(missing: list[int]) -> list[list[int]]:
    out: list[list[int]] = []
    for value in sorted(missing):
        if out and value == out[-1][1] + 1:
            out[-1][1] = value
        else:
            out.append([value, value])
    return out


def _idx_gaps(db: Session, model, run_id: str, expected_last: int | None) -> list[list[int]]:
    """Missing idx ranges within 1..max(expected_last, highest stored idx)."""
    stored = {r[0] for r in db.execute(select(model.idx).where(model.run_id == run_id)).all()}
    last = max([expected_last or 0, *stored]) if (stored or expected_last) else 0
    return _ranges([i for i in range(1, last + 1) if i not in stored])


def compute_gaps(db: Session, run_id: str, meta: dict) -> dict:
    """CONTRACT 9.2: gaps against the relay's end_seq / total_samples / total_events.
    Empty dict = nothing missing."""
    gaps: dict = {}
    end_seq = meta.get("end_seq")
    if isinstance(end_seq, int) and end_seq >= 0:
        have = batch_seqs(db, run_id)
        tombs = {r[0] for r in db.execute(select(RunBatch.batch_seq).where(
            RunBatch.run_id == run_id, RunBatch.kind == DROPPED_KIND)).all()}
        missing = [s for s in range(0, end_seq + 1) if s not in have or s in tombs]
        # tombstones beyond end_seq are impossible for a sane relay; ignore them there
        if missing:
            gaps["batch_seq"] = _ranges(missing)
    samples = _idx_gaps(db, WeightSample, run_id, meta.get("total_samples") if
                        isinstance(meta.get("total_samples"), int) else None)
    if samples:
        gaps["sample_idx"] = samples
    events = _idx_gaps(db, DispenseEvent, run_id, meta.get("total_events") if
                       isinstance(meta.get("total_events"), int) else None)
    if events:
        gaps["event_idx"] = events
    return gaps


def interrupt_run(db: Session, run: DispenseRun, reason: str = "DEVICE_REBOOTED") -> bool:
    """RUNNING -> INTERRUPTED (never resumed). True if it changed. Commits."""
    if run.status != "RUNNING":
        return False
    run.status, run.completed_at = "INTERRUPTED", utcnow()
    run.error_text = reason[:120]
    if run.started_at is not None and run.duration_ms is None:
        run.duration_ms = int((run.completed_at - run.started_at).total_seconds() * 1000)
    db.commit()
    return True


def reevaluate_partial(db: Session, run: DispenseRun) -> None:
    """A late batch may close the gaps of a COMPLETE_PARTIAL run. Caller commits."""
    if run.status != "COMPLETE_PARTIAL" or not run.ingest_json:
        return
    gaps = compute_gaps(db, run.run_id, run.ingest_json)
    run.missing_ranges = gaps or None
    if not gaps:
        run.status = "COMPLETE"


def start_run(db: Session, payload: RunStartIn, *, boot_id: str | None = None,
              mqtt: bool = False) -> RunStartOut:
    """Register a physical dispensing attempt. Idempotent: a retry after a
    lost response returns the already-stored run instead of erroring.
    ``mqtt`` also stores batch_seq 0 in the same transaction."""
    try:
        existing = db.get(DispenseRun, payload.run_id)
        if existing is not None:
            if mqtt and db.get(RunBatch, (payload.run_id, 0)) is None:
                record_batch(db, payload.run_id, 0, "start", 1)
                db.commit()
            return _start_out(existing)

        cfg = payload.config
        # The device echoes only profile_id + version. Both pumps may own the
        # same pair, so resolve the exact row with THIS run's material; if it
        # cannot be resolved the id stays NULL and the snapshot columns remain.
        # Link only when the row also matches what the run reports (target, gains,
        # and it existed before the run started); else NULL + snapshot columns.
        started_at = utcnow()
        profile_row = (profile_service.resolve_version(
            db, payload.profile_id, payload.material_id, payload.profile_version)
            if payload.profile_id and payload.profile_version else None)
        if profile_row is not None and not _row_matches_run(profile_row, payload, started_at):
            profile_row = None
        run = DispenseRun(
            run_id=payload.run_id,
            material_id=payload.material_id,
            channel_id=payload.channel_id,
            pump_id="Pump 1" if payload.material_id == "M1" else "Pump 2",
            relay_id=payload.relay_id or ("Relay 1" if payload.channel_id == "CH1" else "Relay 2"),
            scale_id=payload.scale_id or ("Scale 1" if payload.channel_id == "CH1" else "Scale 2"),
            device_id=payload.device_id or "",
            job_id=payload.job_id,
            test_number=next_test_number(db, payload.target_g),
            target_g=payload.target_g,
            priority=payload.priority,
            status="RUNNING",
            assigned_at=utcnow(),
            started_at=started_at,
            start_weight_g=payload.start_weight_g,
            firmware=payload.firmware,
            profile_id=payload.profile_id,
            profile_version=payload.profile_version,
            profile_version_id=profile_row.profile_version_id if profile_row else None,
            config_snapshot=(cfg.model_dump(exclude_none=True) if cfg else None),
            created_at=utcnow(),
        )
        if cfg is not None:
            run.kp, run.ki, run.kd = cfg.kp, cfg.ki, cfg.kd
            run.integral_max = cfg.integral_max
            run.tolerance_g = cfg.tolerance_g
            run.coarse_transition_g = cfg.coarse_transition_g
            run.max_overshoot_g = cfg.max_overshoot_g
            run.settle_ms = cfg.settle_ms
            run.max_duration_ms = cfg.max_duration_ms
            run.window_ms = cfg.window_ms
            run.min_on_ms = cfg.min_on_ms
            run.min_off_ms = cfg.min_off_ms
            run.correction_limit = cfg.correction_limit
            run.completion_mode = cfg.completion_mode
        if mqtt:
            run.boot_id = boot_id
            record_batch(db, payload.run_id, 0, "start", 1)
        db.add(run)
        _touch_telemetry(db)
        db.commit()
    except IntegrityError:
        # Concurrent duplicate start (same run_id): the winner's row stands.
        db.rollback()
        existing = db.get(DispenseRun, payload.run_id)
        if existing is None:
            raise ServiceError("internal", "run start failed", 500)
        return _start_out(existing)
    except SQLAlchemyError as exc:
        db.rollback()
        raise _db_unavailable(exc)

    return _start_out(run)


def _existing_idx(db: Session, model, run_id: str, idxs: list[int]) -> set[int]:
    if not idxs:
        return set()
    rows = db.execute(
        select(model.idx).where(model.run_id == run_id, model.idx.in_(idxs))
    ).all()
    return {r[0] for r in rows}


def _finish(db: Session, commit: bool) -> None:
    """commit=False (MQTT writer batching) only flushes; the caller commits once and
    falls back to per-item commits if that fails."""
    if commit:
        db.commit()
    else:
        db.flush()


def ingest_batch(db: Session, payload: TelemetryBatchIn, *, batch_seq: int | None = None,
                 commit: bool = True) -> BatchOut:
    """Efficient batch ingestion of real CAS-derived samples. ``batch_seq`` (MQTT)
    stores (run_id, batch_seq) in the same transaction as the samples."""
    run = _get_run_or_409(db, payload.run_id)
    if run.channel_id != payload.channel_id:
        raise ServiceError("conflict", "channel_id does not match run", 409)
    if run.material_id != payload.material_id:
        raise ServiceError("conflict", "material_id does not match run", 409)
    try:
        idxs = [s.idx for s in payload.samples]
        seen = _existing_idx(db, WeightSample, run.run_id, idxs)

        inserted = 0
        batch_max: int | None = None
        base: datetime = run.started_at
        for s in payload.samples:
            if s.idx in seen:
                continue
            row = WeightSample(
                run_id=run.run_id,
                channel_id=run.channel_id,
                material_id=run.material_id,
                idx=s.idx,
                timestamp=base + timedelta(milliseconds=s.elapsed_ms),
                elapsed_ms=s.elapsed_ms,
                uptime_ms=s.uptime_ms,
                seq=s.seq,
                weight_g=s.weight_g,
                target_g=s.target_g,
                error_g=(s.error_g if s.error_g is not None
                         else s.target_g - s.weight_g),
                stable=s.stable,
                weight_age_ms=s.weight_age_ms,
                p_term=s.p_term, i_term=s.i_term, d_term=s.d_term,
                pid_output=s.pid_output,
                relay1=s.relay1, relay2=s.relay2,
                state=s.state or "",
            )
            db.add(row)
            inserted += 1
            if batch_max is None or s.weight_g > batch_max:
                batch_max = s.weight_g

        if inserted:
            run.sample_count = (run.sample_count or 0) + inserted
            if batch_max is not None and (run.max_weight_g is None
                                          or batch_max > run.max_weight_g):
                run.max_weight_g = batch_max
        if batch_seq is not None:
            record_batch(db, run.run_id, batch_seq, "samples", len(payload.samples))
        _touch_telemetry(db)
        _finish(db, commit)
    except IntegrityError:
        if not commit:
            raise
        # A concurrent retry raced us; the unique key kept the data honest.
        db.rollback()
        return BatchOut(run_id=run.run_id, accepted=len(payload.samples),
                        inserted=0, duplicates=len(payload.samples))
    except SQLAlchemyError as exc:
        if commit:
            db.rollback()
        raise _db_unavailable(exc)

    duplicates = len(payload.samples) - inserted
    return BatchOut(run_id=run.run_id, accepted=len(payload.samples),
                    inserted=inserted, duplicates=duplicates)


def ingest_events(db: Session, payload: EventsIn, *, batch_seq: int | None = None,
                  commit: bool = True) -> BatchOut:
    run = _get_run_or_409(db, payload.run_id)
    if run.channel_id != payload.channel_id:
        raise ServiceError("conflict", "channel_id does not match run", 409)
    if run.material_id != payload.material_id:
        raise ServiceError("conflict", "material_id does not match run", 409)
    try:
        idxs = [e.idx for e in payload.events]
        seen = _existing_idx(db, DispenseEvent, run.run_id, idxs)

        inserted = 0
        base: datetime = run.started_at
        for e in payload.events:
            if e.idx in seen:
                continue
            db.add(DispenseEvent(
                run_id=run.run_id,
                material_id=run.material_id,
                idx=e.idx,
                timestamp=(base + timedelta(milliseconds=e.elapsed_ms)
                           if e.elapsed_ms is not None else utcnow()),
                elapsed_ms=e.elapsed_ms,
                event=e.event.value,
                state=e.state,
                weight_g=e.weight_g,
                detail=e.detail,
            ))
            inserted += 1

        if inserted:
            run.event_count = (run.event_count or 0) + inserted
        if batch_seq is not None:
            record_batch(db, run.run_id, batch_seq, "events", len(payload.events))
        _touch_telemetry(db)
        _finish(db, commit)
    except IntegrityError:
        if not commit:
            raise
        db.rollback()
        return BatchOut(run_id=run.run_id, accepted=len(payload.events),
                        inserted=0, duplicates=len(payload.events))
    except SQLAlchemyError as exc:
        if commit:
            db.rollback()
        raise _db_unavailable(exc)

    return BatchOut(run_id=run.run_id, accepted=len(payload.events),
                    inserted=inserted,
                    duplicates=len(payload.events) - inserted)


def complete_run(db: Session, run_id: str, payload: RunCompleteIn, *,
                 meta: dict | None = None, gaps: dict | None = None) -> RunDetail:
    """Mark a run terminal. Idempotent: re-completing returns the stored
    run unchanged (the first completion is the permanent record).
    MQTT passes ``meta`` (end_seq/total_*) and ``gaps``: a COMPLETE with gaps is
    stored as COMPLETE_PARTIAL in the same commit."""
    run = db.get(DispenseRun, run_id)
    if run is None:
        raise ServiceError("not_found", f"unknown run_id: {run_id}", 404)

    try:
        if run.status == "RUNNING":
            run.status = payload.status
            run.completed_at = utcnow()

            # Duration: client value wins; otherwise derive from timestamps.
            if payload.duration_ms is not None:
                run.duration_ms = payload.duration_ms
            else:
                run.duration_ms = int(
                    (run.completed_at - run.started_at).total_seconds() * 1000
                )

            # Max weight: client value merged with the stored sample max.
            sample_max = db.scalar(
                select(func.max(WeightSample.weight_g))
                .where(WeightSample.run_id == run_id)
            )
            candidates = [w for w in (payload.max_weight_g, sample_max,
                                      run.max_weight_g) if w is not None]
            run.max_weight_g = max(candidates) if candidates else None

            if payload.final_weight_g is not None:
                run.final_weight_g = payload.final_weight_g
            elif sample_max is not None and run.final_weight_g is None:
                # No client value: the last stored reading is the honest one.
                last = db.scalar(
                    select(WeightSample.weight_g)
                    .where(WeightSample.run_id == run_id)
                    .order_by(WeightSample.elapsed_ms.desc(),
                              WeightSample.idx.desc())
                    .limit(1)
                )
                run.final_weight_g = last

            if run.final_weight_g is not None:
                run.final_error_g = run.final_weight_g - run.target_g

            if payload.overshoot_g is not None:
                run.overshoot_g = payload.overshoot_g
            elif run.max_weight_g is not None:
                run.overshoot_g = max(0, run.max_weight_g - run.target_g)

            if payload.corrections is not None:
                run.corrections = payload.corrections
            if payload.error is not None:
                run.error_text = payload.error[:120]

            if meta is not None:
                run.ingest_json = meta
                run.missing_ranges = gaps or None
                if gaps and payload.status == "COMPLETE":
                    run.status = "COMPLETE_PARTIAL"

            _touch_telemetry(db)
            db.commit()
    except SQLAlchemyError as exc:
        db.rollback()
        raise _db_unavailable(exc)

    return to_detail(run)
