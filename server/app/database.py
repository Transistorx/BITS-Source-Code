"""Database engine, session management and schema initialisation.

The engine is created once per process with pool_pre_ping so a MySQL restart
(or a dropped WSL network path) recovers on the next request instead of
serving stale connections. Schema creation is idempotent (create_all), which
is the migration/initialisation logic: fresh databases come up complete,
existing databases are left untouched — completed runs and samples are never
dropped or rewritten by startup.
"""

import logging
from collections.abc import Generator
from threading import RLock

from sqlalchemy import create_engine, event, inspect, text
from sqlalchemy.orm import Session, sessionmaker
from sqlalchemy.pool import StaticPool

from .config import settings

_engine = None
_SessionLocal = None
_schema_initialized = False
_schema_lock = RLock()
# Set (once, until restart) when the profile-versioning migration was refused or
# failed, so init_db is not retried per request. Holds the 503 message.
_migration_failed: str | None = None
log = logging.getLogger("database")


def _make_engine(url: str):
    kwargs: dict = {"pool_pre_ping": True, "future": True}
    if url.startswith("sqlite"):
        # SQLite (tests / dev only): one shared connection so in-memory DBs
        # survive across sessions, and FK enforcement per connection.
        kwargs = {
            "connect_args": {"check_same_thread": False},
            "poolclass": StaticPool,
            "future": True,
        }
    engine = create_engine(url, **kwargs)
    if url.startswith("sqlite"):

        @event.listens_for(engine, "connect")
        def _fk_on(dbapi_conn, _record):  # pragma: no cover - driver hook
            dbapi_conn.execute("PRAGMA foreign_keys=ON")

    return engine


def get_engine():
    global _engine, _SessionLocal
    if _engine is None:
        _engine = _make_engine(settings.database_url)
        _SessionLocal = sessionmaker(bind=_engine, autoflush=False,
                                     expire_on_commit=False, future=True)
    return _engine


def reset_engine() -> None:
    """Dispose the engine (tests / forced reconnect). Next use recreates it."""
    global _engine, _SessionLocal, _schema_initialized, _migration_failed
    if _engine is not None:
        _engine.dispose()
    _engine = None
    _SessionLocal = None
    _schema_initialized = False
    _migration_failed = None


def init_db() -> None:
    """Create any missing tables and indexes. Never deletes or alters data."""
    global _schema_initialized, _migration_failed
    with _schema_lock:
        if _schema_initialized:
            return
        if _migration_failed:
            raise RuntimeError(_migration_failed)
        from . import models, models_ops  # noqa: F401 - ensure mappers are registered
        engine = get_engine()
        models.Base.metadata.create_all(engine)
        # Additive migrations for installs that already have the single-channel
        # telemetry schema. Every new field has a safe CH1/default value so
        # historical data remains queryable and unchanged.
        additions = {
            "dispense_runs": {
                "material_id": "VARCHAR(8) NOT NULL DEFAULT 'M1'",
                "pump_id": "VARCHAR(16) NOT NULL DEFAULT 'Pump 1'",
                "assigned_at": "DATETIME(3) NULL",
                "channel_id": "VARCHAR(8) NOT NULL DEFAULT 'CH1'",
                "relay_id": "VARCHAR(16) NOT NULL DEFAULT 'Relay 1'",
                "scale_id": "VARCHAR(16) NOT NULL DEFAULT 'Scale 1'",
                "profile_id": "VARCHAR(64) NULL",
                "profile_version": "INTEGER NULL",
                "boot_id": "VARCHAR(16) NULL",
                "missing_ranges": "JSON NULL",
                "ingest_json": "JSON NULL",
            },
            "weight_samples": {
                "material_id": "VARCHAR(8) NOT NULL DEFAULT 'M1'",
                "channel_id": "VARCHAR(8) NOT NULL DEFAULT 'CH1'",
            },
            "dispense_events": {"material_id": "VARCHAR(8) NOT NULL DEFAULT 'M1'"},
            "tuning_profiles": {"material_id": "VARCHAR(8) NOT NULL DEFAULT 'M1'"},
            "device_status": {
                "command_transport": "VARCHAR(8) NOT NULL DEFAULT 'HTTP'",
                "boot_id": "VARCHAR(16) NULL",
                "caps_json": "JSON NULL",
            },
            "device_commands": {
                "payload_json": "JSON NULL",
                "material_id": "VARCHAR(8) NULL",
                "pump_id": "VARCHAR(16) NULL",
                "assigned_at": "DATETIME(3) NULL",
                "held": "TINYINT(1) NOT NULL DEFAULT 0",
                "promoted": "TINYINT(1) NOT NULL DEFAULT 0",
                "ack_json": "JSON NULL",
                "published_at": "DATETIME(3) NULL",
                "delivered_via": "VARCHAR(8) NULL",
                "delivered_boot_id": "VARCHAR(16) NULL",
                "profile_id": "VARCHAR(64) NULL",
                "profile_version": "INTEGER NULL",
            },
        }
        inspector = inspect(engine)
        # MySQL: these statements (some UPDATEs scan whole tables, every start)
        # run on throw-away connections with the same 30 s lock-wait bound as the
        # migration, so a held table cannot hang init_db (and _schema_lock)
        # forever. They are idempotent and rewrite no already-correct row.
        from .profile_version_migration import _bounded_engine
        bounded, bounded_owned = _bounded_engine(engine)
        with bounded.begin() as conn:
            for table, columns in additions.items():
                existing = {column["name"] for column in inspector.get_columns(table)}
                for column, definition in columns.items():
                    if column not in existing:
                        conn.execute(text(
                            f"ALTER TABLE {table} ADD COLUMN {column} {definition}"
                        ))
            # Preserve historical records while inferring the second fixed
            # material from the already recorded physical channel.
            for table in ("dispense_runs", "weight_samples", "tuning_profiles"):
                conn.execute(text(f"UPDATE {table} SET material_id='M2' WHERE channel_id='CH2'"))
            conn.execute(text("UPDATE dispense_events SET material_id='M2' WHERE run_id IN "
                              "(SELECT run_id FROM dispense_runs WHERE material_id='M2')"))
            conn.execute(text("UPDATE dispense_runs SET pump_id='Pump 2' WHERE channel_id='CH2'"))
            indexes = {i["name"] for i in inspect(engine).get_indexes("dispense_runs")}
            if "ix_runs_channel_target_started" not in indexes:
                conn.execute(text(
                    "CREATE INDEX ix_runs_channel_target_started "
                    "ON dispense_runs (channel_id, target_g, started_at)"
                ))
            if "ix_runs_profile_version" not in indexes:
                conn.execute(text(
                    "CREATE INDEX ix_runs_profile_version "
                    "ON dispense_runs (profile_id, profile_version)"
                ))
            sample_indexes = {i["name"] for i in inspect(engine).get_indexes("weight_samples")}
            if "ix_samples_channel_timestamp" not in sample_indexes:
                conn.execute(text(
                    "CREATE INDEX ix_samples_channel_timestamp "
                    "ON weight_samples (channel_id, timestamp)"
                ))
            # Additive, portable (SQLite and MySQL) indexes for the command queue.
            command_indexes = {i["name"] for i in inspect(engine).get_indexes("device_commands")}
            for name, columns in (
                ("ix_device_commands_local_job", "local_job_id"),
                ("ix_device_commands_channel_state", "channel_id, state"),
                ("ix_device_commands_device_state_held", "device_id, state, held"),
            ):
                if name not in command_indexes:
                    conn.execute(text(f"CREATE INDEX {name} ON device_commands ({columns})"))
        if bounded_owned:
            bounded.dispose()
        # Per-material PID profile versioning: surrogate key, unique key and
        # history backfill. Idempotent; see profile_version_migration.py. The
        # run is gated by migration_state.migration_status (real schema + the
        # recorded state): only MIGRATION_REQUIRED migrates. A recorded failure,
        # a crashed or running migration and an unapproved half-migrated schema
        # are NOT retried here; they fail closed until an operator acts.
        from . import migration_state
        from .profile_version_migration import migrate_profile_versioning
        status = migration_state.migration_status(engine)
        if status["state"] == migration_state.HEALTHY:
            if status["reconcile"]:
                migration_state.reconcile_done(engine)
        elif status["state"] == migration_state.REQUIRED:
            try:
                migrate_profile_versioning(engine)
            except Exception as exc:
                # Sticky until restart: do not re-run heavy locking DDL/UPDATEs on
                # every request. Requests fail closed with 503 (see get_db).
                _migration_failed = (
                    "profile versioning migration did not complete "
                    f"({exc.__class__.__name__}); the server stays unavailable until the cause "
                    "is fixed and it is restarted. See the server log and "
                    "docs/cas-audit/12-profile-versioning-migration.md.")
                log.error("%s Reason: %s", _migration_failed, exc)
                raise
        else:
            _migration_failed = (
                f"profile versioning migration did not complete: {status['state']} "
                f"({status['reason']}); not retried automatically. Run "
                "scripts/migration_status.py and see "
                "docs/cas-audit/12-profile-versioning-migration.md.")
            log.error("%s", _migration_failed)
            raise RuntimeError(_migration_failed)
        # Ranged / staged PID profiles: additive columns, index, status log and a
        # one-time backfill (see ranged_profile_migration.py). Idempotent and
        # refuses on inconsistent data; never activates a profile.
        from .ranged_profile_migration import migrate_ranged_profiles, needs_migration
        if needs_migration(engine):
            try:
                migrate_ranged_profiles(engine)
            except Exception as exc:
                _migration_failed = (
                    "ranged profile migration did not complete "
                    f"({exc.__class__.__name__}); the server stays unavailable until the cause "
                    "is fixed and it is restarted. See the server log "
                    "(table schema_migration_audit, migration 'ranged_profiles').")
                log.error("%s Reason: %s", _migration_failed, exc)
                raise
        from sqlalchemy.orm import Session as OrmSession
        with OrmSession(engine) as db:
            for material_id, name, channel, pump, relay, scale in (
                ("M1", "Material A", "CH1", "Pump 1", "Relay 1", "Scale 1"),
                ("M2", "Material B", "CH2", "Pump 2", "Relay 2", "Scale 2"),
            ):
                if db.get(models.Material, material_id) is None:
                    db.add(models.Material(material_id=material_id, name=name,
                        channel_id=channel, pump_id=pump, relay_id=relay,
                        scale_id=scale, enabled=True))
            db.commit()
        _schema_initialized = True


def current_migration_status() -> dict:
    """Live migration status for the health endpoints. Read-only; never migrates.

    Derived from the real schema + recorded state, then overlaid with this
    process' own sticky failure: once init_db gave up, the process keeps
    answering 503 until restarted even if someone repaired the schema by hand."""
    from . import migration_state
    try:
        status = migration_state.migration_status(get_engine())
    except Exception as exc:
        return migration_state.unknown_status(exc)
    if _migration_failed and status["state"] in (migration_state.HEALTHY,
                                                 migration_state.REQUIRED):
        status = dict(status, state=migration_state.FAILED,
                      reason="PROCESS_STICKY_FAILURE_RESTART_REQUIRED")
    return status


def schema_ready() -> bool:
    """True once init_db completed in this process (requests are being served)."""
    return _schema_initialized and not _migration_failed


def database_ok() -> bool:
    try:
        with get_engine().connect() as conn:
            conn.execute(text("SELECT 1"))
        return True
    except Exception:
        return False


def get_db() -> Generator[Session, None, None]:
    """FastAPI dependency: one session per request, always closed."""
    try:
        if not _schema_initialized:
            init_db()
        if _SessionLocal is None:
            get_engine()
    except Exception as exc:
        from fastapi import HTTPException
        raise HTTPException(
            status_code=503,
            detail=_migration_failed or f"database unavailable: {exc.__class__.__name__}",
        ) from exc
    db = _SessionLocal()
    try:
        yield db
    except Exception:
        db.rollback()
        raise
    finally:
        db.close()
