"""Operator-plane tables (CONTRACT 9.4). Additive: new tables only, nothing altered.

operators       argon2 password hashes + role + lockout counters
operator_sessions  session tokens, stored only as SHA-256 hashes, bound to client_id
operator_audit  one row per write attempt (who, method, args hash, result)
rpc_dedupe      stored responses of write methods for (client_id, corr_id) retry safety

New tables need production migration approval; create_all adds them idempotently.
"""

from datetime import datetime

from sqlalchemy import Boolean, Index, Integer, JSON, String

from sqlalchemy.orm import Mapped, mapped_column

from .models import BIG_ID, TS, Base


class Operator(Base):
    __tablename__ = "operators"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    username: Mapped[str] = mapped_column(String(64), unique=True, nullable=False)
    password_hash: Mapped[str] = mapped_column(String(255), nullable=False)
    role: Mapped[str] = mapped_column(String(16), nullable=False)  # viewer/operator/admin
    disabled: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    failed_attempts: Mapped[int] = mapped_column(Integer, default=0, nullable=False)
    locked_until: Mapped[datetime | None] = mapped_column(TS, nullable=True)
    last_login_at: Mapped[datetime | None] = mapped_column(TS, nullable=True)
    created_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)


class OperatorSession(Base):
    __tablename__ = "operator_sessions"

    token_hash: Mapped[str] = mapped_column(String(64), primary_key=True)
    operator_id: Mapped[int] = mapped_column(BIG_ID, index=True, nullable=False)
    client_id: Mapped[str] = mapped_column(String(128), nullable=False)
    created_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)
    expires_at: Mapped[datetime] = mapped_column(TS, nullable=False)
    revoked: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)


class OperatorAudit(Base):
    __tablename__ = "operator_audit"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow, index=True)
    operator_id: Mapped[int | None] = mapped_column(BIG_ID, nullable=True)
    username: Mapped[str] = mapped_column(String(64), default="-")
    client_id: Mapped[str] = mapped_column(String(128), default="")
    corr_id: Mapped[str] = mapped_column(String(64), default="")
    method: Mapped[str] = mapped_column(String(48), nullable=False)
    args_hash: Mapped[str] = mapped_column(String(64), default="")
    result: Mapped[str] = mapped_column(String(24), nullable=False)  # OK or error code


class RpcDedupe(Base):
    __tablename__ = "rpc_dedupe"

    client_id: Mapped[str] = mapped_column(String(128), primary_key=True)
    corr_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    response_json: Mapped[dict] = mapped_column(JSON, nullable=False)
    created_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)

    __table_args__ = (Index("ix_rpc_dedupe_created", "created_at"),)
