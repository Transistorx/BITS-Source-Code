"""FastAPI application entry point for the dispensing telemetry server."""

from contextlib import asynccontextmanager
import logging
from pathlib import Path

from fastapi import Depends, FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from sqlalchemy import func, select
from starlette.middleware.base import BaseHTTPMiddleware

from .auth import require_read_key
from .config import settings
from . import database
from .database import current_migration_status, database_ok, get_db, init_db
from .models import DispenseRun, ServerMeta
from .routes.runs import router as runs_router
from .routes.telemetry import router as telemetry_router
from .routes.operations import router as operations_router
from .routes.live import router as live_router
from .services.analytics import utcnow

log = logging.getLogger(__name__)
APP_DIR = Path(__file__).resolve().parent
templates = Jinja2Templates(directory=str(APP_DIR / "templates"))

# Bump when any file under app/static changes. Templates stamp every asset URL
# with ?v={{ asset_v }} so a deploy can never collide with a browser-cached body
# from a previous version - the URL itself changes. Paired with the no-cache
# middleware below, a stale script is not merely unlikely, it is unreachable.
ASSET_VERSION = "2026-10-08.14"
templates.env.globals["asset_v"] = ASSET_VERSION


class StaticNoCacheMiddleware(BaseHTTPMiddleware):
    """Force revalidation of /static/* on every use.

    StaticFiles already sends ETag and Last-Modified but no Cache-Control, so
    browsers apply heuristic caching and may keep a copy indefinitely. That is
    how an operator ends up staring at a pre-Delete History page after the
    server has been updated: the HTML is fresh, the script is not. ``no-cache``
    means store-if-you-like but revalidate first - an unchanged file is a 304,
    a changed one is always fetched.
    """

    async def dispatch(self, request, call_next):
        response = await call_next(request)
        if request.url.path.startswith("/static/"):
            response.headers["Cache-Control"] = "no-cache, must-revalidate"
        return response


@asynccontextmanager
async def lifespan(_app: FastAPI):
    from .services.live_state import live_state
    live_state.clear()
    # A DB outage must not keep the observability process from starting. The
    # DB dependency retries schema initialisation when the database returns.
    try:
        init_db()
    except Exception:
        log.exception("Database unavailable during startup; serving degraded")
    from . import mqtt_bridge
    mqtt_bridge.start()  # no-op unless MQTT_ENABLED
    try:
        yield
    finally:
        mqtt_bridge.stop()


app = FastAPI(
    title="Dispense Telemetry",
    version="1.0.0",
    description="Queue operations, staged tuning configuration and telemetry analysis for the ESP32 dispensing controller.",
    lifespan=lifespan,
)

if settings.cors_origins:
    app.add_middleware(
        CORSMiddleware,
        allow_origins=settings.cors_origins,
        allow_methods=["GET", "POST"],
        allow_headers=["Content-Type", "X-API-Key"],
    )

# Registered after CORS so it wraps the innermost app and sees the final
# response from StaticFiles.
app.add_middleware(StaticNoCacheMiddleware)

app.mount("/static", StaticFiles(directory=str(APP_DIR / "static")), name="static")
app.include_router(telemetry_router)
app.include_router(runs_router)
app.include_router(operations_router)
app.include_router(live_router)


@app.get("/health")
def health():
    """Always answer so callers can distinguish server up / database down."""
    from . import mqtt_bridge
    up = database_ok()
    count = 0
    last_seen = None
    migration = current_migration_status()
    healthy = bool(migration and migration["state"] == "HEALTHY")
    if up:
        try:
            from .database import get_engine
            with get_engine().connect() as conn:
                count = conn.execute(select(func.count()).select_from(DispenseRun)).scalar_one()
                value = conn.execute(
                    select(ServerMeta.value).where(ServerMeta.key == "last_telemetry_at")
                ).scalar_one_or_none()
                if value:
                    from datetime import datetime
                    last_seen = datetime.fromisoformat(value)
        except Exception:
            log.exception("Health query failed")
            up = False
            count = 0
            last_seen = None
    return {
        "status": "ok" if up and healthy else "degraded",
        # A reachable database whose profile-versioning migration is not
        # HEALTHY is not "up": the status dot must not claim it is.
        "database": ("up" if healthy else "migration_blocked") if up else "down",
        "last_telemetry_at": last_seen,
        "run_count": count,
        "migration": migration,
        "mqtt": mqtt_bridge.mqtt_state(),
    }


@app.get("/health/live")
def health_live():
    """Liveness: 200 while the process serves HTTP, whatever the database says.

    The body carries the migration status for diagnosis (no SQL, no secrets)."""
    return {"status": "alive", "migration": current_migration_status()}


@app.get("/health/ready")
def health_ready():
    """Readiness: 200 only when the database answers AND the migration is HEALTHY
    AND this process finished schema initialisation. Otherwise 503 with the state
    and reason. Never runs a migration (a HEALTHY schema may be initialised)."""
    up = database_ok()
    migration = current_migration_status()
    if up and migration["state"] == "HEALTHY" and not database.schema_ready():
        try:
            init_db()    # additive only: the schema is already HEALTHY
        except Exception:
            log.exception("Readiness: schema initialisation failed")
        migration = current_migration_status()
    ready = bool(up and migration["state"] == "HEALTHY" and database.schema_ready())
    body = {"ready": ready, "database": "up" if up else "down", "migration": migration,
            "reason": "ok" if ready else (migration["reason"] if up else "DATABASE_DOWN")}
    return JSONResponse(body, status_code=200 if ready else 503)


@app.get("/", response_class=HTMLResponse)
def dashboard_page(request: Request):
    return templates.TemplateResponse(request=request, name="queue.html")


@app.get("/queue", response_class=HTMLResponse)
def queue_page(request: Request):
    return templates.TemplateResponse(request=request, name="queue.html")


@app.get("/tuning", response_class=HTMLResponse)
def tuning_page(request: Request):
    return templates.TemplateResponse(request=request, name="tuning.html")


@app.get("/graphs", response_class=HTMLResponse)
def graphs_page(request: Request):
    return templates.TemplateResponse(request=request, name="index.html")


@app.get("/history", response_class=HTMLResponse)
def history_page(request: Request):
    return templates.TemplateResponse(request=request, name="history.html")


@app.get("/live", response_class=HTMLResponse)
def live_page(request: Request):
    return templates.TemplateResponse(request=request, name="live.html")


@app.get("/runs/{run_id}", response_class=HTMLResponse)
def run_detail_page(request: Request, run_id: str):
    return templates.TemplateResponse(request=request, name="run_detail.html")
