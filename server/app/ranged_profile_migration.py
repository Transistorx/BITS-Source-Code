"""Idempotent, additive schema migration: ranged / staged / versioned PID profiles.

Adds nullable-or-defaulted columns to ``tuning_profiles`` (range, status, staged
fields, validation evidence, schema marker), the index ``ix_tuning_profiles_mat_chan_status``
and the append-only table ``profile_status_log``, then backfills the EXISTING rows
exactly once:

  * range  = [target_g, target_g], applicability 'exact', profile_schema 1;
  * status = 'active' for an active row, 'validated' for an inactive one
    (they were all operator-activatable before), 'deprecated' for a row with no
    target at all (the old NULL "channel-wide" key; it could never be used).

Safety rules (same spirit as profile_version_migration):
- NOTHING that identifies or tunes a profile is touched: profile_version_id,
  profile_id, version, material/channel, target_g, gains, ``active`` and
  created_at stay byte-identical, so every profile_hash is unchanged. The
  UNIQUE key and ``dispense_runs`` / ``device_commands`` are not altered.
- Every step first checks whether it is needed; a second run changes nothing.
  The backfill runs once, in one transaction, guarded by a marker row in
  ``server_meta`` (never re-run, so an operator's later status changes survive).
- It REFUSES (raises, changes nothing) if existing data is inconsistent: two
  active rows for the same pump and target, or an active row with no usable
  target.
- A new profile is NEVER activated by this migration.
"""

import logging
from datetime import datetime, timezone

from sqlalchemy import inspect, select, text

from .profile_version_migration import _bounded_engine

log = logging.getLogger("ranged_profile_migration")

MIGRATION_NAME = "ranged_profiles"
STATE_KEY = "ranged_profiles_migration_state"
MARKER = "ranged_profiles_backfilled"
INDEX_NAME = "ix_tuning_profiles_mat_chan_status"

COLUMNS = {
    "applicability": "VARCHAR(8) NOT NULL DEFAULT 'exact'",
    "target_min_g": "INTEGER NULL",
    "target_max_g": "INTEGER NULL",
    "status": "VARCHAR(12) NOT NULL DEFAULT 'draft'",
    "coarse_threshold_g": "INTEGER NULL",
    "fine_threshold_g": "INTEGER NULL",
    "micro_threshold_g": "INTEGER NULL",
    "coarse_min_on_ms": "INTEGER NULL",
    "fine_min_on_ms": "INTEGER NULL",
    "micro_min_on_ms": "INTEGER NULL",
    "settle_time_ms": "INTEGER NULL",
    "inflight_comp_g": "INTEGER NULL",
    "validated_min_g": "INTEGER NULL",
    "validated_max_g": "INTEGER NULL",
    "validation_ref": "VARCHAR(190) NULL",
    "flow_regime_note": "VARCHAR(255) NULL",
    "profile_schema": "SMALLINT NOT NULL DEFAULT 1",
    "status_changed_at": "DATETIME(3) NULL",
    "status_changed_by": "VARCHAR(64) NULL",
}


def _marker_present(engine) -> bool:
    from . import models
    meta = models.ServerMeta.__table__
    with engine.connect() as conn:
        return conn.execute(select(meta.c["key"]).where(meta.c["key"] == MARKER)).first() is not None


def needs_migration(engine) -> bool:
    """Pure read: True if any column / index / table / the backfill is missing."""
    insp = inspect(engine)
    tables = set(insp.get_table_names())
    if "tuning_profiles" not in tables:
        return False            # fresh DB: create_all builds the final shape
    if "profile_status_log" not in tables:
        return True
    if set(COLUMNS) - {c["name"] for c in insp.get_columns("tuning_profiles")}:
        return True
    if INDEX_NAME not in {i["name"] for i in insp.get_indexes("tuning_profiles")}:
        return True
    return not _marker_present(engine)


def _refuse_if_inconsistent(engine) -> None:
    with engine.connect() as conn:
        twins = conn.execute(text(
            "SELECT material_id, target_g, COUNT(*) FROM tuning_profiles WHERE active = 1 "
            "AND target_g IS NOT NULL GROUP BY material_id, target_g HAVING COUNT(*) > 1")).all()
        no_target = conn.execute(text(
            "SELECT COUNT(*) FROM tuning_profiles WHERE active = 1 "
            "AND (target_g IS NULL OR target_g < 1)")).scalar_one()
    if twins or no_target:
        message = (
            "ranged profile migration REFUSED: tuning_profiles has "
            f"{len(twins)} pump/target group(s) with more than one ACTIVE row "
            f"{[tuple(t[:2]) for t in twins[:5]]} and {no_target} active row(s) without a usable "
            "target_g. Nothing was changed. Fix these rows by hand, then restart.")
        log.error(message)
        raise RuntimeError(message)


def _add_columns(engine) -> list[str]:
    existing = {c["name"] for c in inspect(engine).get_columns("tuning_profiles")}
    missing = [n for n in COLUMNS if n not in existing]
    if not missing:
        return []
    with engine.begin() as conn:
        if engine.dialect.name == "mysql":     # one atomic ALTER
            conn.execute(text("ALTER TABLE tuning_profiles " + ", ".join(
                f"ADD COLUMN {n} {COLUMNS[n]}" for n in missing)))
        else:
            for n in missing:
                conn.execute(text(f"ALTER TABLE tuning_profiles ADD COLUMN {n} {COLUMNS[n]}"))
    return missing


def _backfill(engine) -> dict:
    from . import models
    meta = models.ServerMeta.__table__
    now = datetime.now(timezone.utc).replace(tzinfo=None)
    with engine.begin() as conn:
        if conn.execute(select(meta.c["key"]).where(meta.c["key"] == MARKER)).first() is not None:
            return {"skipped": True}
        who = {"now": now, "who": "migration:ranged-profiles"}
        conn.execute(text(
            "UPDATE tuning_profiles SET status = CASE WHEN active = 1 THEN 'active' "
            "ELSE 'validated' END, status_changed_at = :now, status_changed_by = :who "
            "WHERE target_min_g IS NULL AND target_g IS NOT NULL"), who)
        conn.execute(text(
            "UPDATE tuning_profiles SET status = 'deprecated', status_changed_at = :now, "
            "status_changed_by = :who WHERE target_min_g IS NULL AND target_g IS NULL"), who)
        conn.execute(text(
            "UPDATE tuning_profiles SET target_min_g = target_g, target_max_g = target_g "
            "WHERE target_min_g IS NULL AND target_g IS NOT NULL"))
        bad = conn.execute(text(
            "SELECT COUNT(*) FROM tuning_profiles WHERE (status = 'active') <> (active = 1)")).scalar_one()
        if bad:
            raise RuntimeError(f"ranged profile backfill left {bad} row(s) where status and active "
                               "disagree; rolled back")
        counts = dict(conn.execute(text(
            "SELECT status, COUNT(*) FROM tuning_profiles GROUP BY status")).all())
        conn.execute(meta.insert().values({"key": MARKER, "value": now.isoformat(timespec="seconds")}))
    log.warning("ranged profiles backfilled: %s", counts)
    return {"skipped": False, "by_status": counts}


def migrate_ranged_profiles(engine) -> dict:
    """Run every needed step (recorded in the audit trail); return what was done."""
    from . import models
    from .migration_state import MigrationRecorder

    work, owned = _bounded_engine(engine)
    rec = MigrationRecorder(engine, name=MIGRATION_NAME, state_key=STATE_KEY)
    summary: dict = {"columns_added": [], "index_added": False, "backfill": None}
    try:
        rec.begin()
        try:
            models.ProfileStatusLog.__table__.create(work, checkfirst=True)
            if not _marker_present(work):
                with rec.step("preflight"):
                    _refuse_if_inconsistent(work)
            with rec.step("add_columns"):
                summary["columns_added"] = _add_columns(work)
            if INDEX_NAME not in {i["name"] for i in inspect(work).get_indexes("tuning_profiles")}:
                with rec.step("add_index"):
                    with work.begin() as conn:
                        conn.execute(text(f"CREATE INDEX {INDEX_NAME} ON tuning_profiles "
                                          "(material_id, channel_id, status)"))
                summary["index_added"] = True
            with rec.step("backfill"):
                summary["backfill"] = _backfill(work)
        except BaseException as exc:
            rec.fail("run", exc)
            raise
        rec.finish()
        return summary
    finally:
        rec.close()
        if owned:
            work.dispose()
