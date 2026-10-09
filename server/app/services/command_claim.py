"""Single transport arbiter for command delivery (CONTRACT 8.4).

`claim_pending` is one conditional UPDATE: exactly one caller (HTTP poll or MQTT
publish) can move a row PENDING -> DELIVERED. Terminal states are never touched.
Observability/transport only: nothing here decides what the device executes.
"""
from sqlalchemy import select, update
from sqlalchemy.orm import Session

from ..models import DeviceCommand, DeviceStatus
from .analytics import utcnow

TERMINAL_STATES = ("COMPLETE", "CANCELLED", "FAILED", "SUPERSEDED", "EXPIRED")


def device_transport(db: Session, device_id: str) -> str:
    value = db.scalar(select(DeviceStatus.command_transport).where(
        DeviceStatus.device_id == device_id))
    return "MQTT" if value == "MQTT" else "HTTP"


def device_boot_id(db: Session, device_id: str) -> str | None:
    return db.scalar(select(DeviceStatus.boot_id).where(DeviceStatus.device_id == device_id))


def claim_pending(db: Session, command_id: int, via: str, boot_id: str | None) -> bool:
    """Compare-and-set PENDING -> DELIVERED. True only for the winner. Does not
    commit, so the caller decides when the claim becomes visible."""
    now = utcnow()
    values = {"state": "DELIVERED", "delivered_via": via,
              "delivered_boot_id": boot_id, "updated_at": now}
    if via == "MQTT":
        values["published_at"] = now
    result = db.execute(update(DeviceCommand).where(
        DeviceCommand.id == command_id, DeviceCommand.state == "PENDING",
        DeviceCommand.held.is_(False),
    ).values(**values).execution_options(synchronize_session=False))
    return result.rowcount == 1
