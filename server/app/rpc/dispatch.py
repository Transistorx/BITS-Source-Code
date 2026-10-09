"""Operator-plane request dispatcher (CONTRACT 9.4). Transport-free.

    reply = RpcDispatcher().handle(topic, payload, client_id)

``reply.topic`` / ``reply.payload`` is the QoS 1, non-retained response to publish;
``reply.events`` are extra (topic, body, qos, retain) publishes to make AFTER the
response; ``reply.refresh`` names state topics ("queue", "devices") worth
re-publishing now. Call handle() from a worker thread, not the MQTT network loop.

The dispatcher never drives a relay: control.cmd only creates the same
DeviceCommand row the HTTP route creates; firmware stays the safety authority.
"""

import logging
import time
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone

from pydantic import ValidationError
from sqlalchemy import delete

from ..models_ops import RpcDedupe
from ..services import auth as auth_service
from ..services import control as control_service
from ..services import queue as queue_service
from ..services.errors import ServiceError
from . import envelope as env
from . import limits
from .envelope import RpcError
from .methods import METHODS, Ctx, is_unlimited
from .state import StateClock, build_devices, build_live, build_queue

log = logging.getLogger("rpc")

RES_PREFIX = env.TOPIC_PREFIX + "res/"
EVT_COMMAND = env.TOPIC_PREFIX + "evt/command"
READ_STORE_MAX_BYTES = 8192

# Responses worth replaying for a retried corr_id: the method actually ran.
_NOT_STORED = {"EXPIRED", "BUSY", "UNAUTHENTICATED", "FORBIDDEN", "CONFIRM_REQUIRED",
               "INVALID", "LOCKED", "AUTH_FAILED"}


@dataclass
class Reply:
    topic: str | None
    body: dict
    payload: bytes
    qos: int = 1
    retain: bool = False
    events: list = field(default_factory=list)   # (topic, body_dict, qos, retain)
    refresh: tuple = ()


def _default_session_factory():
    from .. import database
    database.init_db()
    return database._SessionLocal()


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


class RpcDispatcher:
    def __init__(self, session_factory=None, clock=None, live_snapshot=None,
                 state_clock: StateClock | None = None):
        self._session = session_factory or _default_session_factory
        self._clock = clock or _utcnow
        if live_snapshot is None:
            from ..services.live_state import live_state
            live_snapshot = live_state.snapshot
        self._live_snapshot = live_snapshot
        self.state = state_clock or StateClock()
        self.rate = limits.RateLimiter()
        self.inflight = limits.InflightHistory()
        self.dedupe = limits.Dedupe()
        self.throttle = auth_service.LoginThrottle()
        self.confirms = auth_service.ConfirmStore()

    # ---- helpers used by methods.py ---------------------------------------
    def snapshot_bundle(self, db, now: datetime) -> dict:
        """live.resync answer: full non-retained snapshots of live, queue, devices."""
        e = self.state.epoch
        return {
            "live": build_live(self._live_snapshot(), server_time=now, seq=self.state.next(), epoch=e),
            "queue": build_queue(queue_service.read_queue(db), server_time=now,
                                 seq=self.state.next(), epoch=e),
            "devices": build_devices(control_service.read_device_status(db), server_time=now,
                                     seq=self.state.next(), epoch=e),
        }

    def confirm_begin(self, c: Ctx, a) -> dict:
        target = METHODS.get(a.method)
        if target is None or target.dangerous is None:
            raise RpcError("INVALID", "method is not a dangerous action")
        clean = {k: v for k, v in a.args.items() if k != "confirm_token"}
        if not c.principal.allows(target.min_role):
            raise RpcError("FORBIDDEN", "role too low for that method")
        try:
            target.schema.model_validate(clean)
        except ValidationError as exc:
            raise RpcError("INVALID", _verr(exc)) from None
        if not target.dangerous(c.db, clean):
            raise RpcError("INVALID", "that request does not need confirmation")
        digest = auth_service.args_hash(a.method, clean)
        out = self.confirms.begin(c.principal, digest, c.now)
        return dict(out, method=a.method, args_hash=digest)

    # ---- main entry --------------------------------------------------------
    def handle(self, topic: str, payload, client_id: str | None = None,
               now: datetime | None = None) -> Reply:
        now = now or self._clock()
        started = time.monotonic()
        try:
            req = env.parse_request(topic, payload, client_id, now)
        except RpcError as exc:
            return self._reply(self._res_client(topic, client_id), env.peek_corr_id(payload),
                               now, exc.code, exc.message)
        method = METHODS.get(req.method)
        if method is None:
            return self._reply(req.client_id, req.corr_id, now, "INVALID", "unknown method")
        key = (req.client_id, req.corr_id)
        state, stored = self.dedupe.claim(key, now)
        if state == self.dedupe.DONE:
            return self._raw(req.client_id, req.corr_id, stored)
        if state == self.dedupe.IN_PROGRESS:
            return self._reply(req.client_id, req.corr_id, now, "BUSY", "request already in progress")
        reply = None
        try:
            reply = self._execute(req, method, now)
            return reply
        finally:
            store = None
            if reply is not None and reply.body["code"] not in _NOT_STORED and method.store:
                if method.write or len(reply.payload) <= READ_STORE_MAX_BYTES:
                    store = reply.body
            self.dedupe.finish(key, now, store)
            log.info("rpc %s client=%s code=%s ms=%d", req.method, req.client_id,
                     reply.body["code"] if reply else "ERR", (time.monotonic() - started) * 1000)

    # ---- internals ---------------------------------------------------------
    def _execute(self, req: env.Request, method, now: datetime) -> Reply:
        db = self._session()
        try:
            return self._run(db, req, method, now)
        except Exception as exc:  # never let a bug escape as silence
            log.error("rpc %s internal error: %s", req.method, exc.__class__.__name__)
            try:
                db.rollback()
            except Exception:
                pass
            return self._reply(req.client_id, req.corr_id, now, "INTERNAL", "internal error")
        finally:
            db.close()

    def _run(self, db, req: env.Request, method, now: datetime) -> Reply:
        cid, corr = req.client_id, req.corr_id
        if method.write:
            persisted = db.get(RpcDedupe, (cid, corr))
            if persisted is not None and now.replace(tzinfo=None) - persisted.created_at <= limits.DEDUPE_TTL:
                self.dedupe.finish((cid, corr), now, persisted.response_json)
                return self._raw(cid, corr, persisted.response_json)
        try:
            env.check_expiry(req, now)
        except RpcError as exc:
            return self._reply(cid, corr, now, exc.code, exc.message)
        args = {k: v for k, v in req.args.items() if k != "confirm_token"}
        confirm_token = req.args.get("confirm_token")
        if not is_unlimited(req.method, args) and not self.rate.allow(cid, now):
            return self._reply(cid, corr, now, "BUSY", "rate limit: 20 requests/s")
        # A login hash must never cover the password (offline guessing).
        digest = auth_service.args_hash(
            req.method, {"username": str(args.get("username"))[:64]} if method.min_role is None else args)
        principal, username, extra_events, refresh = None, None, [], ()
        code, error, data, cursor = "OK", None, None, None
        err_data = None
        try:
            if method.min_role is None:
                data = self._login(db, args, cid, now)
                username = data["username"]
            else:
                principal = auth_service.resolve_session(db, req.session_token, cid, now)
                if principal is None:
                    raise RpcError("UNAUTHENTICATED", "login required")
                if not principal.allows(method.min_role):
                    raise RpcError("FORBIDDEN", f"requires role {method.min_role}")
                try:
                    parsed = method.schema.model_validate(args)
                except ValidationError as exc:
                    raise RpcError("INVALID", _verr(exc)) from None
                if method.dangerous is not None and method.dangerous(db, args):
                    if not self.confirms.consume(principal, digest, confirm_token, now):
                        raise RpcError("CONFIRM_REQUIRED",
                                       "confirm.begin required for this action")
                if method.history and not self.inflight.acquire(cid):
                    raise RpcError("BUSY", "too many history queries in flight")
                try:
                    data, cursor = method.handler(
                        Ctx(db=db, principal=principal, client_id=cid, now=now, dispatcher=self), parsed)
                finally:
                    if method.history:
                        self.inflight.release(cid)
        except RpcError as exc:
            code, error = exc.code, exc.message
        except auth_service.AuthError as exc:
            code, error = exc.code, exc.message
            username = exc.username
        except ServiceError as exc:
            db.rollback()
            code, error, err_data = env.service_code(exc.code), exc.message, exc.data
        except Exception as exc:
            db.rollback()
            log.error("rpc %s internal error: %s", req.method, exc.__class__.__name__)
            code, error = "INTERNAL", "internal error"
        if method.write:
            self._audit(db, principal, cid, corr, req.method, digest, code, now, username)
        if code != "OK":
            return self._reply(cid, corr, now, code, error, data=err_data)
        reply = self._reply(cid, corr, now, "OK", None, data=data, cursor=cursor)
        if reply.body["code"] == "OK" and method.write and req.method != "auth.login":
            if isinstance(data, dict) and data.get("command_id") is not None:
                evt = {"method": req.method, "command_id": data["command_id"],
                       "state": data.get("state"), "server_time": reply.body["server_time"],
                       "seq": self.state.next()}
                reply.events.append((EVT_COMMAND, evt, 1, False))
            reply.refresh = ("queue", "devices") if req.method.startswith(("queue.", "control.")) \
                else ("devices",) if req.method.startswith("profiles.") else ()
            if method.store:
                self._persist(db, cid, corr, reply.body, now)
        return reply

    def _login(self, db, args: dict, cid: str, now: datetime) -> dict:
        try:
            a = METHODS["auth.login"].schema.model_validate(args)
        except ValidationError as exc:
            raise RpcError("INVALID", _verr(exc)) from None
        return auth_service.login(db, self.throttle, a.username, a.password, cid, now)

    def _audit(self, db, principal, cid, corr, name, digest, code, now, username) -> None:
        try:
            auth_service.audit(db, principal=principal, client_id=cid, corr_id=corr, method=name,
                               args_digest=digest, result=code, now=now, username=username)
        except Exception as exc:
            db.rollback()
            log.error("audit write failed: %s", exc.__class__.__name__)

    def _persist(self, db, cid, corr, body: dict, now: datetime) -> None:
        try:
            n = now.replace(tzinfo=None)
            db.execute(delete(RpcDedupe).where(RpcDedupe.created_at < n - limits.DEDUPE_TTL))
            db.add(RpcDedupe(client_id=cid, corr_id=corr, response_json=body, created_at=n))
            db.commit()
        except Exception as exc:
            db.rollback()
            log.error("dedupe persist failed: %s", exc.__class__.__name__)

    @staticmethod
    def _res_client(topic: str, client_id: str | None) -> str | None:
        try:
            return env.parse_topic(topic)[0] if client_id is None else client_id
        except RpcError:
            return client_id

    def _reply(self, client_id, corr_id, now, code, error, *, data=None, cursor=None) -> Reply:
        body = env.response(corr_id, ok=code == "OK", code=code, error=error, data=data,
                            next_cursor=cursor, server_time=env.iso_ms(now), seq=self.state.next())
        try:
            payload = env.encode(body)
        except ValueError:
            body = env.response(corr_id, ok=False, code="INTERNAL", error="unencodable response",
                                server_time=env.iso_ms(now), seq=self.state.next())
            payload = env.encode(body)
        if len(payload) > env.MAX_RESPONSE_BYTES:
            body = env.response(corr_id, ok=False, code="TOO_LARGE",
                                error="response exceeds 64 KB; use limit and next_cursor",
                                server_time=env.iso_ms(now), seq=self.state.next())
            payload = env.encode(body)
        topic = f"{RES_PREFIX}{client_id}/{corr_id or 'invalid'}" if client_id else None
        return Reply(topic, body, payload)

    @staticmethod
    def _raw(client_id: str, corr_id: str, body: dict) -> Reply:
        return Reply(f"{RES_PREFIX}{client_id}/{corr_id}", body, env.encode(body))


def _verr(exc: ValidationError) -> str:
    parts = [f"{'.'.join(str(p) for p in e['loc']) or 'args'}: {e['msg']}" for e in exc.errors()[:3]]
    return ("; ".join(parts))[:300]
