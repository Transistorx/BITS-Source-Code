"""Regression tests for the safety review of per-pump profile versioning.

F1 no reuse of referenced numbers, NULL-id jobs only match older rows, re-pin
on release/resubmit, migration warning; F2 row/job agreement at delivery;
F3 delete vs queue ordering; F5 backfill collation fail-closed.
"""

import logging
from datetime import timedelta

import pytest
from sqlalchemy.exc import OperationalError
from sqlalchemy.orm import Session

from app import database, models, profile_version_migration
from app import mqtt_bridge
from app.routes.operations import command_wire
from app.services import profiles as profile_service
from app.services.analytics import utcnow
from test_profile_pinning import _seed_device
from test_profile_version_migration import _profile, _rows, old_db  # noqa: F401
from test_profile_versioning import M, _job, activate, bulk_body, make_versions, preview, save, save_both


def _insert_job(device_id, material, version, pvid=None, state="PENDING", created_at=None, pid="default"):
    with Session(database.get_engine()) as db:
        job = models.DeviceCommand(
            device_id=device_id, command_type="JOB", material_id=material, target_g=5000,
            state=state, profile_id=pid, profile_version=version, profile_version_id=pvid,
            created_at=created_at or utcnow(), updated_at=utcnow())
        db.add(job)
        db.commit()
        return job.id


def _wire(job_id):
    with Session(database.get_engine()) as db:
        job = db.get(models.DeviceCommand, job_id)
        http = command_wire(job, db)
        mqtt = mqtt_bridge.build_command_payload(job, db)
    return http, mqtt


def _poll_profile(client, device_id, job_id):
    items = client.get("/api/v1/device/commands", params={"device_id": device_id}).json()
    return next(c for c in items if c["command_id"] == job_id)["profile"]


# ------------------------------------------------ F1(1) no number reuse ----


def test_counter_never_reuses_a_number_history_references(client):
    _seed_device(client)
    rows = make_versions(client, "M1", 3)
    activate(client, rows[2])
    job = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=rows[2]["profile_version_id"])).json()
    # Simulate an operator-deleted row whose job survives (e.g. pre-guard data).
    with Session(database.get_engine()) as db:
        db.delete(db.get(models.TuningProfile, rows[2]["profile_version_id"]))
        db.commit()
    assert preview(client, materials="M1") == {"M1": 4}
    assert save(client, "M1")["version"] == 4
    assert job["profile_version"] == 3


def test_counter_counts_dispense_runs_and_other_pump_is_independent(client):
    with Session(database.get_engine()) as db:
        db.add(models.DispenseRun(
            run_id="r9", device_id="d", job_id=1, material_id="M2", channel_id="CH2",
            pump_id="Pump 2", relay_id="Relay 2", scale_id="Scale 2", target_g=5000,
            priority=0, status="COMPLETE", started_at=utcnow(), profile_id="default",
            profile_version=9))
        db.commit()
    assert preview(client) == {"M1": 1, "M2": 10}
    rows = save_both(client)
    assert (rows["M1"]["version"], rows["M2"]["version"]) == (1, 10)


def test_normal_flows_unchanged(client):
    make_versions(client, "M1", 7)
    make_versions(client, "M2", 4)
    assert preview(client) == {"M1": 8, "M2": 5}
    both = save_both(client)
    assert (both["M1"]["version"], both["M2"]["version"]) == (8, 5)


# ------------------------------------------- F1(2) NULL id only older rows ----


def test_null_id_job_ignores_a_row_created_after_it(client):
    device = _seed_device(client)
    past = utcnow() - timedelta(hours=1)
    job_id = _insert_job(device, "M1", 1, created_at=past)
    save(client, "M1", kp=0.5)                    # v1 now exists, but newer than the job
    assert _poll_profile(client, device, job_id) is None
    http, mqtt = _wire(job_id)
    assert http["profile"] is None and mqtt["profile"] is None


def test_null_id_job_gets_a_row_that_predates_it(client):
    device = _seed_device(client)
    save(client, "M2", kp=0.5)
    job_id = _insert_job(device, "M2", 1, created_at=utcnow() + timedelta(seconds=5))
    assert _poll_profile(client, device, job_id)["kp"] == 0.5


def test_legacy_shared_counter_scenario(old_db):  # noqa: F811
    import sqlite3
    con = sqlite3.connect(old_db)
    for v in range(1, 11):
        con.execute(_profile("p", v, "M1", 0.01))
    con.execute(_profile("p", 11, "M2", 0.02))
    con.execute("INSERT INTO device_commands (device_id, command_type, state, held, promoted, "
                "material_id, target_g, profile_id, profile_version, created_at, updated_at) "
                "VALUES ('dev','JOB','PENDING',0,0,'M2',5000,'p',11,'2026-01-02 00:00:00','2026-01-02 00:00:00')")
    con.execute("DELETE FROM tuning_profiles WHERE profile_id='p' AND material_id='M2'")
    con.commit()
    con.close()

    database.init_db()
    assert _rows(old_db, "SELECT profile_version_id FROM device_commands") == [(None,)]
    with Session(database.get_engine()) as db:
        # v11 is referenced by the job, so M2 starts at 12, never at 1 or 11.
        assert profile_service.next_versions(db, "p", ["M1", "M2"]) == {"M1": 11, "M2": 12}
        fields = {k: v for k, v in dict(
            profile_id="p", target_g=5000, kp=0.9, ki=0.001, kd=0.0, tolerance_g=20,
            max_overshoot_g=100, max_duration_ms=120000, window_ms=500, min_on_ms=40,
            min_off_ms=40).items()}
        (new,) = profile_service.create_versions(db, fields, ["M2"])
        assert new.version == 12
        job = db.get(models.DeviceCommand, 1)
        assert command_wire(job, db)["profile"] is None
        # Even a forced collision on v11 cannot feed the old job (row is newer).
        db.add(models.TuningProfile(
            profile_id="p", version=11, material_id="M2", channel_id="CH2", target_g=5000,
            kp=0.9, ki=0.0, kd=0.0, tolerance_g=20, max_overshoot_g=100, max_duration_ms=120000,
            window_ms=500, min_on_ms=40, min_off_ms=40, active=False, created_at=utcnow()))
        db.commit()
        assert command_wire(job, db)["profile"] is None


# ------------------------------------------------- F1(3) re-pin on copy ----


def _hold(client, command_id):
    assert client.post("/api/v1/queue/control",
                       json={"action": "HOLD", "command_id": command_id}).status_code == 200


def test_release_refuses_a_dangling_pin_with_409(client):
    _seed_device(client)
    row = save(client, "M1")
    activate(client, row)
    job = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=row["profile_version_id"])).json()
    _hold(client, job["command_id"])
    with Session(database.get_engine()) as db:
        db.delete(db.get(models.TuningProfile, row["profile_version_id"]))
        db.commit()
    resp = client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": job["command_id"]})
    assert resp.status_code == 409, resp.text
    assert "can no longer be resolved" in resp.text
    with Session(database.get_engine()) as db:
        old = db.get(models.DeviceCommand, job["command_id"])
        assert old.state == "PENDING" and old.held is True       # nothing superseded
        assert db.query(models.DeviceCommand).filter_by(command_type="JOB").count() == 1


def test_resubmit_refuses_dangling_pin_and_upgrades_null_id(client):
    device = _seed_device(client)
    row = save(client, "M1")
    activate(client, row)
    failed = _insert_job(device, "M1", 1, state="FAILED", created_at=utcnow() + timedelta(seconds=5))
    with Session(database.get_engine()) as db:
        job = db.get(models.DeviceCommand, failed)
        job.error_text = "stale: older than ledger floor"
        db.commit()
    ok = client.post(f"/api/v1/queue/jobs/{failed}/resubmit")
    assert ok.status_code == 200, ok.text
    with Session(database.get_engine()) as db:
        assert db.get(models.DeviceCommand, ok.json()["command_id"]).profile_version_id == row["profile_version_id"]
        db.delete(db.get(models.TuningProfile, row["profile_version_id"]))
        db.commit()
    failed2 = _insert_job(device, "M1", 1, state="FAILED", created_at=utcnow() + timedelta(seconds=5))
    with Session(database.get_engine()) as db:
        db.get(models.DeviceCommand, failed2).error_text = "stale"
        db.commit()
    refused = client.post(f"/api/v1/queue/jobs/{failed2}/resubmit")
    assert refused.status_code == 409, refused.text
    with Session(database.get_engine()) as db:
        assert db.get(models.DeviceCommand, failed2).state == "FAILED"


# ----------------------------------------- F1(4) migration warning, F5 ----


def test_migration_warns_about_live_jobs_left_unlinked(old_db, caplog):  # noqa: F811
    import sqlite3
    con = sqlite3.connect(old_db)
    con.execute(_profile("p", 1, "M1", 0.01))
    for state in ("PENDING", "COMPLETE"):
        con.execute("INSERT INTO device_commands (device_id, command_type, state, held, promoted, "
                    "material_id, target_g, profile_id, profile_version, created_at, updated_at) "
                    f"VALUES ('dev','JOB','{state}',0,0,'M2',5000,'p',9,'2026-01-02','2026-01-02')")
    con.commit()
    con.close()
    with caplog.at_level(logging.WARNING, logger="profile_version_migration"):
        database.init_db()
    text = " ".join(r.getMessage() for r in caplog.records)
    assert "1 live JOB(s)" in text and "ids [1]" in text


def test_backfill_collation_error_fails_closed(caplog):
    class Orig(Exception):
        pass

    class Conn:
        def execute(self, *_a, **_k):
            raise OperationalError("UPDATE", {}, Orig(1267, "Illegal mix of collations"))

    with caplog.at_level(logging.ERROR, logger="profile_version_migration"):
        with pytest.raises(RuntimeError, match="collation mismatch"):
            profile_version_migration._backfill_table(Conn(), "device_commands")
    assert "SHOW FULL COLUMNS" in caplog.text

    class Other:
        def execute(self, *_a, **_k):
            raise OperationalError("UPDATE", {}, Orig(2013, "lost connection"))

    with pytest.raises(OperationalError):
        profile_version_migration._backfill_table(Other(), "device_commands")


# ------------------------------------- F2 row must agree with the job ----


def test_job_pointing_at_the_other_pumps_row_gets_no_gains(client):
    device = _seed_device(client)
    m1, m2 = save(client, "M1", kp=0.01), save(client, "M2", kp=0.02)
    # M2 job, same id+version as the M1 row, but its id points at the M1 row.
    job_id = _insert_job(device, "M2", 1, pvid=m1["profile_version_id"])
    assert _poll_profile(client, device, job_id) is None
    http, mqtt = _wire(job_id)
    assert http["profile"] is None and mqtt["profile"] is None
    # A mismatched version also fails safe.
    bad = _insert_job(device, "M1", 7, pvid=m1["profile_version_id"])
    assert _wire(bad)[0]["profile"] is None and _wire(bad)[1]["profile"] is None
    good = _insert_job(device, "M2", 1, pvid=m2["profile_version_id"])
    assert _wire(good)[0]["profile"]["kp"] == 0.02


# -------------------------------------------- F3 delete vs queue ordering ----


def test_create_job_refused_when_row_deleted_after_lookup(client, monkeypatch):
    _seed_device(client)
    row = save(client, "M1")
    activate(client, row)
    real = profile_service.lock_row

    def delete_then_lock(db, pvid):
        with Session(database.get_engine()) as other:   # a delete that won the race
            other.delete(other.get(models.TuningProfile, pvid))
            other.commit()
        return real(db, pvid)

    monkeypatch.setattr(profile_service, "lock_row", delete_then_lock)
    resp = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=row["profile_version_id"]))
    assert resp.status_code == 409, resp.text
    with Session(database.get_engine()) as db:
        assert db.query(models.DeviceCommand).filter_by(command_type="JOB").count() == 0


def test_delete_locks_the_row_before_it_counts_references(client, monkeypatch):
    row = save(client, "M1")
    order = []
    real_lock, real_refs = profile_service.lock_row, profile_service.reference_counts
    monkeypatch.setattr(profile_service, "lock_row",
                        lambda db, pvid: (order.append("lock"), real_lock(db, pvid))[1])
    monkeypatch.setattr(profile_service, "reference_counts",
                        lambda db, r: (order.append("refs"), real_refs(db, r))[1])
    assert client.delete(f"/api/v1/profile-versions/{row['profile_version_id']}").status_code == 200
    assert order == ["lock", "refs"]


def test_delete_after_job_pinned_is_refused(client):
    _seed_device(client)
    row = save(client, "M1")
    activate(client, row)
    assert client.post("/api/v1/queue/jobs", json=_job(profile_version_id=row["profile_version_id"])).status_code == 200
    client.post(f"/api/v1/profile-versions/{row['profile_version_id']}/deactivate")
    assert client.delete(f"/api/v1/profile-versions/{row['profile_version_id']}").status_code == 409


# ---------------- review 2: created_at guard in backfill, PROFILE history, row checks ----


def _raw_job(con, state, held=0, error=None, created="2026-01-02 00:00:00"):
    con.execute("INSERT INTO device_commands (device_id, command_type, state, held, promoted, "
                "material_id, target_g, profile_id, profile_version, error_text, created_at, updated_at) "
                f"VALUES ('relay-pin','JOB','{state}',{held},0,'M2',5000,'p',11,?,'{created}','{created}')",
                (error,))


def test_backfill_never_links_a_job_to_a_row_created_after_it(old_db, caplog):  # noqa: F811
    """Row A (v11, M2) predates the jobs, was deleted, and the number was re-saved
    as row B at T2 > job time. The jobs must stay NULL and never ship B's gains."""
    import sqlite3
    con = sqlite3.connect(old_db)
    con.execute(_profile("p", 11, "M2", 0.02))
    _raw_job(con, "PENDING")                                   # polled
    _raw_job(con, "PENDING", held=1)                           # released
    _raw_job(con, "FAILED", error="stale")                     # resubmitted
    con.execute(_profile("q", 1, "M1", 0.01))                  # control: provably older
    con.execute("INSERT INTO device_commands (device_id, command_type, state, held, promoted, material_id, "
                "target_g, profile_id, profile_version, created_at, updated_at) VALUES "
                "('relay-pin','JOB','COMPLETE',0,0,'M1',5000,'q',1,'2026-01-02','2026-01-02')")
    con.execute("DELETE FROM tuning_profiles WHERE profile_id='p'")          # row A gone
    con.execute(_profile("p", 11, "M2", 0.99).replace("2026-01-01", "2026-03-01"))   # row B
    con.commit()
    con.close()

    with caplog.at_level(logging.WARNING, logger="profile_version_migration"):
        database.init_db()
    linked = _rows(old_db, "SELECT profile_version_id FROM device_commands ORDER BY id")
    assert linked[:3] == [(None,)] * 3 and linked[3][0] is not None   # control job did link
    text = " ".join(r.getMessage() for r in caplog.records)
    assert "3 live JOB(s)" in text and "ids [1, 2, 3]" in text

    from fastapi.testclient import TestClient
    from app.main import app
    with TestClient(app) as client:
        _seed_device(client)
        assert _poll_profile(client, "relay-pin", 1) is None
        for job_id in (1, 2, 3):
            http, mqtt = _wire(job_id)
            assert http["profile"] is None and mqtt["profile"] is None
        released = client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": 2})
        assert released.status_code == 409, released.text
        resubmit = client.post("/api/v1/queue/jobs/3/resubmit")
        assert resubmit.status_code == 409, resubmit.text


def test_id_linked_job_ignores_a_row_created_after_it(client):
    device = _seed_device(client)
    row = save(client, "M1", kp=0.5)
    past = utcnow() - timedelta(hours=1)
    job_id = _insert_job(device, "M1", 1, pvid=row["profile_version_id"], created_at=past)
    assert _poll_profile(client, device, job_id) is None
    ok = _insert_job(device, "M1", 1, pvid=row["profile_version_id"])
    assert _poll_profile(client, device, ok)["kp"] == 0.5


def test_row_for_job_checks_target_and_fixed_channel(client):
    device = _seed_device(client)
    row = save(client, "M1", kp=0.5)
    job_id = _insert_job(device, "M1", 1, pvid=row["profile_version_id"])
    assert _wire(job_id)[0]["profile"]["kp"] == 0.5
    with Session(database.get_engine()) as db:
        job = db.get(models.DeviceCommand, job_id)
        pv = db.get(models.TuningProfile, row["profile_version_id"])
        assert profile_service.row_for_job(job, pv) is pv
        job.target_g = 2000                                    # different target
        assert profile_service.row_for_job(job, pv) is None
        job.target_g = 5000
        pv.channel_id = "CH2"                                  # breaks M1 -> CH1
        assert profile_service.row_for_job(job, pv) is None
        db.rollback()


def test_counter_v7_v7_then_save_both_is_v8_v8(client):
    make_versions(client, "M1", 7)
    make_versions(client, "M2", 7)
    assert preview(client) == {"M1": 8, "M2": 8}
    both = save_both(client)
    assert (both["M1"]["version"], both["M2"]["version"]) == (8, 8)


def _stage_profile_command(db, material, version, pid="default", clear=False):
    payload = {"material_id": material, "channel_id": "CH1" if material == "M1" else "CH2",
               "target_g": 5000}
    if not clear:
        payload.update({"profile_id": pid, "version": version, "kp": 0.1})
    db.add(models.DeviceCommand(
        device_id="d", command_type="PROFILE_CLEAR" if clear else "PROFILE",
        channel_id=payload["channel_id"], target_g=5000, payload_json=payload,
        state="DELIVERED", created_at=utcnow(), updated_at=utcnow()))


def test_counter_counts_versions_staged_by_profile_commands(client):
    with Session(database.get_engine()) as db:
        _stage_profile_command(db, "M2", 9)
        _stage_profile_command(db, "M1", 30, clear=True)       # clear carries no version
        _stage_profile_command(db, "M1", 40, pid="other")      # other profile_id
        db.commit()
    assert preview(client) == {"M1": 1, "M2": 10}
    assert save(client, "M2")["version"] == 10


def test_delete_blocked_when_a_profile_command_staged_the_version(client):
    _seed_device(client)
    rows = make_versions(client, "M1", 2)
    v2 = rows[1]["profile_version_id"]
    assert client.post(f"/api/v1/profile-versions/{v2}/activate").status_code == 200
    assert client.post(f"/api/v1/profile-versions/{v2}/deactivate").status_code == 200
    resp = client.delete(f"/api/v1/profile-versions/{v2}")      # the PROFILE command stays
    assert resp.status_code == 409 and "referenced" in resp.text
    # A version no PROFILE command mentions can still be deleted.
    assert client.delete(f"/api/v1/profile-versions/{rows[0]['profile_version_id']}").status_code == 200