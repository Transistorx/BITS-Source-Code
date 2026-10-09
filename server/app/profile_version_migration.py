"""Idempotent schema migration: per-material PID profile versioning.

Before: ``tuning_profiles`` was keyed by (profile_id, version), so the version
counter was shared by both pumps and "default v8" could exist only once.

After: ``tuning_profiles.profile_version_id`` (BIGINT auto-increment) is the
primary key, (profile_id, material_id, version) is UNIQUE, and
``device_commands`` / ``dispense_runs`` gain a nullable ``profile_version_id``
(not a foreign key) that is backfilled from their old
(profile_id, material_id, profile_version) snapshot.

Safety rules:
- Called from ``database.init_db`` on every start; every step first checks
  whether it is needed, so a second run changes nothing.
- No profile gain, version number or history row is ever modified or deleted.
  Unmatched references keep a NULL id and their snapshot columns.
- If the existing data would violate the new unique key the migration refuses
  to run and raises a clear error instead of touching anything.
- MySQL does it in one ALTER (atomic per statement). SQLite (tests / dev only)
  rebuilds the table inside one transaction.
"""

import logging
from datetime import datetime, timezone

from sqlalchemy import inspect, select, text
from sqlalchemy.exc import DBAPIError

log = logging.getLogger("profile_version_migration")

UNIQUE_NAME = "uq_tuning_profile_material_version"
BACKFILL_MARKER = "profile_version_id_backfilled"
HISTORY_INDEXES = (
    ("device_commands", "ix_device_commands_profile_version_id"),
    ("dispense_runs", "ix_dispense_runs_profile_version_id"),
)


LOCK_WAIT_SECONDS = 30


def _bounded_engine(engine):
    """MySQL: a private engine whose connections wait at most LOCK_WAIT_SECONDS for
    metadata/row locks, so a held table cannot hang init_db (and _schema_lock)
    forever. The SET SESSION values live only on these throw-away connections, so
    nothing needs restoring on the shared pool. Other dialects: the engine as is.
    Returns (engine_to_use, owned)."""
    if engine.dialect.name != "mysql":
        return engine, False
    from sqlalchemy import create_engine, event
    from sqlalchemy.pool import NullPool

    bounded = create_engine(engine.url, poolclass=NullPool, future=True)

    @event.listens_for(bounded, "connect")
    def _limit_lock_waits(dbapi_conn, _record):  # pragma: no cover - needs MySQL
        cur = dbapi_conn.cursor()
        try:
            cur.execute(f"SET SESSION lock_wait_timeout={LOCK_WAIT_SECONDS}")
            cur.execute(f"SET SESSION innodb_lock_wait_timeout={LOCK_WAIT_SECONDS}")
        finally:
            cur.close()

    return bounded, True


def migrate_profile_versioning(engine) -> dict:
    """Run every needed step; return what was done (for logs and tests).

    On MySQL a lock wait timeout (error 1205, or 1205-like metadata timeout) surfaces as an exception,
    which init_db treats as a FAILED migration (sticky until restart).

    Every step is recorded (state record + append-only audit, separate connection)
    by ``migration_state.MigrationRecorder``. This function does NOT decide whether
    a run is allowed: ``database.init_db`` does that from ``migration_status``."""
    from .migration_state import MigrationRecorder

    work, owned = _bounded_engine(engine)
    recorder = MigrationRecorder(engine)
    try:
        recorder.begin()
        try:
            summary = _migrate(work, recorder)
        except BaseException as exc:
            recorder.fail("run", exc)    # no-op if a step already recorded it
            raise
        recorder.finish()
        return summary
    finally:
        recorder.close()
        if owned:
            work.dispose()


def _migrate(engine, rec) -> dict:
    summary = {"rebuilt": False, "unique_added": False, "backfill": None}
    columns = {c["name"] for c in inspect(engine).get_columns("tuning_profiles")}
    if "profile_version_id" not in columns:
        with rec.step("swap_primary_key"):
            _refuse_if_unique_key_would_fail(engine)
            if engine.dialect.name == "mysql":
                _mysql_swap_primary_key(engine)
            else:
                _sqlite_rebuild(engine)
        summary["rebuilt"] = True
        log.warning("tuning_profiles migrated: surrogate profile_version_id is now the "
                    "primary key; unique key %s added", UNIQUE_NAME)
    elif not _has_unique(engine):
        with rec.step("add_unique_key"):
            _refuse_if_unique_key_would_fail(engine)
            with engine.begin() as conn:
                conn.execute(text(
                    f"CREATE UNIQUE INDEX {UNIQUE_NAME} "
                    "ON tuning_profiles (profile_id, material_id, version)"))
        summary["unique_added"] = True
    else:
        rec.skip("primary_key_and_unique_key", "already present")
    # One step per table: on MySQL every DDL commits on its own anyway.
    for table, name in HISTORY_INDEXES:
        has_column = "profile_version_id" in {c["name"] for c in inspect(engine).get_columns(table)}
        has_index = name in {i["name"] for i in inspect(engine).get_indexes(table)}
        if has_column and has_index:
            rec.skip(f"history_{table}", "column and index already present")
            continue
        with rec.step(f"history_{table}"):
            if not has_column:
                with engine.begin() as conn:
                    conn.execute(text(f"ALTER TABLE {table} ADD COLUMN profile_version_id BIGINT NULL"))
            if not has_index:
                with engine.begin() as conn:
                    conn.execute(text(f"CREATE INDEX {name} ON {table} (profile_version_id)"))
    if _marker_present(engine):
        rec.skip("backfill", "marker present; backfill never re-runs")
    else:
        with rec.step("backfill"):
            summary["backfill"] = _backfill_links(engine)
    return summary


def _marker_present(engine) -> bool:
    from . import models

    meta = models.ServerMeta.__table__
    with engine.connect() as conn:
        return conn.execute(select(meta.c["value"]).where(meta.c["key"] == BACKFILL_MARKER)).first() is not None


def _has_unique(engine) -> bool:
    insp = inspect(engine)
    names = {u.get("name") for u in insp.get_unique_constraints("tuning_profiles")}
    names |= {i["name"] for i in insp.get_indexes("tuning_profiles") if i.get("unique")}
    return UNIQUE_NAME in names


def _refuse_if_unique_key_would_fail(engine) -> None:
    with engine.connect() as conn:
        dupes = conn.execute(text(
            "SELECT profile_id, material_id, version, COUNT(*) FROM tuning_profiles "
            "GROUP BY profile_id, material_id, version HAVING COUNT(*) > 1")).all()
        null_material = conn.execute(text(
            "SELECT COUNT(*) FROM tuning_profiles WHERE material_id IS NULL")).scalar_one()
    if dupes or null_material:
        message = (
            "profile versioning migration REFUSED: tuning_profiles has "
            f"{len(dupes)} duplicate (profile_id, material_id, version) group(s) "
            f"{[tuple(d[:3]) for d in dupes[:5]]} and {null_material} row(s) with NULL "
            "material_id. Nothing was changed. Resolve these rows by hand, then restart.")
        log.error(message)
        raise RuntimeError(message)


def _mysql_swap_primary_key(engine) -> None:
    # One atomic statement: the old composite PK goes away in the same ALTER
    # that installs the surrogate PK and the new unique key.
    with engine.begin() as conn:
        conn.execute(text(
            "ALTER TABLE tuning_profiles "
            "DROP PRIMARY KEY, "
            "ADD COLUMN profile_version_id BIGINT NOT NULL AUTO_INCREMENT FIRST, "
            "ADD PRIMARY KEY (profile_version_id), "
            f"ADD UNIQUE KEY {UNIQUE_NAME} (profile_id, material_id, version)"))


def _sqlite_rebuild(engine) -> None:
    """create new, copy, swap - all inside one transaction (SQLite only)."""
    from . import models

    table = models.TuningProfile.__table__
    insp = inspect(engine)
    old_columns = {c["name"] for c in insp.get_columns("tuning_profiles")}
    old_indexes = [i["name"] for i in insp.get_indexes("tuning_profiles")]
    copy = [c.name for c in table.columns
            if c.name != "profile_version_id" and c.name in old_columns]
    cols = ", ".join(copy)
    order = ", ".join(n for n in ("created_at", "profile_id", "material_id", "version")
                      if n in old_columns)
    with engine.connect() as conn:
        conn.exec_driver_sql("BEGIN")  # pysqlite does not open a txn for DDL
        try:
            conn.exec_driver_sql("ALTER TABLE tuning_profiles RENAME TO tuning_profiles_pv_old")
            for name in old_indexes:
                conn.exec_driver_sql(f'DROP INDEX "{name}"')
            table.create(conn)
            conn.exec_driver_sql(
                f"INSERT INTO tuning_profiles ({cols}) SELECT {cols} "
                f"FROM tuning_profiles_pv_old ORDER BY {order}")
            old_n = conn.exec_driver_sql("SELECT COUNT(*) FROM tuning_profiles_pv_old").scalar()
            new_n = conn.exec_driver_sql("SELECT COUNT(*) FROM tuning_profiles").scalar()
            if old_n != new_n:
                raise RuntimeError(f"tuning_profiles rebuild copied {new_n} of {old_n} rows")
            conn.exec_driver_sql("DROP TABLE tuning_profiles_pv_old")
            conn.commit()
        except BaseException:
            conn.rollback()
            raise


def _backfill_links(engine) -> dict | None:
    """Link every resolvable history row to its exact old profile row, once."""
    from . import models

    meta = models.ServerMeta.__table__
    with engine.begin() as conn:
        done = conn.execute(select(meta.c["value"]).where(meta.c["key"] == BACKFILL_MARKER)).first()
        if done is not None:
            return None
        report = {}
        for table in ("device_commands", "dispense_runs"):
            _backfill_table(conn, table)
            linked = conn.execute(text(
                f"SELECT COUNT(*) FROM {table} WHERE profile_version_id IS NOT NULL")).scalar_one()
            unmatched = conn.execute(text(
                f"SELECT COUNT(*) FROM {table} WHERE profile_version_id IS NULL "
                "AND profile_id IS NOT NULL AND profile_version IS NOT NULL")).scalar_one()
            report[table] = {"linked": linked, "unmatched_left_null": unmatched}
        stamp = datetime.now(timezone.utc).replace(tzinfo=None).isoformat(timespec="seconds")
        conn.execute(meta.insert().values({"key": BACKFILL_MARKER, "value": stamp}))
        log.warning("profile_version_id backfill: %s", report)
        _warn_live_unlinked_jobs(conn)
        return report


# MySQL: 1267 illegal mix of collations, 1270/1271 the multi-column variants.
_COLLATION_ERRORS = {1267, 1270, 1271}


# "When the pin was made": a job is pinned at created_at; a run used its gains
# from started_at. A link is only made if the row provably existed by then.
_PIN_TIME_COLUMN = {"device_commands": "created_at", "dispense_runs": "started_at"}


def _backfill_table(conn, table: str) -> None:
    when = f"{table}.{_PIN_TIME_COLUMN[table]}"
    try:
        conn.execute(text(
            f"UPDATE {table} SET profile_version_id = ("
            "SELECT p.profile_version_id FROM tuning_profiles p "
            f"WHERE p.profile_id = {table}.profile_id "
            f"AND p.material_id = {table}.material_id "
            f"AND p.version = {table}.profile_version "
            f"AND p.created_at IS NOT NULL AND {when} IS NOT NULL "
            f"AND p.created_at <= {when}) "
            "WHERE profile_version_id IS NULL AND profile_id IS NOT NULL "
            "AND profile_version IS NOT NULL AND material_id IS NOT NULL"))
    except DBAPIError as exc:
        code = exc.orig.args[0] if getattr(exc.orig, "args", None) else None
        if code in _COLLATION_ERRORS:
            message = (
                f"profile_version_id backfill FAILED on {table}: MySQL collation mismatch "
                f"(error {code}) between tuning_profiles and {table} "
                "profile_id/material_id. Nothing was changed and startup is stopped. "
                "Align the column collations (see docs/cas-audit/12-profile-versioning-migration.md "
                "pre-flight: SHOW FULL COLUMNS), then restart.")
            log.error(message)
            raise RuntimeError(message) from exc
        raise


def _warn_live_unlinked_jobs(conn) -> None:
    """Jobs that can still reach the device but could not be linked to a row."""
    rows = conn.execute(text(
        "SELECT id FROM device_commands WHERE command_type = 'JOB' "
        "AND profile_version_id IS NULL "
        "AND state IN ('PENDING', 'DELIVERED', 'QUEUED', 'FAILED') ORDER BY id")).all()
    if rows:
        ids = [r[0] for r in rows]
        log.warning(
            "profile versioning: %d live JOB(s) have no linked profile row "
            "(profile_version_id NULL) in PENDING/DELIVERED/QUEUED/FAILED: ids %s. They will "
            "be refused by the device (no gains) unless a profile row that predates them "
            "matches; review or cancel them. See docs/cas-audit/12-profile-versioning-migration.md.",
            len(ids), ids[:50])
