"""Service-layer tests: call app/services/* directly (db session + validated
args), no HTTP. These are the entry points a future MQTT dispatcher reuses; a
refusal is a ServiceError (code, message, http_status), never an HTTPException.
"""

import pathlib

import pytest
from sqlalchemy.orm import Session

from app import database
from app.models import DeviceCommand
from app.schemas import (RunStartIn, TelemetryBatchIn, TuningProfileIn)
from app.services import control, profiles, queue, runs, telemetry_ingest
from app.services.errors import ServiceError
from conftest import DEVICE_ID, make_run_body, make_sample

PROFILE = {"kp": 0.0025, "ki": 0.0003, "kd": 0.0001, "tolerance_g": 20,
           "max_overshoot_g": 100, "max_duration_ms": 120000, "window_ms": 500,
           "min_on_ms": 40, "min_off_ms": 40}


@pytest.fixture()
def db(client):  # `client` gives a fresh empty schema
    with Session(database.get_engine(), expire_on_commit=False) as session:
        yield session


def _status(db, device_id, status):
    control.update_device_status(db, control.DeviceStatusIn(device_id=device_id, status=status))


def _profile(db, material="M1", target=5000, pid="svc-tune", activate=False):
    """Saved as a draft; ``activate=True`` also activates it (a job can only pin an active one)."""
    ch = "CH1" if material == "M1" else "CH2"
    out = profiles.create_profile(db, TuningProfileIn(
        **PROFILE, profile_id=pid, material_id=material, channel_id=ch, target_g=target))
    assert out.status == "draft" and out.active is False
    if activate:
        profiles.activate(db, profiles.row_by_id(db, out.profile_version_id))
    return out


def _job_in(pid="svc-tune", version=1, material="M1", target=5000):
    return queue.QueueJobIn(material_id=material, target_g=target,
                            profile_id=pid, profile_version=version)


def _relay(db):
    _status(db, "relay-svc", {"role": "relay_controller", "channels": [], "queue": []})


def test_services_do_not_import_fastapi():
    for path in pathlib.Path(profiles.__file__).parent.glob("*.py"):
        text = path.read_text(encoding="utf-8")
        assert "import fastapi" not in text and "from fastapi" not in text, path.name


def test_service_error_default_status():
    assert ServiceError("not_found", "x").http_status == 404
    assert ServiceError("conflict", "x").http_status == 409
    assert ServiceError("invalid", "x", 400).http_status == 400


def test_create_job_needs_device_then_pins_profile(db):
    _profile(db, activate=True)
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, _job_in())
    assert exc.value.http_status == 503
    _relay(db)
    out = queue.create_job(db, _job_in())
    assert out["state"] == "PENDING" and out["target_g"] == 5000
    assert out["profile_id"] == "svc-tune" and out["profile_version"] == 1
    assert queue.read_queue(db)["waiting_commands"][0]["command_id"] == out["command_id"]


def test_create_job_refuses_wrong_target_and_missing_version(db):
    _profile(db, activate=True)
    _relay(db)
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, _job_in(target=10000))
    assert exc.value.http_status == 422 and "covers 5000..5000 g" in exc.value.message and exc.value.code == "target_out_of_profile_range"
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, _job_in(version=9))
    assert exc.value.http_status == 422


def test_cancel_waiting_job_and_unknown(db):
    _profile(db, activate=True)
    _relay(db)
    job = queue.create_job(db, _job_in())
    assert queue.cancel_job(db, job["command_id"]) == {"ok": True, "state": "CANCELLED"}
    assert queue.read_queue(db)["waiting_commands"] == []
    with pytest.raises(ServiceError) as exc:
        queue.cancel_job(db, 99999)
    assert exc.value.http_status == 404


def test_resubmit_refused_job_creates_new_command(db):
    _profile(db, activate=True)
    _relay(db)
    job = queue.create_job(db, _job_in())
    with pytest.raises(ServiceError) as exc:  # not refused by the device yet
        queue.resubmit_job(db, job["command_id"])
    assert exc.value.http_status == 409
    control.acknowledge_command(db, job["command_id"], control.DeviceCommandAck(
        state="FAILED", error="stale: older than ledger floor"))
    out = queue.resubmit_job(db, job["command_id"], queue.ResubmitIn())
    assert out["resubmits"] == job["command_id"] and out["command_id"] != job["command_id"]
    assert out["state"] == "PENDING"


def test_control_estop_and_argument_checks(db):
    with pytest.raises(ServiceError) as exc:  # no device yet
        control.control_command(db, control.ControlIn(action="ESTOP"))
    assert exc.value.http_status == 503
    _relay(db)
    out = control.control_command(db, control.ControlIn(action="ESTOP"))
    assert out["state"] == "PENDING"
    with pytest.raises(ServiceError) as exc:
        control.control_command(db, control.ControlIn(action="CANCEL"))
    assert exc.value.http_status == 422
    with pytest.raises(ServiceError) as exc:
        control.control_command(db, control.ControlIn(action="ZERO"))
    assert exc.value.http_status == 422  # ZERO/TARE need channel_id


def test_zero_tare_refused_while_busy_allowed_when_idle(db):
    _status(db, "scale-svc", {"role": "weight_sender", "online": True, "channels": []})
    _status(db, "relay-svc", {"role": "relay_controller", "queue": [],
                              "channels": [{"channel_id": "CH1", "active_job_id": 4,
                                            "state": "DISPENSING"}]})
    for action in ("ZERO", "TARE"):
        with pytest.raises(ServiceError) as exc:
            control.control_command(db, control.ControlIn(action=action, channel_id="CH1"))
        assert exc.value.http_status == 409 and "dispense is active" in exc.value.message
    _status(db, "relay-svc", {"role": "relay_controller", "queue": [],
                              "channels": [{"channel_id": "CH1", "state": "IDLE"}]})
    out = control.control_command(db, control.ControlIn(action="TARE", channel_id="CH1"))
    assert out["device_id"] == "scale-svc" and out["state"] == "PENDING"
    assert db.get(DeviceCommand, out["command_id"]).command_type == "TARE"


def test_run_search_paging_and_filters(db):
    for n in range(5):
        telemetry_ingest.start_run(db, RunStartIn.model_validate(make_run_body(f"svc-run-{n}")))
    page1 = runs.search_runs(db, limit=2, offset=0)
    page2 = runs.search_runs(db, limit=2, offset=2)
    page3 = runs.search_runs(db, limit=2, offset=4)
    assert page1.total == 5 and len(page1.items) == 2 and len(page3.items) == 1
    ids = [r.run_id for p in (page1, page2, page3) for r in p.items]
    assert len(set(ids)) == 5
    assert runs.search_runs(db, status="running").total == 5
    assert runs.search_runs(db, run_id="svc-run-3").total == 1
    with pytest.raises(ServiceError) as exc:
        runs.search_runs(db, status="bogus")
    assert exc.value.http_status == 422


def test_run_samples_roundtrip_and_unknown_run(db):
    telemetry_ingest.start_run(db, RunStartIn.model_validate(make_run_body("svc-run-s")))
    body = {"run_id": "svc-run-s", "material_id": "M1", "channel_id": "CH1",
            "device_id": DEVICE_ID, "samples": [make_sample(i, 100 * i, material_id="M1") for i in range(1, 5)]}
    batch = TelemetryBatchIn.model_validate(body)
    first = telemetry_ingest.ingest_batch(db, batch)
    again = telemetry_ingest.ingest_batch(db, batch)
    assert (first.inserted, again.inserted, again.duplicates) == (4, 0, 4)
    out = runs.run_samples(db, "svc-run-s")
    assert out.count == 4 and [s.idx for s in out.samples] == [1, 2, 3, 4]
    with pytest.raises(ServiceError) as exc:
        runs.run_samples(db, "nope")
    assert exc.value.http_status == 404
    with pytest.raises(ServiceError) as exc:  # telemetry for an unknown run: 409
        telemetry_ingest.ingest_batch(db, TelemetryBatchIn.model_validate(
            {**body, "run_id": "nope"}))
    assert exc.value.http_status == 409


def test_profile_create_versions_and_activate(db):
    v1 = _profile(db)
    v2 = _profile(db)
    assert (v1.version, v2.version) == (1, 2)
    assert profiles.next_version_preview(db, "svc-tune", "M1")["next_version"] == {"M1": 3}
    profiles.activate(db, profiles.row_by_id(db, v1.profile_version_id))
    out = profiles.activate(db, profiles.row_by_pair(db, "svc-tune", 2, "M1"))
    assert out["active"] is True and out["version"] == 2
    active = profiles.active_profile(db, "M1", "CH1", 5000)
    assert active.version == 2
    listed = {p.version: p.active for p in profiles.list_profiles(db, material_id="M1")}
    assert listed == {1: False, 2: True}
    with pytest.raises(ServiceError) as exc:  # active versions cannot be deleted
        profiles.delete(db, profiles.row_by_id(db, v2.profile_version_id))
    assert exc.value.http_status == 409
    with pytest.raises(ServiceError) as exc:
        profiles.active_profile(db, "M1", "CH2", 5000)
    assert exc.value.http_status == 422
