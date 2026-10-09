"""ZERO/TARE operator commands to the weight sender (contract section 7)."""

import json
from datetime import timedelta

import pytest
from sqlalchemy.orm import Session

from app import database, mqtt_bridge
from app.models import DeviceCommand, DeviceStatus
from app.services.analytics import utcnow
from conftest import DEVICE_ID
from test_mqtt_bridge import FakePublisher, _row, _send

SENDER = "bits-sender-0001"
RELAY_STATUS = {"role": "relay_controller", "channels": [
    {"channel_id": "CH1", "state": "IDLE", "relay_on": False, "active_job_id": 0},
    {"channel_id": "CH2", "state": "IDLE", "relay_on": False, "active_job_id": 0}], "queue": []}


@pytest.fixture()
def pub():
    fake = FakePublisher()
    mqtt_bridge.set_publisher(fake)
    yield fake
    mqtt_bridge.set_publisher(None)


def _seed(relay=RELAY_STATUS, sender=True, sender_age_s=0, sender_online=True):
    with Session(database.get_engine()) as db:
        db.add(DeviceStatus(device_id=DEVICE_ID, status_json=relay, updated_at=utcnow()))
        if sender:
            st = {"role": "weight_sender", "cas_link": "ONLINE"}
            if not sender_online:
                st["online"] = False
            db.add(DeviceStatus(device_id=SENDER, status_json=st,
                                updated_at=utcnow() - timedelta(seconds=sender_age_s)))
        db.commit()


@pytest.mark.parametrize("action", ["ZERO", "TARE"])
def test_scale_command_targets_sender_and_publishes(client, pub, action):
    _seed()
    r = client.post("/api/v1/queue/control", json={"action": action, "channel_id": "CH2"})
    assert r.status_code == 200, r.text
    (topic, body, qos, retain), = pub.sent
    assert topic == f"cas/{SENDER}/commands" and qos == 1 and retain is False
    assert body["type"] == action and body["channel_id"] == "CH2"
    assert body["command_id"] == r.json()["command_id"] and body["ttl_ms"] == 10000
    assert _row(body["command_id"]).device_id == SENDER


def test_scale_command_requires_channel(client, pub):
    _seed()
    assert client.post("/api/v1/queue/control", json={"action": "ZERO"}).status_code == 422
    assert pub.sent == []


def test_explicit_device_id_must_be_weight_sender(client, pub):
    _seed()
    r = client.post("/api/v1/queue/control",
                    json={"action": "TARE", "channel_id": "CH1", "device_id": DEVICE_ID})
    assert r.status_code == 422 and pub.sent == []
    r = client.post("/api/v1/queue/control",
                    json={"action": "TARE", "channel_id": "CH1", "device_id": SENDER})
    assert r.status_code == 200


def test_rejected_while_dispense_active(client, pub):
    relay = json.loads(json.dumps(RELAY_STATUS))
    relay["channels"][1].update(state="DISPENSING", relay_on=True, active_job_id=3)
    _seed(relay=relay)
    r = client.post("/api/v1/queue/control", json={"action": "TARE", "channel_id": "CH2"})
    assert r.status_code == 409 and "dispens" in r.text.lower()
    assert pub.sent == []


def test_rejected_when_sender_offline_or_missing(client, pub):
    _seed(sender_age_s=60)
    assert client.post("/api/v1/queue/control",
                       json={"action": "ZERO", "channel_id": "CH2"}).status_code == 409
    with Session(database.get_engine()) as db:
        db.query(DeviceStatus).filter_by(device_id=SENDER).delete()
        db.commit()
    assert client.post("/api/v1/queue/control",
                       json={"action": "ZERO", "channel_id": "CH2"}).status_code == 503
    assert pub.sent == []


def test_rejected_when_presence_offline(client, pub):
    _seed(sender_online=False)
    assert client.post("/api/v1/queue/control",
                       json={"action": "ZERO", "channel_id": "CH2"}).status_code == 409


def test_expired_scale_command_not_replayed_over_http(client, pub):
    _seed()
    cid = client.post("/api/v1/queue/control",
                      json={"action": "TARE", "channel_id": "CH2"}).json()["command_id"]
    with Session(database.get_engine()) as db:
        db.get(DeviceCommand, cid).created_at = utcnow() - timedelta(seconds=60)
        db.commit()
    assert client.get("/api/v1/device/commands", params={"device_id": SENDER}).json() == []
    assert _row(cid).state == "FAILED"


def test_ack_fields_stored_and_readable(client, pub):
    _seed()
    cid = client.post("/api/v1/queue/control",
                      json={"action": "TARE", "channel_id": "CH2"}).json()["command_id"]
    _send("commands/ack", {"command_id": cid, "state": "FAILED", "result": "failed",
                           "channel_id": "CH2",
                           "reason": "VERIFICATION REQUIRED: frame unverified"}, device=SENDER)
    r = client.get(f"/api/v1/queue/control/{cid}")
    assert r.status_code == 200
    body = r.json()
    assert body["state"] == "FAILED" and body["result"] == "failed"
    assert body["reason"].startswith("VERIFICATION REQUIRED")
    # success carries weight/stable
    cid2 = client.post("/api/v1/queue/control",
                       json={"action": "ZERO", "channel_id": "CH2"}).json()["command_id"]
    _send("commands/ack", {"command_id": cid2, "state": "FAILED", "result": "timeout",
                           "reason": "no sample"}, device=SENDER)
    b2 = client.get(f"/api/v1/queue/control/{cid2}").json()
    assert b2["state"] == "FAILED" and b2["result"] == "timeout"


@pytest.mark.parametrize("action", ["ZERO", "TARE"])
def test_mqtt_applied_ack_for_zero_tare_is_invalid(client, pub, action):
    _seed()
    cid = client.post("/api/v1/queue/control",
                      json={"action": action, "channel_id": "CH2"}).json()["command_id"]
    _send("commands/ack", {"command_id": cid, "state": "APPLIED", "result": "success",
                           "weight_g": 0.0, "stable": True}, device=SENDER)
    b = client.get(f"/api/v1/queue/control/{cid}").json()
    assert b["state"] == "FAILED" and b["reason"] == "ACK_INVALID_ZERO_TARE_APPLIED"
    assert b["error"] == "ACK_INVALID_ZERO_TARE_APPLIED" and b["weight_g"] is None


def test_http_applied_ack_for_zero_tare_is_invalid(client, pub):
    _seed()
    cid = client.post("/api/v1/queue/control",
                      json={"action": "TARE", "channel_id": "CH2"}).json()["command_id"]
    r = client.post(f"/api/v1/device/commands/{cid}/ack", json={"state": "APPLIED"})
    assert r.status_code == 200 and r.json()["state"] == "FAILED"
    assert _row(cid).error_text == "ACK_INVALID_ZERO_TARE_APPLIED"


def test_http_ack_cannot_move_expired_command(client, pub):
    _seed()
    cid = client.post("/api/v1/queue/control",
                      json={"action": "TARE", "channel_id": "CH2"}).json()["command_id"]
    with Session(database.get_engine()) as db:
        db.get(DeviceCommand, cid).state = "EXPIRED"
        db.commit()
    r = client.post(f"/api/v1/device/commands/{cid}/ack", json={"state": "QUEUED"})
    assert r.json()["state"] == "EXPIRED"


def test_queue_page_has_zero_tare_buttons_with_confirm():
    from pathlib import Path
    js = (Path(__file__).resolve().parents[1] / "app" / "static" / "js" / "queue.js").read_text(encoding="utf-8")
    assert 'data-scale="ZERO"' in js and 'data-scale="TARE"' in js and "confirm(" in js
