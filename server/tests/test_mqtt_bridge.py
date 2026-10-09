"""MQTT bridge (contract section 7). In-memory publisher, no broker, no paho."""

import json
import re

import pytest
from sqlalchemy.orm import Session

from app import database, mqtt_bridge
from app.config import settings
from app.models import DeviceCommand, DeviceStatus
from conftest import DEVICE_ID, make_event, make_run_body, make_sample


class FakePublisher:
    def __init__(self, fail=False):
        self.sent = []
        self.fail = fail

    def publish(self, topic, payload, qos, retain):
        if self.fail:
            raise OSError("broker down")
        self.sent.append((topic, json.loads(payload), qos, retain))


@pytest.fixture()
def pub():
    fake = FakePublisher()
    mqtt_bridge.set_publisher(fake)
    yield fake
    mqtt_bridge.set_publisher(None)


def _status_body(state="IDLE"):
    return {"channels": [{"channel_id": "CH1", "state": state, "material_id": "M1"}],
            "queue": []}


def _send(topic_tail, body, device=DEVICE_ID, retain=False):
    mqtt_bridge.handle_message(f"cas/{device}/{topic_tail}",
                               body if isinstance(body, bytes) else json.dumps(body).encode(),
                               retain)


def _row(command_id):
    with Session(database.get_engine()) as db:
        return db.get(DeviceCommand, command_id)


def _profile(client, pid="m1-mq"):
    body = {"profile_id": pid, "material_id": "M1", "channel_id": "CH1", "target_g": 5000,
            "kp": 0.02, "ki": 0.001, "kd": 0.0, "tolerance_g": 20, "max_overshoot_g": 100,
            "max_duration_ms": 120000, "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40}
    assert client.post("/api/v1/profiles", json=body).status_code == 200


# --- config / topics -------------------------------------------------------

def test_bridge_disabled_by_default(monkeypatch):
    monkeypatch.delenv("MQTT_ENABLED", raising=False)
    assert settings.mqtt_enabled is False
    mqtt_bridge.start()  # no-op: must not connect or install a publisher
    assert mqtt_bridge._client is None and mqtt_bridge._publisher is None


@pytest.mark.parametrize("topic,expected", [
    (f"cas/{DEVICE_ID}/telemetry/samples", (DEVICE_ID, "samples")),
    (f"cas/{DEVICE_ID}/telemetry/status", (DEVICE_ID, "status")),
    (f"cas/{DEVICE_ID}/telemetry/events", (DEVICE_ID, "events")),
    (f"cas/{DEVICE_ID}/telemetry/live", (DEVICE_ID, "live")),
    (f"cas/{DEVICE_ID}/status", (DEVICE_ID, "presence")),
    (f"cas/{DEVICE_ID}/commands/ack", (DEVICE_ID, "ack")),
    (f"cas/{DEVICE_ID}/commands", None),          # our own outbound topic
    (f"cas/{DEVICE_ID}/telemetry/other", None),
    ("cas/bad id/telemetry/status", None),
    ("xyz/dev/telemetry/status", None),
])
def test_topic_mapping(topic, expected):
    assert mqtt_bridge.parse_topic(topic) == expected


# --- command publish -------------------------------------------------------

def test_command_published_qos1_not_retained(client, pub):
    _send("telemetry/status", _status_body())
    r = client.post("/api/v1/queue/control", json={"action": "READY", "channel_id": "CH1"})
    assert r.status_code == 200
    (topic, body, qos, retain), = pub.sent
    assert topic == f"cas/{DEVICE_ID}/commands" and qos == 1 and retain is False
    assert body["command_id"] == r.json()["command_id"]
    assert body["type"] == "READY" and body["channel_id"] == "CH1"
    assert body["ttl_ms"] == settings.transient_command_ttl_seconds * 1000
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", body["issued_at"])


def test_estop_has_no_expiry_and_envelope_complete(client, pub):
    _send("telemetry/status", _status_body())
    client.post("/api/v1/queue/control", json={"action": "ESTOP"})
    body = pub.sent[0][1]
    for key in ("command_id", "type", "channel_id", "issued_at", "ttl_ms"):
        assert key in body
    assert body["type"] == "ESTOP" and body["ttl_ms"] == 0


def test_job_and_profile_commands_publish_with_channel(client, pub):
    _send("telemetry/status", _status_body())
    _profile(client)
    assert client.post("/api/v1/profiles/m1-mq/1/activate").status_code == 200
    profile = [m for m in pub.sent if m[1]["type"] == "PROFILE"]
    assert profile and profile[0][1]["channel_id"] == "CH1" and profile[0][1]["target_g"] == 5000
    job = client.post("/api/v1/queue/jobs", json={
        "material_id": "M1", "target_g": 5000, "profile_id": "m1-mq", "profile_version": 1})
    assert job.status_code == 200, job.text
    sent = [m for m in pub.sent if m[1]["type"] == "JOB"][0][1]
    assert sent["channel_id"] == "CH1" and sent["profile"]["kp"] == 0.02


def test_broker_down_leaves_row_pending_and_http_fallback_delivers(client):
    _send("telemetry/status", _status_body())
    mqtt_bridge.set_publisher(FakePublisher(fail=True))
    try:
        r = client.post("/api/v1/queue/control", json={"action": "READY", "channel_id": "CH2"})
        assert r.status_code == 200  # publish failure never fails the API
    finally:
        mqtt_bridge.set_publisher(None)
    assert _row(r.json()["command_id"]).state == "PENDING"
    got = client.get("/api/v1/device/commands", params={"device_id": DEVICE_ID}).json()
    assert [c["command_id"] for c in got] == [r.json()["command_id"]]


class _Conn(FakePublisher):
    connected = True

    def is_connected(self):
        return self.connected


def _make_pending(client, action="READY", channel="CH1"):
    r = client.post("/api/v1/queue/control", json={"action": action, "channel_id": channel})
    assert r.status_code == 200
    return r.json()["command_id"]


def test_disconnected_bridge_does_not_publish_or_queue(client):
    _send("telemetry/status", _status_body())
    fake = _Conn()
    fake.connected = False
    mqtt_bridge.set_publisher(fake)
    try:
        cid = _make_pending(client)
        assert fake.sent == [] and _row(cid).state == "PENDING"
        fake.connected = True  # reconnect: nothing is replayed
        assert fake.sent == []
        mqtt_bridge.on_reconnect()
        assert fake.sent == []
    finally:
        mqtt_bridge.set_publisher(None)
    got = client.get("/api/v1/device/commands", params={"device_id": DEVICE_ID}).json()
    assert [c["command_id"] for c in got] == [cid]  # HTTP still delivers


def test_stale_command_never_published(client, pub):
    from datetime import timedelta
    _send("telemetry/status", _status_body())
    cid = _make_pending(client)
    pub.sent.clear()
    with Session(database.get_engine()) as db:
        row = db.get(DeviceCommand, cid)
        row.created_at = row.created_at - timedelta(seconds=120)
        db.commit()
    mqtt_bridge._publish_rows([cid])
    assert pub.sent == []


@pytest.mark.parametrize("ctype,expected", [
    ("JOB", 30000), ("CANCEL", 30000), ("HOLD", 30000), ("RELEASE", 30000),
    ("PROMOTE", 30000), ("PROFILE", 30000), ("ZERO", 3000), ("TARE", 3000),
    ("PUMP_START", 10000), ("READY", 10000), ("ESTOP", 0), ("PUMP_STOP", 0),
])
def test_ttl_ms_by_type(ctype, expected):
    assert mqtt_bridge.command_ttl_ms(ctype) == expected


def test_payload_is_compact_json(client):
    raw = []

    class Raw(FakePublisher):
        def publish(self, topic, payload, qos, retain):
            raw.append(payload)

    mqtt_bridge.set_publisher(Raw())
    try:
        _send("telemetry/status", _status_body())
        _make_pending(client)
    finally:
        mqtt_bridge.set_publisher(None)
    assert raw and ", " not in raw[0] and '": ' not in raw[0]


def test_paho_offline_queue_limited_and_no_republish_on_connect():
    calls = []

    class C:
        def max_queued_messages_set(self, n): calls.append(("q", n))
        def reconnect_delay_set(self, **kw): pass
        def is_connected(self): return False

    mqtt_bridge.configure_client(C())
    assert ("q", 0) in calls  # no cap: a cap of 1 broke ESTOP while awaiting PUBACK
    assert mqtt_bridge.PahoPublisher(C()).is_connected() is False


def test_no_publish_without_publisher(client):
    _send("telemetry/status", _status_body())
    assert client.post("/api/v1/queue/control",
                       json={"action": "READY", "channel_id": "CH1"}).status_code == 200


# --- ingest reuses the HTTP paths ------------------------------------------

def test_status_ingest_uses_http_validation(client):
    _send("telemetry/status", _status_body("DISPENSING"))
    status = client.get("/api/v1/device/status").json()[0]
    assert status["device_id"] == DEVICE_ID
    assert status["status"]["channels"][0]["state"] == "DISPENSING"
    # Same 422 rule as the HTTP route: bad fixed mapping is rejected.
    bad = _status_body()
    bad["channels"][0]["material_id"] = "M2"
    _send("telemetry/status", bad)
    assert client.get("/api/v1/device/status").json()[0]["status"]["channels"][0]["state"] == "DISPENSING"


def test_foreign_device_payload_is_ignored(client):
    body = {"device_id": "someone-else", **_status_body()}
    _send("telemetry/status", body)
    assert client.get("/api/v1/device/status").json() == []


def test_samples_and_events_ingest_and_dedupe(client, start_run):
    start_run("mq-run", 5000)
    samples = {"run_id": "mq-run", "material_id": "M1", "channel_id": "CH1",
               "samples": [make_sample(1, 250, material_id="M1"),
                           make_sample(2, 400, material_id="M1")]}
    _send("telemetry/samples", samples)
    _send("telemetry/samples", samples)  # QoS redelivery: idempotent
    events = {"run_id": "mq-run", "material_id": "M1", "channel_id": "CH1",
              "events": [make_event(1)]}
    _send("telemetry/events", events)
    run = client.get("/api/v1/runs/mq-run").json()
    assert run["sample_count"] == 2 and run["event_count"] == 1


def test_malformed_and_invalid_messages_never_reach_db(client, start_run):
    start_run("mq-bad", 5000)
    _send("telemetry/samples", b"{not json")
    _send("telemetry/samples", {})
    _send("telemetry/samples", {"run_id": "nope", "material_id": "M1",
                                "samples": [make_sample(1, 5)]})  # unknown run
    assert client.get("/api/v1/runs/mq-bad").json()["sample_count"] == 0


def test_retained_telemetry_is_ignored(client):
    _send("telemetry/status", _status_body(), retain=True)
    assert client.get("/api/v1/device/status").json() == []


def test_uptime_is_not_used_for_ordering(client):
    _send("telemetry/status", {**_status_body("IDLE"), "uptime_ms": 999999})
    first = client.get("/api/v1/device/status").json()[0]["updated_at"]
    _send("telemetry/status", {**_status_body("DISPENSING"), "uptime_ms": 5})  # smaller uptime
    now = client.get("/api/v1/device/status").json()[0]
    assert now["status"]["channels"][0]["state"] == "DISPENSING" and now["updated_at"] >= first


# --- ACK -------------------------------------------------------------------

def _queue_ready(client):
    _send("telemetry/status", _status_body())
    return client.post("/api/v1/queue/control",
                       json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]


def test_ack_applies_and_is_idempotent(client):
    cid = _queue_ready(client)
    ack = {"command_id": cid, "state": "APPLIED"}
    _send("commands/ack", ack)
    _send("commands/ack", ack)
    assert _row(cid).state == "APPLIED"
    assert client.get("/api/v1/device/commands", params={"device_id": DEVICE_ID}).json() == []


def test_ack_failed_reason_stored(client):
    cid = _queue_ready(client)
    _send("commands/ack", {"command_id": cid, "state": "FAILED", "reason": "not waiting"})
    row = _row(cid)
    assert row.state == "FAILED" and row.error_text == "not waiting"


def test_late_ack_cannot_resurrect_terminal_command(client):
    cid = _queue_ready(client)
    _send("commands/ack", {"command_id": cid, "state": "FAILED", "reason": "stale"})
    _send("commands/ack", {"command_id": cid, "state": "APPLIED"})  # duplicate/late
    assert _row(cid).state == "FAILED"


def test_ack_from_other_device_or_unknown_ignored(client):
    cid = _queue_ready(client)
    _send("commands/ack", {"command_id": cid, "state": "APPLIED"}, device="other-dev")
    _send("commands/ack", {"command_id": 99999, "state": "APPLIED"})
    _send("commands/ack", {"command_id": cid, "state": "BOGUS"})
    _send("commands/ack", {"state": "APPLIED"})
    assert _row(cid).state == "PENDING"


# --- presence / LWT --------------------------------------------------------

def test_presence_offline_then_online_resets_age(client):
    _send("telemetry/status", _status_body())
    stamp = client.get("/api/v1/device/status").json()[0]["updated_at"]
    _send("status", {"online": False}, retain=True)
    row = client.get("/api/v1/device/status").json()[0]
    assert row["status"]["online"] is False and row["updated_at"] == stamp  # age keeps growing
    with Session(database.get_engine()) as db:
        db.get(DeviceStatus, DEVICE_ID).updated_at = db.get(DeviceStatus, DEVICE_ID).updated_at.replace(year=2020)
        db.commit()
    _send("status", {"online": True})
    row = client.get("/api/v1/device/status").json()[0]
    assert row["status"]["online"] is True and row["age_seconds"] <= 5


# --- unconfirmed publish safety / ttl / integer grams ----------------------

def test_publish_rc_nonzero_is_not_sent_and_clears_published_at(client):
    from types import SimpleNamespace
    _send("telemetry/status", _status_body())
    cleared = []

    class FakeClient:
        _out_message_mutex = __import__("threading").RLock()
        _out_messages = {}
        _inflight_messages = 0
        def is_connected(self): return True
        def publish(self, topic, payload, qos, retain):
            self._out_messages[1] = SimpleNamespace(state="publish")  # parked
            return SimpleNamespace(rc=4, mid=1)  # MQTT_ERR_NO_CONN

    fc = FakeClient()
    mqtt_bridge.set_publisher(mqtt_bridge.PahoPublisher(fc))
    try:
        cid = _make_pending(client)
    finally:
        mqtt_bridge.set_publisher(None)
    row = _row(cid)
    assert row.state == "PENDING" and row.published_at is None
    assert fc._out_messages == {}  # nothing left to replay on reconnect
    mqtt_bridge.on_reconnect()
    assert fc._out_messages == {}


def _real_paho(connected=True, max_queued=0, max_inflight=20):
    import socket
    import paho.mqtt.client as m
    c = m.Client(m.CallbackAPIVersion.VERSION2, client_id="t")
    a, b = socket.socketpair()
    c._test_socks = (a, b)
    c.max_queued_messages_set(max_queued)
    c.max_inflight_messages_set(max_inflight)
    if connected:
        c._state = m._ConnectionState.MQTT_CS_CONNECTED
        c._sock = a
    return c


def test_second_publish_with_queue_full_keeps_estop_untouched():
    c = _real_paho(max_queued=1)
    pub = mqtt_bridge.PahoPublisher(c)
    assert pub.publish("t", "ESTOP", 1, False) is True
    estop_mid = next(iter(c._out_messages))
    c._out_packet.append({"command": 0x80, "mid": 9})  # e.g. a queued SUBSCRIBE
    assert pub.publish("t", "START", 1, False) is False
    assert list(c._out_messages) == [estop_mid]
    assert c._inflight_messages == 1
    assert len(c._out_packet) == 1  # untouched while connected


def test_queue_size_rc_clears_only_own_row(client):
    _send("telemetry/status", _status_body())
    c = _real_paho(max_queued=1)
    pub = mqtt_bridge.PahoPublisher(c)
    assert pub.publish("t", "ESTOP", 1, False) is True
    mqtt_bridge.set_publisher(pub)
    try:
        cid = _make_pending(client)
    finally:
        mqtt_bridge.set_publisher(None)
    row = _row(cid)
    assert row.state == "PENDING" and row.published_at is None
    assert len(c._out_messages) == 1


def test_parked_queued_message_is_not_sent():
    c = _real_paho(max_inflight=1)
    pub = mqtt_bridge.PahoPublisher(c)
    assert pub.publish("t", "ESTOP", 1, False) is True
    assert pub.publish("t", "START", 1, False) is False  # parked as queued
    assert len(c._out_messages) == 1 and c._inflight_messages == 1


def test_nothing_resent_after_disconnect_and_reconnect():
    import paho.mqtt.client as m
    c = _real_paho()
    pub = mqtt_bridge.PahoPublisher(c)
    assert pub.publish("t", "START", 1, False) is True
    c._out_packet.append({"command": 0x30})
    c._state = m._ConnectionState.MQTT_CS_CONNECTION_LOST
    mqtt_bridge.drop_queued_outgoing(c)  # what on_disconnect does
    assert not c._out_messages and not c._out_packet and c._inflight_messages == 0
    # message parked while down (paho NO_CONN path) is dropped on connect
    c._sock = None
    c.publish("t", "START2", qos=1)
    assert c._out_messages
    c._messages_reconnect_reset_out()  # paho does this before CONNACK
    mqtt_bridge.drop_stale_on_connect(c)
    assert not c._out_messages


def test_on_connect_keeps_message_published_after_connack():
    c = _real_paho()
    mqtt_bridge.PahoPublisher(c).publish("t", "ESTOP", 1, False)
    mqtt_bridge.drop_stale_on_connect(c)
    assert len(c._out_messages) == 1


def test_paho_publisher_refuses_when_disconnected():
    class FC:
        def is_connected(self): return False
        def publish(self, *a, **k): raise AssertionError("must not publish")

    assert mqtt_bridge.PahoPublisher(FC()).publish("t", "p", 1, False) is False


def test_expired_ttl_rows_not_published(client, pub):
    from datetime import timedelta
    _send("telemetry/status", _status_body())
    cid = _make_pending(client, "READY", "CH1")
    pub.sent.clear()
    with Session(database.get_engine()) as db:
        row = db.get(DeviceCommand, cid)
        row.created_at = row.created_at - timedelta(seconds=settings.transient_command_ttl_seconds + 5)
        row.published_at = None
        db.commit()
    mqtt_bridge._publish_rows([cid])
    assert pub.sent == [] and _row(cid).published_at is None


def test_ack_weight_g_integer_grams(client, pub):
    from pydantic import ValidationError
    from app.routes.operations import DeviceCommandAck
    assert mqtt_bridge.MqttAck(command_id=1, state="APPLIED", weight_g=5).weight_g == 5
    ok = mqtt_bridge.MqttAck(command_id=1, state="APPLIED", weight_g=0.0)
    assert ok.weight_g == 0 and isinstance(ok.weight_g, int)
    assert DeviceCommandAck(state="APPLIED", weight_g=12.0).weight_g == 12
    for bad in (1.5, "abc"):
        with pytest.raises(ValidationError):
            mqtt_bridge.MqttAck(command_id=1, state="APPLIED", weight_g=bad)
        with pytest.raises(ValidationError):
            DeviceCommandAck(state="APPLIED", weight_g=bad)

