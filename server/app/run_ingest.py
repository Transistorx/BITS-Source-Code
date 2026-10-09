"""MQTT run lifecycle ingest (CONTRACT 9.2). Transport-free and FastAPI-free.

Each ``process_*`` takes a DB session and a decoded body and returns the
``run/ack`` payload to publish, or None when nothing may be acked. The caller
publishes the ack ONLY after the session commits; a ServiceError (DB down) means
no ack, so the relay keeps the item buffered and resends it.

``acked_batch_seq`` is cumulative: the highest n with every batch 0..n stored (-1 =
not even the start). Out-of-order batches are stored but only advance the ack once
the gap fills. Replays are acked again and never double-stored.
"""
import json
import re
from typing import Literal

from pydantic import ValidationError
from sqlalchemy import select

from .models import DeviceCommand, DispenseRun
from .schemas import (EventsIn, RunCompleteIn, RunStartIn, TelemetryBatchIn)
from .services import telemetry_ingest as ingest
from .services.errors import ServiceError

DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
RUN_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")
BOOT_ID_RE = re.compile(r"^[0-9a-fA-F]{8}$")
KINDS = ("start", "samples", "events", "complete")
MAX_BYTES = {"start": 2048, "samples": 8192, "events": 2048, "complete": 1024}
MAX_BATCH_SEQ = 10_000_000
ACK_MAX_BYTES = 512
PROFILE_MISMATCH = "PROFILE_MISMATCH"
FINAL = ("COMPLETE", "FAILED", "CANCELLED", "COMPLETE_PARTIAL")

Kind = Literal["start", "samples", "events", "complete"]


def parse_topic(topic: str) -> tuple[str, str] | None:
    """cas/{dev}/run/{start|samples|events|complete} -> (dev, kind)."""
    parts = topic.split("/")
    if (len(parts) == 4 and parts[0] == "cas" and parts[2] == "run" and parts[3] in KINDS
            and DEVICE_ID_RE.match(parts[1])):
        return parts[1], parts[3]
    return None


def _reject_constant(value):
    raise ValueError(f"non-finite number {value}")


def _finite_float(text: str) -> float:
    number = float(text)
    if number != number or number in (float("inf"), float("-inf")):
        raise ValueError("non-finite number")
    return number


def decode(payload: bytes | str, kind: str) -> dict | None:
    """Bounded, strict JSON object or None (oversize, non-JSON, NaN/Infinity)."""
    if len(payload) > MAX_BYTES[kind]:
        return None
    try:
        body = json.loads(payload, parse_constant=_reject_constant, parse_float=_finite_float)
    except (ValueError, UnicodeDecodeError):
        return None
    return body if isinstance(body, dict) else None


def ack_topic(device_id: str) -> str:
    return f"cas/{device_id}/run/ack"


def encode_ack(ack: dict) -> str:
    """Compact JSON within the 512 B contract limit (long missing_ranges are cut)."""
    ack = dict(ack)
    text = json.dumps(ack, separators=(",", ":"))
    ranges = ack.get("missing_ranges")
    while len(text) > ACK_MAX_BYTES and isinstance(ranges, dict) and ranges:
        key = max(ranges, key=lambda k: len(ranges[k]))
        if len(ranges[key]) > 1:
            ranges[key] = ranges[key][:-1]
        else:
            ranges.pop(key)
        ack["truncated"] = True
        text = json.dumps(ack, separators=(",", ":"))
    return text


def _strict_int(value, low: int = 0, high: int = 2**31 - 1) -> int | None:
    return value if type(value) is int and low <= value <= high else None


def _boot(body: dict) -> str | None:
    value = body.get("boot_id")
    return value.lower() if isinstance(value, str) and BOOT_ID_RE.match(value) else None


def _ack(db, run_id: str, state: str, **extra) -> dict:
    out = {"run_id": run_id,
           "acked_batch_seq": ingest.watermark(ingest.batch_seqs(db, run_id)),
           "state": state}
    out.update({k: v for k, v in extra.items() if v})
    return out


def _unknown(run_id: str) -> dict:
    return {"run_id": run_id, "acked_batch_seq": -1, "state": "UNKNOWN_RUN"}


def _rejected(run_id: str, db=None) -> dict:
    return _ack(db, run_id, "REJECTED") if db is not None else \
        {"run_id": run_id, "acked_batch_seq": -1, "state": "REJECTED"}


def _boot_changed(db, run: DispenseRun, boot_id: str | None) -> bool:
    """CONTRACT 9.2: a different boot_id on a RUNNING run interrupts it for good."""
    if boot_id is None:
        return False
    if run.boot_id is None:
        run.boot_id = boot_id  # legacy run row: adopt the first boot we see
        return False
    if run.boot_id != boot_id and run.status == "RUNNING":
        ingest.interrupt_run(db, run)
    return run.boot_id != boot_id


def _lookup(db, device_id: str, body: dict):
    """(run_id, run, early_ack). early_ack set when the item cannot be processed."""
    run_id = body.get("run_id")
    if not isinstance(run_id, str) or not RUN_ID_RE.match(run_id):
        return None, None, None  # no run_id to address an ack to: drop
    run = db.get(DispenseRun, run_id)
    if run is None:
        return run_id, None, _unknown(run_id)
    if run.device_id != device_id:
        return run_id, run, {"run_id": run_id, "acked_batch_seq": -1, "state": "REJECTED"}
    return run_id, run, None


def _gate(db, run: DispenseRun, boot_id: str | None, batch_seq: int | None,
          allow_partial: bool = True) -> dict | None:
    """Common refusal rules for a data batch on an existing run."""
    if _boot_changed(db, run, boot_id) or run.status == "INTERRUPTED":
        return _ack(db, run.run_id, "INTERRUPTED")
    if run.status in FINAL and not (run.status == "COMPLETE_PARTIAL" and allow_partial):
        stored = batch_seq is not None and batch_seq in ingest.batch_seqs(db, run.run_id)
        return _ack(db, run.run_id, "BATCH_OK" if stored else "REJECTED")
    return None


def process_start(db, device_id: str, body: dict) -> dict | None:
    run_id = body.get("run_id")
    if not isinstance(run_id, str) or not RUN_ID_RE.match(run_id):
        return None
    boot_id = _boot(body)
    # Exact grams: 5000.0 or "5000" are refused, never coerced.
    if _strict_int(body.get("target_g"), 1, 1_000_000) is None:
        return _rejected(run_id)
    cfg = body.get("config")
    if isinstance(cfg, dict):
        cfg = dict(cfg)
        if "settle_ms" not in cfg and "settle_time_ms" in cfg:
            cfg["settle_ms"] = cfg["settle_time_ms"]
        body = {**body, "config": cfg}
    material = body.get("material_id")
    channel = body.get("channel_id") or ("CH1" if material == "M1" else "CH2")
    if body.get("profile_version") is not None and _strict_int(body["profile_version"], 1) is None:
        return _rejected(run_id)
    try:
        payload = RunStartIn.model_validate({**body, "device_id": device_id,
                                             "channel_id": channel})
    except ValidationError:
        return _rejected(run_id)
    existing = db.get(DispenseRun, run_id)
    if existing is not None:
        if existing.device_id != device_id:
            return _rejected(run_id)
        if _boot_changed(db, existing, boot_id) or existing.status == "INTERRUPTED":
            return _ack(db, run_id, "INTERRUPTED")
    elif not _pin_ok(db, device_id, body, payload):
        return _rejected(run_id)
    else:
        _interrupt_previous_boots(db, device_id, payload.channel_id, boot_id)
    ingest.start_run(db, payload, boot_id=boot_id, mqtt=True)
    return _ack(db, run_id, "START_OK")


def _interrupt_previous_boots(db, device_id: str, channel_id: str, boot_id: str | None) -> None:
    """A start from a new boot ends any RUNNING run of an older boot on that channel."""
    if boot_id is None:
        return
    for old in db.scalars(select(DispenseRun).where(
            DispenseRun.device_id == device_id, DispenseRun.channel_id == channel_id,
            DispenseRun.status == "RUNNING")):
        if old.boot_id is not None and old.boot_id != boot_id:
            ingest.interrupt_run(db, old)


def _pin_ok(db, device_id: str, body: dict, payload: RunStartIn) -> bool:
    """CONTRACT 9.3: a run is not accepted for a JOB whose profile pin failed, nor
    as a profile other than the one the job pinned."""
    command_id = _strict_int(body.get("command_id"), 1)
    if command_id is None:
        return True
    cmd = db.get(DeviceCommand, command_id)
    if cmd is None or cmd.device_id != device_id or cmd.command_type != "JOB":
        return True
    if cmd.error_text == PROFILE_MISMATCH:
        return False
    if cmd.profile_id and cmd.profile_version:
        reported = (payload.profile_id, payload.profile_version)
        if reported != (None, None) and reported != (cmd.profile_id, cmd.profile_version):
            return False
    return True


def process_samples(db, device_id: str, body: dict, commit: bool = True) -> dict | None:
    return _process_batch(db, device_id, body, "samples", commit)


def process_events(db, device_id: str, body: dict, commit: bool = True) -> dict | None:
    return _process_batch(db, device_id, body, "events", commit)


def _process_batch(db, device_id: str, body: dict, kind: str, commit: bool) -> dict | None:
    run_id, run, early = _lookup(db, device_id, body)
    if run_id is None:
        return None
    if early is not None:
        return early
    seq = _strict_int(body.get("batch_seq"), 1, MAX_BATCH_SEQ)
    if body.get("dropped") is True:  # tombstone on run/samples or run/events (CONTRACT 9.2)
        return _tombstone(db, run, seq, body, commit)
    items = body.get(kind)
    if seq is None or not isinstance(items, list) or not items:
        return _rejected(run_id, db)
    refusal = _gate(db, run, _boot(body), seq)
    if refusal is not None:
        return refusal
    try:
        if kind == "samples":
            batch = TelemetryBatchIn.model_validate({
                "run_id": run_id, "material_id": run.material_id, "channel_id": run.channel_id,
                "device_id": device_id,
                "samples": [{**s, "material_id": run.material_id, "channel_id": run.channel_id}
                            if isinstance(s, dict) else s for s in items]})
            ingest.ingest_batch(db, batch, batch_seq=seq, commit=commit)
        else:
            batch = EventsIn.model_validate({
                "run_id": run_id, "material_id": run.material_id, "channel_id": run.channel_id,
                "device_id": device_id, "events": items})
            ingest.ingest_events(db, batch, batch_seq=seq, commit=commit)
    except ValidationError:
        return _rejected(run_id, db)
    ingest.reevaluate_partial(db, run)  # a late batch may close a COMPLETE_PARTIAL
    if commit:
        db.commit()
    else:
        db.flush()
    return _ack(db, run_id, "BATCH_OK")


def _tombstone(db, run: DispenseRun, seq: int | None, body: dict, commit: bool) -> dict:
    """CONTRACT 9.2: placeholder for a samples batch the relay dropped on ring overflow
    ({"dropped":true,"samples":k}). Stored idempotently as a gap record so the cumulative
    ack passes it; the lost range stays in missing_ranges (COMPLETE_PARTIAL)."""
    count = _strict_int(body.get("samples"), 0, 1_000_000)
    if seq is None or count is None:
        return _rejected(run.run_id, db)
    refusal = _gate(db, run, _boot(body), seq)
    if refusal is not None:
        return refusal
    ingest.record_batch(db, run.run_id, seq, ingest.DROPPED_KIND, count)
    ingest.reevaluate_partial(db, run)
    if commit:
        db.commit()
    else:
        db.flush()
    return _ack(db, run.run_id, "BATCH_OK")


def process_complete(db, device_id: str, body: dict) -> dict | None:
    run_id, run, early = _lookup(db, device_id, body)
    if run_id is None:
        return None
    if early is not None:
        return early
    status = body.get("status")
    if status not in ("COMPLETE", "FAILED", "CANCELLED"):
        return _rejected(run_id, db)
    if _boot_changed(db, run, _boot(body)) or run.status == "INTERRUPTED":
        return _ack(db, run_id, "INTERRUPTED")
    meta = {k: body[k] for k in ("end_seq", "total_batches", "total_samples", "total_events",
                                 "dropped_samples") if _strict_int(body.get(k), 0) is not None}
    if run.status == "RUNNING":
        error = body.get("error")
        try:
            payload = RunCompleteIn.model_validate({
                "status": status,
                "final_weight_g": _strict_int(body.get("final_weight_g"), -10**6, 10**6),
                "max_weight_g": _strict_int(body.get("max_weight_g"), -10**6, 10**6),
                "duration_ms": _strict_int(body.get("duration_ms"), 0),
                "corrections": _strict_int(body.get("corrections"), 0),
                "error": error[:120] if isinstance(error, str) else None})
        except ValidationError:
            return _rejected(run_id, db)
        gaps = ingest.compute_gaps(db, run_id, meta)
        ingest.complete_run(db, run_id, payload, meta=meta, gaps=gaps)
        db.refresh(run)
    elif run.status == "COMPLETE_PARTIAL" and meta:
        run.ingest_json = {**(run.ingest_json or {}), **meta}
        ingest.reevaluate_partial(db, run)
        db.commit()
    partial = run.status == "COMPLETE_PARTIAL" or bool(run.missing_ranges)
    return _ack(db, run_id, "COMPLETE_PARTIAL" if partial else "COMPLETE",
                missing_ranges=run.missing_ranges if partial else None)


def process(db, device_id: str, kind: str, body: dict) -> dict | None:
    """Single-item entry (each item commits). Raises ServiceError if the DB fails."""
    if kind == "start":
        return process_start(db, device_id, body)
    if kind == "samples":
        return process_samples(db, device_id, body)
    if kind == "events":
        return process_events(db, device_id, body)
    if kind == "complete":
        return process_complete(db, device_id, body)
    return None


__all__ = ["process", "parse_topic", "decode", "encode_ack", "ack_topic", "ServiceError"]

