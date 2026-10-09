"""REQ-WMQ live WebSocket path (LiveHub + /api/v1/live/ws). Written from the spec before implementation.

Assumptions taken from the spec: singleton ``live_hub`` in ``app.services.live_hub``,
class ``LiveHub`` with ``bind_loop/register/unregister/health/on_*_threadsafe``,
route ``/api/v1/live/ws``, frames as listed in the design section 2.
"""
import asyncio
import json
import time

import pytest

from app import mqtt_bridge
from app.services.live_state import live_state
from test_mqtt_bridge import FakePublisher
from wmq_support import (Probe, ack, cmd, command_rows, is_refusal, seed_devices,
                         send_weight, sent_commands, set_link, unique_sender, wait_for,
                         weight_body)

API_KEY = "k-test"
URL = "/api/v1/live/ws"


@pytest.fixture()
def env(client, monkeypatch):
    sender = unique_sender()
    monkeypatch.setenv("API_KEY", API_KEY)
    monkeypatch.setenv("LIVE_WS_DEVICE_WHITELIST", sender)
    monkeypatch.delenv("BENCH_OPEN_WRITES", raising=False)
    seed_devices(sender)
    pub = FakePublisher()
    mqtt_bridge.set_publisher(pub)
    set_link(monkeypatch)
    from app.services.live_hub import live_hub
    yield type("Env", (), {"sender": sender, "pub": pub, "client": client, "hub": live_hub})()
    mqtt_bridge.set_publisher(None)


class Session:
    def __init__(self, env, headers=None, key=API_KEY):
        self.env = env
        self.headers = {"x-api-key": key} if key and headers is None else (headers or {})
        self.cm = None

    def __enter__(self):
        self.cm = self.env.client.websocket_connect(URL, headers=self.headers)
        self.ws = self.cm.__enter__()
        self.probe = Probe(self.ws)
        return self.probe

    def __exit__(self, *exc):
        try:
            self.cm.__exit__(*exc)
        except BaseException:
            pass


def connect(env, **kw):
    return Session(env, **kw)


def accepted(probe, timeout=2.0):
    frame = probe.of_type("cmd_state", timeout, status="accepted")
    assert frame is not None, probe.log
    return frame


def test_REQ_WMQ_48_snapshot_is_first_frame_and_equals_http_telemetry(env, monkeypatch):
    class Clock:
        t = 500.0

        def __call__(self):
            return self.t
    monkeypatch.setattr(live_state, "_clock", Clock())
    live_state.clear()
    send_weight(env.sender, weight_body(weight_g=321))
    with connect(env) as p:
        first = p.next(3)
    http = env.client.get("/api/v1/live/telemetry").json()
    assert first["type"] == "snapshot"
    assert {k: v for k, v in first.items() if k != "type"} == http


def test_REQ_WMQ_09_sender_frame_carries_validity_and_age(env):
    with connect(env) as p:
        p.of_type("snapshot", 3)
        send_weight(env.sender, weight_body(weight_g=2222, uptime_ms=50000))
        frame = p.of_type("sender", 3, device_id=env.sender)
    assert frame is not None
    ch = frame["channels"][0]
    assert ch["channel_id"] == "CH1" and ch["weight_g"] == 2222
    assert ch["weight_valid"] is True and isinstance(ch["weight_age_ms"], int)


def test_REQ_WMQ_09_stale_weight_is_sent_without_a_value(env):
    send_weight(env.sender, weight_body(weight_g=2222, uptime_ms=50000, age_ms=4000))
    with connect(env) as p:
        snap = p.of_type("snapshot", 3)
    ch = snap["senders"][env.sender]["channels"][0]
    assert ch["weight_valid"] is False and ch["weight_g"] is None


def test_REQ_WMQ_46_new_update_reaches_connected_client_via_loop_handoff(env):
    with connect(env) as p:
        p.of_type("snapshot", 3)
        for i in range(5):
            send_weight(env.sender, weight_body(weight_g=100 + i, uptime_ms=60000 + i * 100))
        end = time.monotonic() + 3
        last = None
        while time.monotonic() < end:
            frame = p.of_type("sender", 0.5, device_id=env.sender)
            if frame is None:
                break
            last = frame
            if last["channels"][0]["weight_g"] == 104:
                break
    assert last is not None and last["channels"][0]["weight_g"] == 104


def test_REQ_WMQ_32_command_without_api_key_is_refused(env):
    with connect(env, headers={}, key=None) as p:
        p.send(cmd(env.sender))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == [] and command_rows(env.sender) == []


def test_REQ_WMQ_32_command_with_wrong_key_is_refused(env):
    with connect(env, headers={"x-api-key": "nope"}) as p:
        p.send(cmd(env.sender))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == [] and command_rows(env.sender) == []


def test_REQ_WMQ_32_first_message_auth_enables_commands(env):
    with connect(env, headers={}, key=None) as p:
        p.send({"type": "auth", "api_key": API_KEY})
        p.send(cmd(env.sender))
        accepted(p)
    assert wait_for(lambda: len(sent_commands(env.pub, env.sender)) == 1)


def test_REQ_WMQ_32_first_message_auth_with_wrong_key_is_refused(env):
    with connect(env, headers={}, key=None) as p:
        p.send({"type": "auth", "api_key": "bad"})
        p.send(cmd(env.sender))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == []


def test_REQ_WMQ_32_empty_api_key_refuses_commands_unless_bench_open(env, monkeypatch):
    monkeypatch.setenv("API_KEY", "")
    with connect(env, headers={}, key=None) as p:
        p.send(cmd(env.sender))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == []


def test_REQ_WMQ_32_empty_api_key_with_bench_open_writes_allows_commands_from_loopback(env, monkeypatch):
    from starlette.testclient import TestClient
    from app.main import app
    monkeypatch.setenv("API_KEY", "")
    monkeypatch.setenv("BENCH_OPEN_WRITES", "true")
    with TestClient(app, client=("127.0.0.1", 50001)) as loopback:
        with loopback.websocket_connect(URL) as ws:
            p = Probe(ws)
            p.send(cmd(env.sender))
            accepted(p)


@pytest.mark.parametrize("name", ["ESTOP", "PUMP_START", "zero", "", "CLEAR"])
def test_REQ_WMQ_33_command_outside_zero_tare_is_refused(env, name):
    with connect(env) as p:
        p.send(cmd(env.sender, name=name))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == [] and command_rows(env.sender) == []


@pytest.mark.parametrize("channel", ["CH3", "ch1", "", "CH1;CH2"])
def test_REQ_WMQ_33_bad_channel_is_refused(env, channel):
    with connect(env) as p:
        p.send(cmd(env.sender, channel=channel))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == []


def test_REQ_WMQ_33_device_outside_whitelist_is_refused(env):
    with connect(env) as p:
        p.send(cmd("bits-sender-other"))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, "bits-sender-other") == []


def test_REQ_WMQ_33_empty_whitelist_refuses_every_command(env, monkeypatch):
    monkeypatch.setenv("LIVE_WS_DEVICE_WHITELIST", "")
    with connect(env) as p:
        p.send(cmd(env.sender))
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == [] and command_rows(env.sender) == []


@pytest.mark.parametrize("name", ["ZERO", "TARE"])
def test_REQ_WMQ_39_accepted_command_publishes_exact_payload(env, name):
    with connect(env) as p:
        frame = None
        p.send(cmd(env.sender, "CH2", name))
        frame = accepted(p)
    assert wait_for(lambda: len(sent_commands(env.pub, env.sender)) >= 1)
    (topic, body, qos, retain), = sent_commands(env.pub, env.sender)
    assert qos == 1 and retain is False
    assert body["type"] == name and body["command_type"] == name
    assert body["device_id"] == env.sender and body["channel_id"] == "CH2"
    assert body["ttl_ms"] == 3000
    assert body["command_id"] == frame["command_id"]


def test_REQ_WMQ_36_ws_command_is_a_device_command_row_published_exactly_once(env):
    with connect(env) as p:
        p.send(cmd(env.sender))
        frame = accepted(p)
        time.sleep(0.5)
    rows = command_rows(env.sender)
    assert [r.id for r in rows] == [frame["command_id"]]
    assert frame["command_id"] < 2 ** 31
    assert len(sent_commands(env.pub, env.sender)) == 1


def test_REQ_WMQ_36_ws_and_http_commands_never_share_an_id(env):
    with connect(env) as p:
        p.send(cmd(env.sender, "CH1"))
        ws_id = accepted(p)["command_id"]
    r = env.client.post("/api/v1/queue/control", headers={"x-api-key": API_KEY},
                        json={"action": "TARE", "channel_id": "CH2", "device_id": env.sender})
    assert r.status_code == 200, r.text
    assert r.json()["command_id"] != ws_id
    ids = [m[1]["command_id"] for m in sent_commands(env.pub, env.sender)]
    assert len(ids) == len(set(ids)) == 2


def test_REQ_WMQ_15_command_refused_with_error_when_bridge_not_connected(env, monkeypatch):
    set_link(monkeypatch, connected=False)
    with connect(env) as p:
        p.send(cmd(env.sender))
        frame = p.verdict()
        assert is_refusal(frame)
    assert sent_commands(env.pub, env.sender) == [] and command_rows(env.sender) == []


def test_REQ_WMQ_34_second_command_on_same_channel_is_refused_while_in_flight(env):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1", "ZERO"))
        accepted(a)
        b.send(cmd(env.sender, "CH1", "TARE"))
        assert is_refusal(b.verdict())
        b.send(cmd(env.sender, "CH2", "TARE"))
        accepted(b)
    assert len(sent_commands(env.pub, env.sender)) == 2


def test_REQ_WMQ_34_client_limited_to_one_command_per_second(env):
    with connect(env) as p:
        p.send(cmd(env.sender, "CH1"))
        accepted(p)
        p.send(cmd(env.sender, "CH2"))
        assert is_refusal(p.verdict())
    assert len(sent_commands(env.pub, env.sender)) == 1


def test_REQ_WMQ_34_twenty_rejections_close_the_socket(env):
    with connect(env) as p:
        closed = False
        for _ in range(60):
            try:
                p.send(cmd(env.sender, name="ESTOP"))
            except BaseException:
                closed = True
                break
            time.sleep(0.01)
        if not closed:
            closed = p.until(lambda f: f.get("type") == "_closed", 3) is not None
        assert closed
    assert sent_commands(env.pub, env.sender) == []


def test_REQ_WMQ_35_extra_fields_are_rejected(env):
    with connect(env) as p:
        body = cmd(env.sender)
        body["ttl_ms"] = 99999
        p.send(body)
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == []


def test_REQ_WMQ_35_oversize_frame_is_rejected(env):
    with connect(env) as p:
        body = cmd(env.sender)
        body["device_id"] = env.sender + "x" * 600
        p.send(body)
        assert is_refusal(p.verdict())
    assert sent_commands(env.pub, env.sender) == []


def test_REQ_WMQ_35_garbage_frames_do_not_kill_the_server(env):
    with connect(env) as p:
        p.ws.send_text("{not json")
        p.ws.send_text("[1,2]")
        p.send({"type": "bogus"})
        p.send({"type": "ping"})
        assert p.of_type("pong", 3) is not None


def test_REQ_WMQ_38_ack_goes_to_originating_client_only(env):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1", "ZERO"))
        cid = accepted(a)["command_id"]
        ack(env.sender, cid, "FAILED", channel_id="CH1")
        got = a.of_type("cmd_ack", 3)
        assert got is not None and got["command_id"] == cid
        assert got["state"] == "FAILED" and got["late"] is False
        assert got["device_id"] == env.sender and got["channel"] == "CH1"
        time.sleep(0.3)
        b.send({"type": "ping"})
        seen = b.drain(0.6)
    assert not [f for f in seen if f.get("type") == "cmd_ack"]
    assert any(f.get("type") == "pong" for f in seen)


def test_REQ_WMQ_38_ack_frees_the_channel(env):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1"))
        cid = accepted(a)["command_id"]
        ack(env.sender, cid, "FAILED", channel_id="CH1")
        a.of_type("cmd_ack", 3)
        b.send(cmd(env.sender, "CH1", "TARE"))
        accepted(b)


def test_REQ_WMQ_38_ack_for_unknown_id_is_counted_and_never_broadcast(env):
    r = env.client.post("/api/v1/queue/control", headers={"x-api-key": API_KEY},
                        json={"action": "ZERO", "channel_id": "CH1", "device_id": env.sender})
    cid = r.json()["command_id"]
    before = env.hub.health().get("ack_unknown", 0)
    with connect(env) as p:
        p.of_type("snapshot", 3)
        ack(env.sender, cid, "FAILED", channel_id="CH1")
        wait_for(lambda: env.hub.health().get("ack_unknown", 0) > before)
        p.send({"type": "ping"})
        seen = p.drain(0.6)
    assert env.hub.health()["ack_unknown"] == before + 1
    assert not [f for f in seen if f.get("type") == "cmd_ack"]


def test_REQ_WMQ_16_link_loss_fails_in_flight_within_one_second_and_frees_channel(env, monkeypatch):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1"))
        accepted(a)
        t0 = time.monotonic()
        mqtt_bridge.handle_disconnect(object(), "test")
        lost = a.of_type("cmd_state", 1.2, status="link_lost")
        assert lost is not None and time.monotonic() - t0 <= 1.0
        set_link(monkeypatch)
        b.send(cmd(env.sender, "CH1", "TARE"))
        accepted(b)


def test_REQ_WMQ_42_new_boot_id_fails_in_flight_and_releases_lock(env):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1"))
        accepted(a)
        mqtt_bridge.handle_message(f"cas/{env.sender}/status",
                                   json.dumps({"online": True, "boot_id": "bbbbbbbb"}).encode(),
                                   False)
        frame = a.of_type("cmd_state", 2, status="rebooted")
        assert frame is not None and "reboot" in frame["reason"].lower()
        b.send(cmd(env.sender, "CH1", "TARE"))
        accepted(b)


def test_REQ_WMQ_19_no_ack_in_3s_is_outcome_unknown_and_lock_holds_until_ttl_window(env):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1"))
        cid = accepted(a)["command_id"]
        t0 = time.monotonic()
        unknown = a.of_type("cmd_state", 4.5, status="unknown")
        elapsed = time.monotonic() - t0
        assert unknown is not None and unknown["command_id"] == cid
        assert 2.5 <= elapsed <= 4.0
        assert not [f for f in a.log if f.get("status") == "failed"]
        b.send(cmd(env.sender, "CH1", "TARE"))
        assert is_refusal(b.verdict())
        released = a.of_type("cmd_state", 5.0, status="lock_released")
        assert released is not None
        assert 5.0 <= time.monotonic() - t0 <= 7.5
        b.send(cmd(env.sender, "CH1", "TARE"))
        accepted(b)
    assert len(sent_commands(env.pub, env.sender)) == 2


def test_REQ_WMQ_20_ack_after_lock_release_is_logged_and_dropped(env):
    with connect(env) as a:
        a.send(cmd(env.sender, "CH1"))
        cid = accepted(a)["command_id"]
        assert a.of_type("cmd_state", 8.0, status="lock_released") is not None
        before = env.hub.health().get("ack_unknown", 0)
        ack(env.sender, cid, "FAILED", channel_id="CH1")
        wait_for(lambda: env.hub.health().get("ack_unknown", 0) > before)
        a.send({"type": "ping"})
        seen = a.drain(0.6)
    assert not [f for f in seen if f.get("type") == "cmd_ack"]
    assert env.hub.health()["ack_unknown"] == before + 1


def test_REQ_WMQ_20_late_ack_before_lock_release_is_forwarded_with_late_true(env):
    with connect(env) as a, connect(env) as b:
        a.send(cmd(env.sender, "CH1"))
        cid = accepted(a)["command_id"]
        assert a.of_type("cmd_state", 4.5, status="unknown") is not None
        ack(env.sender, cid, "FAILED", channel_id="CH1")
        late = a.of_type("cmd_ack", 2)
        assert late is not None and late["command_id"] == cid and late["late"] is True
        b.send(cmd(env.sender, "CH1", "TARE"))
        accepted(b)


class FakeWs:
    def __init__(self, stalled=False):
        self.stalled = stalled
        self.sent = []
        self.closed = 0

    async def _record(self, data):
        if self.stalled:
            await asyncio.Event().wait()
        if isinstance(data, (str, bytes)):
            data = json.loads(data)
        self.sent.append((time.monotonic(), data))

    async def accept(self, *a, **k):
        return None

    async def send_json(self, data, *a, **k):
        await self._record(data)

    async def send_text(self, data, *a, **k):
        await self._record(data)

    async def send_bytes(self, data, *a, **k):
        await self._record(data)

    async def close(self, *a, **k):
        self.closed += 1

    def frames(self, type_):
        return [(t, f) for t, f in self.sent if isinstance(f, dict) and f.get("type") == type_]


def _run(coro):
    return asyncio.run(coro)


def test_REQ_WMQ_43_handoff_to_a_closed_loop_is_counted_and_never_raises():
    from app.services.live_hub import LiveHub
    hub = LiveHub()
    loop = asyncio.new_event_loop()
    hub.bind_loop(loop)
    loop.close()
    before = hub.health().get("fanout_failures", 0)
    hub.on_weight_threadsafe("dev-x")
    hub.on_ack_threadsafe(1, "dev-x", {"state": "FAILED"})
    hub.on_reboot_threadsafe("dev-x")
    hub.on_link_lost_threadsafe()
    assert hub.health()["fanout_failures"] == before + 4


def test_REQ_WMQ_43_handoff_without_bound_loop_never_raises():
    from app.services.live_hub import LiveHub
    hub = LiveHub()
    hub.on_weight_threadsafe("dev-x")
    hub.on_link_lost_threadsafe()


def test_REQ_WMQ_43_paho_thread_survives_when_loop_is_gone(env):
    from app.services.live_hub import live_hub
    loop = asyncio.new_event_loop()
    saved = getattr(live_hub, "_loop", None)
    live_hub.bind_loop(loop)
    loop.close()
    try:
        before = live_hub.health().get("fanout_failures", 0)
        send_weight(env.sender, weight_body(uptime_ms=777000))
        mqtt_bridge.enqueue_message(f"cas/{env.sender}/telemetry/weight",
                                    json.dumps(weight_body(uptime_ms=777100)).encode(), False)
        assert live_hub.health()["fanout_failures"] >= before + 1
        assert mqtt_bridge.mqtt_state()["fanout_failures"] >= before + 1
    finally:
        if saved is not None:
            live_hub.bind_loop(saved)


def test_REQ_WMQ_46_paused_client_gets_one_latest_slot_per_device_not_a_queue():
    from app.services.live_hub import LiveHub

    async def scenario():
        hub = LiveHub()
        hub.bind_loop(asyncio.get_running_loop())
        slot = hub.register(FakeWs(stalled=True))
        for i in range(1000):
            hub.on_weight_threadsafe("dev-a")
            hub.on_weight_threadsafe("dev-b")
        await asyncio.sleep(0.2)
        assert len(slot.dirty) <= 2
        assert len(slot.acks) <= 8
        assert slot.acks.maxlen == 8
        hub.unregister(slot)
    _run(scenario())


def test_REQ_WMQ_46_dirty_entry_is_one_per_device_regardless_of_update_count():
    from app.services.live_hub import LiveHub

    async def scenario():
        hub = LiveHub()
        hub.bind_loop(asyncio.get_running_loop())
        slot = hub.register(FakeWs(stalled=True))
        for _ in range(500):
            hub.on_weight_threadsafe("dev-a")
        await asyncio.sleep(0.2)
        assert len(slot.dirty) <= 1
        hub.unregister(slot)
    _run(scenario())


def test_REQ_WMQ_47_stalled_client_is_disconnected_and_others_get_updates_fast():
    from app.services.live_hub import LiveHub
    dev = "dev-slow-1"

    async def scenario():
        live_state.clear()
        hub = LiveHub()
        hub.bind_loop(asyncio.get_running_loop())
        stalled, good = FakeWs(stalled=True), FakeWs()
        s_slot = hub.register(stalled)
        g_slot = hub.register(good)
        live_state.update_sender_weight(dev, "CH1", 1000, 5, 10)
        t0 = time.monotonic()
        hub.on_weight_threadsafe(dev)
        for _ in range(30):
            await asyncio.sleep(0.01)
            if good.frames("sender"):
                break
        frames = good.frames("sender")
        assert frames and frames[0][0] - t0 <= 0.1
        await asyncio.sleep(2.8)
        assert stalled.closed >= 1
        assert good.closed == 0
        live_state.update_sender_weight(dev, "CH1", 1500, 6, 10)
        t1 = time.monotonic()
        hub.on_weight_threadsafe(dev)
        await asyncio.sleep(0.15)
        assert len(good.frames("sender")) >= 2
        assert good.frames("sender")[-1][0] - t1 <= 0.1
        hub.unregister(g_slot)
        hub.unregister(s_slot)
    _run(scenario())


def test_REQ_WMQ_47_unregistered_client_receives_nothing_more():
    from app.services.live_hub import LiveHub

    async def scenario():
        hub = LiveHub()
        hub.bind_loop(asyncio.get_running_loop())
        ws = FakeWs()
        slot = hub.register(ws)
        hub.unregister(slot)
        live_state.clear()
        live_state.update_sender_weight("dev-u", "CH1", 1000, 5, 10)
        hub.on_weight_threadsafe("dev-u")
        await asyncio.sleep(0.2)
        assert ws.frames("sender") == []
    _run(scenario())
