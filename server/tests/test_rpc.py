"""Operator plane (CONTRACT 9.4-9.6): dispatcher called directly with synthetic envelopes.

Own throw-away SQLite engine per test; no broker, no server, no relay.
"""

import itertools
import json
import logging
from datetime import datetime, timedelta, timezone

import pytest
from argon2 import PasswordHasher
from sqlalchemy import create_engine, select
from sqlalchemy.orm import sessionmaker
from sqlalchemy.pool import StaticPool

from app import models
from app import models_ops  # noqa: F401  (registers operator tables on Base)
from app.models import DeviceCommand, DeviceStatus, DispenseRun, Material, WeightSample
from app.models_ops import Operator, OperatorAudit
from app.rpc import state as rpc_state
from app.rpc.dispatch import RpcDispatcher
from app.rpc.envelope import iso_ms
from app.rpc.methods import METHODS
from app.services import auth as auth_service
from app.services import control as control_service

PW = "correct-horse-1"
T0 = datetime(2026, 10, 8, 12, 0, 0, tzinfo=timezone.utc)
RELAY, SENDER = "bits-relay-0001", "bits-sender-0001"
RELAY_STATUS = {"role": "relay_controller", "channels": [
    {"channel_id": "CH1", "state": "IDLE", "relay_on": False, "active_job_id": 0},
    {"channel_id": "CH2", "state": "IDLE", "relay_on": False, "active_job_id": 0}], "queue": []}
PROFILE = {"profile_id": "default", "material_id": "M1", "channel_id": "CH1", "target_g": 5000,
           "kp": 0.01, "ki": 0.001, "kd": 0.0, "tolerance_g": 20, "max_overshoot_g": 50,
           "max_duration_ms": 120000, "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40}


class Clock:
    def __init__(self):
        self.now = T0

    def __call__(self):
        return self.now


class Harness:
    def __init__(self):
        engine = create_engine("sqlite://", connect_args={"check_same_thread": False},
                               poolclass=StaticPool, future=True)
        models.Base.metadata.create_all(engine)
        self.Session = sessionmaker(bind=engine, expire_on_commit=False, autoflush=False, future=True)
        self.clock = Clock()
        self.live = {"controller": None, "weight": None}
        self.d = self.dispatcher()
        self.n = itertools.count(1)
        with self.Session() as db:
            for mid, ch in (("M1", "CH1"), ("M2", "CH2")):
                db.add(Material(material_id=mid, name=mid, channel_id=ch, pump_id=f"Pump {mid[-1]}",
                                relay_id=f"Relay {mid[-1]}", scale_id=f"Scale {mid[-1]}", enabled=True))
            db.add(DeviceStatus(device_id=RELAY, status_json=RELAY_STATUS, updated_at=datetime.utcnow()))
            db.add(DeviceStatus(device_id=SENDER, updated_at=datetime.utcnow(),
                                status_json={"role": "weight_sender", "cas_link": "ONLINE"}))
            db.commit()
            for name, role in (("vue1", "viewer"), ("opr1", "operator"), ("adm1", "admin")):
                auth_service.create_operator(db, name, PW, role)

    def dispatcher(self):
        return RpcDispatcher(session_factory=self.Session, clock=self.clock,
                             live_snapshot=lambda: self.live)

    def call(self, method, args=None, *, token=None, client="app_a", corr=None, ttl=10000,
             issued=None, raw=None, disp=None):
        corr = corr or f"c{next(self.n)}"
        body = {"v": 1, "corr_id": corr, "method": method, "args": args or {},
                "session_token": token, "issued_at": iso_ms(issued or self.clock.now),
                "ttl_ms": ttl, "client_seq": 1}
        topic = f"bits/v1/ops/req/{client}/{method}"
        reply = (disp or self.d).handle(topic, raw if raw is not None else json.dumps(body), client)
        assert reply.body["corr_id"] == (corr if raw is None else reply.body["corr_id"])
        return reply.body

    def login(self, user="opr1", client="app_a", password=PW):
        r = self.call("auth.login", {"username": user, "password": password}, client=client)
        return r["data"]["session_token"] if r["ok"] else r

    def confirm(self, token, method, args, client="app_a"):
        r = self.call("confirm.begin", {"method": method, "args": args}, token=token, client=client)
        assert r["ok"], r
        return r["data"]["confirm_token"]

    def rows(self, model, *where):
        with self.Session() as db:
            return list(db.scalars(select(model).where(*where)))


@pytest.fixture()
def h():
    auth_service.set_hasher(PasswordHasher(time_cost=1, memory_cost=8, parallelism=1))
    yield Harness()
    auth_service.set_hasher(PasswordHasher())


def _seed_run(h, run_id="run-a", target=5000, samples=0, status="COMPLETE"):
    with h.Session() as db:
        db.add(DispenseRun(run_id=run_id, target_g=target, status=status, started_at=datetime(2026, 10, 1),
                           device_id=RELAY))
        db.flush()
        for i in range(samples):
            db.add(WeightSample(run_id=run_id, idx=i, timestamp=datetime(2026, 10, 1), elapsed_ms=i * 100,
                                weight_g=(i * 37) % 500, target_g=target, error_g=0, stable=False,
                                relay1=True, relay2=False, state="COARSE_DISPENSE"))
        db.commit()


# ---- envelope --------------------------------------------------------------

def test_envelope_malformed_oversize_nonfinite_rejected(h):
    tok = h.login("vue1")
    ok = h.call("queue.list", token=tok)
    assert ok["ok"] and set(ok) == {"corr_id", "ok", "code", "error", "data", "next_cursor",
                                    "server_time", "seq"}
    good = {"v": 1, "corr_id": "x1", "method": "queue.list", "args": {}, "session_token": tok,
            "issued_at": iso_ms(T0), "ttl_ms": 5000, "client_seq": 1}
    cases = {
        "oversize": json.dumps(dict(good, args={"pad": "x" * 9000})),
        "nan": json.dumps(good).replace('"ttl_ms": 5000', '"ttl_ms": NaN'),
        "inf_arg": json.dumps(good).replace('"args": {}', '"args": {"a": Infinity}'),
        "array": "[1,2]", "garbage": "{not json", "bad_version": json.dumps(dict(good, v=2)),
        "bad_corr": json.dumps(dict(good, corr_id="has space")),
        "wrong_method": json.dumps(dict(good, method="run.get")),
        "no_ttl": json.dumps({k: v for k, v in good.items() if k != "ttl_ms"}),
        "float_ttl": json.dumps(dict(good, ttl_ms=5000.5)),
        "bad_time": json.dumps(dict(good, issued_at="yesterday")),
        "future": json.dumps(dict(good, issued_at=iso_ms(T0 + timedelta(minutes=5)))),
        "args_list": json.dumps(dict(good, args=[1])),
    }
    for name, raw in cases.items():
        r = h.call("queue.list", raw=raw, token=tok, corr="x1")
        assert not r["ok"] and r["code"] == "INVALID", name
    r = h.d.handle("bits/v1/ops/req/app_a/nope", json.dumps(dict(good, method="nope")), "app_a")
    assert r.body["code"] == "INVALID"
    # topic client must match the authenticated client
    r = h.d.handle("bits/v1/ops/req/other/queue.list", json.dumps(good), "app_a")
    assert r.body["code"] == "INVALID"


def test_ttl_expiry(h):
    tok = h.login("vue1")
    assert h.call("queue.list", token=tok, ttl=5000, issued=T0 - timedelta(seconds=4))["ok"]
    r = h.call("queue.list", token=tok, ttl=5000, issued=T0 - timedelta(seconds=6))
    assert r["code"] == "EXPIRED"


def test_response_topic_and_reply_shape(h):
    body = {"v": 1, "corr_id": "abc", "method": "materials.list", "args": {}, "session_token": h.login("vue1"),
            "issued_at": iso_ms(T0), "ttl_ms": 5000}
    reply = h.d.handle("bits/v1/ops/req/app_a/materials.list", json.dumps(body), "app_a")
    assert reply.topic == "bits/v1/ops/res/app_a/abc" and reply.qos == 1 and reply.retain is False
    assert json.loads(reply.payload)["data"]["items"][0]["material_id"] == "M1"


# ---- auth --------------------------------------------------------------------

def test_login_success_and_failure_modes(h):
    r = h.call("auth.login", {"username": "opr1", "password": PW})
    assert r["ok"] and r["data"]["role"] == "operator" and len(r["data"]["session_token"]) >= 32
    bad = h.call("auth.login", {"username": "opr1", "password": "nope-nope-nope"}, client="app_b")
    unknown = h.call("auth.login", {"username": "ghost", "password": PW}, client="app_c")
    assert bad["code"] == unknown["code"] == "AUTH_FAILED" and bad["error"] == unknown["error"]
    assert h.call("queue.list")["code"] == "UNAUTHENTICATED"
    assert h.call("queue.list", token="garbage")["code"] == "UNAUTHENTICATED"
    assert h.call("auth.login", {"username": "opr1"})["code"] == "INVALID"
    with h.Session() as db:
        stored = db.scalar(select(Operator).where(Operator.username == "opr1")).password_hash
    assert stored.startswith("$argon2") and PW not in stored


def test_lockout_backoff_and_recovery(h):
    for _ in range(5):
        assert h.call("auth.login", {"username": "opr1", "password": "wrong-wrong-1"})["code"] == "AUTH_FAILED"
    locked = h.call("auth.login", {"username": "opr1", "password": PW})
    assert locked["code"] == "LOCKED"
    # a different client is blocked by the per-account lock too
    assert h.call("auth.login", {"username": "opr1", "password": PW}, client="app_z")["code"] == "LOCKED"
    h.clock.now = T0 + timedelta(seconds=31)
    assert h.call("auth.login", {"username": "opr1", "password": PW})["ok"]


def test_unknown_user_flood_is_throttled_per_client(h):
    for _ in range(5):
        h.call("auth.login", {"username": "ghost", "password": PW}, client="app_f")
    assert h.call("auth.login", {"username": "opr1", "password": PW}, client="app_f")["code"] == "LOCKED"


def test_session_expiry_and_client_binding(h):
    tok = h.login("opr1")
    assert h.call("queue.list", token=tok)["ok"]
    assert h.call("queue.list", token=tok, client="app_other")["code"] == "UNAUTHENTICATED"
    h.clock.now = T0 + timedelta(hours=8, seconds=1)
    assert h.call("queue.list", token=tok, issued=h.clock.now)["code"] == "UNAUTHENTICATED"


def test_disabled_operator_session_dies(h):
    tok = h.login("opr1")
    with h.Session() as db:
        db.scalar(select(Operator).where(Operator.username == "opr1")).disabled = True
        db.commit()
    assert h.call("queue.list", token=tok)["code"] == "UNAUTHENTICATED"
    assert h.call("auth.login", {"username": "opr1", "password": PW})["code"] == "AUTH_FAILED"


def test_session_token_stored_hashed(h):
    tok = h.login("opr1")
    from app.models_ops import OperatorSession
    with h.Session() as db:
        rows = list(db.scalars(select(OperatorSession)))
    assert rows and all(tok != r.token_hash and len(r.token_hash) == 64 for r in rows)


# ---- roles ---------------------------------------------------------------------

def test_every_method_needs_a_session_except_login(h):
    for name, m in METHODS.items():
        if name == "auth.login":
            continue
        assert h.call(name, client=f"app_{name}")["code"] == "UNAUTHENTICATED", name


def test_role_enforcement_for_every_method(h):
    viewer, operator = h.login("vue1", "app_v"), h.login("opr1", "app_o")
    for name, m in METHODS.items():
        if m.min_role in ("operator", "admin"):
            assert h.call(name, token=viewer, client="app_v")["code"] == "FORBIDDEN", name
        if m.min_role == "admin":
            assert h.call(name, token=operator, client="app_o")["code"] == "FORBIDDEN", name
    # viewers can still read
    assert h.call("queue.list", token=viewer, client="app_v")["ok"]
    assert h.call("materials.list", token=viewer, client="app_v")["ok"]
    assert h.call("materials.update", {"material_id": "M1", "name": "Sugar"},
                  token=h.login("adm1", "app_a"))["ok"]


# ---- confirm flow ----------------------------------------------------------------

def test_confirm_flow_single_use_bound_and_expiring(h):
    tok = h.login("opr1")
    args = {"action": "CLEAR"}
    assert h.call("control.cmd", args, token=tok)["code"] == "CONFIRM_REQUIRED"
    ct = h.confirm(tok, "control.cmd", args)
    ok = h.call("control.cmd", dict(args, confirm_token=ct), token=tok)
    assert ok["ok"] and ok["data"]["state"] == "PENDING"
    # single use
    assert h.call("control.cmd", dict(args, confirm_token=ct), token=tok)["code"] == "CONFIRM_REQUIRED"
    # bound to args
    ct = h.confirm(tok, "control.cmd", {"action": "PUMP_START", "channel_id": "CH1"})
    assert h.call("control.cmd", {"action": "PUMP_START", "channel_id": "CH2", "confirm_token": ct},
                  token=tok)["code"] == "CONFIRM_REQUIRED"
    # bound to session
    ct = h.confirm(tok, "control.cmd", args)
    other = h.login("adm1", "app_a")
    assert h.call("control.cmd", dict(args, confirm_token=ct), token=other)["code"] == "CONFIRM_REQUIRED"
    # 15 s expiry
    ct = h.confirm(tok, "control.cmd", args)
    h.clock.now = T0 + timedelta(seconds=16)
    assert h.call("control.cmd", dict(args, confirm_token=ct), token=tok,
                  issued=h.clock.now)["code"] == "CONFIRM_REQUIRED"
    # confirm.begin refuses actions that are not dangerous
    assert h.call("confirm.begin", {"method": "control.cmd", "args": {"action": "ESTOP"}},
                  token=tok)["code"] == "INVALID"
    assert h.call("confirm.begin", {"method": "queue.list", "args": {}}, token=tok)["code"] == "INVALID"


def test_estop_pump_stop_cancel_never_gated_or_rate_limited(h):
    tok = h.login("opr1")
    for i in range(25):  # same instant, over the 20/s limit
        for args in ({"action": "ESTOP"}, {"action": "PUMP_STOP", "channel_id": "CH1"},
                     {"action": "CANCEL", "channel_id": "CH2"}):
            r = h.call("control.cmd", args, token=tok)
            assert r["ok"], (i, args, r)
    assert len(h.rows(DeviceCommand, DeviceCommand.command_type == "ESTOP")) == 25


def test_dangerous_set(h):
    op, ad = h.login("opr1", "app_o"), h.login("adm1", "app_a")
    _seed_run(h)
    assert h.call("control.cmd", {"action": "PUMP_START", "channel_id": "CH1"}, token=op,
                  client="app_o")["code"] == "CONFIRM_REQUIRED"
    assert h.call("run.delete", {"run_id": "run-a"}, token=ad)["code"] == "CONFIRM_REQUIRED"
    assert h.call("profiles.delete", {"profile_version_id": 1}, token=ad)["code"] == "CONFIRM_REQUIRED"
    assert h.call("profiles.deactivate", {"profile_version_id": 1}, token=ad)["code"] == "CONFIRM_REQUIRED"
    assert h.call("profiles.activate", {"profile_version_id": 1}, token=op,
                  client="app_o")["code"] == "CONFIRM_REQUIRED"
    assert h.call("profiles.deprecate", {"profile_version_id": 1}, token=ad)["code"] == "CONFIRM_REQUIRED"
    bulk = {k: v for k, v in PROFILE.items() if k not in ("material_id", "channel_id")}
    assert h.call("profiles.bulk", dict(bulk, materials=["M1"]), token=op,
                  client="app_o")["code"] == "CONFIRM_REQUIRED"
    ct = h.confirm(ad, "run.delete", {"run_id": "run-a"})
    assert h.call("run.delete", {"run_id": "run-a", "confirm_token": ct}, token=ad)["ok"]
    assert h.rows(DispenseRun) == []


def _active_profile(h, tok, body=None):
    """profiles.create (always a draft) then profiles.activate (confirmed): a job
    can only pin an ACTIVE profile. Returns the profile_version_id."""
    prof = h.call("profiles.create", dict(body or PROFILE), token=tok)
    assert prof["ok"], prof
    assert prof["data"]["status"] == "draft" and prof["data"]["active"] is False
    pvid = prof["data"]["profile_version_id"]
    ct = h.confirm(tok, "profiles.activate", {"profile_version_id": pvid})
    done = h.call("profiles.activate", {"profile_version_id": pvid, "confirm_token": ct}, token=tok)
    assert done["ok"], done
    return pvid


def _make_job(h, tok, target=5000, **kw):
    pvid = _active_profile(h, tok)
    return h.call("queue.job.create", {"material_id": "M1", "target_g": target,
                  "profile_version_id": pvid, **kw}, token=tok)


def test_cancel_active_job_needs_confirm_waiting_does_not(h):
    tok = h.login("opr1")
    waiting = _make_job(h, tok)
    assert waiting["ok"], waiting
    assert h.call("queue.job.cancel", {"command_id": waiting["data"]["command_id"]}, token=tok)["ok"]
    active = _make_job(h, tok)["data"]["command_id"]
    with h.Session() as db:
        job = db.get(DeviceCommand, active)
        job.state, job.local_job_id, job.channel_id = "RUNNING", 3, "CH1"
        db.commit()
    assert h.call("queue.job.cancel", {"command_id": active}, token=tok)["code"] == "CONFIRM_REQUIRED"
    ct = h.confirm(tok, "queue.job.cancel", {"command_id": active})
    r = h.call("queue.job.cancel", {"command_id": active, "confirm_token": ct}, token=tok)
    assert r["ok"], r
    cancels = h.rows(DeviceCommand, DeviceCommand.command_type == "CANCEL")
    assert len(cancels) == 1 and cancels[0].local_job_id == 3


# ---- dedupe, rate limits -------------------------------------------------------------

def test_dedupe_returns_stored_response_without_second_execution(h):
    tok = h.login("opr1")
    prof = _active_profile(h, tok)
    args = {"material_id": "M1", "target_g": 5000, "profile_version_id": prof}
    first = h.call("queue.job.create", args, token=tok, corr="job-1")
    h.clock.now = T0 + timedelta(minutes=2)
    again = h.call("queue.job.create", args, token=tok, corr="job-1", issued=T0)  # even past its ttl
    assert first["ok"] and again == first
    assert len(h.rows(DeviceCommand, DeviceCommand.command_type == "JOB")) == 1
    # same corr_id from another client is a different key
    other = h.call("queue.job.create", args, token=h.login("opr1", "app_b"), client="app_b", corr="job-1")
    assert other["ok"] and other["data"]["command_id"] != first["data"]["command_id"]
    # after a backend restart the persisted write response still wins
    restarted = h.dispatcher()
    h.clock.now = T0 + timedelta(minutes=3)
    replay = h.call("queue.job.create", args, token=tok, corr="job-1", issued=h.clock.now, disp=restarted)
    assert replay == first
    assert len(h.rows(DeviceCommand, DeviceCommand.command_type == "JOB")) == 2
    # 5 minutes later the key is forgotten
    h.clock.now = T0 + timedelta(minutes=9)
    late = h.call("queue.job.create", args, token=h.login("opr1", "app_a"), corr="job-1", issued=h.clock.now)
    assert late["ok"] and late["data"]["command_id"] not in (first["data"]["command_id"],
                                                             other["data"]["command_id"])


def test_cancel_retry_executes_once(h):
    tok = h.login("opr1")
    a = h.call("control.cmd", {"action": "CANCEL", "channel_id": "CH1"}, token=tok, corr="cx")
    b = h.call("control.cmd", {"action": "CANCEL", "channel_id": "CH1"}, token=tok, corr="cx")
    assert a == b and len(h.rows(DeviceCommand, DeviceCommand.command_type == "CANCEL")) == 1


def test_in_progress_duplicate_is_busy(h):
    tok = h.login("vue1")
    h.d.dedupe.claim(("app_a", "slow"), h.clock.now)
    assert h.call("queue.list", token=tok, corr="slow")["code"] == "BUSY"


def test_rate_limit_20_per_second_per_client(h):
    tok = h.login("vue1")
    codes = [h.call("materials.list", token=tok)["code"] for _ in range(24)]
    assert codes.count("OK") == 19 and codes.count("BUSY") == 5   # 1 login + 19 reads = 20
    other = h.login("vue1", "app_b")
    assert h.call("materials.list", token=other, client="app_b")["ok"]
    h.clock.now = T0 + timedelta(seconds=1.1)
    assert h.call("materials.list", token=tok, issued=h.clock.now)["ok"]


def test_max_four_inflight_history_queries(h):
    tok = h.login("vue1")
    for _ in range(4):
        assert h.d.inflight.acquire("app_a")
    assert h.call("runs.search", token=tok)["code"] == "BUSY"
    assert h.call("queue.list", token=tok)["ok"]            # not a history query
    h.d.inflight.release("app_a")
    assert h.call("runs.search", token=tok)["ok"]
    assert h.d.inflight._n["app_a"] == 3                    # released after the call


# ---- paging, size, decimation ---------------------------------------------------------

def test_runs_search_cursor_paging_limit_50(h):
    tok = h.login("vue1")
    with h.Session() as db:
        for i in range(120):
            db.add(DispenseRun(run_id=f"r{i:03d}", target_g=5000, status="COMPLETE",
                               started_at=datetime(2026, 10, 1) + timedelta(minutes=i), device_id=RELAY))
        db.commit()
    assert h.call("runs.search", {"limit": 51}, token=tok)["code"] == "INVALID"
    seen, cursor = [], None
    for _ in range(5):
        r = h.call("runs.search", {"limit": 50, **({"cursor": cursor} if cursor else {})}, token=tok)
        assert r["ok"] and len(r["data"]["items"]) <= 50 and r["data"]["total"] == 120
        seen += [i["run_id"] for i in r["data"]["items"]]
        cursor = r["next_cursor"]
        if not cursor:
            break
    assert len(seen) == len(set(seen)) == 120 and cursor is None
    assert h.call("runs.search", {"cursor": "!!"}, token=tok)["code"] == "INVALID"


def test_run_samples_paging_decimation_and_caps(h):
    tok = h.login("vue1")
    _seed_run(h, "run-s", samples=600)
    assert h.call("run.samples", {"run_id": "run-s", "limit": 2001}, token=tok)["code"] == "INVALID"
    assert h.call("run.samples", {"run_id": "run-s", "max_points": 2001}, token=tok)["code"] == "INVALID"
    assert h.call("run.samples", {"run_id": "nope"}, token=tok)["code"] == "NOT_FOUND"
    # explicit limit pages
    idx, cursor = [], None
    while True:
        r = h.call("run.samples", {"run_id": "run-s", "limit": 100,
                                   **({"cursor": cursor} if cursor else {})}, token=tok)
        assert r["ok"] and len(r["data"]["samples"]) <= 100
        idx += [s["idx"] for s in r["data"]["samples"]]
        cursor = r["next_cursor"]
        if not cursor:
            break
    assert idx == list(range(600))
    # from_seq
    r = h.call("run.samples", {"run_id": "run-s", "from_seq": 590}, token=tok)
    assert [s["idx"] for s in r["data"]["samples"]] == list(range(590, 600)) and r["next_cursor"] is None
    # page size cap 48 KB even with limit 2000; whole reply under 64 KB
    reply = h.d.handle("bits/v1/ops/req/app_a/run.samples", json.dumps(
        {"v": 1, "corr_id": "big", "method": "run.samples", "args": {"run_id": "run-s", "limit": 2000},
         "session_token": tok, "issued_at": iso_ms(T0), "ttl_ms": 5000}), "app_a")
    assert reply.body["ok"] and reply.body["next_cursor"] is not None
    assert len(json.dumps(reply.body["data"]["samples"], separators=(",", ":"))) <= 48 * 1024
    assert len(reply.payload) <= 65536
    # server-side min/max decimation keeps real samples, first and last
    r = h.call("run.samples", {"run_id": "run-s", "max_points": 50, "limit": 2000}, token=tok)
    got = r["data"]["samples"]
    assert r["data"]["decimated"] and 4 <= len(got) <= 50
    assert got[0]["idx"] == 0 and got[-1]["idx"] == 599
    assert all(s["weight_g"] == (s["idx"] * 37) % 500 for s in got)
    assert [s["idx"] for s in got] == sorted(s["idx"] for s in got)


def test_run_events_and_get(h):
    tok = h.login("vue1")
    _seed_run(h, "run-e")
    assert h.call("run.get", {"run_id": "run-e"}, token=tok)["data"]["run_id"] == "run-e"
    assert h.call("run.events", {"run_id": "run-e", "limit": 50}, token=tok)["data"]["events"] == []
    assert h.call("run.events", {"run_id": "run-e", "limit": 51}, token=tok)["code"] == "INVALID"


def test_oversized_response_is_refused_not_truncated_silently(h):
    tok = h.login("vue1")
    with h.Session() as db:
        for dev in (RELAY, SENDER):
            db.get(DeviceStatus, dev).status_json = dict(RELAY_STATUS, blob="x" * 70000)
        db.commit()
    r = h.call("queue.list", token=tok)
    assert r["code"] == "TOO_LARGE" and r["data"] is None


# ---- graphs.category, arbitrary targets -------------------------------------------------

@pytest.mark.parametrize("target", [5000, 10000, 15000, 20000])
def test_graphs_category_allowed(h, target):
    tok = h.login("vue1")
    _seed_run(h, "g1", target=target)
    r = h.call("graphs.category", {"target_g": target}, token=tok)
    assert r["ok"] and [i["run_id"] for i in r["data"]["items"]] == ["g1"]


@pytest.mark.parametrize("target", [0, -5000, 1, 4999, 5001, 7000, 25000, 100000])
def test_graphs_category_refuses_everything_else(h, target):
    tok = h.login("vue1")
    assert h.call("graphs.category", {"target_g": target}, token=tok)["code"] == "REFUSED"


def test_graphs_category_rejects_non_integers(h):
    tok = h.login("vue1")
    for bad in ("5000", 5000.0, True, None):
        assert h.call("graphs.category", {"target_g": bad}, token=tok)["code"] == "INVALID"


def test_history_and_queue_accept_arbitrary_targets(h):
    tok = h.login("opr1")
    _seed_run(h, "odd", target=7000)
    r = h.call("runs.search", {"target_g": 7000}, token=tok)
    assert r["ok"] and r["data"]["total"] == 1
    assert h.call("runs.latest", {"target_g": 7000}, token=tok)["data"]["items"][0]["run_id"] == "odd"
    # the queue lists a 7000 g job and create_job is not blocked by the Test Graph rule
    with h.Session() as db:
        db.add(DeviceCommand(device_id=RELAY, command_type="JOB", material_id="M1", target_g=7000,
                             state="PENDING", priority=0))
        db.commit()
    q = h.call("queue.list", token=tok)
    assert q["data"]["waiting_commands"][0]["target_g"] == 7000
    prof = _active_profile(h, tok)
    r = h.call("queue.job.create", {"material_id": "M1", "target_g": 7000, "profile_version_id": prof}, token=tok)
    assert r["code"] == "TARGET_OUT_OF_PROFILE_RANGE" and "Test Graph" not in r["error"] and "7000" in r["error"]
    assert h.call("queue.job.create", {"material_id": "M1", "target_g": 7000}, token=tok)[
        "code"] == "NO_COMPATIBLE_PROFILE"
    assert h.call("queue.job.create", {"material_id": "M1", "target_g": 20001, "profile_version_id": prof},
                  token=tok)["code"] == "INVALID"


# ---- ZERO/TARE ----------------------------------------------------------------------------

@pytest.mark.parametrize("action", ["ZERO", "TARE"])
def test_zero_tare_end_failed_verification_required(h, action):
    tok = h.login("opr1")
    r = h.call("control.cmd", {"action": action, "channel_id": "CH1"}, token=tok)
    assert r["ok"] and r["data"]["device_id"] == SENDER
    cid = r["data"]["command_id"]
    # sender claims success: the server still refuses APPLIED
    with h.Session() as db:
        control_service.acknowledge_command(db, cid, control_service.DeviceCommandAck(
            state="APPLIED", result="success", weight_g=0, stable=True))
    got = h.call("command.get", {"command_id": cid}, token=tok)["data"]
    assert got["state"] == "FAILED" and got["result"] == "failed" and got["weight_g"] is None
    # sender refuses honestly
    cid2 = h.call("control.cmd", {"action": action, "channel_id": "CH2"}, token=tok)["data"]["command_id"]
    with h.Session() as db:
        control_service.acknowledge_command(db, cid2, control_service.DeviceCommandAck(
            state="FAILED", result="failed", reason="VERIFICATION REQUIRED: frame unverified"))
    got = h.call("command.get", {"command_id": cid2}, token=tok)["data"]
    assert got["state"] == "FAILED" and got["reason"].startswith("VERIFICATION REQUIRED")


def test_zero_tare_keep_busy_and_stale_gating(h):
    tok = h.login("opr1")
    with h.Session() as db:
        st = db.get(DeviceStatus, RELAY)
        busy = json.loads(json.dumps(RELAY_STATUS))
        busy["channels"][1].update(state="DISPENSING", relay_on=True, active_job_id=3)
        st.status_json = busy
        db.commit()
    assert h.call("control.cmd", {"action": "TARE", "channel_id": "CH2"}, token=tok)["code"] == "CONFLICT"
    with h.Session() as db:
        db.get(DeviceStatus, RELAY).status_json = RELAY_STATUS
        db.get(DeviceStatus, SENDER).updated_at = datetime.utcnow() - timedelta(seconds=60)
        db.commit()
    assert h.call("control.cmd", {"action": "ZERO", "channel_id": "CH1"}, token=tok)["code"] == "CONFLICT"
    assert h.call("control.cmd", {"action": "ZERO"}, token=tok)["code"] == "INVALID"


# ---- audit, secrets --------------------------------------------------------------------------

def test_audit_row_for_every_write_and_none_for_reads(h):
    op, v = h.login("opr1", "app_o"), h.login("vue1", "app_v")
    h.call("queue.list", token=op, client="app_o")
    h.call("control.cmd", {"action": "ESTOP"}, token=op, client="app_o")
    h.call("control.cmd", {"action": "CLEAR"}, token=op, client="app_o")            # CONFIRM_REQUIRED
    h.call("control.cmd", {"action": "ESTOP"}, token=v, client="app_v")             # FORBIDDEN
    h.call("control.cmd", {"action": "ESTOP"})                                       # UNAUTHENTICATED
    h.call("control.cmd", {"action": "NOPE"}, token=op, client="app_o")             # INVALID
    rows = h.rows(OperatorAudit)
    ctl = [r for r in rows if r.method == "control.cmd"]
    assert [(r.username, r.result) for r in ctl] == [
        ("opr1", "OK"), ("opr1", "CONFIRM_REQUIRED"), ("vue1", "FORBIDDEN"), ("-", "UNAUTHENTICATED"),
        ("opr1", "INVALID")]
    assert all(len(r.args_hash) == 64 and r.client_id and r.corr_id for r in ctl)
    assert ctl[0].args_hash == auth_service.args_hash("control.cmd", {"action": "ESTOP"})
    assert not [r for r in rows if r.method == "queue.list"]
    logins = [r for r in rows if r.method == "auth.login"]
    assert len(logins) == 2 and all(r.result == "OK" and r.username in ("opr1", "vue1") for r in logins)


def test_no_secret_in_logs_or_audit(h, caplog):
    caplog.set_level(logging.DEBUG)
    secrets_seen = []
    tok = h.login("opr1")
    secrets_seen += [tok, PW]
    h.call("auth.login", {"username": "opr1", "password": "wrong-wrong-xx"})
    secrets_seen.append("wrong-wrong-xx")
    ct = h.confirm(tok, "control.cmd", {"action": "CLEAR"})
    secrets_seen.append(ct)
    h.call("control.cmd", {"action": "CLEAR", "confirm_token": ct}, token=tok)
    h.call("auth.login", {"username": "opr1"})   # validation error path
    text = caplog.text
    assert "rpc" in text  # logging happened
    for secret in secrets_seen:
        assert secret not in text
    with h.Session() as db:
        dump = json.dumps([[str(getattr(r, c.name)) for c in r.__table__.columns]
                           for model in (OperatorAudit,) for r in db.scalars(select(model))])
    for secret in secrets_seen:
        assert secret not in dump


def test_cli_create_operator_reads_password_from_env(h, monkeypatch, capsys):
    from app import database
    from app.rpc import admin
    database.reset_engine()
    monkeypatch.setenv("BITS_OPERATOR_PASSWORD", "cli-password-123")
    assert admin.main(["create-operator", "--username", "cliuser", "--role", "admin"]) == 0
    out = capsys.readouterr()
    assert "cli-password-123" not in out.out + out.err
    assert admin.main(["create-operator", "--username", "cliuser"]) == 1       # duplicate
    monkeypatch.setenv("BITS_OPERATOR_PASSWORD", "short")
    assert admin.main(["create-operator", "--username", "other"]) == 1
    database.reset_engine()


# ---- live / state builders ---------------------------------------------------------------------

def _snap(age_s=0.2, weight=1234, valid=True, ch_age=150):
    return {"controller": {"device_id": RELAY, "online": age_s <= 10, "age_seconds": age_s,
                           "status": {"role": "relay_controller", "channels": [
                               {"channel_id": "CH1", "state": "DISPENSING", "weight_g": weight,
                                "weight_valid": valid, "weight_age_ms": ch_age, "stable": True},
                               {"channel_id": "CH2", "state": "IDLE", "weight_g": 0,
                                "weight_valid": False, "weight_age_ms": None, "stable": True}]}},
            "weight": None}


def test_live_builder_nulls_invalid_and_stale_weight():
    now = T0
    live = rpc_state.build_live(_snap(), server_time=now, seq=7, epoch="abcd1234")
    assert (live["seq"], live["epoch"], live["server_time"]) == (7, "abcd1234", iso_ms(now))
    dev = live["devices"][0]
    assert dev["age_ms"] == 200 and dev["stale"] is False
    ch1, ch2 = dev["channels"]
    assert ch1["weight_g"] == 1234 and ch1["weight_age_ms"] == 150 and ch1["stable"] is True
    assert ch2["weight_g"] is None and ch2["weight_valid"] is False and ch2["stable"] is False  # never 0
    old = rpc_state.build_live(_snap(ch_age=4000), server_time=now, seq=8, epoch="e")
    assert old["devices"][0]["channels"][0]["weight_g"] is None
    assert old["devices"][0]["channels"][0]["weight_stale"] is True
    dead = rpc_state.build_live(_snap(age_s=30), server_time=now, seq=9, epoch="e")["devices"][0]
    assert dead["stale"] is True and dead["online"] is False
    assert all(c["weight_g"] is None and c["stable"] is False for c in dead["channels"])
    assert rpc_state.build_live({"controller": None, "weight": None}, server_time=now, seq=1,
                                epoch="e")["devices"] == []


def test_state_payloads_fit_their_caps_and_carry_envelope():
    jobs = [{"command_id": i, "material_id": "M1", "target_g": 5000, "state": "PENDING",
             "profile_id": "default", "profile_version": 1} for i in range(400)]
    q = rpc_state.build_queue({"device_id": RELAY, "waiting_commands": jobs, "failed_commands": []},
                              server_time=T0, seq=3, epoch="e")
    assert len(json.dumps(q, separators=(",", ":"))) <= 16384 and q["truncated"] is True
    assert {"server_time", "seq", "epoch"} <= set(q)
    devs = rpc_state.build_devices([{"device_id": RELAY, "age_seconds": 2, "status": RELAY_STATUS},
                                    {"device_id": SENDER, "age_seconds": 40,
                                     "status": {"role": "weight_sender"}}],
                                   server_time=T0, seq=4, epoch="e")
    assert [d["stale"] for d in devs["devices"]] == [False, True]


def test_coalescer_2hz_with_1hz_heartbeat():
    c = rpc_state.LiveCoalescer()
    assert c.mark("CH1", 0.0) is True
    assert c.mark("CH1", 0.1) is False and c.mark("CH1", 0.3) is False
    assert c.due(0.4) == []
    assert c.due(0.5) == ["CH1"]                 # coalesced update goes out at 2 Hz
    assert c.due(0.9) == []                      # nothing new, no heartbeat yet
    assert c.due(1.5) == ["CH1"]                 # heartbeat
    assert c.mark("CH1", 1.55) is False


def test_state_clock_is_monotonic_with_epoch():
    a, b = rpc_state.StateClock(), rpc_state.StateClock()
    assert a.epoch != b.epoch and [a.next(), a.next(), a.next()] == [1, 2, 3]


def test_live_resync_returns_full_snapshot_bundle(h):
    h.live = _snap()
    tok = h.login("vue1")
    r = h.call("live.resync", token=tok)
    assert r["ok"] and set(r["data"]) == {"live", "queue", "devices"}
    assert r["data"]["live"]["epoch"] == h.d.state.epoch
    seqs = [r["data"][k]["seq"] for k in ("live", "queue", "devices")]
    assert seqs == sorted(seqs) and len(set(seqs)) == 3




def test_devices_state_flags_stalled_runs_without_a_status_and_omits_it_when_unknown():
    from datetime import datetime, timezone
    now = datetime.now(timezone.utc)
    st = [{"run_id": "r1", "device_id": "d1", "channel_id": "CH1", "flag": "STALLED"}]
    out = rpc_state.build_devices([], server_time=now, seq=1, epoch="e", stalled=st)
    assert out["stalled_runs"] == st
    assert "stalled_runs" not in rpc_state.build_devices([], server_time=now, seq=1, epoch="e")
