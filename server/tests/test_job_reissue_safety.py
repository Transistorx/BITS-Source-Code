"""Held-JOB reissue / resubmit safety: no path may create a second live copy of
a job the device may already own (double dispense). See contract section 7."""

import pytest
from fastapi import HTTPException
from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app import database, mqtt_bridge
from app.models import DeviceCommand
from app.routes import operations
from app.routes.operations import ControlIn
from conftest import DEVICE_ID
from test_mqtt_bridge import FakePublisher, pub  # noqa: F401  (fixture)
from test_operations import _job, _seed_profiles

ResubmitIn = getattr(operations, "ResubmitIn", lambda **kw: None)
STALE = "stale: older than ledger floor"
UNKNOWN = "STALE_UNKNOWN: id below replay floor, job not in queue"


def _session():
    database.get_engine()
    return Session(database.get_engine(), expire_on_commit=False)


def _setup(client, device="relay-safe"):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": device, "status": {"channels": [], "queue": []}})
    return device


def _rows(**where):
    with _session() as db:
        q = select(DeviceCommand).where(DeviceCommand.command_type == "JOB")
        return list(db.scalars(q.order_by(DeviceCommand.id)))


def _control_rows(kind):
    with _session() as db:
        return list(db.scalars(select(DeviceCommand).where(DeviceCommand.command_type == kind)))


def _failed_job(client, device, error):
    old = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    client.get("/api/v1/device/commands", params={"device_id": device})
    r = client.post(f"/api/v1/device/commands/{old}/ack", json={"state": "FAILED", "error": error})
    assert r.status_code == 200
    return old


# ---- H1: MQTT-published rows count as handed out --------------------------

def test_mqtt_published_job_is_marked_and_hold_release_are_forwarded(client, pub):
    device = _setup(client)
    created = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    assert [m[1]["command_id"] for m in pub.sent] == [created]
    with _session() as db:
        assert db.get(DeviceCommand, created).published_at is not None

    assert client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": created}).status_code == 200
    assert client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": created}).status_code == 200

    assert [r.id for r in _rows()] == [created]  # never reissued: no second JOB
    kinds = {r.command_type: r for r in (_control_rows("HOLD") + _control_rows("RELEASE"))}
    assert set(kinds) == {"HOLD", "RELEASE"}
    assert all(r.payload_json == {"remote_command_id": created} for r in kinds.values())


def test_cancel_of_mqtt_published_pending_job_is_forwarded_not_local(client, pub):
    _setup(client)
    created = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    r = client.post(f"/api/v1/queue/jobs/{created}/cancel")
    assert r.status_code == 200 and r.json()["state"] == "CANCELLING"
    cancels = _control_rows("CANCEL")
    assert [c.payload_json for c in cancels] == [{"remote_command_id": created}]


def test_publish_failure_leaves_row_unpublished(client):
    _setup(client)
    mqtt_bridge.set_publisher(FakePublisher(fail=True))
    try:
        created = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    finally:
        mqtt_bridge.set_publisher(None)
    with _session() as db:
        assert db.get(DeviceCommand, created).published_at is None


def test_row_held_before_publish_is_never_published(client, pub):
    _setup(client)
    with _session() as db:
        row = DeviceCommand(device_id="relay-safe", command_type="JOB", material_id="M1",
                            target_g=5000, priority=0, state="PENDING", held=True,
                            profile_id="tune-m1-5k", profile_version=1)
        db.add(row)
        db.commit()
        rid = row.id
    assert pub.sent == []
    with _session() as db:
        assert db.get(DeviceCommand, rid).published_at is None


def test_hold_flag_only_path_loses_to_a_concurrent_publish(client):
    """HOLD read the row as never-handed-out, but it was published meanwhile:
    the flag-only shortcut must not apply; the HOLD goes to the device."""
    _setup(client)
    created = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    stale = _session()
    keep = stale.get(DeviceCommand, created)  # loaded while published_at is NULL
    with _session() as other:
        other.execute(DeviceCommand.__table__.update().where(
            DeviceCommand.id == created).values(published_at=operations.utcnow()))
        other.commit()
    operations._queue_order(ControlIn(action="HOLD", command_id=created), stale)
    holds = _control_rows("HOLD")
    assert [h.payload_json for h in holds] == [{"remote_command_id": created}]
    stale.close()


# ---- H2: atomic claims ----------------------------------------------------

def test_concurrent_release_creates_exactly_one_successor(client):
    device = _setup(client)
    old = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": old})
    a, b = _session(), _session()
    keep = b.get(DeviceCommand, old)  # B holds a stale PENDING+held view (strong ref)
    payload = ControlIn(action="RELEASE", command_id=old)
    operations._queue_order(payload, a)
    with pytest.raises(HTTPException) as exc:
        operations._queue_order(payload, b)
    assert exc.value.status_code == 409
    a.close(); b.close()
    live = [r for r in _rows() if r.state in ("PENDING", "DELIVERED", "QUEUED")]
    assert len(live) == 1 and live[0].id != old
    delivered = client.get("/api/v1/device/commands", params={"device_id": device}).json()
    assert [d["command_id"] for d in delivered if d["command_type"] == "JOB"] == [live[0].id]


def test_concurrent_resubmit_creates_exactly_one_successor(client):
    device = _setup(client)
    old = _failed_job(client, device, STALE)
    a, b = _session(), _session()
    keep = b.get(DeviceCommand, old)  # strong ref keeps the stale view
    operations.resubmit_failed_job(old, ResubmitIn(), a)
    with pytest.raises(HTTPException) as exc:
        operations.resubmit_failed_job(old, ResubmitIn(), b)
    assert exc.value.status_code == 409
    a.close(); b.close()
    assert len([r for r in _rows() if r.state == "PENDING"]) == 1


# ---- M1: STALE_UNKNOWN and resubmit eligibility ----------------------------

def test_stale_unknown_is_flagged_and_needs_confirmation(client):
    device = _setup(client)
    old = _failed_job(client, device, UNKNOWN)
    failed = client.get("/api/v1/queue").json()["failed_commands"]
    assert [f["command_id"] for f in failed] == [old]
    assert failed[0]["outcome_unknown"] is True

    refused = client.post(f"/api/v1/queue/jobs/{old}/resubmit", json={})
    assert refused.status_code == 409 and "already" in refused.json()["detail"].lower()
    assert client.post(f"/api/v1/queue/jobs/{old}/resubmit",
                       json={"confirm_unknown": False}).status_code == 409
    assert [r.id for r in _rows()] == [old]

    ok = client.post(f"/api/v1/queue/jobs/{old}/resubmit", json={"confirm_unknown": True})
    assert ok.status_code == 200, ok.text
    assert ok.json()["command_id"] > old
    assert client.get("/api/v1/queue").json()["failed_commands"] == []


def test_plain_stale_refusal_is_not_flagged_unknown_and_needs_no_confirm(client):
    device = _setup(client)
    old = _failed_job(client, device, STALE)
    failed = client.get("/api/v1/queue").json()["failed_commands"]
    assert failed[0]["outcome_unknown"] is False
    assert client.post(f"/api/v1/queue/jobs/{old}/resubmit").status_code == 200


def test_operator_cancelled_job_is_never_resubmittable(client):
    device = _setup(client)
    old = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    client.get("/api/v1/device/commands", params={"device_id": device})  # DELIVERED
    assert client.post(f"/api/v1/queue/jobs/{old}/cancel").status_code == 200
    # device then refuses the (cancelled) JOB as stale
    client.post(f"/api/v1/device/commands/{old}/ack", json={"state": "FAILED", "error": STALE})
    assert client.get("/api/v1/queue").json()["failed_commands"] == []
    r = client.post(f"/api/v1/queue/jobs/{old}/resubmit", json={"confirm_unknown": True})
    assert r.status_code == 409
    assert [x.id for x in _rows()] == [old]


def test_cancelled_pending_and_failed_without_error_are_not_resubmittable(client):
    device = _setup(client)
    pending = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    client.post(f"/api/v1/queue/jobs/{pending}/cancel")
    assert client.post(f"/api/v1/queue/jobs/{pending}/resubmit").status_code == 409
    noerr = _failed_job(client, device, None)
    assert client.get("/api/v1/queue").json()["failed_commands"] == []
    assert client.post(f"/api/v1/queue/jobs/{noerr}/resubmit").status_code == 409


def test_superseded_row_is_not_resubmittable(client):
    _setup(client)
    old = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": old})
    client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": old})
    assert client.post(f"/api/v1/queue/jobs/{old}/resubmit",
                       json={"confirm_unknown": True}).status_code == 409


