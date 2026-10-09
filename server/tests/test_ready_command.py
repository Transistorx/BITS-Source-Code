"""Operator READY command: server only relays the intent, firmware decides."""
from sqlalchemy.orm import Session
from app.database import get_engine
from app.models import DeviceCommand


def _device(client):
    client.post("/api/v1/device/status", json={"device_id": "relay-a", "status": {"channels": []}})


def test_ready_creates_command_and_is_delivered(client):
    _device(client)
    r = client.post("/api/v1/queue/control", json={"action": "READY", "channel_id": "CH2"})
    assert r.status_code == 200, r.text
    cid = r.json()["command_id"]
    with Session(get_engine()) as db:
        row = db.get(DeviceCommand, cid)
        assert row.command_type == "READY" and row.channel_id == "CH2"
        device = row.device_id
    rows = client.get("/api/v1/device/commands", params={"device_id": device}).json()
    item = next(x for x in rows if x["command_id"] == cid)
    assert item["command_type"] == "READY" and item["channel_id"] == "CH2"


def test_ready_requires_channel(client):
    r = client.post("/api/v1/queue/control", json={"action": "READY"})
    assert r.status_code == 422


def test_ready_rejects_bad_channel(client):
    r = client.post("/api/v1/queue/control", json={"action": "READY", "channel_id": "CH9"})
    assert r.status_code == 422


def test_ready_failed_ack_surfaces_error(client):
    _device(client)
    cid = client.post("/api/v1/queue/control",
                      json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]
    r = client.post(f"/api/v1/device/commands/{cid}/ack",
                    json={"state": "FAILED", "error": "not awaiting ready"})
    assert r.status_code == 200 and r.json()["state"] == "FAILED"
    with Session(get_engine()) as db:
        assert db.get(DeviceCommand, cid).error_text == "not awaiting ready"

