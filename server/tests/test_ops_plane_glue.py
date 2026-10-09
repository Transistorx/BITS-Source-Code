"""Operator plane wired into the headless Service (CONTRACT 9.4, 9.5): driven through the
in-process fake paho client of test_service. No broker, no network; service threads are real."""
import json
import time

import pytest
from argon2 import PasswordHasher
from sqlalchemy import select

from app import database, service as service_mod
from app.models import DeviceStatus, DispenseRun
from app.rpc.envelope import iso_ms
from app.services import auth as auth_service
from app.services.analytics import utcnow
from datetime import datetime, timezone

from test_service import DB_LOCK, DEV, FakeClient, _RC, new_service, wait_for

PW = "correct-horse-1"
OPS = "bits/v1/ops/"


@pytest.fixture()
def fake():
    return FakeClient()


@pytest.fixture()
def svc(client, fake):
    auth_service.set_hasher(PasswordHasher(time_cost=1, memory_cost=8, parallelism=1))
    with DB_LOCK, database._SessionLocal() as db:
        auth_service.create_operator(db, "opr1", PW, "operator")
        db.add(DeviceStatus(device_id=DEV, updated_at=datetime.utcnow(), status_json={
            "role": "relay_controller", "channels": [
                {"channel_id": "CH1", "state": "IDLE", "relay_on": False, "active_job_id": 0}]}))
        db.commit()
    s = new_service(fake)
    s.start()
    fake.connack()
    assert wait_for(lambda: s.recovered)
    yield s
    s.stop()
    auth_service.set_hasher(PasswordHasher())


def req(fake, method, args=None, corr="c1", token=None, client_id="app_a", **over):
    body = {"v": 1, "corr_id": corr, "method": method, "args": args or {}, "session_token": token,
            "issued_at": iso_ms(datetime.now(timezone.utc)), "ttl_ms": 10000, "client_seq": 1}
    body.update(over)
    fake.on_message(fake, None, type("M", (), {
        "topic": f"{OPS}req/{client_id}/{method}", "payload": json.dumps(body).encode(), "retain": False})())


def published(fake, prefix=None):
    with fake._lock:
        out = list(fake.published)
    return [m for m in out if prefix is None or m[0].startswith(prefix)]


def res(fake, corr, client_id="app_a"):
    topic = f"{OPS}res/{client_id}/{corr}"
    assert wait_for(lambda: [m for m in published(fake) if m[0] == topic], 5), published(fake)[-5:]
    return [m for m in published(fake) if m[0] == topic][0]


def login(fake):
    req(fake, "auth.login", {"username": "opr1", "password": PW}, corr="login")
    return json.loads(res(fake, "login")[1])["data"]["session_token"]


def test_request_subscription_is_qos1_and_granted_before_connected(client, fake):
    s = new_service(fake)
    s.start()
    try:
        fake.connack(suback=False)
        assert (OPS + "req/+/+", 1) in [(t, q) for t, q, _m in fake.subs]
        first, last = fake.subs[:-1], fake.subs[-1]
        for _t, _q, mid in first:
            fake.on_subscribe(fake, None, mid, [_RC()], None)
        assert s.mqtt_state() == "DISCONNECTED"             # one SUBACK missing: not CONNECTED
        fake.on_subscribe(fake, None, last[2], [_RC()], None)
        assert s.mqtt_state() == "CONNECTED"
    finally:
        s.stop()


def test_reply_is_qos1_non_retained_on_the_client_topic_then_events_then_refresh(svc, fake):
    tok = login(fake)
    topic, payload, qos, retain = res(fake, "login")
    assert topic == f"{OPS}res/app_a/login" and qos == 1 and retain is False
    body = json.loads(payload)
    assert set(body) == {"corr_id", "ok", "code", "error", "data", "next_cursor", "server_time", "seq"}
    n = len(published(fake))
    req(fake, "control.cmd", {"action": "ESTOP"}, corr="stop", token=tok)
    topic, payload, qos, retain = res(fake, "stop")
    assert json.loads(payload)["ok"] is True
    assert wait_for(lambda: any(m[0] == OPS + "state/devices" for m in published(fake)[n:]), 5)
    order = [m[0] for m in published(fake)[n:] if m[0].startswith(OPS) and
             (m[0].startswith(OPS + "res/") or m[0] == OPS + "evt/command" or m[0].startswith(OPS + "state/q")
              or m[0] == OPS + "state/devices")]
    assert order[0] == f"{OPS}res/app_a/stop" and order[1] == OPS + "evt/command"
    assert order[2:4] == [OPS + "state/queue", OPS + "state/devices"]
    evt = [m for m in published(fake) if m[0] == OPS + "evt/command"][-1]
    assert evt[2] == 1 and evt[3] is False
    for name in ("queue", "devices"):
        t, p, q, r = [m for m in published(fake) if m[0] == OPS + "state/" + name][-1]
        assert q == 1 and r is True


def test_state_live_is_retained_qos0_and_evt_live_is_non_retained_qos0_with_contiguous_seq(svc, fake):
    assert wait_for(lambda: published(fake, OPS + "state/live"), 5)
    t, p, q, r = published(fake, OPS + "state/live")[0]
    assert q == 0 and r is True
    assert wait_for(lambda: len(published(fake, OPS + "evt/live")) >= 3, 6)
    evts = published(fake, OPS + "evt/live")
    assert all(m[2] == 0 and m[3] is False for m in evts)
    seqs = [json.loads(m[1])["seq"] for m in evts]
    assert seqs == list(range(seqs[0], seqs[0] + len(seqs)))          # gap detection is meaningful
    assert len(p) <= 4096 and all(len(m[1]) <= 2048 for m in evts)
    epochs = {json.loads(m[1])["epoch"] for m in evts} | {json.loads(p)["epoch"]}
    assert epochs == {svc.epoch}                                       # one epoch for the whole backend
    assert json.loads(published(fake, OPS + "state/backend")[0][1])["epoch"] == svc.epoch


def test_backend_never_publishes_weight_ctl_or_any_cas_topic_except_commands_and_run_acks(svc, fake):
    tok = login(fake)
    req(fake, "control.cmd", {"action": "ESTOP"}, corr="s1", token=tok)
    res(fake, "s1")
    time.sleep(0.5)
    assert svc.publish("cas/snd1/weight/ctl", "{}", 0, False) is False      # hard refusal
    assert not [m for m in published(fake) if m[0].endswith("weight/ctl")]
    cas = {m[0] for m in published(fake) if m[0].startswith("cas/")}
    assert all(t.endswith("/commands") or t.endswith("/run/ack") for t in cas), cas
    assert all(m[0].startswith(("cas/", OPS)) for m in published(fake))
    assert svc.ops._pub("cas/x/commands", "{}", 1, False) is False          # ops plane stays in bits/v1/ops


def test_retained_oversize_and_foreign_requests_are_ignored(svc, fake):
    n = len(published(fake))
    fake.on_message(fake, None, type("M", (), {"topic": f"{OPS}req/app_a/live.resync",
                    "payload": b"{}", "retain": True})())
    fake.on_message(fake, None, type("M", (), {"topic": f"{OPS}req/app_a/live.resync",
                    "payload": b"x" * 9000, "retain": False})())
    time.sleep(0.5)
    assert not [m for m in published(fake)[n:] if m[0].startswith(OPS + "res/")]
    # a request whose topic client differs from the envelope-authenticated one is answered INVALID
    tok = login(fake)
    req(fake, "queue.list", corr="x", token=tok, client_id="app_b")
    assert json.loads(res(fake, "x", "app_b")[1])["code"] == "UNAUTHENTICATED"   # session bound to app_a


def test_rpc_queue_overflow_is_counted_not_blocking(client, fake):
    s = new_service(fake)
    s.ops._q = __import__("queue").Queue(maxsize=2)          # no workers started: nothing drains
    for i in range(5):
        s.ops._on_request(f"{OPS}req/app_a/queue.list", b"{}", False)
    assert s.ops.stats["dropped"] == 3 and s.ops._q.qsize() == 2


def test_bulk_ingest_overflow_is_counted_and_reported_in_backend_state(client, fake, monkeypatch):
    monkeypatch.setattr(service_mod, "BULK_MAX", 3)
    s = new_service(fake)
    s._accepting = True                                      # writer not started: queue cannot drain
    s._client = fake
    for _ in range(8):
        s._on_message(fake, None, type("M", (), {"topic": "cas/rly1/telemetry/live",
                                                 "payload": b"{}", "retain": False})())
    assert s.stats["bulk_dropped"] == 5 and s.queue_depths()["bulk"] == 3
    assert s.state_snapshot()["drops"]["bulk"] == 5
    s._client = None


def test_birth_with_new_boot_id_interrupts_a_running_run_even_without_a_new_run(svc, fake):
    with DB_LOCK, database._SessionLocal() as db:
        db.add(DispenseRun(run_id="r1", target_g=5000, status="RUNNING", started_at=utcnow(),
                           device_id=DEV, boot_id="aaaaaaaa"))
        db.commit()
    fake.deliver("status", {"online": True, "boot_id": "aaaaaaaa", "caps": ["cmd_mqtt"]}, retain=False,
                 topic=f"cas/{DEV}/status")
    time.sleep(0.4)
    with DB_LOCK, database._SessionLocal() as db:
        assert db.get(DispenseRun, "r1").status == "RUNNING"          # same boot (reconnect)
    fake.deliver("status", {"online": True, "boot_id": "bbbbbbbb", "caps": ["cmd_mqtt"]}, retain=True,
                 topic=f"cas/{DEV}/status")
    time.sleep(0.4)
    with DB_LOCK, database._SessionLocal() as db:
        assert db.get(DispenseRun, "r1").status == "RUNNING"          # retained replay is not a boot
    fake.deliver("status", {"online": True, "boot_id": "bbbbbbbb", "caps": ["cmd_mqtt"]}, retain=False,
                 topic=f"cas/{DEV}/status")
    assert wait_for(lambda: _status("r1") == "INTERRUPTED", 5)


def _status(run_id):
    with DB_LOCK, database._SessionLocal() as db:
        return db.get(DispenseRun, run_id).status
