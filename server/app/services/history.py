"""Paged, size-bounded history reads for the operator plane (CONTRACT 9.6).

Read-only. Samples are paged by ``idx`` (monotonic per run); decimation keeps
REAL stored samples only (per bucket the min and max weight), never averages.
"""

import json

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from ..models import DispenseEvent, WeightSample
from ..schemas import CANONICAL_TARGETS_G
from .analytics import to_event_out, to_sample_out
from .errors import ServiceError
from .runs import run_or_404

GRAPH_TARGETS_G = CANONICAL_TARGETS_G  # (5000, 10000, 15000, 20000)
SAMPLE_PAGE_BYTES = 48 * 1024


def check_graph_target(target_g: int) -> None:
    if target_g not in GRAPH_TARGETS_G:
        raise ServiceError(
            "refused", f"target_g {target_g} is not a Test Graph category "
            f"(allowed {list(GRAPH_TARGETS_G)}); use Run History", 403)


def _compact(obj) -> str:
    return json.dumps(obj, separators=(",", ":"), default=str)


def minmax_decimate(rows: list, max_points: int) -> list:
    """<= max_points rows, in order, first and last kept, min and max weight per bucket."""
    n = len(rows)
    if max_points < 4 or n <= max_points:
        return rows
    buckets = (max_points - 2) // 2
    inner = rows[1:-1]
    size = len(inner) / buckets
    keep = {0, n - 1}
    for b in range(buckets):
        lo, hi = int(b * size), max(int((b + 1) * size), int(b * size) + 1)
        chunk = range(lo, min(hi, len(inner)))
        if not chunk:
            continue
        lo_i = min(chunk, key=lambda i: inner[i].weight_g)
        hi_i = max(chunk, key=lambda i: inner[i].weight_g)
        keep.add(lo_i + 1)
        keep.add(hi_i + 1)
    return [rows[i] for i in sorted(keep)]


def run_samples_page(db: Session, run_id: str, *, from_seq: int = 0, limit: int = 500,
                     max_points: int = 0) -> dict:
    run_or_404(db, run_id)
    rows = list(db.scalars(
        select(WeightSample).where(WeightSample.run_id == run_id, WeightSample.idx >= from_seq)
        .order_by(WeightSample.idx)))
    remaining = len(rows)
    decimated = False
    if max_points and remaining > max_points:
        rows, decimated = minmax_decimate(rows, max_points), True
    out, used = [], 2
    for row in rows[:limit]:
        item = to_sample_out(row).model_dump(mode="json")
        size = len(_compact(item)) + 1
        if out and used + size > SAMPLE_PAGE_BYTES:
            break
        out.append(item)
        used += size
    more = len(out) < len(rows)
    total = db.scalar(select(func.count()).select_from(WeightSample)
                      .where(WeightSample.run_id == run_id)) or 0
    return {"run_id": run_id, "count": total, "decimated": decimated, "samples": out,
            "next_seq": (out[-1]["idx"] + 1) if more and out else None}


def run_events_page(db: Session, run_id: str, *, offset: int = 0, limit: int = 50) -> dict:
    run_or_404(db, run_id)
    rows = list(db.scalars(
        select(DispenseEvent).where(DispenseEvent.run_id == run_id)
        .order_by(DispenseEvent.timestamp, DispenseEvent.idx).offset(offset).limit(limit + 1)))
    page = [to_event_out(r).model_dump(mode="json") for r in rows[:limit]]
    return {"run_id": run_id, "events": page, "more": len(rows) > limit}
