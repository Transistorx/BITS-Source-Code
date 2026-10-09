"""Read-only dashboard, history, live monitor and export endpoints, plus PID profiles.

Thin HTTP wrappers: query validation, auth dependencies and response framing
(CSV) stay here; the logic is in app/services/{runs,profiles}.py (ServiceError
-> HTTPException via routes/_adapt.py).
"""

from datetime import datetime

from fastapi import APIRouter, Depends, Query
from fastapi.responses import Response
from sqlalchemy.orm import Session

from ..auth import require_read_key, require_write_key
from ..database import get_db
from ..schemas import (
    BulkProfilesOut,
    CANONICAL_TARGETS_G,
    DashboardOut,
    EventsOut,
    LiveOut,
    RunCard,
    RunDetail,
    RunSearchOut,
    SamplesOut,
    TargetGroup,
    TuningProfileBulkIn,
    TuningProfileIn,
    TuningProfileOut,
)
from ..services import profiles as profile_service
from ..services import runs as run_service
from ._adapt import http_errors

router = APIRouter(
    prefix="/api/v1",
    tags=["runs"],
    dependencies=[Depends(require_read_key)],
)


@router.get("/targets", response_model=list[TargetGroup])
def targets(db: Session = Depends(get_db)):
    return run_service.targets(db)


@router.get("/dashboard", response_model=DashboardOut)
def dashboard(db: Session = Depends(get_db)):
    return run_service.dashboard(db)


@router.get("/runs/latest", response_model=list[RunCard])
def latest_runs(
    material_id: str | None = Query(default=None, pattern="^(M1|M2)$"),
    channel_id: str | None = Query(default=None, pattern="^(CH1|CH2)$"),
    target_g: int | None = Query(default=None, gt=0, le=1_000_000),
    limit: int = Query(default=10, ge=1, le=100),
    db: Session = Depends(get_db),
):
    return run_service.latest_runs(db, material_id, channel_id, target_g, limit)


@router.get("/runs", response_model=RunSearchOut)
def search_runs(
    material_id: str | None = Query(default=None, pattern="^(M1|M2)$"),
    channel_id: str | None = Query(default=None, pattern="^(CH1|CH2)$"),
    target_g: int | None = Query(default=None, gt=0, le=1_000_000),
    from_time: datetime | None = Query(default=None, alias="from"),
    to_time: datetime | None = Query(default=None, alias="to"),
    run_id: str | None = Query(default=None, min_length=1, max_length=64),
    status: str | None = Query(default=None, max_length=16),
    result: str | None = Query(default=None, max_length=16),
    profile_id: str | None = Query(default=None, min_length=1, max_length=64),
    profile_version: int | None = Query(default=None, ge=1),
    profile_version_id: int | None = Query(default=None, ge=1),
    limit: int = Query(default=50, ge=1, le=500),
    offset: int = Query(default=0, ge=0),
    db: Session = Depends(get_db),
):
    with http_errors():
        return run_service.search_runs(
            db, material_id=material_id, channel_id=channel_id, target_g=target_g,
            from_time=from_time, to_time=to_time, run_id=run_id, status=status,
            result=result, profile_id=profile_id, profile_version=profile_version,
            profile_version_id=profile_version_id, limit=limit, offset=offset)


@router.get("/runs/{run_id}/samples", response_model=SamplesOut)
def run_samples(
    run_id: str,
    max_points: int = Query(default=0, ge=0, le=100_000,
                            description="Optional view-only LTTB downsampling; 0 returns every sample."),
    db: Session = Depends(get_db),
):
    with http_errors():
        return run_service.run_samples(db, run_id, max_points)


@router.get("/runs/{run_id}/events", response_model=EventsOut)
def run_events(run_id: str, db: Session = Depends(get_db)):
    with http_errors():
        return run_service.run_events(db, run_id)


@router.get("/runs/{run_id}/full")
def run_full(run_id: str, db: Session = Depends(get_db)):
    with http_errors():
        return run_service.run_full(db, run_id)


@router.get("/runs/{run_id}/csv")
def run_csv(run_id: str, db: Session = Depends(get_db)):
    with http_errors():
        out = run_service.run_csv(db, run_id)
    return Response(
        content=out["content"],
        media_type="text/csv; charset=utf-8",
        headers={"Content-Disposition": f'attachment; filename="{out["filename"]}"'},
    )


@router.get("/runs/{run_id}", response_model=RunDetail)
def run_detail(run_id: str, db: Session = Depends(get_db)):
    with http_errors():
        return run_service.run_detail(db, run_id)


@router.delete("/runs/{run_id}", dependencies=[Depends(require_write_key)])
def delete_run(run_id: str, db: Session = Depends(get_db)):
    """Remove one stored run and its own telemetry (see services.runs.delete_run)."""
    with http_errors():
        return run_service.delete_run(db, run_id)


@router.get("/live", response_model=LiveOut)
def live(db: Session = Depends(get_db)):
    return run_service.read_live(db)


@router.get("/profiles", response_model=list[TuningProfileOut])
def profiles(material_id: str | None = Query(default=None, pattern="^(M1|M2)$"),
             channel_id: str | None = Query(default=None, pattern="^(CH1|CH2)$"),
             target_g: int | None = Query(default=None, description=
                                          "Grams; profiles whose range contains this target"),
             db: Session = Depends(get_db)):
    with http_errors():
        return profile_service.list_profiles(db, material_id, channel_id, target_g)


@router.get("/profiles/active", response_model=TuningProfileOut)
def active_profile(material_id: str = Query(pattern="^(M1|M2)$"),
                   channel_id: str = Query(pattern="^(CH1|CH2)$"),
                   target_g: int = Query(..., description="Job target in grams; 404 if no active "
                                          "profile covers it, 409 if several do"),
                   db: Session = Depends(get_db)):
    with http_errors():
        return profile_service.active_profile(db, material_id, channel_id, target_g)


@router.post("/profiles", response_model=TuningProfileOut,
             dependencies=[Depends(require_write_key)])
def create_profile(payload: TuningProfileIn, db: Session = Depends(get_db)):
    with http_errors():
        return profile_service.create_profile(db, payload)


@router.post("/profiles/bulk", response_model=BulkProfilesOut,
             dependencies=[Depends(require_write_key)])
def create_profiles_bulk(payload: TuningProfileBulkIn, db: Session = Depends(get_db)):
    """Save the same gains for several pumps in ONE transaction."""
    with http_errors():
        return profile_service.create_profiles_bulk(db, payload)


@router.get("/profiles/next-version")
def next_profile_versions(
    profile_id: str = Query(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$"),
    materials: str = Query(default="M1,M2", description="Comma separated: M1,M2"),
    db: Session = Depends(get_db),
):
    """Read-only preview computed by the same service the save uses."""
    with http_errors():
        return profile_service.next_version_preview(db, profile_id, materials)


MaterialQuery = Query(default=None, pattern="^(M1|M2)$",
                      description="Disambiguates a profile_id+version shared by both pumps")


@router.post("/profiles/{profile_id}/{version}/activate",
             dependencies=[Depends(require_write_key)])
def activate_profile(profile_id: str, version: int, material_id: str | None = MaterialQuery,
                     db: Session = Depends(get_db)):
    """Legacy route, kept for compatibility; prefer /profile-versions/{id}/activate."""
    with http_errors():
        return profile_service.activate(
            db, profile_service.row_by_pair(db, profile_id, version, material_id))


@router.post("/profile-versions/{profile_version_id}/activate",
             dependencies=[Depends(require_write_key)])
def activate_profile_version(profile_version_id: int, db: Session = Depends(get_db)):
    with http_errors():
        return profile_service.activate(db, profile_service.row_by_id(db, profile_version_id))


@router.post("/profiles/{profile_id}/{version}/deactivate",
             dependencies=[Depends(require_write_key)])
def deactivate_profile(profile_id: str, version: int, material_id: str | None = MaterialQuery,
                       db: Session = Depends(get_db)):
    """Legacy route, kept for compatibility; prefer /profile-versions/{id}/deactivate."""
    with http_errors():
        return profile_service.deactivate(
            db, profile_service.row_by_pair(db, profile_id, version, material_id))


@router.post("/profile-versions/{profile_version_id}/deactivate",
             dependencies=[Depends(require_write_key)])
def deactivate_profile_version(profile_version_id: int, db: Session = Depends(get_db)):
    with http_errors():
        return profile_service.deactivate(db, profile_service.row_by_id(db, profile_version_id))


@router.delete("/profiles/{profile_id}/{version}",
               dependencies=[Depends(require_write_key)])
def delete_profile(profile_id: str, version: int, material_id: str | None = MaterialQuery,
                   db: Session = Depends(get_db)):
    """Legacy route, kept for compatibility; prefer /profile-versions/{id}."""
    with http_errors():
        return profile_service.delete(
            db, profile_service.row_by_pair(db, profile_id, version, material_id))


@router.delete("/profile-versions/{profile_version_id}",
               dependencies=[Depends(require_write_key)])
def delete_profile_version(profile_version_id: int, db: Session = Depends(get_db)):
    with http_errors():
        return profile_service.delete(db, profile_service.row_by_id(db, profile_version_id))
