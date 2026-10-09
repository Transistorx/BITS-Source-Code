"""Operator queue and device command/status APIs.

Thin HTTP wrappers: auth dependencies and request models live at this edge,
the logic is in app/services/{queue,control,materials}.py (ServiceError ->
HTTPException via routes/_adapt.py). Operator actions (CANCEL, PAUSE, RESUME,
ESTOP, CLEAR, HOLD, RELEASE, PROMOTE, PUMP_*, READY, ZERO, TARE) are queued as
device commands only; the firmware stays the safety authority.
"""

from fastapi import APIRouter, Depends, Query
from pydantic import BaseModel, ConfigDict, Field
from sqlalchemy.orm import Session

from ..auth import require_read_key, require_write_key
from ..database import get_db
from ..services import control as control_service
from ..services import materials as materials_service
from ..services import queue as queue_service
from ..services.analytics import utcnow  # noqa: F401  (re-exported for tests)
from ..services.control import (ControlIn, DeviceCommandAck, DeviceStatusIn,  # noqa: F401
                                command_wire)
from ..services.materials import MaterialLabelIn
from ..services.queue import QueueJobIn, ResubmitIn, STALE_UNKNOWN_PREFIX  # noqa: F401
from ._adapt import http_errors

router = APIRouter(prefix="/api/v1", tags=["operations"])

_with_gains = control_service.with_gains  # kept for callers/tests that import it here


@router.post("/queue/jobs", dependencies=[Depends(require_write_key)])
def create_job(payload: QueueJobIn, db: Session = Depends(get_db)):
    with http_errors():
        return queue_service.create_job(db, payload)


@router.get("/queue", dependencies=[Depends(require_read_key)])
def read_queue(db: Session = Depends(get_db)):
    with http_errors():
        return queue_service.read_queue(db)


@router.post("/queue/jobs/{command_id}/cancel", dependencies=[Depends(require_write_key)])
def cancel_waiting_job(command_id: int, db: Session = Depends(get_db)):
    with http_errors():
        return queue_service.cancel_job(db, command_id)


@router.get("/queue/control/{command_id}", dependencies=[Depends(require_read_key)])
def read_control_command(command_id: int, db: Session = Depends(get_db)):
    with http_errors():
        return control_service.read_control_command(db, command_id)


@router.post("/queue/control", dependencies=[Depends(require_write_key)])
def control_channel(payload: ControlIn, db: Session = Depends(get_db)):
    with http_errors():
        return control_service.control_command(db, payload)


def _queue_order(payload: ControlIn, db: Session) -> dict:
    """HOLD/RELEASE/PROMOTE (HTTP-error flavour of services.queue.queue_order)."""
    with http_errors():
        return queue_service.queue_order(db, action=payload.action,
                                         command_id=payload.command_id,
                                         local_job_id=payload.local_job_id)


@router.post("/queue/jobs/{command_id}/resubmit", dependencies=[Depends(require_write_key)])
def resubmit_failed_job(command_id: int, payload: ResubmitIn | None = None,
                        db: Session = Depends(get_db)):
    """Re-queue a JOB the device refused (e.g. stale id) under a new command id."""
    with http_errors():
        return queue_service.resubmit_job(db, command_id, payload)


@router.get("/device/commands", dependencies=[Depends(require_read_key)])
def device_commands(device_id: str = Query(min_length=1, max_length=64),
                    db: Session = Depends(get_db)):
    with http_errors():
        return control_service.poll_commands(db, device_id)


class CommandTransportIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    transport: str = Field(pattern="^(HTTP|MQTT)$")


@router.put("/device/{device_id}/command-transport", dependencies=[Depends(require_write_key)])
def set_command_transport(device_id: str, payload: CommandTransportIn,
                          db: Session = Depends(get_db)):
    with http_errors():
        return control_service.set_command_transport(db, device_id, payload.transport)


@router.post("/device/commands/{command_id}/ack", dependencies=[Depends(require_write_key)])
def acknowledge_command(command_id: int, payload: DeviceCommandAck,
                        db: Session = Depends(get_db)):
    with http_errors():
        return control_service.acknowledge_command(db, command_id, payload)


@router.post("/live/status", dependencies=[Depends(require_write_key)])
async def update_live_status(payload: DeviceStatusIn):
    """Latest state only: never queues a DB write or delays behind history."""
    with http_errors():
        return control_service.update_live_status(payload)


@router.post("/device/status", dependencies=[Depends(require_write_key)])
def update_device_status(payload: DeviceStatusIn, db: Session = Depends(get_db)):
    with http_errors():
        return control_service.update_device_status(db, payload)


@router.get("/device/status", dependencies=[Depends(require_read_key)])
def read_device_status(db: Session = Depends(get_db)):
    return control_service.read_device_status(db)


@router.get("/materials", dependencies=[Depends(require_read_key)])
def read_materials(db: Session = Depends(get_db)):
    return materials_service.read_materials(db)


@router.patch("/materials/{material_id}", dependencies=[Depends(require_write_key)])
def rename_material(material_id: str, payload: MaterialLabelIn,
                    db: Session = Depends(get_db)):
    with http_errors():
        return materials_service.rename_material(db, material_id, payload)
