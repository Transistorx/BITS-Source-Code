"""Read-side run history, dashboard, live monitor and run deletion.

Transport-neutral: (db session, validated args) in, pydantic model / dict out,
ServiceError on refusal. Profile list/create/activate logic is in profiles.py.
"""

from datetime import datetime, timezone

from sqlalchemy import delete as sql_delete, func, select
from sqlalchemy.orm import Session

from ..models import DispenseEvent, DispenseRun, RunBatch, ServerMeta, WeightSample
from ..schemas import (
    DashboardOut,
    EventsOut,
    LiveChannelOut,
    LiveOut,
    RunCard,
    RunDetail,
    RunSearchOut,
    SamplesOut,
    TargetGroup,
)
from .analytics import (
    PREFERRED_TARGETS,
    build_dashboard,
    build_run_csv,
    fetch_samples,
    to_card,
    to_detail,
    to_event_out,
    to_sample_out,
    utcnow,
)
from .errors import ServiceError

TERMINAL = ("COMPLETE", "FAILED", "CANCELLED")


def _utc_naive(value: datetime | None) -> datetime | None:
    if value is None:
        return None
    if value.tzinfo is None:
        return value
    return value.astimezone(timezone.utc).replace(tzinfo=None)


def run_or_404(db: Session, run_id: str) -> DispenseRun:
    run = db.get(DispenseRun, run_id)
    if run is None:
        raise ServiceError("not_found", f"unknown run_id: {run_id}", 404)
    return run


def targets(db: Session) -> list[TargetGroup]:
    values = db.execute(
        select(DispenseRun.material_id, DispenseRun.channel_id, DispenseRun.target_g,
               func.count(DispenseRun.run_id))
        .group_by(DispenseRun.material_id, DispenseRun.channel_id, DispenseRun.target_g)
    ).all()
    ordered = sorted(values, key=lambda row: (
        row[0], PREFERRED_TARGETS.index(row[2]) if row[2] in PREFERRED_TARGETS else len(PREFERRED_TARGETS),
        row[2],
    ))
    return [TargetGroup(material_id=material, channel_id=ch, target_g=g, run_count=n, runs=[])
            for material, ch, g, n in ordered]


def dashboard(db: Session) -> DashboardOut:
    return build_dashboard(db, per_target=10)


def latest_runs(db: Session, material_id: str | None = None, channel_id: str | None = None,
                target_g: int | None = None, limit: int = 10) -> list[RunCard]:
    stmt = select(DispenseRun)
    if material_id is not None:
        stmt = stmt.where(DispenseRun.material_id == material_id)
    if channel_id is not None:
        stmt = stmt.where(DispenseRun.channel_id == channel_id)
    if target_g is not None:
        stmt = stmt.where(DispenseRun.target_g == target_g)
    runs = db.scalars(
        stmt.order_by(DispenseRun.started_at.desc(), DispenseRun.run_id.desc()).limit(limit)
    )
    return [to_card(run) for run in runs]


def search_runs(db: Session, *, material_id: str | None = None, channel_id: str | None = None,
                target_g: int | None = None, from_time: datetime | None = None,
                to_time: datetime | None = None, run_id: str | None = None,
                status: str | None = None, result: str | None = None,
                profile_id: str | None = None, profile_version: int | None = None,
                profile_version_id: int | None = None, limit: int = 50,
                offset: int = 0) -> RunSearchOut:
    start, end = _utc_naive(from_time), _utc_naive(to_time)
    if start and end and start > end:
        raise ServiceError("invalid", "from must be earlier than or equal to to", 422)
    selected_status = status or result
    if selected_status and selected_status.upper() not in ("RUNNING", *TERMINAL):
        raise ServiceError("invalid", "invalid result/status", 422)

    filters = []
    if material_id is not None:
        filters.append(DispenseRun.material_id == material_id)
    if channel_id is not None:
        filters.append(DispenseRun.channel_id == channel_id)
    if target_g is not None:
        filters.append(DispenseRun.target_g == target_g)
    if start is not None:
        filters.append(DispenseRun.started_at >= start)
    if end is not None:
        filters.append(DispenseRun.started_at <= end)
    if run_id is not None:
        filters.append(DispenseRun.run_id == run_id)
    if profile_id is not None:
        filters.append(DispenseRun.profile_id == profile_id)
    if profile_version is not None:
        filters.append(DispenseRun.profile_version == profile_version)
    if profile_version_id is not None:
        filters.append(DispenseRun.profile_version_id == profile_version_id)
    if selected_status:
        filters.append(DispenseRun.status == selected_status.upper())

    total = db.scalar(select(func.count()).select_from(DispenseRun).where(*filters)) or 0
    rows = db.scalars(
        select(DispenseRun).where(*filters)
        .order_by(DispenseRun.started_at.desc(), DispenseRun.run_id.desc())
        .offset(offset).limit(limit)
    )
    return RunSearchOut(items=[to_card(r) for r in rows], total=total,
                        limit=limit, offset=offset)


def run_samples(db: Session, run_id: str, max_points: int = 0) -> SamplesOut:
    run_or_404(db, run_id)
    all_count = db.scalar(
        select(func.count()).select_from(WeightSample).where(WeightSample.run_id == run_id)
    ) or 0
    rows, decimated = fetch_samples(db, run_id, max_points=max_points)
    return SamplesOut(run_id=run_id, count=all_count, decimated=decimated,
                      samples=[to_sample_out(row) for row in rows])


def run_events(db: Session, run_id: str) -> EventsOut:
    run_or_404(db, run_id)
    rows = db.scalars(
        select(DispenseEvent).where(DispenseEvent.run_id == run_id)
        .order_by(DispenseEvent.timestamp, DispenseEvent.idx)
    )
    return EventsOut(run_id=run_id, events=[to_event_out(row) for row in rows])


def run_full(db: Session, run_id: str) -> dict:
    run = run_or_404(db, run_id)
    samples = db.scalars(
        select(WeightSample).where(WeightSample.run_id == run_id)
        .order_by(WeightSample.elapsed_ms, WeightSample.idx)
    )
    events = db.scalars(
        select(DispenseEvent).where(DispenseEvent.run_id == run_id)
        .order_by(DispenseEvent.timestamp, DispenseEvent.idx)
    )
    sample_out = [to_sample_out(row) for row in samples]
    event_out = [to_event_out(row) for row in events]
    return {"run": to_detail(run), "samples": sample_out, "events": event_out}


def run_csv(db: Session, run_id: str) -> dict:
    """CSV text plus a filesystem-safe filename; the transport frames it."""
    run = run_or_404(db, run_id)
    content = build_run_csv(db, run)
    safe_id = "".join(ch for ch in run_id if ch.isalnum() or ch in "._-")
    return {"content": content, "filename": f"run-{safe_id}.csv"}


def run_detail(db: Session, run_id: str) -> RunDetail:
    return to_detail(run_or_404(db, run_id))


def delete_run(db: Session, run_id: str) -> dict:
    """Remove one stored run and its own telemetry, nothing else.

    The authoritative identity is ``dispense_runs.run_id`` - the primary key -
    never the displayed ``test_number``, which is a per-target counter and
    repeats across materials and targets. Children are matched on that same
    ``run_id``: only this run's ``weight_samples`` and ``dispense_events`` go
    with it. ``tuning_profiles`` and ``materials`` are referenced by column,
    not foreign key, and are never touched.

    Children are deleted before their parent and everything commits in a single
    transaction, so a failure cannot strand orphaned telemetry or leave a run
    half-removed. 404 when the run does not exist.
    """
    run = run_or_404(db, run_id)
    try:
        samples = db.execute(
            sql_delete(WeightSample)
            .where(WeightSample.run_id == run_id)
            .execution_options(synchronize_session=False)
        ).rowcount or 0
        events = db.execute(
            sql_delete(DispenseEvent)
            .where(DispenseEvent.run_id == run_id)
            .execution_options(synchronize_session=False)
        ).rowcount or 0
        db.execute(sql_delete(RunBatch).where(RunBatch.run_id == run_id)
                   .execution_options(synchronize_session=False))
        db.delete(run)
        db.commit()
    except Exception:
        db.rollback()
        raise
    return {
        "ok": True,
        "run_id": run_id,
        "deleted": {"weight_samples": samples, "dispense_events": events},
    }


def read_live(db: Session) -> LiveOut:
    active_rows = list(db.scalars(
        select(DispenseRun).where(DispenseRun.status == "RUNNING")
        .order_by(DispenseRun.started_at.desc())
    ))
    active_by_channel = {run.channel_id: run for run in active_rows}
    meta = db.get(ServerMeta, "last_telemetry_at")
    channels = []
    for channel_id in ("CH1", "CH2"):
        active = active_by_channel.get(channel_id)
        rows = []
        if active is not None:
            rows = list(db.scalars(
                select(WeightSample).where(WeightSample.run_id == active.run_id)
                .order_by(WeightSample.elapsed_ms.desc(), WeightSample.idx.desc()).limit(300)
            ))
            rows.reverse()
        samples = [to_sample_out(row) for row in rows]
        channels.append(LiveChannelOut(
            channel_id=channel_id, active_run=to_card(active) if active else None,
            is_live=active is not None, samples=samples,
            last_sample=samples[-1] if samples else None,
        ))
    active = active_by_channel.get("CH1") or (active_rows[0] if active_rows else None)
    ch1 = channels[0]
    return LiveOut(
        server_time=utcnow(), database="up",
        last_telemetry_at=datetime.fromisoformat(meta.value) if meta else None,
        active_run=to_card(active) if active else None,
        is_live=bool(active_rows),
        samples=ch1.samples,
        last_sample=ch1.last_sample,
        channels=channels,
    )
