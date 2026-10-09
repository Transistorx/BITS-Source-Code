"""Operator accounts, sessions, lockout, confirm tokens and the write audit.

Transport-neutral (db session in, plain values out). Passwords are argon2 hashed;
session and confirm tokens are random, only the SHA-256 of a session token is
stored. Nothing here logs a password or a token. Operators gate who may ask the
backend for things; they never sit in the safety loop.
"""

import hashlib
import json
import re
import secrets
import threading
from dataclasses import dataclass
from datetime import datetime, timedelta

from argon2 import PasswordHasher
from argon2.exceptions import InvalidHashError, VerificationError
from sqlalchemy import select
from sqlalchemy.orm import Session

from ..models_ops import Operator, OperatorAudit, OperatorSession
from .errors import ServiceError

ROLES = {"viewer": 1, "operator": 2, "admin": 3}
SESSION_TTL = timedelta(hours=8)
CONFIRM_TTL_S = 15.0
LOCK_AFTER = 5            # consecutive failures before backoff starts
LOCK_BASE_S = 30
LOCK_MAX_S = 900
MIN_PASSWORD_LEN = 10
USERNAME_RE = re.compile(r"^[A-Za-z0-9._-]{3,64}$")

_hasher = PasswordHasher()
_DUMMY_HASH = _hasher.hash("dummy-password-for-timing")


def set_hasher(hasher: PasswordHasher) -> None:
    """Tests only: cheaper argon2 parameters."""
    global _hasher, _DUMMY_HASH
    _hasher = hasher
    _DUMMY_HASH = hasher.hash("dummy-password-for-timing")


@dataclass(frozen=True)
class Principal:
    operator_id: int
    username: str
    role: str
    token_hash: str

    def allows(self, min_role: str) -> bool:
        return ROLES[self.role] >= ROLES[min_role]


def _naive(now: datetime) -> datetime:
    return now.replace(tzinfo=None)


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def args_hash(method: str, args: dict) -> str:
    """Stable hash of a request's method + args (confirm_token excluded)."""
    clean = {k: v for k, v in args.items() if k != "confirm_token"}
    blob = json.dumps([method, clean], sort_keys=True, separators=(",", ":"), default=str)
    return hashlib.sha256(blob.encode()).hexdigest()


def create_operator(db: Session, username: str, password: str, role: str) -> Operator:
    if not USERNAME_RE.match(username or ""):
        raise ServiceError("invalid", "username must be 3-64 of A-Z a-z 0-9 . _ -", 422)
    if role not in ROLES:
        raise ServiceError("invalid", "role must be viewer, operator or admin", 422)
    if len(password or "") < MIN_PASSWORD_LEN:
        raise ServiceError("invalid", f"password must be at least {MIN_PASSWORD_LEN} characters", 422)
    if db.scalar(select(Operator).where(Operator.username == username)) is not None:
        raise ServiceError("conflict", "operator already exists", 409)
    row = Operator(username=username, password_hash=_hasher.hash(password), role=role)
    db.add(row)
    db.commit()
    return row


class LoginThrottle:
    """Per client_id backoff, so unknown usernames cannot be guessed at 20 req/s."""

    def __init__(self):
        self._lock = threading.Lock()
        self._state: dict[str, tuple[int, datetime | None]] = {}

    def locked(self, client_id: str, now: datetime) -> bool:
        with self._lock:
            _, until = self._state.get(client_id, (0, None))
            return until is not None and now < until

    def failure(self, client_id: str, now: datetime) -> None:
        with self._lock:
            if len(self._state) > 2000:
                self._state.clear()
            n, _ = self._state.get(client_id, (0, None))
            n += 1
            self._state[client_id] = (n, now + _backoff(n) if n >= LOCK_AFTER else None)

    def success(self, client_id: str) -> None:
        with self._lock:
            self._state.pop(client_id, None)


def _backoff(failures: int) -> timedelta:
    return timedelta(seconds=min(LOCK_MAX_S, LOCK_BASE_S * 2 ** max(0, failures - LOCK_AFTER)))


class AuthError(Exception):
    def __init__(self, code: str, message: str, username: str | None = None):
        super().__init__(message)
        self.code, self.message, self.username = code, message, username


def login(db: Session, throttle: LoginThrottle, username: str, password: str,
          client_id: str, now: datetime) -> dict:
    """Verify credentials and open a session bound to client_id.

    Generic AUTH_FAILED for unknown user, wrong password and disabled account;
    LOCKED while a backoff is running (even for the right password)."""
    n = _naive(now)
    if throttle.locked(client_id, n):
        raise AuthError("LOCKED", "too many failed attempts; try again later")
    op = db.scalar(select(Operator).where(Operator.username == username))
    if op is not None and op.locked_until is not None and n < op.locked_until:
        raise AuthError("LOCKED", "too many failed attempts; try again later", op.username)
    ok = False
    try:
        _hasher.verify(op.password_hash if op is not None else _DUMMY_HASH, password)
        ok = op is not None and not op.disabled
    except (VerificationError, InvalidHashError):
        ok = False
    if not ok:
        throttle.failure(client_id, n)
        if op is not None:
            op.failed_attempts += 1
            if op.failed_attempts >= LOCK_AFTER:
                op.locked_until = n + _backoff(op.failed_attempts)
            db.commit()
        raise AuthError("AUTH_FAILED", "invalid credentials",
                        op.username if op is not None else None)
    throttle.success(client_id)
    op.failed_attempts, op.locked_until, op.last_login_at = 0, None, n
    token = secrets.token_urlsafe(32)
    expires = n + SESSION_TTL
    db.add(OperatorSession(token_hash=token_hash(token), operator_id=op.id,
                           client_id=client_id, created_at=n, expires_at=expires))
    _purge_sessions(db, n)
    db.commit()
    return {"session_token": token, "role": op.role, "username": op.username,
            "expires_in_s": int(SESSION_TTL.total_seconds())}


def _purge_sessions(db: Session, n: datetime) -> None:
    for old in db.scalars(select(OperatorSession).where(
            OperatorSession.expires_at < n - timedelta(days=1)).limit(200)):
        db.delete(old)


def resolve_session(db: Session, token: str | None, client_id: str,
                    now: datetime) -> Principal | None:
    """Principal for a live session bound to this client_id, else None."""
    if not token:
        return None
    th = token_hash(token)
    sess = db.get(OperatorSession, th)
    if sess is None or sess.revoked or sess.client_id != client_id or _naive(now) >= sess.expires_at:
        return None
    op = db.get(Operator, sess.operator_id)
    if op is None or op.disabled or op.role not in ROLES:
        return None
    return Principal(op.id, op.username, op.role, th)


def audit(db: Session, *, principal: Principal | None, client_id: str, corr_id: str,
          method: str, args_digest: str, result: str, now: datetime,
          username: str | None = None) -> None:
    """One row per write attempt. Never stores args, only their hash."""
    db.add(OperatorAudit(
        at=_naive(now), operator_id=principal.operator_id if principal else None,
        username=(principal.username if principal else (username or "-"))[:64],
        client_id=client_id[:128], corr_id=corr_id[:64], method=method[:48],
        args_hash=args_digest, result=result[:24]))
    db.commit()


class ConfirmStore:
    """Single-use, 15 s confirm tokens bound to session + method/args hash."""

    def __init__(self, ttl_s: float = CONFIRM_TTL_S):
        self._ttl = timedelta(seconds=ttl_s)
        self._lock = threading.Lock()
        self._tokens: dict[str, tuple[str, str, datetime]] = {}

    def begin(self, principal: Principal, digest: str, now: datetime) -> dict:
        token = secrets.token_urlsafe(24)
        with self._lock:
            self._tokens = {k: v for k, v in self._tokens.items() if v[2] > now}
            if len(self._tokens) > 500:
                self._tokens.clear()
            self._tokens[token_hash(token)] = (principal.token_hash, digest, now + self._ttl)
        return {"confirm_token": token, "expires_in_ms": int(self._ttl.total_seconds() * 1000)}

    def consume(self, principal: Principal, digest: str, token: str | None,
                now: datetime) -> bool:
        if not token or not isinstance(token, str):
            return False
        with self._lock:
            entry = self._tokens.pop(token_hash(token), None)
        if entry is None:
            return False
        session_hash, expected, expires = entry
        return (secrets.compare_digest(session_hash, principal.token_hash)
                and secrets.compare_digest(expected, digest) and now < expires)
