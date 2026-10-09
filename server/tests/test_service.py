"""Headless MQTT backend service (CONTRACT 9.1-9.3, 9.8) driven through an in-process
fake paho client: no broker, no network. The service threads are real."""
import hashlib
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest
from sqlalchemy import select
from sqlalchemy.exc import OperationalError
from sqlalchemy.orm import Session

from app import database, mqtt_bridge, recovery, service as service_mod
from app.models import DeviceCommand, DeviceStatus, DispenseRun, RunBatch, WeightSample
from app.services import profile_pin
from app.services.analytics import utcnow
from conftest import make_sample

DEV = "bits-a4cf12ab34cd"
RUN = "bits-a4cf12ab34cd-ch1-j7-1834-a3f9"
BOOT_A, BOOT_B = "aabbccdd", "11223344"
SERVER_DIR = Path(__file__).resolve().parents[1]


class _Info:
    def __init__(self, mid):
        self.rc, self.mid = 0, mid


class _RC:
    is_failure = False


class _Msg:
    def __init__(self, topic, payload, retain=False):
        self.topic, self.payload, self.retain = topic, payload, retain


class FakeClient:
    def __init__(self):
        self.published, self.subs, self.connected, self._mid = [], [], False, 0
        self.will = None
        self._lock = threading.Lock()

    def max_queued_messages_set(self, _n): ...
    def reconnect_delay_set(self, **_kw): ...
    def username_pw_set(self, *_a): ...
    def connect_async(self, *_a, **_kw): ...
    def loop_start(self): ...
    def loop_stop(self): ...
    def will_set(self, topic, payload, qos=0, retain=False):
        self.will = (topic, json.loads(payload), qos, retain)

    def disconnect(self):
        self.connected = False

    def is_connected(self):
        return self.connected

    def subscribe(self, topic, qos=0):
        self._mid += 1
        self.subs.append((topic, qos, self._mid))
        return (0, self._mid)

    def publish(self, topic, payload, qos=0, retain=False):
        with self._lock:
            self.published.append((topic, payload, qos, retain))
            return _Info(len(self.published))

    # test drivers
    def connack(self, suback=True):
        self.connected = True
        self.subs = []
        self.on_connect(self, None, {}, _RC(), None)
        if suback:
            for _t, _q, mid in list(self.subs):
                self.on_subscribe(self, None, mid, [_RC()], None)

    def drop_link(self):
        self.connected = False
        self.on_disconnect(self, None, None, "lost", None)

    def deliver(self, tail, body, device=DEV, retain=False, topic=None):
        payload = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.on_message(self, None, _Msg(topic or f"cas/{device}/{tail}", payload, retain))

    def acks(self, run_id=None):
        with self._lock:
            out = [json.loads(p) for t, p, _q, _r in self.published if t.endswith("/run/ack")]
        return [a for a in out if run_id in (None, a["run_id"])]

    def states(self):
        with self._lock:
            return [json.loads(p) for t, p, _q, r in self.published
                    if t == "bits/v1/ops/state/backend"]


# SQLite uses one shared connection (StaticPool): any other thread opening/closing a session
# rolls back the service threads' in-flight transaction. Tests therefore share the service's
# lock for every DB access they make while the service is running (MySQL has a real pool).
DB_LOCK = threading.RLock()


def new_service(fake_client):
    s = service_mod.Service(client_factory=lambda: fake_client)
    s._sqlite_lock = DB_LOCK
    return s


def wait_for(cond, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


@pytest.fixture()
def fake():
    return FakeClient()


@pytest.fixture()
def svc(client, fake):
    s = new_service(fake)
    s.start()
    fake.connack()
    assert wait_for(lambda: s.recovered)
    yield s
    s.stop()


def start_body(run_id=RUN, boot=BOOT_A, **kw):
    body = {"run_id": run_id, "boot_id": boot, "message_id": f"{boot}-1", "material_id": "M1",
            "channel_id": "CH1", "device_id": DEV, "job_id": 7, "target_g": 5000, "priority": 0,
            "firmware": "relay 6.1", "start_weight_g": 0, "profile_id": "default",
            "profile_version": 3,
            "config": {"kp": 0.0025, "ki": 0.0003, "kd": 0.0001, "tolerance_g": 20,
                       "max_overshoot_g": 100, "max_duration_ms": 120000, "window_ms": 500,
                       "min_on_ms": 40, "min_off_ms": 40, "inflight_comp_g": 5,
                       "settle_time_ms": 1500}}
    body.update(kw)
    return body


def samples_body(seq, idxs, run_id=RUN, boot=BOOT_A):
    return {"run_id": run_id, "boot_id": boot, "batch_seq": seq,
            "samples": [make_sample(i, 100 * i) for i in idxs]}


def complete_body(end_seq, total_samples, run_id=RUN, boot=BOOT_A, **kw):
    body = {"run_id": run_id, "boot_id": boot, "message_id": f"{boot}-99", "status": "COMPLETE",
            "final_weight_g": 5003, "max_weight_g": 5003, "duration_ms": 9000, "error": "",
            "end_seq": end_seq, "total_batches": end_seq, "total_samples": total_samples,
            "total_events": 0, "dropped_samples": 0}
    body.update(kw)
    return body


def run_row(run_id=RUN):
    with DB_LOCK, Session(database.get_engine()) as db:
        return db.get(DispenseRun, run_id)


def sample_count(run_id=RUN):
    with DB_LOCK, Session(database.get_engine()) as db:
        return len(db.scalars(select(WeightSample).where(WeightSample.run_id == run_id)).all())


def batches(run_id=RUN):
    with DB_LOCK, Session(database.get_engine()) as db:
        return sorted(r.batch_seq for r in db.scalars(
            select(RunBatch).where(RunBatch.run_id == run_id)))


def last_ack(fake, run_id=RUN, n=1):
    assert wait_for(lambda: len(fake.acks(run_id)) >= n), f"no ack #{n}: {fake.acks(run_id)}"
    return fake.acks(run_id)[n - 1]


# --- process / lifecycle ------------------------------------------------------

def test_service_path_has_no_fastapi():
    code = ("import sys; sys.path.insert(0, %r); import app.service, app.run_ingest, app.recovery; "
            "bad=[m for m in ('fastapi','starlette','uvicorn') if m in sys.modules]; "
            "sys.exit(1 if bad else 0)" % str(SERVER_DIR))
    done = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True,
                          cwd=str(SERVER_DIR))
    assert done.returncode == 0, done.stderr[-800:]


def test_connected_only_after_connack_and_all_subacks(client, fake):
    s = new_service(fake)
    s.start()
    try:
        assert s.mqtt_state() == "DISCONNECTED"
        fake.connack(suback=False)
        assert s.mqtt_state() == "DISCONNECTED" and not s.recovered
        topics = {t for t, _q, _m in fake.subs}
        assert {"cas/+/run/start", "cas/+/run/samples", "cas/+/run/events",
                "cas/+/run/complete"} <= topics
        assert all(q == 1 for t, q, _m in fake.subs if "/run/" in t)
        *first, last = fake.subs
        for _t, _q, mid in first:
            fake.on_subscribe(fake, None, mid, [_RC()], None)
        assert s.mqtt_state() == "DISCONNECTED"
        fake.on_subscribe(fake, None, last[2], [_RC()], None)
        assert s.mqtt_state() == "CONNECTED"
        assert fake.will[0] == "bits/v1/ops/state/backend" and fake.will[1] == {"online": False}
        assert fake.will[3] is True
    finally:
        s.stop()


def test_heartbeat_is_retained_state_and_graceful_stop_publishes_offline(client, fake):
    s = new_service(fake)
    s.start()
    fake.connack()
    assert wait_for(lambda: fake.states())
    hb = fake.states()[0]
    for key in ("online", "seq", "epoch", "uptime_s", "db", "mqtt", "queues", "drops",
                "writer_lag_ms", "last_ingest_age_ms"):
        assert key in hb
    assert hb["online"] is True and hb["db"]["ok"] is True and hb["mqtt"] == "CONNECTED"
    assert all(retain for t, _p, _q, retain in fake.published if t.endswith("state/backend"))
    s.stop()
    assert fake.states()[-1]["online"] is False
    assert s.mqtt_state() == "STOPPED"


# --- run ingest ------------------------------------------------------------

def test_start_acked_after_commit_with_exact_target_and_profile(svc, fake):
    fake.deliver("run/start", start_body())
    ack = last_ack(fake)
    assert ack == {"run_id": RUN, "acked_batch_seq": 0, "state": "START_OK"}
    run = run_row()
    assert run.target_g == 5000 and run.profile_id == "default" and run.profile_version == 3
    assert run.boot_id == BOOT_A and run.status == "RUNNING" and run.device_id == DEV
    assert run.settle_ms == 1500  # contract settle_time_ms mapped
    assert batches() == [0]
    topic, _p, qos, retain = [m for m in fake.published if m[0].endswith("/run/ack")][0]
    assert topic == f"cas/{DEV}/run/ack" and qos == 1 and retain is False


def test_non_integer_target_is_refused_not_coerced(svc, fake):
    fake.deliver("run/start", start_body(target_g=5000.0))
    assert last_ack(fake)["state"] == "REJECTED"
    assert run_row() is None


def test_duplicate_batch_is_acked_again_never_double_stored(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2, 3]))
    fake.deliver("run/samples", samples_body(1, [1, 2, 3]))
    assert wait_for(lambda: len(fake.acks()) >= 3)
    assert [a["acked_batch_seq"] for a in fake.acks()] == [0, 1, 1]
    assert sample_count() == 3 and run_row().sample_count == 3 and batches() == [0, 1]


def test_out_of_order_batch_acks_only_the_contiguous_watermark(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(2, [4, 5]))
    assert last_ack(fake, n=2)["acked_batch_seq"] == 0  # seq 1 still missing
    fake.deliver("run/samples", samples_body(1, [1, 2, 3]))
    assert last_ack(fake, n=3)["acked_batch_seq"] == 2
    assert sample_count() == 5


def test_complete_without_gaps_is_complete(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    fake.deliver("run/complete", complete_body(1, 2))
    ack = last_ack(fake, n=3)
    assert ack["state"] == "COMPLETE" and ack["acked_batch_seq"] == 1
    run = run_row()
    assert run.status == "COMPLETE" and run.missing_ranges is None and run.final_weight_g == 5003


def test_gap_gives_complete_partial_then_late_batch_upgrades(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    fake.deliver("run/samples", samples_body(3, [5, 6]))
    fake.deliver("run/complete", complete_body(3, 6))
    ack = last_ack(fake, n=4)
    assert ack["state"] == "COMPLETE_PARTIAL" and ack["acked_batch_seq"] == 1
    assert ack["missing_ranges"]["batch_seq"] == [[2, 2]]
    assert ack["missing_ranges"]["sample_idx"] == [[3, 4]]
    run = run_row()
    assert run.status == "COMPLETE_PARTIAL" and run.missing_ranges["batch_seq"] == [[2, 2]]
    fake.deliver("run/samples", samples_body(2, [3, 4]))
    assert wait_for(lambda: run_row().status == "COMPLETE"), (run_row().status, run_row().missing_ranges, run_row().ingest_json, batches(), sample_count(), fake.acks(), svc.stats)
    assert run_row().missing_ranges is None


def test_overflow_dropped_samples_are_reported_as_idx_ranges(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2, 3, 7, 8]))  # idx 4..6 dropped on relay
    fake.deliver("run/complete", complete_body(1, 8, dropped_samples=3))
    ack = last_ack(fake, n=3)
    assert ack["state"] == "COMPLETE_PARTIAL" and ack["missing_ranges"] == {"sample_idx": [[4, 6]]}


def tomb_body(seq, count, run_id=RUN, boot=BOOT_A):
    return {"run_id": run_id, "boot_id": boot, "batch_seq": seq, "dropped": True, "samples": count}


def test_tombstone_advances_the_ack_and_the_run_ends_partial_with_the_gap(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    fake.deliver("run/samples", tomb_body(2, 2))              # idx 3,4 dropped on the relay
    fake.deliver("run/samples", samples_body(3, [5, 6]))
    fake.deliver("run/samples", tomb_body(2, 2))              # replay: idempotent
    assert wait_for(lambda: len(fake.acks()) >= 5)
    assert [a["acked_batch_seq"] for a in fake.acks()] == [0, 1, 2, 3, 3]
    assert batches() == [0, 1, 2, 3] and sample_count() == 4
    fake.deliver("run/complete", complete_body(3, 6, dropped_samples=2))
    ack = last_ack(fake, n=6)
    assert ack["state"] == "COMPLETE_PARTIAL"
    assert ack["missing_ranges"] == {"batch_seq": [[2, 2]], "sample_idx": [[3, 4]]}
    run = run_row()
    assert run.status == "COMPLETE_PARTIAL" and run.ingest_json["dropped_samples"] == 2


def test_tombstone_after_complete_still_acks_and_malformed_one_is_rejected(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", {**tomb_body(1, 2), "samples": "2"})   # not an integer count
    assert last_ack(fake, n=2)["state"] == "REJECTED" and batches() == [0]
    fake.deliver("run/samples", tomb_body(1, 2))
    fake.deliver("run/complete", complete_body(1, 2, dropped_samples=2))
    assert last_ack(fake, n=4)["state"] == "COMPLETE_PARTIAL"
    fake.deliver("run/samples", tomb_body(1, 2))
    ack = last_ack(fake, n=5)
    assert ack["state"] == "BATCH_OK" and ack["acked_batch_seq"] == 1
    assert run_row().status == "COMPLETE_PARTIAL"           # a tombstone never closes the gap


def test_late_real_batch_replaces_a_tombstone_and_upgrades(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", tomb_body(1, 2))
    fake.deliver("run/complete", complete_body(1, 2, dropped_samples=2))
    assert last_ack(fake, n=3)["state"] == "COMPLETE_PARTIAL"
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    assert wait_for(lambda: run_row().status == "COMPLETE"), run_row().missing_ranges


def test_events_tombstone_is_stored_idempotently_and_run_ends_partial(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/events", tomb_body(1, 0))
    fake.deliver("run/events", tomb_body(1, 0))               # replay: idempotent
    assert wait_for(lambda: len(fake.acks()) >= 3)
    assert [a["state"] for a in fake.acks()] == ["START_OK", "BATCH_OK", "BATCH_OK"]
    assert batches() == [0, 1]
    fake.deliver("run/complete", complete_body(1, 0))
    ack = last_ack(fake, n=4)
    assert ack["state"] == "COMPLETE_PARTIAL" and ack["missing_ranges"] == {"batch_seq": [[1, 1]]}
    assert run_row().status == "COMPLETE_PARTIAL"


def test_bad_events_tombstone_is_rejected(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/events", {**tomb_body(1, 0), "samples": "0"})
    assert last_ack(fake, n=2)["state"] == "REJECTED" and batches() == [0]
    fake.deliver("run/events", {**tomb_body(1, 0), "batch_seq": "1"})
    assert last_ack(fake, n=3)["state"] == "REJECTED" and batches() == [0]


def test_run_resumes_normally_after_a_long_outage(svc, fake):
    """No data for longer than RUN_STALE_SECONDS: flagged STALLED, never finalised; the
    relay's resend continues ingest and the flag clears."""
    from datetime import timedelta
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    assert last_ack(fake, n=2)["acked_batch_seq"] == 1
    with DB_LOCK, Session(database.get_engine()) as db:
        old = utcnow() - timedelta(minutes=5)
        for b in db.scalars(select(RunBatch).where(RunBatch.run_id == RUN)):
            b.received_at = old
        db.commit()
    with DB_LOCK, Session(database.get_engine()) as db:
        assert recovery.stalled_runs(db, svc.run_stale_s) == [
            {"run_id": RUN, "device_id": DEV, "channel_id": "CH1", "flag": "STALLED"}]
        assert db.get(DispenseRun, RUN).status == "RUNNING"
    fake.deliver("run/samples", samples_body(2, [3, 4]))
    fake.deliver("run/complete", complete_body(2, 4))
    assert last_ack(fake, n=4)["state"] == "COMPLETE"
    assert run_row().status == "COMPLETE"
    with DB_LOCK, Session(database.get_engine()) as db:
        assert recovery.stalled_runs(db, svc.run_stale_s) == []


def test_status_with_a_new_boot_id_interrupts_a_run_whose_birth_was_missed(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("telemetry/status", {"role": "relay_controller", "boot_id": BOOT_A,
                                      "uptime_ms": 10, "channels": []})
    time.sleep(0.3)
    assert run_row().status == "RUNNING"
    fake.deliver("telemetry/status", {"role": "relay_controller", "boot_id": BOOT_B,
                                      "uptime_ms": 20, "channels": []})
    assert wait_for(lambda: run_row().status == "INTERRUPTED")
    assert run_row().error_text == "DEVICE_REBOOTED"


def test_relay_reboot_mid_run_interrupts_and_is_never_resumed(svc, fake):
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    fake.deliver("run/samples", samples_body(2, [3, 4], boot=BOOT_B))
    ack = last_ack(fake, n=3)
    assert ack["state"] == "INTERRUPTED"
    assert run_row().status == "INTERRUPTED" and sample_count() == 2
    fake.deliver("run/complete", complete_body(2, 4, boot=BOOT_B))
    assert last_ack(fake, n=4)["state"] == "INTERRUPTED"
    assert run_row().status == "INTERRUPTED"


def test_new_boot_start_interrupts_older_running_run_on_channel(svc, fake):
    fake.deliver("run/start", start_body())
    other = "bits-a4cf12ab34cd-ch1-j8-12-bbbb"
    fake.deliver("run/start", start_body(run_id=other, boot=BOOT_B))
    assert last_ack(fake, other)["state"] == "START_OK"
    assert run_row().status == "INTERRUPTED" and run_row(other).status == "RUNNING"


def test_unknown_run_is_told_so(svc, fake):
    fake.deliver("run/samples", samples_body(1, [1]))
    ack = last_ack(fake)
    assert ack["state"] == "UNKNOWN_RUN" and ack["acked_batch_seq"] == -1


def test_malformed_oversize_and_non_finite_are_dropped_without_ack(svc, fake):
    fake.deliver("run/start", b"{not json")
    fake.deliver("run/start", b'{"run_id":"x","target_g":NaN}')
    fake.deliver("run/samples", b"x" * 9000)
    fake.deliver("run/start", json.dumps(start_body()).encode(), retain=True)
    other = dict(start_body(run_id="other-run"), config={"kp": 1e999})
    fake.deliver("run/start", json.dumps(other).replace("1e+999", "1e999").encode())
    fake.deliver("run/start", start_body(device_id="someone-else"))  # body/topic mismatch
    assert wait_for(lambda: svc.stats["malformed"] >= 3)
    time.sleep(0.3)
    assert fake.acks() == [] and svc.stats["oversize"] == 1 and svc.stats["retained_ignored"] == 1
    assert run_row() is None and run_row("other-run") is None


def test_invalid_sample_batch_is_rejected_with_ack(svc, fake):
    fake.deliver("run/start", start_body())
    bad = samples_body(1, [1])
    bad["samples"][0]["weight_g"] = -5
    fake.deliver("run/samples", bad)
    assert last_ack(fake, n=2)["state"] == "REJECTED" and sample_count() == 0


def test_no_ack_when_commit_fails_then_replay_is_acked(svc, fake, monkeypatch):
    fake.deliver("run/start", start_body())
    last_ack(fake)
    real = Session.commit
    failing = {"on": True}

    def boom(self, *a, **kw):
        if failing["on"]:
            raise OperationalError("COMMIT", {}, Exception("db down"))
        return real(self, *a, **kw)

    monkeypatch.setattr(Session, "commit", boom)
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    assert wait_for(lambda: svc.stats["write_fail"] >= 1)
    time.sleep(0.3)
    assert len(fake.acks()) == 1 and sample_count() == 0  # only the START_OK
    failing["on"] = False
    fake.deliver("run/samples", samples_body(1, [1, 2]))  # relay resends the unacked item
    assert last_ack(fake, n=2)["acked_batch_seq"] == 1 and sample_count() == 2


def test_samples_for_one_run_share_a_commit(svc, fake):
    fake.deliver("run/start", start_body())
    last_ack(fake)
    before = svc.stats["commits"]
    for seq in range(1, 6):
        fake.deliver("run/samples", samples_body(seq, [seq * 10 + 1, seq * 10 + 2]))
    assert wait_for(lambda: len(fake.acks()) >= 6)
    assert sample_count() == 10 and svc.stats["commits"] - before <= 3
    assert [a["acked_batch_seq"] for a in fake.acks()][-1] == 5


def test_backend_restart_replay_is_safe(client, fake):
    s1 = new_service(fake)
    s1.start()
    fake.connack()
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2, 3]))
    assert wait_for(lambda: len(fake.acks()) >= 2)
    s1.stop()
    fake2 = FakeClient()
    s2 = new_service(fake2)
    s2.start()
    fake2.connack()
    try:
        fake2.deliver("run/start", start_body())      # relay resends everything unacked
        fake2.deliver("run/samples", samples_body(1, [1, 2, 3]))
        fake2.deliver("run/samples", samples_body(2, [4]))
        assert wait_for(lambda: len(fake2.acks()) >= 3)
        # cumulative watermark: the replayed start/batch 1 are re-acked at 1, batch 2 -> 2
        assert [a["acked_batch_seq"] for a in fake2.acks()] == [1, 1, 2]
        assert sample_count() == 4 and batches() == [0, 1, 2] and run_row().status == "RUNNING"
        assert s1.epoch != s2.epoch
    finally:
        s2.stop()


# --- queue overflow ---------------------------------------------------------

def test_bulk_lane_drops_and_counts_but_priority_never_drops(client, fake, monkeypatch):
    monkeypatch.setattr(service_mod, "BULK_MAX", 3)
    s = new_service(fake)
    s._accepting = True  # writer not started: queues only fill
    for i in range(10):
        s._on_message(fake, None, _Msg(f"cas/{DEV}/run/samples",
                                         json.dumps(samples_body(i + 1, [i + 1])).encode()))
    for i in range(10):
        s._on_message(fake, None, _Msg(f"cas/{DEV}/run/events", b"{}"))
        s._on_message(fake, None, _Msg(f"cas/{DEV}/commands/ack", b"{}"))
    s._on_message(fake, None, _Msg(f"cas/{DEV}/run/complete", b"{}"))
    depths = s.queue_depths()
    assert depths == {"prio": 21, "bulk": 3} and s.stats["bulk_dropped"] == 7


def test_network_callback_does_not_parse_or_touch_db(client, fake, monkeypatch):
    s = new_service(fake)
    s._accepting = True
    monkeypatch.setattr(database, "get_engine", lambda: pytest.fail("db on network thread"))
    monkeypatch.setattr(json, "loads", lambda *a, **k: pytest.fail("parse on network thread"))
    s._on_message(fake, None, _Msg(f"cas/{DEV}/run/start", b"{}"))
    assert s.queue_depths()["prio"] == 1


# --- startup recovery ---------------------------------------------------------

def _seed_recovery(old):
    with DB_LOCK, Session(database.get_engine()) as db:
        db.add(DeviceStatus(device_id=DEV, status_json={}, command_transport="MQTT",
                            updated_at=utcnow()))
        pending = DeviceCommand(device_id=DEV, command_type="READY", state="PENDING",
                                created_at=old, updated_at=old)
        tare = DeviceCommand(device_id=DEV, command_type="TARE", state="PENDING",
                             created_at=old, updated_at=old)
        fresh = DeviceCommand(device_id=DEV, command_type="READY", state="PENDING")
        delivered = DeviceCommand(device_id=DEV, command_type="JOB", state="DELIVERED",
                                  delivered_via="MQTT", created_at=old, updated_at=old)
        stop = DeviceCommand(device_id=DEV, command_type="ESTOP", state="PENDING",
                             created_at=old, updated_at=old)
        db.add_all([pending, tare, fresh, delivered, stop])
        run = DispenseRun(run_id=RUN, material_id="M1", channel_id="CH1", device_id=DEV,
                          target_g=5000, status="RUNNING", started_at=old, boot_id=BOOT_A)
        recent = DispenseRun(run_id="recent-run", material_id="M1", channel_id="CH1",
                             device_id=DEV, target_g=5000, status="RUNNING", started_at=old,
                             boot_id=BOOT_A)
        http_run = DispenseRun(run_id="http-run", material_id="M2", channel_id="CH2",
                               device_id=DEV, target_g=5000, status="RUNNING", started_at=old)
        db.add_all([run, recent, http_run])
        db.flush()
        db.add(RunBatch(run_id=RUN, batch_seq=0, kind="start", received_at=old))
        db.add(RunBatch(run_id="recent-run", batch_seq=0, kind="start", received_at=utcnow()))
        db.commit()
        return [c.id for c in (pending, tare, fresh, delivered, stop)]


def test_startup_recovery_after_suback_only_and_never_on_reconnect(client, fake, monkeypatch):
    from datetime import timedelta
    pending, tare, fresh, delivered, stop = _seed_recovery(utcnow() - timedelta(minutes=10))
    calls = []
    real = recovery.startup_recovery
    monkeypatch.setattr(recovery, "startup_recovery",
                        lambda db, stale: calls.append(1) or real(db, stale))
    s = new_service(fake)
    s.start()
    try:
        fake.connack(suback=False)
        time.sleep(0.3)
        assert calls == [] and not s.recovered      # not before the SUBACKs
        for _t, _q, mid in list(fake.subs):
            fake.on_subscribe(fake, None, mid, [_RC()], None)
        assert wait_for(lambda: s.recovered)
        with DB_LOCK, Session(database.get_engine()) as db:
            get = lambda i: db.get(DeviceCommand, i)  # noqa: E731
            assert get(pending).state == "EXPIRED"
            assert get(tare).state == "FAILED" and get(tare).ack_json["result"] == "timeout"
            assert get(fresh).state == "PENDING" and get(stop).state == "PENDING"
            assert get(delivered).state == "FAILED"
            assert get(delivered).error_text == "DELIVERED_NO_ACK_OUTCOME_UNKNOWN"
            # CONTRACT 9.8: staleness alone never finalises a run; it is only flagged
            assert db.get(DispenseRun, RUN).status == "RUNNING"
            assert db.get(DispenseRun, "recent-run").status == "RUNNING"
            assert db.get(DispenseRun, "http-run").status == "RUNNING"
            assert [r["run_id"] for r in recovery.stalled_runs(db, 30)] == [RUN]
        fake.drop_link()
        fake.connack()
        time.sleep(0.3)
        assert calls == [1]                          # never again on reconnect
        assert fake.published and not any(t.startswith("cas/") and t.endswith("/commands")
                                          for t, *_ in fake.published)  # nothing resent
    finally:
        s.stop()


# --- profile pin (9.3) ------------------------------------------------------

PROFILE = {"profile_id": "m1-mq", "version": 1, "kp": 0.02, "ki": 0.001, "kd": 0.0,
           "tolerance_g": 20, "max_overshoot_g": 100, "max_duration_ms": 120000,
           "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40}


def test_profile_hash_definition():
    canon = ('{"kd":0.0,"ki":0.001,"kp":0.02,"max_duration_ms":120000,"max_overshoot_g":100,'
             '"min_off_ms":40,"min_on_ms":40,"profile_id":"m1-mq","tolerance_g":20,'
             '"version":1,"window_ms":500}')
    assert profile_pin.canonical_profile(PROFILE) == canon
    assert profile_pin.profile_hash(PROFILE) == hashlib.sha256(canon.encode()).hexdigest()[:16]
    assert len(profile_pin.profile_hash(PROFILE)) == 16


def _job(client):
    with DB_LOCK, Session(database.get_engine()) as db:
        if db.get(DeviceStatus, DEV) is None:
            db.add(DeviceStatus(device_id=DEV, status_json={}, updated_at=utcnow()))
            db.commit()
    body = {"profile_id": "m1-mq", "material_id": "M1", "channel_id": "CH1", "target_g": 5000,
            "kp": 0.02, "ki": 0.001, "kd": 0.0, "tolerance_g": 20, "max_overshoot_g": 100,
            "max_duration_ms": 120000, "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40}
    with DB_LOCK:
        made = client.post("/api/v1/profiles", json=body)
        assert made.status_code == 200
        assert client.post(f"/api/v1/profile-versions/{made.json()['profile_version_id']}/activate"
                           ).status_code == 200
        r = client.post("/api/v1/queue/jobs", json={"material_id": "M1", "target_g": 5000,
                                                    "profile_id": "m1-mq", "profile_version": 1})
    assert r.status_code == 200, r.text
    cid = r.json()["command_id"]
    with DB_LOCK, Session(database.get_engine()) as db:
        row = db.get(DeviceCommand, cid)
        wire = mqtt_bridge.build_command_payload(row, db)
    assert wire["profile_hash"] == profile_pin.profile_hash(wire["profile"])
    return cid, wire


def _ack(fake, cid, **kw):
    body = {"command_id": cid, "state": "QUEUED", "local_job_id": 4, "channel_id": "CH1"}
    body.update(kw)
    fake.deliver("commands/ack", body)


def _cmd(cid):
    with DB_LOCK, Session(database.get_engine()) as db:
        return db.get(DeviceCommand, cid)


def test_job_dispensable_only_when_applied_profile_matches_pin(svc, fake, client):
    cid, wire = _job(client)
    good = {"profile_id": "m1-mq", "version": 1, "hash": wire["profile_hash"]}
    _ack(fake, cid, applied_profile=good)
    assert wait_for(lambda: _cmd(cid).state == "QUEUED")
    assert _cmd(cid).error_text is None


@pytest.mark.parametrize("applied", [
    None,
    {"profile_id": "m1-mq", "version": 1, "hash": "0" * 16},
    {"profile_id": "m1-mq", "version": 2, "hash": "PLACEHOLDER"},
    {"profile_id": "other", "version": 1, "hash": "PLACEHOLDER"},
    "garbage",
])
def test_profile_mismatch_fails_the_job_and_rejects_its_run(svc, fake, client, applied):
    cid, wire = _job(client)
    if isinstance(applied, dict) and applied["hash"] == "PLACEHOLDER":
        applied = dict(applied, hash=wire["profile_hash"])
    _ack(fake, cid, **({"applied_profile": applied} if applied is not None else {}))
    assert wait_for(lambda: _cmd(cid).state == "FAILED")
    assert _cmd(cid).error_text == "PROFILE_MISMATCH"
    fake.deliver("run/start", start_body(command_id=cid, profile_id="m1-mq", profile_version=1))
    assert last_ack(fake)["state"] == "REJECTED" and run_row() is None


def test_run_reporting_a_different_profile_than_the_job_pin_is_rejected(svc, fake, client):
    cid, wire = _job(client)
    _ack(fake, cid, applied_profile={"profile_id": "m1-mq", "version": 1,
                                     "hash": wire["profile_hash"]})
    assert wait_for(lambda: _cmd(cid).state == "QUEUED")
    fake.deliver("run/start", start_body(command_id=cid, profile_id="m1-mq", profile_version=9))
    assert last_ack(fake)["state"] == "REJECTED"
    fake.deliver("run/start", start_body(command_id=cid, profile_id="m1-mq", profile_version=1))
    assert last_ack(fake, n=2)["state"] == "START_OK"


def test_zero_tare_applied_ack_still_fails(svc, fake, client):
    with DB_LOCK, Session(database.get_engine()) as db:
        row = DeviceCommand(device_id=DEV, command_type="TARE", state="DELIVERED",
                            channel_id="CH1", material_id="M1")
        db.add(row)
        db.commit()
        cid = row.id
    _ack(fake, cid, state="APPLIED", result="success")
    assert wait_for(lambda: _cmd(cid).state == "FAILED")
    assert _cmd(cid).state != "APPLIED"


def test_legacy_bridge_still_accepts_job_ack_without_applied_profile(client):
    cid, _wire = _job(client)
    mqtt_bridge.handle_message(f"cas/{DEV}/commands/ack", json.dumps(
        {"command_id": cid, "state": "QUEUED", "local_job_id": 4}).encode())
    assert _cmd(cid).state == "QUEUED"


def test_hooks_run_on_scheduler_thread(svc):
    seen = []
    svc.register_hook("probe", 0.1, lambda: seen.append(threading.current_thread().name))
    assert wait_for(lambda: len(seen) >= 2)
    assert set(seen) == {"svc-sched"}


def test_stop_drains_queued_work_before_exit(client, fake):
    s = new_service(fake)
    s.start()
    fake.connack()
    fake.deliver("run/start", start_body())
    fake.deliver("run/samples", samples_body(1, [1, 2]))
    s.stop()
    assert sample_count() == 2
    assert [a["acked_batch_seq"] for a in fake.acks()] == [0, 1]








