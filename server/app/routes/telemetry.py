"""Telemetry write endpoints (ESP32 -> server). Contract section 3.

Thin HTTP wrappers over app/services/telemetry_ingest.py (ServiceError ->
HTTPException via routes/_adapt.py). The write-key dependency stays here.
"""

from fastapi import APIRouter, Depends
from sqlalchemy.orm import Session

from ..auth import require_write_key
from ..database import get_db
from ..schemas import (
    BatchOut,
    EventsIn,
    RunCompleteIn,
    RunDetail,
    RunStartIn,
    RunStartOut,
    TelemetryBatchIn,
)
from ..services import telemetry_ingest as ingest
from ._adapt import http_errors

router = APIRouter(
    prefix="/api/v1",
    tags=["telemetry"],
    dependencies=[Depends(require_write_key)],
)


@router.post("/runs/start", response_model=RunStartOut)
def start_run(payload: RunStartIn, db: Session = Depends(get_db)):
    """Register a physical dispensing attempt. Idempotent: a retry after a
    lost response returns the already-stored run instead of erroring."""
    with http_errors():
        return ingest.start_run(db, payload)


@router.post("/telemetry/batch", response_model=BatchOut)
def ingest_batch(payload: TelemetryBatchIn, db: Session = Depends(get_db)):
    """Efficient batch ingestion of real CAS-derived samples."""
    with http_errors():
        return ingest.ingest_batch(db, payload)


@router.post("/events", response_model=BatchOut)
def ingest_events(payload: EventsIn, db: Session = Depends(get_db)):
    with http_errors():
        return ingest.ingest_events(db, payload)


@router.post("/runs/{run_id}/complete", response_model=RunDetail)
def complete_run(run_id: str, payload: RunCompleteIn,
                 db: Session = Depends(get_db)):
    """Mark a run terminal. Idempotent: re-completing returns the stored
    run unchanged (the first completion is the permanent record)."""
    with http_errors():
        return ingest.complete_run(db, run_id, payload)
