"""Query/serialisation helpers and optional view decimation.

All raw samples stay in MySQL forever; decimation here is a READ-side view
optimisation only (contract §4, max_points).
"""

import csv
import io
from datetime import datetime, timezone

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from ..models import DispenseEvent, DispenseRun, WeightSample
from ..schemas import (
    DashboardOut,
    EventOut,
    RunCard,
    RunDetail,
    SampleOut,
    TargetGroup,
    TargetSummary,
)

# Dashboard organisation: the four common bench targets first, in order,
# then every other discovered target ascending. Nothing is hardcoded about
# WHICH targets exist — this only orders what the database reports.
PREFERRED_TARGETS = (5000, 10000, 15000, 20000)
TERMINAL_STATUSES = ("COMPLETE", "FAILED", "CANCELLED")


def utcnow() -> datetime:
    return datetime.now(timezone.utc).replace(tzinfo=None)


def to_card(run: DispenseRun) -> RunCard:
    pct = None
    if run.final_error_g is not None and run.target_g:
        pct = round(100.0 * run.final_error_g / run.target_g, 4)
    return RunCard(
        run_id=run.run_id,
        material_id=run.material_id,
        pump_id=run.pump_id,
        channel_id=run.channel_id,
        relay_id=run.relay_id,
        scale_id=run.scale_id,
        test_number=run.test_number,
        device_id=run.device_id or "",
        job_id=run.job_id,
        target_g=run.target_g,
        priority=run.priority,
        status=run.status,
        assigned_at=run.assigned_at,
        started_at=run.started_at,
        completed_at=run.completed_at,
        duration_ms=run.duration_ms,
        kp=run.kp, ki=run.ki, kd=run.kd,
        profile_id=run.profile_id, profile_version=run.profile_version,
        profile_version_id=run.profile_version_id,
        start_weight_g=run.start_weight_g,
        final_weight_g=run.final_weight_g,
        final_error_g=run.final_error_g,
        final_error_pct=pct,
        overshoot_g=run.overshoot_g,
        max_weight_g=run.max_weight_g,
        corrections=run.corrections,
        error_text=run.error_text,
        sample_count=run.sample_count or 0,
        event_count=run.event_count or 0,
    )


def to_detail(run: DispenseRun) -> RunDetail:
    card = to_card(run)
    extra = RunDetail(
        **card.model_dump(),
        integral_max=run.integral_max,
        tolerance_g=run.tolerance_g,
        coarse_transition_g=run.coarse_transition_g,
        max_overshoot_g=run.max_overshoot_g,
        settle_ms=run.settle_ms,
        max_duration_ms=run.max_duration_ms,
        window_ms=run.window_ms,
        min_on_ms=run.min_on_ms,
        min_off_ms=run.min_off_ms,
        correction_limit=run.correction_limit,
        completion_mode=run.completion_mode,
        firmware=run.firmware,
        created_at=run.created_at,
        config_snapshot=run.config_snapshot,
    )
    return extra


def to_sample_out(s: WeightSample) -> SampleOut:
    return SampleOut(
        idx=s.idx, material_id=s.material_id, channel_id=s.channel_id, timestamp=s.timestamp, elapsed_ms=s.elapsed_ms,
        uptime_ms=s.uptime_ms, seq=s.seq, weight_g=s.weight_g,
        source=s.source or "CAS",
        target_g=s.target_g, error_g=s.error_g, stable=bool(s.stable),
        weight_age_ms=s.weight_age_ms, p_term=s.p_term, i_term=s.i_term,
        d_term=s.d_term, pid_output=s.pid_output,
        relay1=bool(s.relay1), relay2=bool(s.relay2), state=s.state or "",
    )


def to_event_out(e: DispenseEvent) -> EventOut:
    return EventOut(
        idx=e.idx, material_id=e.material_id, timestamp=e.timestamp, elapsed_ms=e.elapsed_ms,
        event=e.event, state=e.state, weight_g=e.weight_g, detail=e.detail,
    )


def fetch_samples(db: Session, run_id: str,
                  max_points: int = 0) -> tuple[list[WeightSample], bool]:
    """All samples for a run ascending by (elapsed_ms, idx); optionally
    LTTB-decimated for the view. Returns (rows, decimated)."""
    rows = list(db.scalars(
        select(WeightSample)
        .where(WeightSample.run_id == run_id)
        .order_by(WeightSample.elapsed_ms, WeightSample.idx)
    ))
    if max_points and max_points > 0 and len(rows) > max_points:
        return lttb(rows, max_points), True
    return rows, False


def lttb(rows: list[WeightSample], threshold: int) -> list[WeightSample]:
    """Largest-Triangle-Three-Buckets downsampling on (elapsed_ms, weight_g).

    Keeps first/last points and the visually significant peaks — good for
    weight curves, and every returned row is a REAL stored sample (never
    interpolated, never averaged into existence).
    """
    n = len(rows)
    if threshold >= n or threshold < 3:
        return rows

    sampled: list[WeightSample] = [rows[0]]
    bucket_size = (n - 2) / (threshold - 2)

    a = 0  # index of the previously selected point
    for i in range(1, threshold - 1):
        # Average point of the NEXT bucket (triangle's third vertex).
        start = int((i) * bucket_size) + 1
        end = int((i + 1) * bucket_size) + 1
        end = min(end, n)
        avg_x = avg_y = 0.0
        cnt = end - start
        if cnt <= 0:
            cnt = 1
            start, end = start, start + 1
        for j in range(start, min(end, n)):
            avg_x += rows[j].elapsed_ms
            avg_y += rows[j].weight_g
        avg_x /= cnt
        avg_y /= cnt

        # Largest triangle in the CURRENT bucket.
        b_start = int((i - 1) * bucket_size) + 1
        b_end = int(i * bucket_size) + 1
        b_end = min(b_end, n)
        ax, ay = rows[a].elapsed_ms, rows[a].weight_g
        best_area = -1.0
        best = b_start
        for j in range(b_start, max(b_start + 1, b_end)):
            area = abs(
                (ax - avg_x) * (rows[j].weight_g - ay)
                - (ax - rows[j].elapsed_ms) * (avg_y - ay)
            )
            if area > best_area:
                best_area = area
                best = j
        sampled.append(rows[best])
        a = best

    sampled.append(rows[-1])
    return sampled


def next_test_number(db: Session, target_g: int) -> int:
    """Per-target ordinal for display. Computed inside the run-start
    transaction; display-only, never an identity."""
    count = db.scalar(
        select(func.count()).select_from(DispenseRun)
        .where(DispenseRun.target_g == target_g)
    ) or 0
    return count + 1


def target_summary(db: Session, target_g: int, channel_id: str = "CH1") -> TargetSummary:
    row = db.execute(
        select(
            func.avg(DispenseRun.final_error_g),
            func.avg(func.abs(DispenseRun.final_error_g)),
            func.avg(DispenseRun.overshoot_g),
            func.avg(DispenseRun.duration_ms),
        ).where(
            DispenseRun.target_g == target_g,
            DispenseRun.channel_id == channel_id,
            DispenseRun.status.in_(TERMINAL_STATUSES),
        )
    ).one()
    return TargetSummary(
        avg_final_error_g=round(row[0], 3) if row[0] is not None else None,
        avg_abs_error_g=round(row[1], 3) if row[1] is not None else None,
        avg_overshoot_g=round(row[2], 3) if row[2] is not None else None,
        avg_duration_ms=round(row[3], 1) if row[3] is not None else None,
    )


def build_dashboard(db: Session, per_target: int = 10) -> DashboardOut:
    target_rows = list(db.execute(
        select(DispenseRun.material_id, DispenseRun.channel_id, DispenseRun.target_g).distinct()
    ))
    groups: list[TargetGroup] = []
    ordered = []
    for material, channel in (("M1", "CH1"), ("M2", "CH2")):
        targets = sorted(t for m, ch, t in target_rows if m == material and ch == channel)
        ordered.extend((material, channel, t) for t in PREFERRED_TARGETS if t in targets)
        ordered.extend((material, channel, t) for t in targets if t not in PREFERRED_TARGETS)

    for material, channel, target in ordered:
        runs = list(db.scalars(
            select(DispenseRun)
            .where(DispenseRun.material_id == material, DispenseRun.target_g == target,
                   DispenseRun.channel_id == channel)
            .order_by(DispenseRun.started_at.desc(), DispenseRun.created_at.desc())
            .limit(per_target)
        ))
        count = db.scalar(
            select(func.count()).select_from(DispenseRun)
            .where(DispenseRun.material_id == material, DispenseRun.target_g == target,
                   DispenseRun.channel_id == channel)
        ) or 0
        groups.append(TargetGroup(
            material_id=material,
            channel_id=channel,
            target_g=target,
            run_count=count,
            last_run_at=runs[0].started_at if runs else None,
            summary=target_summary(db, target, channel),
            runs=[to_card(r) for r in runs],
        ))
    return DashboardOut(generated_at=utcnow(), targets=groups)


CSV_COLUMNS = [
    "idx", "material_id", "channel_id", "timestamp", "elapsed_ms", "uptime_ms", "seq", "weight_g",
    "target_g", "error_g", "stable", "weight_age_ms", "p_term", "i_term",
    "d_term", "pid_output", "relay1", "relay2", "state",
]


def build_run_csv(db: Session, run: DispenseRun) -> str:
    """`#`-comment run header + one row per stored sample (complete, not
    decimated: the download is the raw record)."""
    buf = io.StringIO()
    header_lines = [
        ("run_id", run.run_id),
        ("material_id", run.material_id),
        ("channel_id", run.channel_id),
        ("profile_id", run.profile_id or ""),
        ("profile_version", run.profile_version or ""),
        ("device_id", run.device_id),
        ("test_number", run.test_number),
        ("job_id", run.job_id),
        ("target_g", run.target_g),
        ("priority", run.priority),
        ("status", run.status),
        ("assigned_at_utc", run.assigned_at.isoformat(timespec="milliseconds") if run.assigned_at else ""),
        ("started_at_utc", run.started_at.isoformat(timespec="milliseconds")),
        ("completed_at_utc",
         run.completed_at.isoformat(timespec="milliseconds")
         if run.completed_at else ""),
        ("duration_ms", run.duration_ms if run.duration_ms is not None else ""),
        ("final_weight_g", run.final_weight_g if run.final_weight_g is not None else ""),
        ("final_error_g", run.final_error_g if run.final_error_g is not None else ""),
        ("overshoot_g", run.overshoot_g if run.overshoot_g is not None else ""),
        ("max_weight_g", run.max_weight_g if run.max_weight_g is not None else ""),
        ("kp", run.kp if run.kp is not None else ""),
        ("ki", run.ki if run.ki is not None else ""),
        ("kd", run.kd if run.kd is not None else ""),
        ("tolerance_g", run.tolerance_g if run.tolerance_g is not None else ""),
        ("coarse_transition_g",
         run.coarse_transition_g if run.coarse_transition_g is not None else ""),
        ("settle_ms", run.settle_ms if run.settle_ms is not None else ""),
        ("window_ms", run.window_ms if run.window_ms is not None else ""),
        ("min_on_ms", run.min_on_ms if run.min_on_ms is not None else ""),
        ("min_off_ms", run.min_off_ms if run.min_off_ms is not None else ""),
        ("max_overshoot_g", run.max_overshoot_g if run.max_overshoot_g is not None else ""),
        ("completion_mode", run.completion_mode or ""),
        ("corrections", run.corrections if run.corrections is not None else ""),
        ("error_text", run.error_text or ""),
        ("firmware", run.firmware or ""),
        ("sample_count", run.sample_count or 0),
    ]
    for k, v in header_lines:
        buf.write(f"# {k}={v}\n")

    writer = csv.writer(buf, lineterminator="\n")
    writer.writerow(CSV_COLUMNS)
    for s in db.scalars(
        select(WeightSample)
        .where(WeightSample.run_id == run.run_id)
        .order_by(WeightSample.elapsed_ms, WeightSample.idx)
    ):
        writer.writerow([
            s.idx,
            s.material_id,
            s.channel_id,
            s.timestamp.isoformat(timespec="milliseconds"),
            s.elapsed_ms,
            s.uptime_ms if s.uptime_ms is not None else "",
            s.seq if s.seq is not None else "",
            s.weight_g, s.target_g, s.error_g,
            int(bool(s.stable)),
            s.weight_age_ms if s.weight_age_ms is not None else "",
            s.p_term if s.p_term is not None else "",
            s.i_term if s.i_term is not None else "",
            s.d_term if s.d_term is not None else "",
            s.pid_output if s.pid_output is not None else "",
            int(bool(s.relay1)), int(bool(s.relay2)),
            s.state or "",
        ])
    return buf.getvalue()
