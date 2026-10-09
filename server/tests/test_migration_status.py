"""Migration status (health/readiness), state record, audit trail and helper scripts.

SQLite only. The helper scripts are exercised against temporary SQLite files; no
MySQL, no live database. See docs/cas-audit/12-profile-versioning-migration.md.
"""

import importlib.util
import json
import sqlite3
from datetime import timedelta
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import inspect, text

from app import database, migration_state as ms
from app import profile_version_migration as pvm
from app.main import app

# Reuse the pre-versioning schema builder and seed of the migration tests.
from test_profile_version_migration import (  # noqa: F401  (old_db is a fixture)
    OLD_INDEXES,
    OLD_PROFILES,
    _rows,
    _seed,
    old_db,
)

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"


def _status(now=None):
    return ms.migration_status(database.get_engine(), now=now)


def _set_record(path, **rec):
    con = sqlite3.connect(path)
    con.execute("DELETE FROM server_meta WHERE key = ?", (ms.STATE_KEY,))
    con.execute("INSERT INTO server_meta (key, value) VALUES (?, ?)", (ms.STATE_KEY, ms._encode(rec)))
    con.commit()
    con.close()


def _heartbeat(minutes_ago=0.0):
    return ms._iso(ms._utcnow() - timedelta(minutes=minutes_ago))


def _prepared_engine():
    """Engine on the old schema with the new audit table present (as init_db does)."""
    from app import models
    engine = database.get_engine()
    models.Base.metadata.create_all(engine)
    return engine


# ------------------------------------------------------------ status function

def test_old_schema_is_migration_required(old_db):
    status = _status()
    assert status["state"] == ms.REQUIRED and status["reason"] == "SCHEMA_OLD"
    assert status["schema_class"] == "OLD" and status["resumable"] is True
    assert status["schema"]["surrogate_pk"] is False and status["schema"]["unique_key"] is False
    assert status["schema"]["backfill_marker"] is False
    assert status["recorded"] is None


def test_fully_migrated_is_healthy_and_records_done_with_audit(old_db):
    _seed(old_db)
    database.init_db()
    status = _status()
    assert status["state"] == ms.HEALTHY and status["reason"] == "OK"
    assert status["schema_class"] == "COMPLETE"
    assert all(v is True for k, v in status["schema"].items()
               if k in ("surrogate_pk", "unique_key", "backfill_marker"))
    assert status["recorded"]["state"] == "DONE" and status["recorded"]["finished_at"]
    audit = ms.audit_tail(database.get_engine(), 100)
    pairs = {(r["step"], r["outcome"]) for r in audit}
    for step in ("swap_primary_key", "history_device_commands", "history_dispense_runs", "backfill"):
        assert (step, "START") in pairs and (step, "OK") in pairs, step
    assert ("run", "START") in pairs and ("run", "OK") in pairs
    ids = [r["id"] for r in audit]
    assert ids == sorted(ids)
    # A restart of a HEALTHY database neither migrates nor writes new audit rows.
    count = len(ms.audit_tail(database.get_engine(), 1000))
    database.reset_engine()
    database.init_db()
    assert len(ms.audit_tail(database.get_engine(), 1000)) == count


def test_fresh_in_progress_is_distinguished_from_crashed(old_db):
    _prepared_engine()
    _set_record(old_db, s="IN_PROGRESS", step="backfill", hb=_heartbeat(1), st=_heartbeat(2))
    fresh = _status()
    assert fresh["state"] == ms.IN_PROGRESS and fresh["reason"] == "MIGRATION_RUNNING"
    assert fresh["resumable"] is False
    _set_record(old_db, s="IN_PROGRESS", step="backfill", hb=_heartbeat(60), st=_heartbeat(61))
    crashed = _status()
    assert crashed["state"] == ms.FAILED and crashed["reason"] == "STALE_IN_PROGRESS_CRASHED"
    assert crashed["resumable"] is True and crashed["stale_after_seconds"] == ms.STALE_AFTER_SECONDS
    # The clock is injectable: the same record is fresh a minute after its heartbeat.
    soon = ms._utcnow() - timedelta(minutes=59)
    assert _status(now=soon)["state"] == ms.IN_PROGRESS


def test_recorded_failure_is_failed_and_carries_no_sql(old_db):
    _prepared_engine()
    _set_record(old_db, s="FAILED", step="swap_primary_key", err="OperationalError/OperationalError code=1205",
                fin=_heartbeat(5))
    status = _status()
    assert status["state"] == ms.FAILED and status["reason"] == "RECORDED_FAILURE"
    assert status["resumable"] is True      # untouched old schema
    assert status["recorded"]["error"].startswith("OperationalError")
    assert "SELECT" not in json.dumps(status) and "ALTER" not in json.dumps(status)


def test_safe_error_never_contains_sql_or_params():
    from sqlalchemy.exc import OperationalError
    exc = OperationalError("ALTER TABLE tuning_profiles ... password=hunter2", {"p": "x"},
                           Exception(1205, "Lock wait timeout"))
    summary = ms.safe_error(exc)
    assert "hunter2" not in summary and "ALTER" not in summary and "1205" in summary
    assert ms.safe_error(ValueError("secret detail")) == "ValueError"


def test_half_migrated_schema_is_detected_from_the_real_schema_without_a_record(old_db):
    _seed(old_db)
    engine = _prepared_engine()
    pvm._sqlite_rebuild(engine)          # the key swap committed; nothing else happened
    status = _status()
    assert status["recorded"] is None
    assert status["schema_class"] == "PARTIAL"
    assert status["state"] == ms.FAILED and status["reason"] == "PARTIAL_SCHEMA_NOT_APPROVED"
    assert status["resumable"] is True
    assert status["schema"]["surrogate_pk"] is True
    assert status["schema"]["history_columns"] == {"device_commands": False, "dispense_runs": False}
    # ...and a stale "DONE" record does not hide it.
    _set_record(old_db, s="DONE", step="complete", hb=_heartbeat(1))
    stale = _status()
    assert stale["state"] == ms.FAILED and stale["reason"] == "RECORD_SAYS_DONE_SCHEMA_INCOMPLETE"


def test_marker_without_structure_is_not_resumable(old_db):
    _prepared_engine()
    with database.get_engine().begin() as conn:
        conn.execute(text("INSERT INTO server_meta (key, value) VALUES (:k, 'x')"),
                     {"k": pvm.BACKFILL_MARKER})
    status = _status()
    assert status["state"] == ms.FAILED and status["resumable"] is False
    with pytest.raises(ValueError, match="retry not allowed"):
        ms.approve_retry(database.get_engine())


def test_backfill_pending_without_a_record_is_required_but_with_a_failure_is_failed(old_db):
    _seed(old_db)
    database.init_db()
    with database.get_engine().begin() as conn:
        conn.execute(text("DELETE FROM server_meta WHERE key IN (:m, :s)"),
                     {"m": pvm.BACKFILL_MARKER, "s": ms.STATE_KEY})
    pending = _status()
    assert pending["state"] == ms.REQUIRED and pending["reason"] == "BACKFILL_PENDING"
    _set_record(old_db, s="FAILED", step="backfill", err="RuntimeError", fin=_heartbeat(1))
    failed = _status()
    assert failed["state"] == ms.FAILED and failed["resumable"] is True


def test_complete_schema_with_a_stale_failure_record_is_reconciled(old_db):
    _seed(old_db)
    database.init_db()
    _set_record(old_db, s="FAILED", step="complete", err="crash before DONE", fin=_heartbeat(5))
    status = _status()
    assert status["state"] == ms.HEALTHY and status["reconcile"] is True
    database.reset_engine()
    database.init_db()                        # reconciles the record
    assert _status()["recorded"]["state"] == "DONE"


# ------------------------------------------------- audit and retry gating

def test_audit_rows_survive_a_rolled_back_backfill_and_resume_needs_approval(old_db, monkeypatch):
    _seed(old_db)
    engine = _prepared_engine()
    real = pvm._warn_live_unlinked_jobs

    def boom(conn):
        raise RuntimeError("simulated crash after the marker insert, inside the transaction")

    monkeypatch.setattr(pvm, "_warn_live_unlinked_jobs", boom)
    with pytest.raises(RuntimeError, match="simulated"):
        pvm.migrate_profile_versioning(engine)
    # Backfill and marker rolled back together...
    assert _rows(old_db, "SELECT COUNT(*) FROM server_meta WHERE key = '%s'" % pvm.BACKFILL_MARKER) == [(0,)]
    assert _rows(old_db, "SELECT COUNT(*) FROM dispense_runs WHERE profile_version_id IS NOT NULL") == [(0,)]
    # ...but the structural steps committed and the audit rows are all there.
    audit = [(r["step"], r["outcome"]) for r in ms.audit_tail(engine, 100)]
    assert ("swap_primary_key", "OK") in audit
    assert ("backfill", "START") in audit and ("backfill", "FAIL") in audit
    assert ("backfill", "OK") not in audit
    status = _status()
    assert status["state"] == ms.FAILED and status["reason"] == "RECORDED_FAILURE"
    assert status["resumable"] is True and status["recorded"]["step"] == "backfill"
    assert "simulated" in status["recorded"]["error"]

    # A restart does NOT retry on its own.
    monkeypatch.setattr(pvm, "_warn_live_unlinked_jobs", real)
    calls = []
    real_migrate = pvm.migrate_profile_versioning
    monkeypatch.setattr(pvm, "migrate_profile_versioning",
                        lambda e: (calls.append(1), real_migrate(e))[1])
    database.reset_engine()
    with pytest.raises(RuntimeError, match="did not complete.*MIGRATION_FAILED"):
        database.init_db()
    assert calls == [] and _rows(old_db, "SELECT COUNT(*) FROM server_meta WHERE key = '%s'" % pvm.BACKFILL_MARKER) == [(0,)]
    # The operator clears the marker; only then the next start resumes.
    approved = ms.approve_retry(database.get_engine(), "unit test")
    assert approved["state"] == ms.REQUIRED and approved["reason"] == "RESUME_APPROVED"
    assert any(r["step"] == "operator_retry" for r in ms.audit_tail(database.get_engine(), 100))
    database.reset_engine()
    database.init_db()
    assert calls == [1]
    assert _status()["state"] == ms.HEALTHY
    assert _rows(old_db, "SELECT COUNT(*) FROM dispense_runs WHERE profile_version_id IS NOT NULL") == [(3,)]
    # Retry is refused when there is nothing to retry.
    with pytest.raises(ValueError, match="retry not allowed"):
        ms.approve_retry(database.get_engine())


def test_fresh_in_progress_blocks_startup_and_nothing_is_migrated(old_db, monkeypatch):
    _seed(old_db)
    _prepared_engine()
    _set_record(old_db, s="IN_PROGRESS", step="swap_primary_key", hb=_heartbeat(1))
    calls = []
    monkeypatch.setattr(pvm, "migrate_profile_versioning", lambda e: calls.append(1))
    database.reset_engine()
    with pytest.raises(RuntimeError, match="MIGRATION_IN_PROGRESS"):
        database.init_db()
    assert calls == []
    assert "profile_version_id" not in {c["name"] for c in inspect(database.get_engine()).get_columns("tuning_profiles")}


def test_failed_run_is_not_retried_on_later_restarts_until_reset(old_db, monkeypatch):
    con = sqlite3.connect(old_db)
    con.execute("DROP TABLE tuning_profiles")
    con.execute(OLD_PROFILES.replace("material_id VARCHAR(8) NOT NULL DEFAULT 'M1'", "material_id VARCHAR(8)"))
    con.execute("INSERT INTO tuning_profiles VALUES ('x', 1, NULL, 'CH1', 5000, 0.01, 0.001, 0.0, 20, 100, "
                "120000, 500, 40, 40, 0, '2026-01-01 00:00:00')")
    con.commit()
    con.close()
    calls = []
    real = pvm.migrate_profile_versioning
    monkeypatch.setattr(pvm, "migrate_profile_versioning", lambda e: (calls.append(1), real(e))[1])
    with pytest.raises(RuntimeError, match="REFUSED"):
        database.init_db()
    assert calls == [1]
    assert _status()["recorded"]["state"] == "FAILED"
    for _ in range(2):                       # two later restarts: still not retried
        database.reset_engine()
        with pytest.raises(RuntimeError, match="did not complete"):
            database.init_db()
    assert calls == [1]
    # The process-sticky overlay holds even if the record is approved meanwhile.
    cur = database.current_migration_status()
    assert cur["state"] == ms.FAILED
    # Once the data is fixed by hand and the operator approves, the next start resumes.
    con = sqlite3.connect(old_db)
    con.execute("UPDATE tuning_profiles SET material_id = 'M1'")
    con.commit()
    con.close()
    ms.approve_retry(database.get_engine(), "fixed material_id")
    database.reset_engine()
    database.init_db()
    assert calls == [1, 1] and _status()["state"] == ms.HEALTHY


# ------------------------------------------------------- health endpoints

NON_HEALTHY = ("fresh_in_progress", "stale_in_progress", "recorded_failure", "partial_unrecorded")


def _make_non_healthy(old_db, kind):
    engine = _prepared_engine()
    if kind == "fresh_in_progress":
        _set_record(old_db, s="IN_PROGRESS", step="backfill", hb=_heartbeat(1))
    elif kind == "stale_in_progress":
        _set_record(old_db, s="IN_PROGRESS", step="backfill", hb=_heartbeat(90))
    elif kind == "recorded_failure":
        _set_record(old_db, s="FAILED", step="swap_primary_key", err="RuntimeError", fin=_heartbeat(1))
    else:
        pvm._sqlite_rebuild(engine)
    database.reset_engine()


@pytest.mark.parametrize("kind", NON_HEALTHY)
def test_non_healthy_states_fail_closed_but_stay_diagnosable(old_db, kind):
    _seed(old_db)
    _make_non_healthy(old_db, kind)
    expected_state = ms.IN_PROGRESS if kind == "fresh_in_progress" else ms.FAILED
    with TestClient(app) as client:
        live = client.get("/health/live")
        assert live.status_code == 200
        body = live.json()
        assert body["status"] == "alive" and body["migration"]["state"] == expected_state
        ready = client.get("/health/ready")
        assert ready.status_code == 503
        rbody = ready.json()
        assert rbody["ready"] is False and rbody["migration"]["state"] == expected_state
        assert rbody["reason"] == rbody["migration"]["reason"]
        legacy = client.get("/health")
        assert legacy.status_code == 200
        lbody = legacy.json()
        assert set(lbody) >= {"status", "database", "last_telemetry_at", "run_count", "migration"}
        assert lbody["status"] == "degraded" and lbody["database"] != "up"
        assert lbody["migration"]["state"] == expected_state
        for endpoint in ("/api/v1/device/commands?device_id=bits-a4cf12ab34cd", "/api/v1/queue",
                         "/api/v1/device/status", "/api/v1/runs"):
            response = client.get(endpoint)
            assert response.status_code == 503, endpoint
            assert "did not complete" in response.json()["detail"]
        # Database-independent live ingress and static files stay reachable.
        assert client.get("/api/v1/live/telemetry").status_code == 200
    assert "profile_version_id" not in {c["name"] for c in inspect(database.get_engine()).get_columns("dispense_runs")} \
        or kind == "partial_unrecorded"


def test_healthy_database_is_ready_and_legacy_health_is_unchanged(client):
    legacy = client.get("/health").json()
    assert legacy["status"] == "ok" and legacy["database"] == "up"
    assert legacy["last_telemetry_at"] is None and legacy["run_count"] == 0
    assert legacy["migration"]["state"] == ms.HEALTHY
    live = client.get("/health/live")
    assert live.status_code == 200 and live.json()["migration"]["state"] == ms.HEALTHY
    ready = client.get("/health/ready")
    assert ready.status_code == 200
    assert ready.json()["ready"] is True and ready.json()["reason"] == "ok"
    assert client.get("/api/v1/queue").status_code == 200


def test_database_down_is_not_ready_but_live(client, monkeypatch):
    monkeypatch.setenv("DATABASE_URL", "mysql+pymysql://u:p@127.0.0.1:1/none?charset=utf8mb4")
    database.reset_engine()
    try:
        assert client.get("/health/live").status_code == 200
        ready = client.get("/health/ready")
        assert ready.status_code == 503 and ready.json()["reason"] == "DATABASE_DOWN"
        assert client.get("/health/live").json()["migration"]["state"] == ms.UNKNOWN
    finally:
        database.reset_engine()


def test_status_endpoints_do_not_trigger_a_migration(old_db, monkeypatch):
    """Old schema + the app started with the migration disabled: probing health
    must never migrate (only init_db does, and only from MIGRATION_REQUIRED)."""
    calls = []
    monkeypatch.setattr(pvm, "migrate_profile_versioning", lambda e: calls.append(1))
    database.reset_engine()
    monkeypatch.setattr(database, "init_db", lambda: None)
    import app.main as main_module
    monkeypatch.setattr(main_module, "init_db", lambda: None)
    with TestClient(app) as client:
        assert client.get("/health/live").json()["migration"]["state"] == ms.REQUIRED
        assert client.get("/health/ready").status_code == 503
        assert client.get("/health").status_code == 200
    assert calls == []


# ---------------------------------------------------------- helper scripts

def _load(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    import sys
    sys.path.insert(0, str(SCRIPTS))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(SCRIPTS))
    return module


@pytest.mark.parametrize("script, argv", [
    ("dryrun_profile_migration", []),
    ("migration_status", ["--retry"]),
    ("verify_profile_migration", ["snapshot", "--out", "unused.json"]),
])
def test_scripts_refuse_a_non_dryrun_database_and_never_print_the_password(
        script, argv, monkeypatch, capsys, tmp_path):
    monkeypatch.setenv("DATABASE_URL", "mysql+pymysql://dispense:hunter2pw@127.0.0.1:3306/dispense_telemetry")
    module = _load(script)
    monkeypatch.chdir(tmp_path)
    assert module.main(argv) == 2
    out = capsys.readouterr().out
    assert "hunter2pw" not in out
    assert "database='dispense_telemetry'" in out and "REFUSED" in out
    assert "DATABASE_URL_in_environment=yes" in out
    assert not (tmp_path / "unused.json").exists()


def test_guard_name_rule():
    target = _load("_target")
    assert target.is_dryrun_name("dispense_telemetry_dryrun")
    assert not target.is_dryrun_name("dispense_telemetry")
    assert not target.is_dryrun_name("dryrun_but_live")


def _dryrun_db(tmp_path, monkeypatch):
    """The pre-versioning schema in a file whose name ends in _dryrun."""
    path = tmp_path / "rehearsal_dryrun"
    monkeypatch.setenv("DATABASE_URL", "sqlite:///" + path.as_posix())
    database.reset_engine()
    from app import models
    engine = database.get_engine()
    models.Base.metadata.create_all(engine)
    with engine.begin() as conn:
        conn.execute(text("DROP TABLE tuning_profiles"))
        conn.execute(text(OLD_PROFILES))
        for ddl in OLD_INDEXES:
            conn.execute(text(ddl))
        for table in ("device_commands", "dispense_runs"):
            conn.execute(text(f"DROP INDEX ix_{table}_profile_version_id"))
            conn.execute(text(f"ALTER TABLE {table} DROP COLUMN profile_version_id"))
        conn.execute(text("DELETE FROM server_meta"))
        # A production database already has the two fixed material rows.
        for mid, name, ch, pump in (("M1", "Material A", "CH1", "Pump 1"), ("M2", "Material B", "CH2", "Pump 2")):
            conn.execute(text("INSERT INTO materials (material_id, name, channel_id, pump_id, relay_id, "
                              "scale_id, enabled) VALUES (:m, :n, :c, :p, :r, :s, 1)"),
                         {"m": mid, "n": name, "c": ch, "p": pump, "r": "Relay " + pump[-1],
                          "s": "Scale " + pump[-1]})
    database.reset_engine()
    return path


def test_status_script_is_read_only_by_default_and_retry_is_guarded(tmp_path, monkeypatch, capsys):
    path = _dryrun_db(tmp_path, monkeypatch)
    _seed(path)
    script = _load("migration_status")
    before = _rows(path, "SELECT COUNT(*) FROM server_meta"), _rows(path, "SELECT COUNT(*) FROM schema_migration_audit")
    assert script.main([]) == 1                      # not HEALTHY -> exit 1
    out = capsys.readouterr().out
    assert "state=MIGRATION_REQUIRED" in out and "password" not in out.lower()
    assert (_rows(path, "SELECT COUNT(*) FROM server_meta"),
            _rows(path, "SELECT COUNT(*) FROM schema_migration_audit")) == before
    # --retry on a database that is not failed is refused with exit code 3.
    assert script.main(["--retry"]) == 3
    # Make it a resumable failure: retry now clears the marker (no table is touched).
    _set_record(path, s="FAILED", step="swap_primary_key", err="RuntimeError", fin=_heartbeat(1))
    assert script.main(["--retry", "--note", "unit"]) == 0
    assert _status()["reason"] == "RESUME_APPROVED"
    assert _rows(path, "SELECT COUNT(*) FROM tuning_profiles") == [(4,)]


def test_verify_script_passes_on_a_clean_migration_and_catches_tampering(tmp_path, monkeypatch, capsys):
    path = _dryrun_db(tmp_path, monkeypatch)
    _seed(path)
    verify = _load("verify_profile_migration")
    snapshot_file = tmp_path / "before.json"
    assert verify.main(["snapshot", "--out", str(snapshot_file)]) == 0
    baseline = json.loads(snapshot_file.read_text(encoding="utf-8"))
    assert baseline["counts"]["tuning_profiles"] == 4 and baseline["counts"]["dispense_runs"] == 6
    assert "password" not in snapshot_file.read_text(encoding="utf-8").lower()

    dry = _load("dryrun_profile_migration")
    assert dry.main([]) == 0
    capsys.readouterr()

    # Two seeded live jobs have no linkable profile row -> unreviewed = FAIL.
    assert verify.main(["verify", "--baseline", str(snapshot_file)]) == 1
    out = capsys.readouterr().out
    assert "FAIL  live_jobs_all_linked_or_reviewed" in out
    assert out.count("FAIL ") == 1
    assert verify.main(["verify", "--baseline", str(snapshot_file),
                        "--reviewed-unlinked", "3,4"]) == 0
    out = capsys.readouterr().out
    assert "PASS  controller_wire_key_set" in out and "PASS  migration_status_healthy" in out
    assert "PASS  checksum_unchanged_dispense_runs" in out and "PASS  no_fk_to_tuning_profiles" in out

    engine = database.get_engine()
    results = {r["check"]: r for r in verify.run_checks(engine, baseline, [3, 4])}
    assert all(r["status"] != "FAIL" for r in results.values())

    # Tampering is detected: a changed old column, a lost row, a wrong link.
    with engine.begin() as conn:
        conn.execute(text("UPDATE tuning_profiles SET kp = 9.9 WHERE profile_id = 'lonely'"))
        conn.execute(text("DELETE FROM dispense_runs WHERE run_id = 'r-none'"))
        conn.execute(text("UPDATE dispense_runs SET profile_version_id = "
                          "(SELECT profile_version_id FROM tuning_profiles WHERE profile_id = 'lonely') "
                          "WHERE run_id = 'r-m1-v1'"))
    failed = {r["check"] for r in verify.run_checks(engine, baseline, [3, 4]) if r["status"] == "FAIL"}
    assert {"checksum_unchanged_tuning_profiles", "checksum_unchanged_dispense_runs",
            "row_counts_equal", "references_consistent_dispense_runs"} <= failed


def test_verify_script_writes_nothing(tmp_path, monkeypatch):
    path = _dryrun_db(tmp_path, monkeypatch)
    _seed(path)
    verify = _load("verify_profile_migration")
    engine = database.get_engine()
    snapshot = verify.take_snapshot(engine)

    def dump():
        con = sqlite3.connect(path)
        try:
            return list(con.iterdump())
        finally:
            con.close()

    before = dump()
    verify.run_checks(engine, snapshot, [])
    assert dump() == before
    source = (SCRIPTS / "verify_profile_migration.py").read_text(encoding="utf-8")
    for forbidden in ("init_db", "create_all", ".commit(", "INSERT ", "DELETE FROM", "ALTER TABLE", "DROP "):
        assert forbidden not in source, forbidden
