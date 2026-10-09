import asyncio
import json
import logging
from typing import Any
from urllib.parse import urlsplit

from fastapi import APIRouter, Header, WebSocket
from pydantic import BaseModel, ValidationError

from ..config import settings
from ..services.live_hub import (CLOSE_POLICY, REJECTIONS_PER_MINUTE, HubFull, WsAuthIn,
                                 WsCmdIn, WsPingIn, live_hub)

log = logging.getLogger(__name__)
router = APIRouter(prefix="/api/v1", tags=["live"])

MAX_FRAME_BYTES = 512
CLOSE_TRY_AGAIN = 1013
LOOPBACK_HOSTS = ("127.0.0.1", "::1", "::ffff:127.0.0.1", "localhost")
_MODELS: dict[str, type[BaseModel]] = {"auth": WsAuthIn, "cmd": WsCmdIn, "ping": WsPingIn}


def _error(code: str, message: str) -> dict:
    return {"type": "error", "code": code, "message": message}


def _parse(raw: str) -> tuple[BaseModel | None, str, str]:
    if len(raw.encode("utf-8", "replace")) > MAX_FRAME_BYTES:
        return None, "frame_too_large", f"frame exceeds {MAX_FRAME_BYTES} bytes"
    try:
        body = json.loads(raw)
    except ValueError:
        return None, "bad_json", "frame is not valid JSON"
    if not isinstance(body, dict):
        return None, "bad_frame", "frame must be a JSON object"
    kind = body.get("type")
    model = _MODELS.get(kind) if isinstance(kind, str) else None
    if model is None:
        return None, "unknown_type", "unknown frame type"
    try:
        return model.model_validate(body), "", ""
    except ValidationError as exc:
        first = exc.errors()[0]
        loc = ".".join(str(x) for x in first.get("loc", ())) or "frame"
        return None, "invalid", f"{loc}: {first.get('type', 'invalid')}"


async def _handle_frame(slot, raw: str) -> None:
    msg, code, message = _parse(raw)
    if msg is None:
        _reject(slot, code, message)
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
    if isinstance(msg, WsCmdIn):
        state = await live_hub.submit_cmd(slot, msg)
        live_hub.push(slot, state.frame())
        if state.status == "refused" and slot.rejections.count(live_hub.now()) >= REJECTIONS_PER_MINUTE:
            live_hub.request_close(slot, CLOSE_POLICY, "too many rejected commands")


def _reject(slot, code: str, message: str) -> None:
    live_hub.push(slot, _error(code, message))
    if live_hub.note_rejection(slot):
        live_hub.request_close(slot, CLOSE_POLICY, "too many rejected frames")


def _peer(ws: WebSocket) -> str:
    client = ws.client
    return client.host if client is not None else ""


def _is_loopback(ws: WebSocket) -> bool:
    host = _peer(ws).lower()
    return host in LOOPBACK_HOSTS or host.startswith("127.")


def _origin_allowed(ws: WebSocket) -> bool:
    origin = ws.headers.get("origin")
    if origin is None:
        return True
    origin = origin.strip().lower().rstrip("/")
    allowed = [o.strip().lower().rstrip("/") for o in settings.cors_origins]
    if "*" in allowed or origin in allowed:
        return True
    host = (ws.headers.get("host") or "").strip().lower()
    parts = urlsplit(origin)
    return bool(host) and parts.scheme in ("http", "https") and parts.netloc == host


async def _close_quiet(ws: WebSocket, code: int, reason: str) -> None:
    try:
        await asyncio.wait_for(ws.close(code=code, reason=reason[:120]), 1.0)
    except Exception:
        pass


async def _auth_first_frame(ws: WebSocket) -> bool:
    timeout = settings.live_ws_auth_timeout_ms / 1000
    try:
        message = await asyncio.wait_for(ws.receive(), timeout)
    except asyncio.TimeoutError:
        await _close_quiet(ws, CLOSE_POLICY, "auth required")
        return False
    except Exception:
        return False
    if message.get("type") != "websocket.receive":
        return False
    text = message.get("text")
    reason = "auth required"
    if text is not None:
        msg, _code, _detail = _parse(text)
        if isinstance(msg, WsAuthIn):
            if live_hub.key_matches(msg.api_key):
                return True
            reason = "api key rejected"
    await _close_quiet(ws, CLOSE_POLICY, reason)
    return False


@router.websocket("/live/ws")
async def live_ws(ws: WebSocket, x_api_key: str | None = Header(default=None)):
    if not live_hub.loop_bound:
        await ws.close(code=CLOSE_TRY_AGAIN, reason="live hub not ready")
        return
    if not _origin_allowed(ws):
        log.warning("live ws refused: origin mismatch from %s", _peer(ws))
        await ws.close(code=CLOSE_POLICY, reason="origin not allowed")
        return
    if not live_hub.pending_begin():
        log.warning("live ws refused: client cap %d reached", settings.max_ws_clients)
        await ws.close(code=CLOSE_TRY_AGAIN, reason="too many clients")
        return
    slot = None
    pending = True
    recv: asyncio.Future[Any] | None = None
    try:
        await ws.accept()
        authed = live_hub.key_matches(x_api_key)
        first_frame_auth = False
        if live_hub.reads_gated() and not authed:
            if not await _auth_first_frame(ws):
                log.info("live ws refused: unauthenticated read from %s", _peer(ws))
                return
            authed = first_frame_auth = True
        try:
            slot = live_hub.register(ws, authed=authed, loopback=_is_loopback(ws))
        except HubFull:
            await _close_quiet(ws, CLOSE_TRY_AGAIN, "too many clients")
            return
        pending = False
        live_hub.pending_end()
        if first_frame_auth:
            live_hub.push(slot, {"type": "auth_ok"})
        while not slot.closed:
            gone = slot.gone
            if gone is None:
                break
            recv = asyncio.ensure_future(ws.receive())
            waiting: set[asyncio.Future[Any]] = {recv, gone}
            done, _ = await asyncio.wait(waiting, return_when=asyncio.FIRST_COMPLETED)
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
        log.info("live ws client receive loop ended", exc_info=True)
    finally:
        if pending:
            live_hub.pending_end()
        if recv is not None:
            recv.cancel()
        if slot is not None:
            code, reason = slot.close_code, slot.close_reason
            live_hub.unregister(slot)
            if code is not None:
                await _close_quiet(ws, code, reason)
