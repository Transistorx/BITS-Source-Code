"""init_db migration from the composite-PK tuning_profiles to the surrogate key.

Built on a throwaway SQLite file shaped like the pre-versioning schema
(PRIMARY KEY (profile_id, version); runs/commands with id+version only). The
live MySQL path is NOT exercised here - see docs/cas-audit/12-profile-versioning-migration.md.
"""

import sqlite3

import pytest
from sqlalchemy import inspect, text

from app import database

OLD_PROFILES = """
CREATE TABLE tuning_profiles (
    profile_id VARCHAR(64) NOT NULL,
    version INTEGER NOT NULL,
    material_id VARCHAR(8) NOT NULL DEFAULT 'M1',
    channel_id VARCHAR(8) NOT NULL,
    target_g INTEGER,
    kp FLOAT NOT NULL, ki FLOAT NOT NULL, kd FLOAT NOT NULL,
    tolerance_g INTEGER NOT NULL, max_overshoot_g INTEGER NOT NULL,
    max_duration_ms INTEGER NOT NULL, window_ms INTEGER NOT NULL,
    min_on_ms INTEGER NOT NULL, min_off_ms INTEGER NOT NULL,
    active BOOLEAN NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL,
    PRIMARY KEY (profile_id, version)
)"""
OLD_INDEXES = (
    "CREATE INDEX ix_tuning_profiles_material_id ON tuning_profiles (material_id)",
    "CREATE INDEX ix_tuning_profiles_channel_id ON tuning_profiles (channel_id)",
    "CREATE INDEX ix_tuning_profiles_target_g ON tuning_profiles (target_g)",
    "CREATE INDEX ix_tuning_profiles_active ON tuning_profiles (active)",
)


def _profile(pid, version, material, kp, active=0):
    channel = "CH1" if material == "M1" else "CH2"
    return (f"INSERT INTO tuning_profiles VALUES ('{pid}', {version}, '{material}', '{channel}', "
            f"5000, {kp}, 0.001, 0.0, 20, 100, 120000, 500, 40, 40, {active}, '2026-01-01 00:00:00')")


@pytest.fixture()
def old_db(tmp_path, monkeypatch):
    """A database in the previous schema, then pointed at by the app."""
    path = tmp_path / "old.db"
    monkeypatch.setenv("DATABASE_URL", "sqlite:///" + path.as_posix())
    database.reset_engine()
    engine = database.get_engine()
    from app import models
    models.Base.metadata.create_all(engine)
    with engine.begin() as conn:
        # Rewind to the old shape: composite-PK profiles, no profile_version_id.
        conn.execute(text("DROP TABLE tuning_profiles"))
        conn.execute(text(OLD_PROFILES))
        for ddl in OLD_INDEXES:
            conn.execute(text(ddl))
        for table in ("device_commands", "dispense_runs"):
            conn.execute(text(f"DROP INDEX ix_{table}_profile_version_id"))
            conn.execute(text(f"ALTER TABLE {table} DROP COLUMN profile_version_id"))
        conn.execute(text("DELETE FROM server_meta"))
    database.reset_engine()
    yield path
    database.reset_engine()


def _seed(path):
    con = sqlite3.connect(path)
    cur = con.cursor()
    # The shared 'default-10kg' id: M1 owns v1, M2 owns v2 (old global counter),
    # plus an M1 v3 that history references and one profile nothing references.
    for sql in (
        _profile("default-10kg", 1, "M1", 0.011, active=1),
        _profile("default-10kg", 2, "M2", 0.022),
        _profile("default-10kg", 3, "M1", 0.033),
        _profile("lonely", 1, "M2", 0.044),
    ):
        cur.execute(sql)
    run_cols = ("run_id, material_id, channel_id, pump_id, relay_id, scale_id, device_id, job_id, "
                "test_number, target_g, priority, status, started_at, created_at, sample_count, "
                "event_count, profile_id, profile_version")

    def run(run_id, material, pid, ver):
        ch = "CH1" if material == "M1" else "CH2"
        pump = "Pump 1" if material == "M1" else "Pump 2"
        cur.execute(
            f"INSERT INTO dispense_runs ({run_cols}) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (run_id, material, ch, pump, "Relay " + pump[-1], "Scale " + pump[-1], "dev", 1, 1,
             5000, 0, "COMPLETE", "2026-01-02 00:00:00", "2026-01-02 00:00:00", 0, 0, pid, ver))

    run("r-m1-v1", "M1", "default-10kg", 1)
    run("r-m2-v2", "M2", "default-10kg", 2)
    run("r-m1-v3", "M1", "default-10kg", 3)
    run("r-wrong-pump", "M2", "default-10kg", 1)       # that pair exists only for M1
    run("r-deleted", "M1", "deleted-profile", 4)       # profile row long gone
    run("r-none", "M1", None, None)
    for state, material, pid, ver in (("COMPLETE", "M1", "default-10kg", 1),
                                      ("PENDING", "M2", "default-10kg", 2),
                                      ("FAILED", "M2", "deleted-profile", 9),
                                      ("PENDING", None, "default-10kg", 1)):
        cur.execute("INSERT INTO device_commands (device_id, command_type, state, held, promoted, "
                    "material_id, target_g, profile_id, profile_version, created_at, updated_at) "
                    "VALUES ('dev','JOB',?,0,0,?,5000,?,?,'2026-01-02 00:00:00','2026-01-02 00:00:00')",
                    (state, material, pid, ver))
    con.commit()
    con.close()


def _rows(path, sql):
    con = sqlite3.connect(path)
    try:
        return con.execute(sql).fetchall()
    finally:
        con.close()


PROFILE_SNAPSHOT = ("SELECT profile_id, version, material_id, kp, active, created_at "
                    "FROM tuning_profiles ORDER BY profile_id, version")
RUN_SNAPSHOT = ("SELECT run_id, material_id, profile_id, profile_version "
                "FROM dispense_runs ORDER BY run_id")
CMD_SNAPSHOT = ("SELECT id, state, material_id, profile_id, profile_version "
                "FROM device_commands ORDER BY id")


def test_migration_links_history_to_exact_rows_and_is_idempotent(old_db, caplog):
    _seed(old_db)
    before = _rows(old_db, PROFILE_SNAPSHOT)
    before_runs = _rows(old_db, RUN_SNAPSHOT)
    before_cmds = _rows(old_db, CMD_SNAPSHOT)

    database.init_db()
    insp = inspect(database.get_engine())

    # Key changes.
    assert insp.get_pk_constraint("tuning_profiles")["constrained_columns"] == ["profile_version_id"]
    unique = [i for i in insp.get_indexes("tuning_profiles")
              if i["name"] == "uq_tuning_profile_material_version"]
    unique += [u for u in insp.get_unique_constraints("tuning_profiles")
               if u["name"] == "uq_tuning_profile_material_version"]
    assert unique and unique[0]["column_names"] == ["profile_id", "material_id", "version"]
    for table in ("device_commands", "dispense_runs"):
        assert "profile_version_id" in {c["name"] for c in insp.get_columns(table)}
        assert f"ix_{table}_profile_version_id" in {i["name"] for i in insp.get_indexes(table)}
    assert "ix_tuning_profiles_active" in {i["name"] for i in insp.get_indexes("tuning_profiles")}

    # Nothing lost or changed: every profile row, gain, version and flag survives.
    assert _rows(old_db, PROFILE_SNAPSHOT) == before
    ids = {(p, v, m): i for i, p, v, m in _rows(
        old_db, "SELECT profile_version_id, profile_id, version, material_id FROM tuning_profiles")}
    assert len(ids) == 4 and len(set(ids.values())) == 4
    # Snapshot columns of history are untouched.
    assert _rows(old_db, RUN_SNAPSHOT) == before_runs
    assert _rows(old_db, CMD_SNAPSHOT) == before_cmds

    # Resolvable references point at exactly the row they used; others stay NULL.
    run_ids = dict(_rows(old_db, "SELECT run_id, profile_version_id FROM dispense_runs"))
    assert run_ids["r-m1-v1"] == ids[("default-10kg", 1, "M1")]
    assert run_ids["r-m2-v2"] == ids[("default-10kg", 2, "M2")]
    assert run_ids["r-m1-v3"] == ids[("default-10kg", 3, "M1")]
    assert run_ids["r-wrong-pump"] is None             # never re-pointed by id+version alone
    assert run_ids["r-deleted"] is None and run_ids["r-none"] is None
    cmd = _rows(old_db, "SELECT state, profile_version_id FROM device_commands ORDER BY id")
    assert cmd[0][1] == ids[("default-10kg", 1, "M1")]
    assert cmd[1][1] == ids[("default-10kg", 2, "M2")]
    assert cmd[2][1] is None                           # deleted-profile reference
    assert cmd[3][1] is None                           # legacy job without a material

    # The app works on migrated data: both pumps may now own the same pair.
    from fastapi.testclient import TestClient
    from app.main import app
    with TestClient(app) as client:
        body = {"profile_id": "default-10kg", "material_id": "M2", "channel_id": "CH2",
                "target_g": 5000, "kp": 0.01, "ki": 0.0, "kd": 0.0, "tolerance_g": 20,
                "max_overshoot_g": 100, "max_duration_ms": 120000, "window_ms": 500,
                "min_on_ms": 40, "min_off_ms": 40}
        created = client.post("/api/v1/profiles", json=body)
        assert created.status_code == 200 and created.json()["version"] == 3   # M2 was at v2
        used = ids[("default-10kg", 1, "M1")]
        assert client.delete(f"/api/v1/profile-versions/{used}").status_code == 409   # active
        assert client.post(f"/api/v1/profile-versions/{used}/deactivate").status_code == 200
        referenced = client.delete(f"/api/v1/profile-versions/{used}")
        assert referenced.status_code == 409 and "referenced" in referenced.text
        assert client.delete(f"/api/v1/profile-versions/{ids[('lonely', 1, 'M2')]}").status_code == 200

    # Second start is a no-op: same ids, same links, no rebuild, no log noise.
    snapshot = (_rows(old_db, "SELECT profile_version_id, profile_id, version, material_id "
                              "FROM tuning_profiles ORDER BY 1"),
                _rows(old_db, "SELECT run_id, profile_version_id FROM dispense_runs ORDER BY 1"),
                _rows(old_db, "SELECT id, profile_version_id FROM device_commands ORDER BY 1"))
    database.reset_engine()
    caplog.clear()
    database.init_db()
    assert (_rows(old_db, "SELECT profile_version_id, profile_id, version, material_id "
                          "FROM tuning_profiles ORDER BY 1"),
            _rows(old_db, "SELECT run_id, profile_version_id FROM dispense_runs ORDER BY 1"),
            _rows(old_db, "SELECT id, profile_version_id FROM device_commands ORDER BY 1")) == snapshot
    assert "migrated" not in caplog.text and "backfill" not in caplog.text


def test_migration_summary_reports_what_it_did(old_db):
    _seed(old_db)
    from app import models
    from app.profile_version_migration import migrate_profile_versioning
    engine = database.get_engine()
    models.Base.metadata.create_all(engine)
    first = migrate_profile_versioning(engine)
    assert first["rebuilt"] is True
    assert first["backfill"]["dispense_runs"] == {"linked": 3, "unmatched_left_null": 2}
    assert first["backfill"]["device_commands"] == {"linked": 2, "unmatched_left_null": 2}
    assert migrate_profile_versioning(engine) == {
        "rebuilt": False, "unique_added": False, "backfill": None}


def test_migration_refuses_when_the_unique_key_would_fail(old_db):
    """A row without material_id cannot be keyed. The migration must raise and
    leave the table exactly as it was."""
    con = sqlite3.connect(old_db)
    con.execute("DROP TABLE tuning_profiles")
    con.execute(OLD_PROFILES.replace("material_id VARCHAR(8) NOT NULL DEFAULT 'M1'",
                                     "material_id VARCHAR(8)"))
    con.execute(_profile("x", 1, "M1", 0.01).replace("'x', 1, 'M1'", "'x', 1, NULL"))
    con.commit()
    con.close()
    from app import models
    from app.profile_version_migration import migrate_profile_versioning
    engine = database.get_engine()
    models.Base.metadata.create_all(engine)
    with pytest.raises(RuntimeError, match="REFUSED"):
        migrate_profile_versioning(engine)
    assert "profile_version_id" not in {c["name"] for c in inspect(engine).get_columns("tuning_profiles")}
    assert _rows(old_db, "SELECT COUNT(*) FROM tuning_profiles") == [(1,)]


def test_failed_migration_is_sticky_until_restart(old_db, monkeypatch, caplog):
    """Refused/failed migration: logged once, not retried per request, requests
    fail closed with 503, and a restart (reset_engine) clears it."""
    import app.profile_version_migration as pvm
    from fastapi import HTTPException
    con = sqlite3.connect(old_db)
    con.execute("DROP TABLE tuning_profiles")
    con.execute(OLD_PROFILES.replace("material_id VARCHAR(8) NOT NULL DEFAULT 'M1'",
                                     "material_id VARCHAR(8)"))
    con.execute(_profile("x", 1, "M1", 0.01).replace("'x', 1, 'M1'", "'x', 1, NULL"))
    con.commit()
    con.close()
    calls = []
    real = pvm.migrate_profile_versioning
    monkeypatch.setattr(pvm, "migrate_profile_versioning",
                        lambda e: (calls.append(1), real(e))[1])
    with pytest.raises(RuntimeError, match="REFUSED"):
        database.init_db()
    assert calls == [1]
    # Further attempts (e.g. one per request) do not touch the migration again.
    for _ in range(3):
        with pytest.raises(RuntimeError, match="did not complete"):
            database.init_db()
    assert calls == [1]
    assert sum("did not complete" in r.getMessage() for r in caplog.records
               if r.levelname == "ERROR") == 1
    gen = database.get_db()
    with pytest.raises(HTTPException) as err:
        next(gen)
    assert err.value.status_code == 503 and "did not complete" in err.value.detail
    assert calls == [1]
    database.reset_engine()
    assert database._migration_failed is None


def test_bounded_engine_only_wraps_mysql(old_db):
    from app.profile_version_migration import _bounded_engine
    engine = database.get_engine()
    assert _bounded_engine(engine) == (engine, False)