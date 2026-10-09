"""CONTRACT section 8 stage 1: atomic claim, boot_id, transport flag, weight topic, health."""

import json
from datetime import timedelta

import pytest
from sqlalchemy.orm import Session

from app import database, mqtt_bridge
from app.models import DeviceCommand, DeviceStatus
from app.services import command_claim
from app.services.analytics import utcnow
from app.services.live_state import live_state

DEV = "relay-s1"
BOOT_A, BOOT_B = "aaaaaaaa", "bbbbbbbb"


class Fake:
    def __init__(self, ok=True):
        self.sent, self.ok = [], ok

    def publish(self, topic, payload, qos, retain):
        if not self.ok:
            return False
        self.sent.append(json.loads(payload))
        return True


@pytest.fixture()
def pub():
    fake = Fake()
    mqtt_bridge.set_publisher(fake)
    yield fake
    mqtt_bridge.set_publisher(None)


def send(tail, body, device=DEV, retain=False):
    raw = body if isinstance(body, bytes) else json.dumps(body).encode()
    mqtt_bridge.handle_message(f"cas/{device}/{tail}", raw, retain)


def birth(boot=BOOT_A, caps=("cmd_mqtt",), **kw):
    send("status", {"online": True, "boot_id": boot, "caps": list(caps)}, **kw)


def row(cid):
    with Session(database.get_engine()) as db:
        return db.get(DeviceCommand, cid)


def dev_row():
    with Session(database.get_engine()) as db:
        return db.get(DeviceStatus, DEV)


def new_cmd(kind="READY", state="PENDING", age_s=0, **kw):
    with Session(database.get_engine()) as db:
        created = utcnow() - timedelta(seconds=age_s)
        r = DeviceCommand(device_id=DEV, command_type=kind, channel_id="CH1", state=state,
                          created_at=created, updated_at=created, **kw)
        db.add(r)
        db.commit()
        return r.id


def mqtt_device(client):
    birth()
    r = client.put(f"/api/v1/device/{DEV}/command-transport", json={"transport": "MQTT"})
    assert r.status_code == 200, r.text


def http_ids(client):
    return [c["command_id"] for c in
            client.get("/api/v1/device/commands", params={"device_id": DEV}).json()]


# --- transport flag --------------------------------------------------------

def test_default_transport_is_http_and_mqtt_refused_without_cap(client):
    birth(caps=())
    assert dev_row().command_transport == "HTTP"
    r = client.put(f"/api/v1/device/{DEV}/command-transport", json={"transport": "MQTT"})
    assert r.status_code == 409 and dev_row().command_transport == "HTTP"
    assert client.put("/api/v1/device/nope/command-transport",
                      json={"transport": "MQTT"}).status_code == 404
    assert client.put(f"/api/v1/device/{DEV}/command-transport",
                      json={"transport": "X"}).status_code == 422


def test_mqtt_allowed_with_cap_and_cap_loss_reverts(client):
    mqtt_device(client)
    assert dev_row().command_transport == "MQTT"
    birth(boot=BOOT_A, caps=())
    assert dev_row().command_transport == "HTTP"


# --- claim race ------------------------------------------------------------

def test_claim_is_single_winner():
    cid = new_cmd()
    with Session(database.get_engine()) as a, Session(database.get_engine()) as b:
        assert command_claim.claim_pending(a, cid, "HTTP", None) is True
        a.commit()
        assert command_claim.claim_pending(b, cid, "MQTT", BOOT_A) is False
    r = row(cid)
    assert r.state == "DELIVERED" and r.delivered_via == "HTTP"


def test_mqtt_device_row_claimed_by_mqtt_and_never_served_by_http(client, pub):
    mqtt_device(client)
    r = client.post("/api/v1/queue/control", json={"action": "READY", "channel_id": "CH1"})
    cid = r.json()["command_id"]
    got = row(cid)
    assert got.state == "DELIVERED" and got.delivered_via == "MQTT"
    assert got.delivered_boot_id == BOOT_A and got.published_at is not None
    assert [m["command_id"] for m in pub.sent] == [cid]
    assert http_ids(client) == []
    mqtt_bridge.set_publisher(None)  # broker not available: row stays PENDING
    pend = new_cmd("PUMP_STOP")      # HTTP must still not take it
    assert http_ids(client) == [] and row(pend).state == "PENDING"


def test_http_device_keeps_push_on_top_and_http_claim_wins(client, pub):
    birth()  # HTTP transport (default)
    cid = client.post("/api/v1/queue/control",
                      json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]
    assert row(cid).state == "PENDING" and row(cid).published_at is not None and pub.sent
    assert http_ids(client) == [cid]
    got = row(cid)
    assert got.state == "DELIVERED" and got.delivered_via == "HTTP"


def test_http_row_claimed_by_mqtt_meanwhile_is_skipped(client, monkeypatch):
    """HTTP read the row PENDING, but MQTT wins the CAS before HTTP's own claim."""
    cid = new_cmd()
    real = command_claim.claim_pending

    def lose(db, command_id, via, boot):
        with Session(database.get_engine()) as other:
            real(other, command_id, "MQTT", BOOT_A)
            other.commit()
        return real(db, command_id, via, boot)

    monkeypatch.setattr(command_claim, "claim_pending", lose)
    assert http_ids(client) == []
    assert row(cid).delivered_via == "MQTT"


def test_mqtt_publish_failure_releases_only_our_claim(client):
    mqtt_device(client)
    mqtt_bridge.set_publisher(Fake(ok=False))
    try:
        cid = client.post("/api/v1/queue/control",
                          json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]
    finally:
        mqtt_bridge.set_publisher(None)
    r = row(cid)
    assert r.state == "PENDING" and r.delivered_via is None and r.published_at is None
    assert http_ids(client) == []  # still MQTT-only


# --- boot_id ---------------------------------------------------------------

def test_boot_change_closes_unacked_rows_and_never_resends(client, pub):
    mqtt_device(client)
    q = client.post("/api/v1/queue/control",
                    json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]
    send("commands/ack", {"command_id": q, "state": "QUEUED", "boot_id": BOOT_A})
    stop = client.post("/api/v1/queue/control", json={"action": "ESTOP"}).json()["command_id"]
    sent_before = len(pub.sent)
    birth(boot=BOOT_B)
    for cid in (q, stop):
        r = row(cid)
        assert r.state == "FAILED" and r.error_text == mqtt_bridge.REBOOT_REASON
    assert len(pub.sent) == sent_before  # nothing resent, STOP-class included
    # late / duplicate ACK on a terminal row is ignored
    send("commands/ack", {"command_id": q, "state": "APPLIED", "boot_id": BOOT_B})
    assert row(q).state == "FAILED"
    assert dev_row().boot_id == BOOT_B


def test_same_boot_and_first_boot_do_not_close(client, pub):
    mqtt_device(client)
    cid = client.post("/api/v1/queue/control",
                      json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]
    birth(boot=BOOT_A)
    assert row(cid).state == "DELIVERED"


def test_boot_change_via_status_and_ack_bodies(client, pub):
    mqtt_device(client)
    cid = client.post("/api/v1/queue/control",
                      json={"action": "READY", "channel_id": "CH1"}).json()["command_id"]
    send("telemetry/status", {"boot_id": BOOT_B, "channels": [], "queue": []})
    assert row(cid).state == "FAILED"
    cid2 = new_cmd("PUMP_STOP", state="DELIVERED", delivered_via="MQTT", delivered_boot_id=BOOT_B)
    send("commands/ack", {"command_id": cid2, "state": "QUEUED", "boot_id": "cccccccc"})
    assert row(cid2).state == "FAILED"


# --- restart / replay ------------------------------------------------------

def test_startup_recovery_sends_only_pending_within_ttl(client):
    mqtt_device(client)  # no publisher yet: seeded rows stay PENDING (server was down)
    fresh = new_cmd("READY")
    old = new_cmd("JOB", age_s=3600, target_g=5000, material_id="M1")
    stop = new_cmd("ESTOP", age_s=3600)
    stale = new_cmd("PUMP_STOP", state="DELIVERED", age_s=3600, delivered_via="MQTT")
    recent = new_cmd("READY", state="DELIVERED", delivered_via="MQTT")
    pub = Fake()
    mqtt_bridge.set_publisher(pub)
    try:
        mqtt_bridge.startup_recovery()
        assert sorted(m["command_id"] for m in pub.sent) == sorted([fresh, stop])
        assert row(old).state == "EXPIRED"
        assert row(stale).state == "FAILED" and row(recent).state == "DELIVERED"
        pub.sent.clear()
        mqtt_bridge.on_reconnect()  # reconnect never republishes
        assert pub.sent == []
    finally:
        mqtt_bridge.set_publisher(None)


def test_startup_recovery_ignores_http_devices(client):
    birth()
    old = new_cmd("JOB", age_s=3600, target_g=5000, material_id="M1")
    pub = Fake()
    mqtt_bridge.set_publisher(pub)
    try:
        mqtt_bridge.startup_recovery()
    finally:
        mqtt_bridge.set_publisher(None)
    assert row(old).state == "PENDING" and pub.sent == []


# --- presence / retained ---------------------------------------------------

def test_retained_online_true_is_hint_only_and_lwt_false_is_immediate(client):
    birth()
    before = dev_row().updated_at
    send("status", {"online": True}, retain=True)
    d = dev_row()
    assert d.status_json.get("online_hint") is True and d.updated_at == before
    send("status", {"online": False})
    assert dev_row().status_json["online"] is False  # OFFLINE, no e-stop semantics
    assert "estop" not in json.dumps(dev_row().status_json).lower()


def test_retained_birth_alone_creates_row_with_stale_age(client):
    birth(retain=True)
    d = dev_row()
    assert d.boot_id == BOOT_A and d.caps_json == ["cmd_mqtt"] and d.updated_at.year == 1970


def test_retained_telemetry_ignored(client):
    send("telemetry/status", {"channels": [], "queue": []}, retain=True)
    assert dev_row() is None


# --- weight topic ----------------------------------------------------------

def test_subscriptions_use_reduced_weight_only():
    topics = [t for t, _ in mqtt_bridge.SUBSCRIPTIONS]
    assert "cas/+/telemetry/weight" in topics
    assert not any(t.endswith("weight/ctl") or t == "cas/+/weight" for t in topics)
    assert mqtt_bridge.parse_topic("cas/snd/weight/ctl") is None
    assert mqtt_bridge.parse_topic("cas/snd/telemetry/weight") == ("snd", "weight")


def test_weight_topic_feeds_live_state_and_rejects_bad_payloads(client):
    live_state.clear()
    good = {"uptime_ms": 1000, "channel": "CH1", "weight_g": 1234, "stable": True, "age_ms": 20}
    send("telemetry/weight", good, device="snd")
    assert live_state.snapshot()["diagnostics"]["received"] == 1
    for bad in ({**good, "uptime_ms": 2000, "weight_g": 1.5}, {**good, "channel": "CH3"},
                {**good, "uptime_ms": 3000, "age_ms": -1}):
        send("telemetry/weight", bad, device="snd")
    send("telemetry/weight", b'{"uptime_ms":4000,"channel":"CH1","weight_g":NaN,"age_ms":1}',
         device="snd")
    send("telemetry/weight", b"x" * 600, device="snd")  # over max bytes
    send("telemetry/weight", good, device="snd", retain=True)  # retained: ignored
    assert live_state.snapshot()["diagnostics"]["received"] == 1


def test_non_finite_numbers_rejected(client):
    send("telemetry/status", b'{"channels":[{"channel_id":"CH1","weight_age_ms":Infinity}]}')
    send("telemetry/status", b'{"x":1e999}')
    assert dev_row() is None


# --- callbacks do not run SQL ----------------------------------------------

def test_sql_kinds_are_queued_not_handled_on_callback_thread(monkeypatch):
    called = []
    monkeypatch.setattr(mqtt_bridge, "handle_message", lambda *a: called.append(a))
    drained = []
    while not mqtt_bridge._queue.empty():
        drained.append(mqtt_bridge._queue.get_nowait())
    mqtt_bridge.enqueue_message(f"cas/{DEV}/telemetry/status", b"{}", False)
    assert called == [] and mqtt_bridge._queue.qsize() == 1
    mqtt_bridge.enqueue_message(f"cas/{DEV}/telemetry/live", b"{}", False)
    assert len(called) == 1  # memory-only kind runs inline
    mqtt_bridge._queue.get_nowait()


def test_queue_is_bounded(monkeypatch):
    monkeypatch.setattr(mqtt_bridge, "_queue", mqtt_bridge.queue.Queue(maxsize=2))
    for _ in range(5):
        mqtt_bridge.enqueue_message(f"cas/{DEV}/telemetry/status", b"{}", False)
    assert mqtt_bridge._queue.qsize() == 2


# --- health ----------------------------------------------------------------

class FakeClient:
    def __init__(self, rc=0):
        self.mids, self.rc = [], rc

    def subscribe(self, topic, qos=0):
        self.mids.append(len(self.mids) + 1)
        return (self.rc, self.mids[-1])


@pytest.fixture()
def bridge_state(monkeypatch):
    monkeypatch.setattr(mqtt_bridge, "_client", object())
    monkeypatch.setattr(mqtt_bridge, "_recovered", True)  # no recovery side effects
    mqtt_bridge.handle_disconnect(FakeClient(), "reset")


def test_health_unknown_when_bridge_not_started(client):
    assert client.get("/health").json()["mqtt"]["state"] == "UNKNOWN"


def test_health_connected_only_after_connect_and_all_subacks(client, bridge_state):
    c = FakeClient()
    assert client.get("/health").json()["mqtt"]["state"] == "DISCONNECTED"
    mqtt_bridge.handle_connect(c, 0)
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"  # no SUBACK yet
    for mid in c.mids[:-1]:
        mqtt_bridge.handle_subscribe(mid, [0])
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"
    mqtt_bridge.handle_subscribe(c.mids[-1], [1])
    assert client.get("/health").json()["mqtt"]["state"] == "CONNECTED"
    mqtt_bridge.handle_disconnect(c, 0)
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"


def test_health_not_connected_when_suback_refused_or_connect_failed(bridge_state):
    class Failure:
        is_failure = True

    mqtt_bridge.handle_connect(FakeClient(), Failure())
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"
    c = FakeClient()
    mqtt_bridge.handle_connect(c, 0)
    for mid in c.mids:
        mqtt_bridge.handle_subscribe(mid, [128])
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"
    c2 = FakeClient(rc=4)  # subscribe call itself failed
    mqtt_bridge.handle_connect(c2, 0)
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"
