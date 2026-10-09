"""Persisted state, audit trail and live status of the profile-versioning migration.

Three sources of truth, deliberately separate:

1. The ACTUAL schema (inspector): has the surrogate primary key, the unique key,
   the history columns and indexes, and the backfill marker. This is what decides
   whether the database is usable. It cannot be stale.
2. ``server_meta['profile_version_migration_state']``: a compact JSON record of the
   last run (NOT_STARTED / IN_PROGRESS / DONE / FAILED, step, timestamps, build,
   sanitised error, ``ok`` = operator approved a resume). It decides whether an
   automatic start is ALLOWED; it never overrides the schema.
3. ``schema_migration_audit``: append-only rows (START / OK / FAIL / SKIP per step).

State and audit rows are written on a SEPARATE connection, so they survive the
implicit commit of each MySQL DDL statement and the rollback of a failed step.
Write failures here are logged and never abort the migration itself.

``migration_status(engine)`` is a pure read. Possible states:

  HEALTHY                 schema complete (incl. backfill marker).
  MIGRATION_REQUIRED      old schema, or only the backfill is pending on a database with
                          no failure on record, or an operator approved a resume.
                          The ONLY state in which startup runs the migration.
  MIGRATION_IN_PROGRESS   a run recorded a heartbeat less than STALE_AFTER_SECONDS ago.
  MIGRATION_FAILED        a failure is on record, a run crashed (heartbeat older than
                          STALE_AFTER_SECONDS), or the schema is half-migrated without
                          an approved resume. Never retried automatically.
  UNKNOWN                 (database layer only) the database could not be read.
"""

import json
import logging
from contextlib import contextmanager
from datetime import datetime, timezone

from sqlalchemy import create_engine, inspect, select
from sqlalchemy.exc import DBAPIError
from sqlalchemy.pool import NullPool

from .profile_version_migration import (
    BACKFILL_MARKER,
    HISTORY_INDEXES,
    _bounded_engine,
)

log = logging.getLogger("migration_state")

MIGRATION_NAME = "profile_versioning"
STATE_KEY = "profile_version_migration_state"
HEALTHY = "HEALTHY"
REQUIRED = "MIGRATION_REQUIRED"
IN_PROGRESS = "MIGRATION_IN_PROGRESS"
FAILED = "MIGRATION_FAILED"
UNKNOWN = "UNKNOWN"

# A run refreshes its heartbeat at the start and end of every step. The slowest
# single step is bounded by the 30 s lock wait plus the table work, so 15 minutes
# without a heartbeat means the process died (or hung) mid-migration.
STALE_AFTER_SECONDS = 900
_VALUE_LIMIT = 190          # server_meta.value is VARCHAR(190)


def _utcnow() -> datetime:
    return datetime.now(timezone.utc).replace(tzinfo=None)


def _iso(value: datetime) -> str:
    return value.isoformat(timespec="seconds")


def build_id() -> str:   # noqa: D103
    try:
        from .main import ASSET_VERSION
        return ASSET_VERSION
    except Exception:  # pragma: no cover - only if main cannot be imported
        return "unknown"


def safe_error(exc: BaseException) -> str:
    """One short line that never contains SQL, parameters or credentials."""
    name = exc.__class__.__name__
    if isinstance(exc, DBAPIError):
        orig = getattr(exc, "orig", None)
        args = getattr(orig, "args", None) or ()
        code = args[0] if args and isinstance(args[0], int) else None
        return f"{name}/{orig.__class__.__name__}" + (f" code={code}" if code else "")
    if isinstance(exc, RuntimeError):   # our own REFUSED / FAILED messages
        return f"{name}: {' '.join(str(exc).split())}"[:200]
    return name


def _separate_engine(engine):
    """An engine whose connections are NOT the migration's own (returns (engine, owned))."""
    if engine.dialect.name == "mysql":
        return _bounded_engine(engine)
    database = engine.url.database
    if engine.dialect.name == "sqlite" and database and ":memory:" not in database:
        return create_engine(engine.url, poolclass=NullPool, future=True), True
    return engine, False    # in-memory SQLite: a second connection would be a second DB


def _encode(record: dict) -> str:
    record = {k: v for k, v in record.items() if v not in (None, "", False)}
    text_ = json.dumps(record, separators=(",", ":"))
    over = len(text_) - _VALUE_LIMIT
    if over > 0 and record.get("err"):
        record["err"] = record["err"][:max(0, len(record["err"]) - over - 3)] + "..."
        text_ = json.dumps(record, separators=(",", ":"))
    return text_[:_VALUE_LIMIT]


class MigrationRecorder:
    """Writes the state record and audit rows for one migration run."""

    def __init__(self, engine, name: str = MIGRATION_NAME, state_key: str = STATE_KEY):
        self.name, self.state_key = name, state_key
        self.engine, self._owned = _separate_engine(engine)
        self.record = {"s": "NOT_STARTED", "b": build_id()}
        self.failed = False

    def close(self) -> None:
        if self._owned:
            self.engine.dispose()

    def audit(self, step: str, outcome: str, detail: str = "") -> None:
        from . import models
        try:
            with self.engine.begin() as conn:
                conn.execute(models.SchemaMigrationAudit.__table__.insert().values(
                    ts=_utcnow(), migration=self.name, step=step[:64],
                    outcome=outcome, detail=(detail or "")[:500]))
        except Exception:
            log.exception("could not write migration audit row (%s %s)", step, outcome)

    def _write(self, **changes) -> None:
        from . import models
        self.record.update(changes)
        self.record["hb"] = _iso(_utcnow())
        value = _encode(self.record)
        meta = models.ServerMeta.__table__
        try:
            with self.engine.begin() as conn:
                updated = conn.execute(meta.update().where(meta.c["key"] == self.state_key)
                                       .values(value=value)).rowcount
                if not updated:
                    conn.execute(meta.insert().values({"key": self.state_key, "value": value}))
        except Exception:
            log.exception("could not write migration state record")

    def begin(self) -> None:
        self.record = {"s": "IN_PROGRESS", "step": "start", "st": _iso(_utcnow()),
                       "b": build_id()}
        self._write()
        self.audit("run", "START", f"build={build_id()}")

    @contextmanager
    def step(self, name: str):
        self.audit(name, "START")
        self._write(s="IN_PROGRESS", step=name)
        try:
            yield
        except BaseException as exc:
            self.fail(name, exc)
            raise
        self.audit(name, "OK")
        self._write(s="IN_PROGRESS", step=name)    # heartbeat

    def skip(self, name: str, detail: str) -> None:
        self.audit(name, "SKIP", detail)
        self._write(s="IN_PROGRESS", step=name)

    def fail(self, name: str, exc: BaseException) -> None:
        if self.failed:
            return
        self.failed = True
        error = safe_error(exc)
        self.audit(name, "FAIL", error)
        self._write(s="FAILED", step=name, err=error, fin=_iso(_utcnow()), ok=None)

    def finish(self) -> None:
        self._write(s="DONE", step="complete", fin=_iso(_utcnow()), err=None, ok=None)
        self.audit("run", "OK", "migration complete")


# --------------------------------------------------------------------- status

def read_record(engine) -> dict | None:
    """The state record, {} fields normalised; None if absent. Corrupt -> s=CORRUPT."""
    from . import models
    meta = models.ServerMeta.__table__
    with engine.connect() as conn:
        raw = conn.execute(select(meta.c["value"]).where(meta.c["key"] == STATE_KEY)).scalar_one_or_none()
    if raw is None:
        return None
    try:
        data = json.loads(raw)
        if not isinstance(data, dict):
            raise ValueError
        return data
    except ValueError:
        return {"s": "CORRUPT", "err": "state record unreadable"}


def schema_facts(engine) -> dict:
    """What the database ACTUALLY looks like (read-only)."""
    from . import models
    from .profile_version_migration import _has_unique
    insp = inspect(engine)
    tables = set(insp.get_table_names())
    facts = {
        "tuning_profiles_present": "tuning_profiles" in tables,
        "surrogate_pk": False, "unique_key": False,
        "history_columns": {}, "history_indexes": {},
        "backfill_marker": False, "state_record_table": "server_meta" in tables,
        "audit_table": "schema_migration_audit" in tables,
    }
    if facts["tuning_profiles_present"]:
        pk = insp.get_pk_constraint("tuning_profiles").get("constrained_columns")
        facts["surrogate_pk"] = pk == ["profile_version_id"]
        facts["unique_key"] = bool(_has_unique(engine))
    for table, index in HISTORY_INDEXES:
        if table in tables:
            facts["history_columns"][table] = "profile_version_id" in {
                c["name"] for c in insp.get_columns(table)}
            facts["history_indexes"][table] = index in {i["name"] for i in insp.get_indexes(table)}
        else:
            facts["history_columns"][table] = False
            facts["history_indexes"][table] = False
    if facts["state_record_table"]:
        meta = models.ServerMeta.__table__
        with engine.connect() as conn:
            facts["backfill_marker"] = conn.execute(
                select(meta.c["key"]).where(meta.c["key"] == BACKFILL_MARKER)).first() is not None
    return facts


def _classify(facts: dict) -> tuple[str, bool, bool]:
    """(schema_class, schema_resumable, only_backfill_pending)."""
    structure = (facts["surrogate_pk"] and facts["unique_key"]
                 and all(facts["history_columns"].values())
                 and all(facts["history_indexes"].values()))
    marker = facts["backfill_marker"]
    if not facts["tuning_profiles_present"]:
        return "MISSING", True, False
    if structure and marker:
        return "COMPLETE", True, False
    touched = (facts["surrogate_pk"] or facts["unique_key"] or marker
               or any(facts["history_columns"].values())
               or any(facts["history_indexes"].values()))
    if not touched:
        return "OLD", True, False
    # Half-migrated. Resuming re-runs only idempotent, guarded steps, but a marker
    # without its structure (or structure on the old key) cannot be reasoned about.
    resumable = True
    if marker and not structure:
        resumable = False
    if not facts["surrogate_pk"] and (facts["unique_key"] or marker):
        resumable = False
    return "PARTIAL", resumable, structure and not marker


def _public_record(rec: dict | None) -> dict | None:
    if rec is None:
        return None
    return {"state": rec.get("s"), "step": rec.get("step"), "started_at": rec.get("st"),
            "heartbeat_at": rec.get("hb"), "finished_at": rec.get("fin"),
            "build": rec.get("b"), "error": rec.get("err"),
            "resume_approved": bool(rec.get("ok"))}


def _heartbeat_age(rec: dict, now: datetime) -> float | None:
    try:
        return max(0.0, (now - datetime.fromisoformat(rec["hb"])).total_seconds())
    except (KeyError, TypeError, ValueError):
        return None


def migration_status(engine, now: datetime | None = None) -> dict:
    """Pure read: derive the state from the real schema AND the recorded state."""
    now = now or _utcnow()
    facts = schema_facts(engine)
    schema_class, schema_resumable, backfill_only = _classify(facts)
    rec = read_record(engine) if facts["state_record_table"] else None
    recorded = rec.get("s") if rec else None
    age = _heartbeat_age(rec, now) if rec else None
    approved = bool(rec and rec.get("ok"))
    reconcile = False

    def done(state, reason, resumable):
        return {
            "migration": MIGRATION_NAME, "state": state, "reason": reason,
            "resumable": bool(resumable), "schema_class": schema_class, "schema": facts,
            "recorded": _public_record(rec), "heartbeat_age_seconds":
                None if age is None else int(age),
            "stale_after_seconds": STALE_AFTER_SECONDS, "build": build_id(),
            "reconcile": reconcile,
        }

    fresh = recorded == "IN_PROGRESS" and age is not None and age < STALE_AFTER_SECONDS
    if schema_class == "COMPLETE":
        if fresh:
            return done(IN_PROGRESS, "FINALISING", False)
        reconcile = recorded != "DONE"
        return done(HEALTHY, "OK" if not reconcile else "COMPLETE_RECORD_RECONCILED", False)
    if fresh:
        return done(IN_PROGRESS, "MIGRATION_RUNNING", False)
    if recorded == "IN_PROGRESS":
        return done(FAILED, "STALE_IN_PROGRESS_CRASHED", schema_resumable)
    if recorded in ("FAILED", "CORRUPT"):
        return done(FAILED, "RECORDED_FAILURE", schema_resumable)
    if recorded == "DONE":
        return done(FAILED, "RECORD_SAYS_DONE_SCHEMA_INCOMPLETE", schema_resumable)
    # No record, or NOT_STARTED (possibly an operator-approved resume).
    if schema_class in ("OLD", "MISSING"):
        return done(REQUIRED, "RESUME_APPROVED" if approved else "SCHEMA_OLD", True)
    if approved and schema_resumable:
        return done(REQUIRED, "RESUME_APPROVED", True)
    if backfill_only and not rec:
        return done(REQUIRED, "BACKFILL_PENDING", True)
    return done(FAILED, "PARTIAL_SCHEMA_NOT_APPROVED", schema_resumable)


def unknown_status(exc: BaseException) -> dict:
    return {"migration": MIGRATION_NAME, "state": UNKNOWN, "reason": "DATABASE_UNREADABLE",
            "resumable": False, "schema_class": None, "schema": None, "recorded": None,
            "heartbeat_age_seconds": None, "stale_after_seconds": STALE_AFTER_SECONDS,
            "build": build_id(), "reconcile": False, "error": safe_error(exc)}


def reconcile_done(engine) -> None:
    """Schema is complete but the record says otherwise (e.g. crash before DONE)."""
    rec = MigrationRecorder(engine)
    try:
        rec.audit("reconcile", "SKIP", "schema complete; state record set to DONE")
        rec._write(s="DONE", step="complete", fin=_iso(_utcnow()), err=None, ok=None)
    finally:
        rec.close()


def approve_retry(engine, operator_note: str = "") -> dict:
    """Operator action: allow the next start to resume a FAILED, resumable migration.

    Only clears the failure marker (state -> NOT_STARTED with ok=1). It changes no
    table. Raises ValueError unless the status is FAILED and resumable."""
    status = migration_status(engine)
    if status["state"] != FAILED or not status["resumable"]:
        raise ValueError(
            f"retry not allowed: state={status['state']} reason={status['reason']} "
            f"resumable={status['resumable']}")
    rec = MigrationRecorder(engine)
    try:
        rec.audit("operator_retry", "OK",
                  f"cleared {status['reason']} at step={(status['recorded'] or {}).get('step')}"
                  + (f"; {operator_note}" if operator_note else ""))
        rec.record = {"s": "NOT_STARTED", "b": build_id(), "ok": 1}
        rec._write()
    finally:
        rec.close()
    return migration_status(engine)


def audit_tail(engine, limit: int = 30) -> list[dict]:
    from . import models
    table = models.SchemaMigrationAudit.__table__
    if "schema_migration_audit" not in inspect(engine).get_table_names():
        return []
    with engine.connect() as conn:
        rows = conn.execute(select(table).order_by(table.c.id.desc()).limit(limit)).all()
    return [{"id": r.id, "ts": r.ts.isoformat() if r.ts else None, "step": r.step,
             "outcome": r.outcome, "detail": r.detail} for r in reversed(rows)]
