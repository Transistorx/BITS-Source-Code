"""Startup recovery and command expiry for the headless service (CONTRACT 8.4, 9.8).

Pure DB logic, no transport and no FastAPI. Nothing here republishes or resends a
command: the server never replays a command after a restart (9.8); the operator
re-issues with a new command_id. Observability bookkeeping only.
"""
import logging
from datetime import datetime

from sqlalchemy import func, select

from .models import DeviceCommand, DeviceStatus, DispenseRun, RunBatch
from .mqtt_bridge import _stale, _too_old
from .services.analytics import utcnow

log = logging.getLogger("service.recovery")

NO_ACK_REASON = "DELIVERED_NO_ACK_OUTCOME_UNKNOWN"
STALLED = "STALLED"  # state flag, never a run status


def _mqtt_devices(db) -> list[str]:
    return list(db.scalars(select(DeviceStatus.device_id).where(
        DeviceStatus.command_transport == "MQTT")))


def expire_pending(db, now: datetime | None = None) -> int:
    """PENDING rows past their TTL on MQTT-transport devices. ZERO/TARE end FAILED
    with result timeout (8.4), everything else EXPIRED. STOP-class has ttl 0 and
    never expires. Commits."""
    now = now or utcnow()
    devices = _mqtt_devices(db)
    if not devices:
        return 0
    count = 0
    for row in db.scalars(select(DeviceCommand).where(
            DeviceCommand.device_id.in_(devices), DeviceCommand.state == "PENDING")):
        if not _too_old(row):
            continue
        row.updated_at, row.error_text = now, "expired before delivery"
        if row.command_type in ("ZERO", "TARE"):
            row.state = "FAILED"
            row.ack_json = {**(row.ack_json or {}), "result": "timeout"}
        else:
            row.state = "EXPIRED"
        count += 1
    if count:
        db.commit()
    return count


def close_stale_delivered(db, now: datetime | None = None) -> int:
    """MQTT-delivered rows with no terminal ACK older than the command TTL: the
    outcome is unknown, so FAILED and never resent. Commits."""
    now = now or utcnow()
    count = 0
    for row in db.scalars(select(DeviceCommand).where(
            DeviceCommand.state == "DELIVERED", DeviceCommand.delivered_via == "MQTT")):
        if _stale(row.updated_at, now):
            row.state, row.updated_at, row.error_text = "FAILED", now, NO_ACK_REASON
            count += 1
    if count:
        db.commit()
    return count


def stalled_runs(db, stale_s: float, now: datetime | None = None) -> list[dict]:
    """MQTT-origin RUNNING runs whose last stored batch (or start) is older than
    ``stale_s`` (CONTRACT 9.8). A flag for the operator only: staleness never changes a
    run's status. It clears by itself when the relay's data resumes."""
    now = now or utcnow()
    last = dict(db.execute(select(RunBatch.run_id, func.max(RunBatch.received_at))
                           .group_by(RunBatch.run_id)).all())
    out = []
    for run in db.scalars(select(DispenseRun).where(DispenseRun.status == "RUNNING")):
        seen = last.get(run.run_id)
        if seen is None:
            continue  # HTTP-origin run (dual-run bench): not ours to flag
        if (now - seen).total_seconds() > stale_s:
            out.append({"run_id": run.run_id, "device_id": run.device_id,
                        "channel_id": run.channel_id, "flag": STALLED})
    return out


def startup_recovery(db, run_stale_s: float = 30.0) -> dict:
    """Once per process, only after every SUBACK (never on reconnect). ``run_stale_s`` is
    kept for callers; a run is never finalised by staleness (only boot_id change/terminal)."""
    now = utcnow()
    result = {"expired": expire_pending(db, now),
              "delivered_closed": close_stale_delivered(db, now)}
    log.info("startup recovery: %s", result)
    return result
