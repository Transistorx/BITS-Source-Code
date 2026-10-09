"""Regression cases for stop ordering, late ACKs and reconnect replay."""
from datetime import timedelta
from sqlalchemy.orm import Session
from app.database import get_engine
from app.models import DeviceCommand
from app.services.analytics import utcnow


def command(kind, channel="CH1", **fields):
    with Session(get_engine()) as db:
        row=DeviceCommand(device_id="relay-race",command_type=kind,channel_id=channel,
            state="PENDING",created_at=utcnow(),updated_at=utcnow(),**fields)
        db.add(row);db.commit();return row.id


def test_estop_and_stop_bypass_pending_start_backlog(client):
    command("PUMP_START")
    stop=command("PUMP_STOP")
    estop=command("ESTOP",None)
    rows=client.get("/api/v1/device/commands",params={"device_id":"relay-race"}).json()
    assert [r["command_id"] for r in rows[:2]]==[estop,stop]


def test_late_job_ack_does_not_revive_cancelling_job(client):
    job=command("JOB",target_g=5000,material_id="M1")
    with Session(get_engine()) as db:
        db.get(DeviceCommand,job).state="CANCELLING";db.commit()
    response=client.post(f"/api/v1/device/commands/{job}/ack",json={"state":"QUEUED","local_job_id":7})
    assert response.status_code==200
    with Session(get_engine()) as db:
        row=db.get(DeviceCommand,job)
        assert row.state=="CANCELLING" and row.local_job_id==7


def test_late_ack_cannot_downgrade_terminal_job(client):
    job=command("JOB",target_g=5000,material_id="M1")
    with Session(get_engine()) as db:
        db.get(DeviceCommand,job).state="COMPLETE";db.commit()
    client.post(f"/api/v1/device/commands/{job}/ack",json={"state":"QUEUED","local_job_id":7})
    with Session(get_engine()) as db:
        assert db.get(DeviceCommand,job).state=="COMPLETE"


def test_reconnect_does_not_replay_old_manual_start(client):
    start=command("PUMP_START")
    stop=command("PUMP_STOP")
    with Session(get_engine()) as db:
        db.get(DeviceCommand,start).created_at=utcnow()-timedelta(minutes=1);db.commit()
    rows=client.get("/api/v1/device/commands",params={"device_id":"relay-race"}).json()
    assert start not in [r["command_id"] for r in rows]
    assert stop in [r["command_id"] for r in rows]


def test_acknowledged_stop_supersedes_unacknowledged_start_after_reboot(client):
    start=command("PUMP_START")
    stop=command("PUMP_STOP")
    client.post(f"/api/v1/device/commands/{stop}/ack",json={"state":"APPLIED"})
    rows=client.get("/api/v1/device/commands",params={"device_id":"relay-race"}).json()
    assert start not in [r["command_id"] for r in rows]


def test_late_queued_ack_does_not_downgrade_running_job(client):
    job=command("JOB",target_g=5000,material_id="M1")
    with Session(get_engine()) as db:
        db.get(DeviceCommand,job).state="RUNNING";db.commit()
    client.post(f"/api/v1/device/commands/{job}/ack",json={"state":"QUEUED","local_job_id":7})
    with Session(get_engine()) as db:
        assert db.get(DeviceCommand,job).state=="RUNNING"
