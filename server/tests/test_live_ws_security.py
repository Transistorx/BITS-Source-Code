"""Live WebSocket security gates (REQ-WMQ-32, HZ-10): reads gated like HTTP, Origin check,
bench writes only from loopback, client cap, constant-time key compare, loop-gone handoff."""
import asyncio
import time

import pytest
from starlette.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from app import auth as auth_module
from app.main import app
from app.services import live_hub as hub_module
from test_live_ws import API_KEY, URL, Session, accepted, connect, env  # noqa: F401
from wmq_support import Probe, cmd, is_refusal, send_weight, weight_body

SAME_HOST_ORIGIN = "http://testserver"


class CodeProbe(Probe):
    def _pump(self):
        try:
            while True:
                self.q.put(self.ws.receive_json())
        except WebSocketDisconnect as exc:
            self.q.put({"type": "_closed", "code": exc.code, "reason": exc.reason})
        except BaseException as exc:
            self.q.put({"type": "_closed", "exc": repr(exc)})


class CodeSession(Session):
    def __enter__(self):
        self.cm = self.env.client.websocket_connect(URL, headers=self.headers)
        self.ws = self.cm.__enter__()
        self.probe = CodeProbe(self.ws)
        return self.probe


def connect_codes(env, **kw):
    return CodeSession(env, **kw)


def refused_at_handshake(env, headers):
    with pytest.raises(WebSocketDisconnect) as info:
        with env.client.websocket_connect(URL, headers=headers):
            pass
    return info.value


def closed_code(frame):
    assert frame is not None and frame.get("type") == "_closed", frame
    return frame.get("code")


@pytest.fixture()
def gated(env, monkeypatch):
    monkeypatch.setenv("READS_REQUIRE_KEY", "true")
    monkeypatch.setenv("LIVE_WS_AUTH_TIMEOUT_MS", "300")
    send_weight(env.sender, weight_body(weight_g=111))
    return env


def test_reads_gated_no_key_gets_no_frame_and_policy_close(gated):
    with connect_codes(gated, headers={}, key=None) as p:
        first = p.next(2.0)
    assert closed_code(first) == 1008, first
    assert not [f for f in p.log if f.get("type") in ("snapshot", "sender", "broker")]
    assert gated.hub.health()["clients"] == 0


def test_reads_gated_header_key_gets_snapshot_first(gated):
    with connect(gated) as p:
        first = p.next(2.0)
    assert first["type"] == "snapshot" and gated.sender in first["senders"]


def test_reads_gated_first_frame_auth_unlocks_snapshot_and_commands(gated):
    with connect_codes(gated, headers={}, key=None) as p:
        p.send({"type": "auth", "api_key": API_KEY})
        frames = [p.next(2.0) for _ in range(3)]
        assert [f["type"] for f in frames if f] == ["snapshot", "broker", "auth_ok"]
        p.send(cmd(gated.sender))
        accepted(p)


def test_reads_gated_non_auth_first_frame_is_policy_closed_without_data(gated):
    with connect_codes(gated, headers={}, key=None) as p:
        p.send(cmd(gated.sender))
        first = p.next(2.0)
    assert closed_code(first) == 1008
    assert not [f for f in p.log if f.get("type") in ("snapshot", "sender", "cmd_state")]


def test_reads_gated_wrong_first_frame_key_is_policy_closed(gated):
    with connect_codes(gated, headers={}, key=None) as p:
        p.send({"type": "auth", "api_key": "bad"})
        first = p.next(2.0)
    assert closed_code(first) == 1008
    assert not [f for f in p.log if f.get("type") == "snapshot"]


def test_reads_gated_oversize_first_frame_is_policy_closed(gated):
    with connect_codes(gated, headers={}, key=None) as p:
        p.send({"type": "auth", "api_key": "x" * 600})
        first = p.next(2.0)
    assert closed_code(first) == 1008


def test_reads_gated_but_empty_api_key_matches_http_open_reads(gated, monkeypatch):
    monkeypatch.setenv("API_KEY", "")
    with connect_codes(gated, headers={}, key=None) as p:
        first = p.next(2.0)
    assert first["type"] == "snapshot"


def test_reads_not_gated_keeps_snapshot_first_without_key(env):
    with connect(env, headers={}, key=None) as p:
        first = p.next(2.0)
    assert first["type"] == "snapshot"


def test_origin_mismatch_is_refused_at_handshake(env):
    exc = refused_at_handshake(env, {"origin": "http://evil.example", "x-api-key": API_KEY})
    assert exc.code == 1008
    assert env.hub.health()["clients"] == 0


def test_origin_null_is_refused(env):
    assert refused_at_handshake(env, {"origin": "null", "x-api-key": API_KEY}).code == 1008


def test_origin_same_host_is_accepted(env):
    with connect(env, headers={"origin": SAME_HOST_ORIGIN, "x-api-key": API_KEY}) as p:
        assert p.next(2.0)["type"] == "snapshot"


def test_origin_in_cors_allowlist_is_accepted(env, monkeypatch):
    monkeypatch.setenv("CORS_ORIGINS", "http://ui.example:8080, https://ops.example")
    with connect(env, headers={"origin": "https://ops.example", "x-api-key": API_KEY}) as p:
        assert p.next(2.0)["type"] == "snapshot"
    assert refused_at_handshake(env, {"origin": "https://other.example"}).code == 1008


def test_origin_absent_is_accepted_for_non_browser_clients(env):
    with connect(env, headers={"x-api-key": API_KEY}) as p:
        assert p.next(2.0)["type"] == "snapshot"


@pytest.fixture()
def loopback(env):
    with TestClient(app, client=("127.0.0.1", 50001)) as c:
        yield c


def test_bench_open_writes_refused_for_non_loopback_client(env, monkeypatch):
    monkeypatch.setenv("API_KEY", "")
    monkeypatch.setenv("BENCH_OPEN_WRITES", "true")
    with connect(env, headers={}, key=None) as p:
        p.send(cmd(env.sender))
        verdict = p.verdict()
    assert is_refusal(verdict) and verdict["reason"] == "not authenticated"


def test_bench_open_writes_allowed_for_loopback_client(env, loopback, monkeypatch):
    monkeypatch.setenv("API_KEY", "")
    monkeypatch.setenv("BENCH_OPEN_WRITES", "true")
    with loopback.websocket_connect(URL) as ws:
        from wmq_support import Probe
        p = Probe(ws)
        p.send(cmd(env.sender))
        accepted(p)


def test_max_ws_clients_refuses_beyond_cap_and_frees_on_close(env, monkeypatch):
    monkeypatch.setenv("MAX_WS_CLIENTS", "2")
    a, b = Session(env), Session(env)
    pa, pb = a.__enter__(), b.__enter__()
    try:
        assert pa.next(2.0)["type"] == "snapshot" and pb.next(2.0)["type"] == "snapshot"
        assert refused_at_handshake(env, {"x-api-key": API_KEY}).code == 1013
    finally:
        a.__exit__(None, None, None)
    for _ in range(50):
        if env.hub.health()["clients"] <= 1:
            break
        time.sleep(0.02)
    try:
        with connect(env) as pc:
            assert pc.next(2.0)["type"] == "snapshot"
    finally:
        b.__exit__(None, None, None)


def test_api_key_compare_is_constant_time(monkeypatch):
    calls = []

    def recorder(a, b):
        calls.append((a, b))
        return a == b

    monkeypatch.setenv("API_KEY", "k-test")
    monkeypatch.setattr(hub_module.hmac, "compare_digest", recorder)
    monkeypatch.setattr(auth_module.hmac, "compare_digest", recorder)
    assert hub_module.LiveHub.key_matches("k-test") is True
    assert hub_module.LiveHub.key_matches("k-tesT") is False
    assert hub_module.LiveHub.key_matches(None) is False
    assert hub_module.LiveHub.key_matches("k\u00e9y") is False
    auth_module._check("k-test")
    with pytest.raises(Exception):
        auth_module._check("wrong")
    assert len(calls) >= 4


def test_bind_id_handoff_without_loop_or_with_closed_loop_is_counted_never_raises():
    hub = hub_module.LiveHub()
    inf = hub_module.InFlight(None, "dev-x", "CH1", 1)
    before = hub.health()
    hub.bind_id_threadsafe(inf, 7)
    assert hub.health()["fanout_unbound"] == before["fanout_unbound"] + 1
    loop = asyncio.new_event_loop()
    hub.bind_loop(loop)
    loop.close()
    hub.bind_id_threadsafe(inf, 8)
    assert hub.health()["fanout_failures"] == 1
    assert inf.command_id is None
