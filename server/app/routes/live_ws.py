import asyncio
import json
import logging

from fastapi import APIRouter, Header, WebSocket
from pydantic import ValidationError

from ..services.live_hub import (CLOSE_POLICY, REJECTIONS_PER_MINUTE, WsAuthIn, WsCmdIn,
                                 WsPingIn, live_hub)

log = logging.getLogger(__name__)
router = APIRouter(prefix="/api/v1", tags=["live"])

MAX_FRAME_BYTES = 512
CLOSE_TRY_AGAIN = 1013
_MODELS = {"auth": WsAuthIn, "cmd": WsCmdIn, "ping": WsPingIn}


def _error(code: str, message: str) -> dict:
    return {"type": "error", "code": code, "message": message}


async def _handle_frame(slot, raw: str) -> None:
    if len(raw.encode("utf-8", "replace")) > MAX_FRAME_BYTES:
        _reject(slot, "frame_too_large", f"frame exceeds {MAX_FRAME_BYTES} bytes")
        return
    try:
        body = json.loads(raw)
    except ValueError:
        _reject(slot, "bad_json", "frame is not valid JSON")
        return
    if not isinstance(body, dict):
        _reject(slot, "bad_frame", "frame must be a JSON object")
        return
    model = _MODELS.get(body.get("type"))
    if model is None:
        _reject(slot, "unknown_type", "unknown frame type")
        return
    try:
        msg = model.model_validate(body)
    except ValidationError as exc:
        first = exc.errors()[0]
        loc = ".".join(str(x) for x in first.get("loc", ())) or "frame"
        _reject(slot, "invalid", f"{loc}: {first.get('type', 'invalid')}")
        return
    if isinstance(msg, WsPingIn):
        live_hub.push(slot, {"type": "pong"})
        return
    if isinstance(msg, WsAuthIn):
        if live_hub.key_matches(msg.api_key):
            slot.authed = True
            live_hub.push(slot, {"type": "auth_ok"})
        else:
            _reject(slot, "unauthorized", "api key rejected")
        return
    state = await live_hub.submit_cmd(slot, msg)
    live_hub.push(slot, state.frame())
    if state.status == "refused" and slot.rejections.count(live_hub.now()) >= REJECTIONS_PER_MINUTE:
        live_hub.request_close(slot, CLOSE_POLICY, "too many rejected commands")


def _reject(slot, code: str, message: str) -> None:
    live_hub.push(slot, _error(code, message))
    if live_hub.note_rejection(slot):
        live_hub.request_close(slot, CLOSE_POLICY, "too many rejected frames")


@router.websocket("/live/ws")
async def live_ws(ws: WebSocket, x_api_key: str | None = Header(default=None)):
    if not live_hub.loop_bound:
        await ws.close(code=CLOSE_TRY_AGAIN, reason="live hub not ready")
        return
    await ws.accept()
    slot = live_hub.register(ws, authed=live_hub.key_matches(x_api_key))
    recv = None
    try:
        while not slot.closed:
            recv = asyncio.ensure_future(ws.receive())
            done, _ = await asyncio.wait({recv, slot.gone}, return_when=asyncio.FIRST_COMPLETED)
            if recv not in done:
                recv.cancel()
                recv = None
                break
            message = recv.result()
            recv = None
            kind = message.get("type")
            if kind == "websocket.disconnect":
                break
            if kind != "websocket.receive":
                continue
            text = message.get("text")
            if text is None:
                _reject(slot, "bad_frame", "text frames only")
                continue
            await _handle_frame(slot, text)
    except Exception:
        log.info("live ws client %d receive loop ended", slot.id, exc_info=True)
    finally:
        if recv is not None:
            recv.cancel()
        code, reason = slot.close_code, slot.close_reason
        live_hub.unregister(slot)
        if code is not None:
            try:
                await asyncio.wait_for(ws.close(code=code, reason=reason[:120]), 1.0)
            except Exception:
                pass
