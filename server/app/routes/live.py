"""Database-independent live ingress and coalesced browser event stream."""
import asyncio
import json

from fastapi import APIRouter, Depends, Header, Request
from fastapi.responses import StreamingResponse

from ..auth import _check
from ..config import settings
from ..services.live_state import live_state

router = APIRouter(prefix="/api/v1", tags=["live"])


async def authorize_read(x_api_key: str | None = Header(default=None)):
    if settings.reads_require_key:
        _check(x_api_key)


@router.get("/live/telemetry", dependencies=[Depends(authorize_read)])
async def telemetry():
    return live_state.snapshot()


async def event_stream(request):
    last_revision = -1
    ticks = 0
    while not await request.is_disconnected():
        snapshot = live_state.snapshot()
        # Send at most 10 Hz; each connection holds just one snapshot. Re-send
        # once per second even without ingress so stale UI is invalidated.
        if snapshot["revision"] != last_revision or ticks % 10 == 0:
            last_revision = snapshot["revision"]
            yield "data: " + json.dumps(snapshot, separators=(",", ":")) + "\n\n"
        ticks += 1
        await asyncio.sleep(.1)


@router.get("/live/events", dependencies=[Depends(authorize_read)])
async def events(request: Request):
    return StreamingResponse(event_stream(request), media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})
