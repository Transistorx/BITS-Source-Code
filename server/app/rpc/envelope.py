"""Request/response envelopes for the operator plane (CONTRACT 9.4).

Request  {v, corr_id, method, args, session_token, issued_at, ttl_ms, client_seq}
Response {corr_id, ok, code, error, data, next_cursor, server_time, seq}
"""

import json
import re
from collections import deque
from dataclasses import asdict, dataclass, is_dataclass
from datetime import date, datetime, time as dtime, timedelta, timezone
from decimal import Decimal
from enum import Enum
from pathlib import PurePath
from uuid import UUID

from pydantic import BaseModel

TOPIC_PREFIX = "bits/v1/ops/"
MAX_REQUEST_BYTES = 8192
MAX_RESPONSE_BYTES = 65536
MAX_TTL_MS = 120_000
MAX_FUTURE_SKEW = timedelta(seconds=60)
CORR_RE = re.compile(r"^[A-Za-z0-9._:-]{1,64}$")
METHOD_RE = re.compile(r"^[a-z][a-z0-9_.]{0,47}$")
CLIENT_RE = re.compile(r"^[A-Za-z0-9._:-]{1,128}$")


class RpcError(Exception):
    """A refusal with a stable wire code (see CODES)."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code, self.message = code, message


CODES = ("OK", "INVALID", "UNAUTHENTICATED", "AUTH_FAILED", "LOCKED", "FORBIDDEN", "EXPIRED",
         "BUSY", "NOT_FOUND", "CONFLICT", "UNAVAILABLE", "CONFIRM_REQUIRED", "TOO_LARGE",
         "REFUSED", "INTERNAL",
         "NO_COMPATIBLE_PROFILE", "AMBIGUOUS_PROFILE", "TARGET_OUT_OF_PROFILE_RANGE")

_SERVICE_CODES = {"invalid": "INVALID", "not_found": "NOT_FOUND", "conflict": "CONFLICT",
                  "unavailable": "UNAVAILABLE", "refused": "REFUSED", "internal": "INTERNAL",
                  "no_compatible_profile": "NO_COMPATIBLE_PROFILE",
                  "ambiguous_profile": "AMBIGUOUS_PROFILE",
                  "target_out_of_profile_range": "TARGET_OUT_OF_PROFILE_RANGE"}


def service_code(code: str) -> str:
    return _SERVICE_CODES.get(code, "INTERNAL")


@dataclass(frozen=True)
class Request:
    client_id: str
    method: str
    corr_id: str
    args: dict
    session_token: str | None
    issued_at: datetime
    ttl_ms: int
    client_seq: int | None


def _no_constant(name):
    raise ValueError("non-finite number")


def json_safe(obj):
    """Local JSON-safe encoder (the MQTT service must not import FastAPI).
    Matches fastapi.encoders.jsonable_encoder for the types used on the wire.
    Non-finite floats pass through here and are rejected by encode()."""
    if isinstance(obj, BaseModel):
        return json_safe(obj.model_dump(mode="json"))
    if is_dataclass(obj) and not isinstance(obj, type):
        return json_safe(asdict(obj))
    if isinstance(obj, Enum):
        return obj.value
    if isinstance(obj, PurePath):
        return str(obj)
    if obj is None or isinstance(obj, (str, int, float)):
        return obj
    if isinstance(obj, dict):
        return {json_safe(k): json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, set, frozenset, tuple, deque)):
        return [json_safe(v) for v in obj]
    if isinstance(obj, (datetime, date, dtime)):
        return obj.isoformat()
    if isinstance(obj, timedelta):
        return obj.total_seconds()
    if isinstance(obj, Decimal):
        return int(obj) if obj.as_tuple().exponent >= 0 else float(obj)
    if isinstance(obj, UUID):
        return str(obj)
    if isinstance(obj, bytes):
        return obj.decode()
    raise TypeError(f"{type(obj).__name__} is not JSON serializable")


def iso_ms(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.") + \
        f"{moment.microsecond // 1000:03d}Z"


def parse_topic(topic: str) -> tuple[str, str]:
    """-> (client_id, method) from bits/v1/ops/req/{client_id}/{method}."""
    parts = topic.split("/")
    if len(parts) != 6 or "/".join(parts[:4]) != TOPIC_PREFIX + "req":
        raise RpcError("INVALID", "bad request topic")
    client_id, method = parts[4], parts[5]
    if not CLIENT_RE.match(client_id) or not METHOD_RE.match(method):
        raise RpcError("INVALID", "bad request topic")
    return client_id, method


def peek_corr_id(payload) -> str | None:
    """Best-effort corr_id so even a refused request can be answered on its topic."""
    try:
        corr = json.loads(payload).get("corr_id")
        return corr if isinstance(corr, str) and CORR_RE.match(corr) else None
    except Exception:
        return None


def parse_request(topic: str, payload: bytes | str, client_id: str | None, now: datetime) -> Request:
    """Validate the envelope. Raises RpcError(INVALID) for malformed input.
    Expiry is checked separately (check_expiry) so a stored reply can win."""
    topic_client, topic_method = parse_topic(topic)
    if client_id is not None and client_id != topic_client:
        raise RpcError("INVALID", "client_id does not match request topic")
    raw = payload.encode() if isinstance(payload, str) else bytes(payload)
    if len(raw) > MAX_REQUEST_BYTES:
        raise RpcError("INVALID", f"request larger than {MAX_REQUEST_BYTES} bytes")
    try:
        body = json.loads(raw.decode("utf-8"), parse_constant=_no_constant)
    except (ValueError, UnicodeDecodeError, RecursionError):
        raise RpcError("INVALID", "request is not valid finite JSON") from None
    if not isinstance(body, dict):
        raise RpcError("INVALID", "request must be a JSON object")
    if type(body.get("v")) is not int or body["v"] != 1:
        raise RpcError("INVALID", "unsupported envelope version")
    corr = body.get("corr_id")
    if not isinstance(corr, str) or not CORR_RE.match(corr):
        raise RpcError("INVALID", "corr_id must be 1-64 of A-Z a-z 0-9 . _ : -")
    if body.get("method") != topic_method:
        raise RpcError("INVALID", "method does not match request topic")
    args = body.get("args", {})
    if args is None:
        args = {}
    if not isinstance(args, dict):
        raise RpcError("INVALID", "args must be an object")
    token = body.get("session_token")
    if token is not None and (not isinstance(token, str) or len(token) > 128):
        raise RpcError("INVALID", "session_token must be a string")
    ttl = body.get("ttl_ms")
    if type(ttl) is not int or not 1 <= ttl <= MAX_TTL_MS:
        raise RpcError("INVALID", f"ttl_ms must be an integer 1..{MAX_TTL_MS}")
    issued = body.get("issued_at")
    try:
        if not isinstance(issued, str) or not issued.endswith("Z"):
            raise ValueError
        issued_at = datetime.fromisoformat(issued[:-1] + "+00:00")
    except ValueError:
        raise RpcError("INVALID", "issued_at must be UTC ISO-8601 with Z") from None
    if issued_at - now > MAX_FUTURE_SKEW:
        raise RpcError("INVALID", "issued_at is in the future")
    seq = body.get("client_seq")
    if seq is not None and (type(seq) is not int or seq < 0):
        raise RpcError("INVALID", "client_seq must be a non-negative integer")
    return Request(topic_client, topic_method, corr, args, token, issued_at, ttl, seq)


def check_expiry(req: Request, now: datetime) -> None:
    if now > req.issued_at + timedelta(milliseconds=req.ttl_ms):
        raise RpcError("EXPIRED", "request is past its ttl_ms")


def response(corr_id: str | None, *, ok: bool, code: str, error: str | None = None,
             data=None, next_cursor: str | None = None, server_time: str, seq: int) -> dict:
    return {"corr_id": corr_id, "ok": ok, "code": code, "error": error,
            "data": json_safe(data) if data is not None else None,
            "next_cursor": next_cursor, "server_time": server_time, "seq": seq}


def encode(body: dict) -> bytes:
    return json.dumps(body, separators=(",", ":"), allow_nan=False).encode()
