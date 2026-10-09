"""READ-ONLY verification of the profile-versioning migration.

    python scripts/verify_profile_migration.py snapshot --out before.json   # BEFORE migrating
    python scripts/verify_profile_migration.py verify --baseline before.json  # AFTER

Runs only SELECTs (and one read-only ORM session that is rolled back). It never
creates, alters or writes anything and never prints the password. Same guard as the
other helpers: refuses unless the resolved database name ends in '_dryrun' or
--i-know is given. Intended for the disposable rehearsal database.

Exit code 0 only if every check passes. See docs/cas-audit/12-profile-versioning-migration.md
section 'Verification checklist' for the equivalent SQL and expected results.
"""

import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _target  # noqa: E402

COUNT_TABLES = ("tuning_profiles", "device_commands", "dispense_runs", "weight_samples",
                "dispense_events", "materials", "device_status", "server_meta")
# Old-column checksums: the migration only ADDS profile_version_id to these.
CHECKSUM_TABLES = {
    "tuning_profiles": ("profile_id", "material_id", "version"),
    "device_commands": ("id",),
    "dispense_runs": ("run_id",),
}
LIVE_STATES = ("PENDING", "DELIVERED", "QUEUED", "FAILED")
# Wire shape of one command as the controller receives it (routes/operations.py).
WIRE_BASE_KEYS = {"command_id", "command_type", "target_g", "material_id", "priority",
                  "local_job_id", "channel_id", "profile_id", "profile_version"}
WIRE_PROFILE_KEYS = {"profile_id", "version", "kp", "ki", "kd", "tolerance_g",
                     "max_overshoot_g", "max_duration_ms", "window_ms", "min_on_ms", "min_off_ms"}


def _migration_keys():
    from app.migration_state import STATE_KEY
    from app.profile_version_migration import BACKFILL_MARKER
    from app import ranged_profile_migration as ranged
    return (BACKFILL_MARKER, STATE_KEY, ranged.MARKER, ranged.STATE_KEY)


def _table(engine, name):
    from sqlalchemy import MetaData, Table
    return Table(name, MetaData(), autoload_with=engine)


def count_rows(engine) -> dict:
    from sqlalchemy import func, select
    counts = {}
    with engine.connect() as conn:
        for name in COUNT_TABLES:
            table = _table(engine, name)
            query = select(func.count()).select_from(table)
            if name == "server_meta":     # the migration adds its own two keys
                query = query.where(table.c["key"].notin_(_migration_keys()))
            counts[name] = conn.execute(query).scalar_one()
    return counts


def checksum_table(engine, name: str) -> dict:
    """sha256 over every OLD column (sorted by name; profile_version_id excluded)."""
    from sqlalchemy import select
    table = _table(engine, name)
    from app.ranged_profile_migration import COLUMNS as RANGED_COLUMNS   # additive, later migration
    columns = sorted(c.name for c in table.columns
                     if c.name != "profile_version_id" and c.name not in RANGED_COLUMNS)
    order = [table.c[c] for c in CHECKSUM_TABLES[name]]
    digest = hashlib.sha256()
    rows = 0
    with engine.connect() as conn:
        for row in conn.execute(select(*[table.c[c] for c in columns]).order_by(*order)):
            digest.update(repr(tuple(row)).encode("utf-8"))
            rows += 1
    return {"columns": columns, "rows": rows, "sha256": digest.hexdigest()}


def foreign_keys(engine) -> list:
    from sqlalchemy import inspect
    insp = inspect(engine)
    found = []
    for table in sorted(insp.get_table_names()):
        for fk in insp.get_foreign_keys(table):
            found.append([table, list(fk["constrained_columns"]), fk["referred_table"],
                          list(fk["referred_columns"])])
    return sorted(found)


def live_job_pins(engine) -> dict:
    from sqlalchemy import select
    table = _table(engine, "device_commands")
    query = select(table.c["id"], table.c["profile_id"], table.c["material_id"],
                   table.c["profile_version"]).where(
        table.c["command_type"] == "JOB", table.c["state"].in_(LIVE_STATES))
    with engine.connect() as conn:
        return {str(r[0]): [r[1], r[2], r[3]] for r in conn.execute(query)}


def take_snapshot(engine) -> dict:
    return {
        "format": 1, "taken_at": datetime.now(timezone.utc).replace(tzinfo=None).isoformat(
            timespec="seconds"),
        "database": engine.url.database,
        "counts": count_rows(engine),
        "checksums": {name: checksum_table(engine, name) for name in CHECKSUM_TABLES},
        "foreign_keys": foreign_keys(engine),
        "live_job_pins": live_job_pins(engine),
    }


def _result(name, ok, detail, note=False):
    return {"check": name, "status": "NOTE" if note else ("PASS" if ok else "FAIL"),
            "detail": detail}


def _scalar(engine, sql, **params):
    from sqlalchemy import text
    with engine.connect() as conn:
        return conn.execute(text(sql), params).scalar_one()


def _rows(engine, sql):
    from sqlalchemy import text
    with engine.connect() as conn:
        return [tuple(r) for r in conn.execute(text(sql))]


def run_checks(engine, baseline: dict, reviewed_unlinked=()) -> list:
    from sqlalchemy import inspect
    results = []
    insp0 = inspect(engine)
    unmigrated = [t for t in ("tuning_profiles", "device_commands", "dispense_runs")
                  if "profile_version_id" not in {c["name"] for c in insp0.get_columns(t)}]
    if unmigrated:
        return [_result("schema_migrated", False,
                        f"profile_version_id missing on {unmigrated}; run the migration first")]
    # 1. row counts
    counts = count_rows(engine)
    diffs = {k: [baseline["counts"].get(k), v] for k, v in counts.items()
             if baseline["counts"].get(k) != v}
    results.append(_result("row_counts_equal", not diffs,
                           f"before/after differ: {diffs}" if diffs else str(counts)))
    # 2. uniqueness / key structure
    insp = inspect(engine)
    pk = insp.get_pk_constraint("tuning_profiles")["constrained_columns"]
    uniques = [u["column_names"] for u in insp.get_unique_constraints("tuning_profiles")
               if u["name"] == "uq_tuning_profile_material_version"]
    uniques += [i["column_names"] for i in insp.get_indexes("tuning_profiles")
                if i["name"] == "uq_tuning_profile_material_version" and i.get("unique")]
    ok = pk == ["profile_version_id"] and uniques and uniques[0] == [
        "profile_id", "material_id", "version"]
    results.append(_result("keys_primary_and_unique", bool(ok),
                           f"pk={pk} uq_tuning_profile_material_version={uniques[:1]}"))
    dupes = _scalar(engine, "SELECT COUNT(*) FROM (SELECT 1 FROM tuning_profiles GROUP BY "
                            "profile_id, material_id, version HAVING COUNT(*) > 1) d")
    results.append(_result("no_duplicate_profile_versions", dupes == 0, f"duplicate groups={dupes}"))
    cols_ok = all(
        "profile_version_id" in {c["name"] for c in insp.get_columns(t)} for t in
        ("device_commands", "dispense_runs"))
    results.append(_result("history_columns_present", cols_ok, "device_commands, dispense_runs"))
    # 3. foreign keys unchanged, and deliberately none to tuning_profiles
    fks = foreign_keys(engine)
    results.append(_result("foreign_keys_identical", fks == baseline["foreign_keys"],
                           f"before={baseline['foreign_keys']} after={fks}"
                           if fks != baseline["foreign_keys"] else f"{len(fks)} foreign key(s)"))
    expected = {("weight_samples", "dispense_runs"), ("dispense_events", "dispense_runs")}
    have = {(f[0], f[2]) for f in fks}
    results.append(_result("run_child_foreign_keys_intact", expected <= have, f"found={sorted(have)}"))
    to_profiles = [f for f in fks if f[2] == "tuning_profiles"]
    results.append(_result("no_fk_to_tuning_profiles", not to_profiles,
                           "deliberately none (history must outlive a deleted profile)"
                           if not to_profiles else f"unexpected: {to_profiles}"))
    # 4. unchanged-data checksum of every old column
    for name in CHECKSUM_TABLES:
        now = checksum_table(engine, name)
        before = baseline["checksums"].get(name)
        same = before is not None and before["sha256"] == now["sha256"] and before["rows"] == now["rows"]
        results.append(_result(f"checksum_unchanged_{name}", same,
                               f"rows={now['rows']} sha256={now['sha256'][:16]}"
                               + ("" if same else f" (before {before and before['sha256'][:16]})")))
    # 5. historical references: linked rows agree with their profile row
    for table, when in (("device_commands", "created_at"), ("dispense_runs", "started_at")):
        linked = _scalar(engine, f"SELECT COUNT(*) FROM {table} WHERE profile_version_id IS NOT NULL")
        unlinked = _scalar(engine, f"SELECT COUNT(*) FROM {table} WHERE profile_version_id IS NULL "
                                   "AND profile_id IS NOT NULL AND profile_version IS NOT NULL")
        bad = _scalar(engine, f"SELECT COUNT(*) FROM {table} h JOIN tuning_profiles p "
                              "ON p.profile_version_id = h.profile_version_id WHERE "
                              "h.profile_id <> p.profile_id OR p.version <> h.profile_version "
                              "OR COALESCE(h.material_id, '') <> p.material_id")
        dangling = _scalar(engine, f"SELECT COUNT(*) FROM {table} h LEFT JOIN tuning_profiles p "
                                   "ON p.profile_version_id = h.profile_version_id "
                                   "WHERE h.profile_version_id IS NOT NULL "
                                   "AND p.profile_version_id IS NULL")
        late = _scalar(engine, f"SELECT COUNT(*) FROM {table} h JOIN tuning_profiles p "
                               "ON p.profile_version_id = h.profile_version_id "
                               f"WHERE p.created_at > h.{when}")
        results.append(_result(f"references_consistent_{table}", bad == 0 and dangling == 0 and late == 0,
                               f"linked={linked} unlinked_left_null={unlinked} "
                               f"snapshot_mismatch={bad} dangling={dangling} profile_newer_than_use={late}"))
    # 6. queued / live jobs
    reviewed = {int(i) for i in reviewed_unlinked}
    states = ",".join(f"'{s}'" for s in LIVE_STATES)
    unlinked_live = [r for r in _rows(
        engine, "SELECT id, state, material_id, profile_id, profile_version FROM device_commands "
                f"WHERE command_type = 'JOB' AND profile_version_id IS NULL AND state IN ({states}) "
                "ORDER BY id") if r[0] not in reviewed]
    results.append(_result("live_jobs_all_linked_or_reviewed", not unlinked_live,
                           "none" if not unlinked_live else f"UNREVIEWED unlinked live jobs: {unlinked_live}"))
    pins_before = baseline.get("live_job_pins", {})
    pins_now = live_job_pins(engine)
    moved = {k: [v, pins_now.get(k)] for k, v in pins_before.items()
             if k in pins_now and pins_now[k] != v}
    results.append(_result("live_job_pins_unchanged", not moved,
                           f"changed: {moved}" if moved else f"{len(pins_before)} live job(s) compared"))
    # 7. profile identity: one id on both pumps resolves to two rows
    shared = _rows(engine, "SELECT profile_id, version, COUNT(*), COUNT(DISTINCT profile_version_id) "
                           "FROM tuning_profiles GROUP BY profile_id, version "
                           "HAVING COUNT(DISTINCT material_id) > 1")
    broken = [s for s in shared if s[2] != s[3]]
    results.append(_result("shared_profile_ids_resolve_to_distinct_rows", not broken,
                           f"{len(shared)} id+version pair(s) owned by both pumps; broken={broken}",
                           note=not shared))
    # 8. controller wire compatibility, from the app's own (read-only) serialiser
    results.append(_wire_check(engine))
    # 9. health / readiness state
    from app.migration_state import HEALTHY, migration_status
    status = migration_status(engine)
    results.append(_result("migration_status_healthy", status["state"] == HEALTHY,
                           f"state={status['state']} reason={status['reason']} "
                           f"resumable={status['resumable']}"))
    return results


def _wire_check(engine) -> dict:
    from sqlalchemy import select
    from sqlalchemy.orm import Session

    from app.models import DeviceCommand
    from app.routes.operations import command_wire

    with Session(engine) as db:
        try:
            rows = list(db.scalars(select(DeviceCommand).where(
                DeviceCommand.command_type == "JOB", DeviceCommand.profile_version_id.is_not(None),
                DeviceCommand.profile_id.is_not(None), DeviceCommand.profile_version.is_not(None)
            ).order_by(DeviceCommand.id.desc()).limit(25)))
            problems = []
            for row in rows:
                item = command_wire(row, db)
                expected = WIRE_BASE_KEYS | set(row.payload_json or {}) | {"profile"}
                if set(item) != expected:
                    problems.append((row.id, "keys", sorted(set(item) ^ expected)))
                    continue
                profile = item["profile"]
                if profile is None:
                    problems.append((row.id, "profile is null (device would refuse)"))
                elif set(profile) != WIRE_PROFILE_KEYS:
                    problems.append((row.id, "profile keys", sorted(set(profile) ^ WIRE_PROFILE_KEYS)))
                elif profile["version"] != item["profile_version"]:
                    problems.append((row.id, "nested version != top-level"))
            if "profile_version_id" in json.dumps(
                    [command_wire(r, db) for r in rows[:1]], default=str):
                problems.append(("profile_version_id leaked onto the wire",))
        finally:
            db.rollback()
    if not rows:
        return _result("controller_wire_key_set", True,
                       "no linked JOB rows to check (create a pinned test job in the rehearsal DB)",
                       note=True)
    return _result("controller_wire_key_set", not problems,
                   f"{len(rows)} linked JOB(s) checked" if not problems else f"problems: {problems}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="mode", required=True)
    snap = sub.add_parser("snapshot", help="save pre-migration counts/checksums/foreign keys")
    snap.add_argument("--out", required=True)
    ver = sub.add_parser("verify", help="compare the migrated database to a snapshot")
    ver.add_argument("--baseline", required=True)
    ver.add_argument("--reviewed-unlinked", default="",
                     help="comma-separated device_commands ids of unlinked live jobs you reviewed")
    ver.add_argument("--json", action="store_true")
    for p in (snap, ver):
        p.add_argument("--i-know", action="store_true",
                       help="allow a database name not ending in _dryrun (dangerous)")
    args = parser.parse_args(argv)

    url, line = _target.resolve()
    print(line)
    if not _target.allowed(url, args.i_know):
        print(_target.REFUSAL)
        return 2
    from app import database
    engine = database.get_engine()
    try:
        if args.mode == "snapshot":
            Path(args.out).write_text(json.dumps(take_snapshot(engine), indent=2), encoding="utf-8")
            print(f"snapshot written: {args.out}")
            return 0
        baseline = json.loads(Path(args.baseline).read_text(encoding="utf-8"))
        reviewed = [x for x in args.reviewed_unlinked.split(",") if x.strip()]
        results = run_checks(engine, baseline, reviewed)
    finally:
        engine.dispose()
    if args.json:
        print(json.dumps(results, indent=2))
    else:
        for r in results:
            print(f"{r['status']:<5} {r['check']}: {r['detail']}")
    failed = [r for r in results if r["status"] == "FAIL"]
    print(f"{len(results) - len(failed)}/{len(results)} checks ok" + (" - FAILED" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
