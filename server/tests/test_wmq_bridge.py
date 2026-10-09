"""REQ-WMQ server bridge, config and live_state intake. Written from the spec before implementation."""
import json
import re
import statistics
import time
from pathlib import Path

import pytest
from sqlalchemy.orm import Session

from app import database, mqtt_bridge
from app.config import settings
from app.models import DeviceCommand, DeviceStatus
from app.services.analytics import utcnow
from app.services.live_state import live_state
from conftest import DEVICE_ID
from test_mqtt_bridge import FakePublisher
from wmq_support import (DROP, RELAY_STATUS, send_weight, set_link, weight_body,
                         weight_topic)

DEV = "bits-sender-w001"
GOLDEN = Path(__file__).parent / "golden" / "weight_v1.json"


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


@pytest.fixture()
def clock(monkeypatch):
    c = Clock()
    monkeypatch.setattr(live_state, "_clock", c)
    live_state.clear()
    yield c
    live_state.clear()


@pytest.fixture()
def pub():
    fake = FakePublisher()
    mqtt_bridge.set_publisher(fake)
    yield fake
    mqtt_bridge.set_publisher(None)


def _channels(device=DEV):
    snap = live_state.snapshot()
    return {c["channel_id"]: c for c in snap["senders"].get(device, {}).get("channels", [])}


def _diag():
    return live_state.snapshot()["diagnostics"]


def test_REQ_WMQ_12_weight_with_channel_field_is_accepted(clock):
    send_weight(DEV, weight_body(channel="CH1", src_uart="UART1", weight_g=1234))
    ch = _channels()["CH1"]
    assert ch["weight_g"] == 1234 and ch["weight_valid"] is True


def test_REQ_WMQ_12_channel_field_wins_over_src_uart(clock):
    send_weight(DEV, weight_body(channel="CH2", src_uart="UART1"))
    assert set(_channels()) == {"CH2"}


@pytest.mark.parametrize("uart,channel", [("UART1", "CH1"), ("UART2", "CH2")])
def test_REQ_WMQ_12_legacy_src_uart_only_derives_channel(clock, uart, channel):
    send_weight(DEV, weight_body(channel=DROP, src_uart=uart, weight_g=555))
    assert _channels()[channel]["weight_g"] == 555


def test_REQ_WMQ_12_legacy_and_new_bodies_mix_per_channel(clock):
    send_weight(DEV, weight_body(channel=DROP, src_uart="UART1", uptime_ms=1000))
    clock.t += 1
    send_weight(DEV, weight_body(channel="CH2", src_uart="UART2", uptime_ms=1500))
    assert set(_channels()) == {"CH1", "CH2"}


@pytest.mark.skipif(not GOLDEN.exists(), reason="golden/weight_v1.json not created yet")
def test_REQ_WMQ_12_golden_firmware_payload_is_parsed(clock):
    raw = GOLDEN.read_bytes()
    body = json.loads(raw)
    send_weight(DEV, raw)
    channel = body.get("channel") or {"UART1": "CH1", "UART2": "CH2"}[body["src_uart"]]
    assert _channels()[channel]["weight_g"] == body["weight_g"]


BAD_BODIES = {
    "not_json": b"{nope",
    "json_list": b"[1,2,3]",
    "nan": b'{"uptime_ms":1,"channel":"CH1","weight_g":NaN,"age_ms":1}',
    "weight_str": json.dumps(weight_body(weight_g="12")).encode(),
    "weight_float": json.dumps(weight_body(weight_g=1.5)).encode(),
    "age_negative": json.dumps(weight_body(age_ms=-1)).encode(),
    "age_sentinel": json.dumps(weight_body(age_ms=4294967295)).encode(),
    "uptime_missing": json.dumps(weight_body(uptime_ms=DROP)).encode(),
    "channel_ch3": json.dumps(weight_body(channel="CH3")).encode(),
    "uart9": json.dumps(weight_body(channel=DROP, src_uart="UART9")).encode(),
    "no_channel_no_uart": json.dumps(weight_body(channel=DROP, src_uart=DROP)).encode(),
    "oversize": json.dumps(weight_body(pad="x" * 600)).encode(),
}
SCHEMA_REJECTS = ("weight_str", "weight_float", "age_negative", "age_sentinel",
                  "uptime_missing", "channel_ch3", "uart9", "no_channel_no_uart")


@pytest.mark.parametrize("name", sorted(BAD_BODIES))
def test_REQ_WMQ_13_bad_message_rejected_and_bridge_keeps_working(clock, name):
    mqtt_bridge.handle_message(weight_topic(DEV), BAD_BODIES[name], False)
    mqtt_bridge.enqueue_message(weight_topic(DEV), BAD_BODIES[name], False)
    assert _channels() == {}
    clock.t += 1
    send_weight(DEV, weight_body(uptime_ms=20000, weight_g=42))
    assert _channels()["CH1"]["weight_g"] == 42


@pytest.mark.parametrize("name", SCHEMA_REJECTS)
def test_REQ_WMQ_13_schema_reject_is_counted_with_reason_per_device(clock, name):
    before = _diag().get("weight_rejected", 0)
    mqtt_bridge.handle_message(weight_topic(DEV), BAD_BODIES[name], False)
    diag = _diag()
    assert diag["weight_rejected"] == before + 1
    last = diag["last_reject"]
    assert DEV in last and last[DEV]


def test_REQ_WMQ_13_reject_visible_on_http_telemetry(client, clock):
    mqtt_bridge.handle_message(weight_topic(DEV), BAD_BODIES["channel_ch3"], False)
    diag = client.get("/api/v1/live/telemetry").json()["diagnostics"]
    assert diag["weight_rejected"] >= 1 and DEV in diag["last_reject"]


def test_REQ_WMQ_11_retained_weight_is_ignored(clock):
    send_weight(DEV, weight_body(), retain=True)
    assert _channels() == {}


def test_REQ_WMQ_08_heartbeat_with_unchanged_weight_is_accepted_then_goes_invalid(clock):
    send_weight(DEV, weight_body(uptime_ms=10000, cas_seq=7, age_ms=10))
    clock.t += 1
    send_weight(DEV, weight_body(uptime_ms=11000, cas_seq=7, age_ms=1010))
    assert _diag()["received"] == 2
    assert _channels()["CH1"]["weight_g"] == 1234
    clock.t += 1.5
    assert _channels()["CH1"]["weight_valid"] is True
    clock.t += 2.0
    ch = _channels()["CH1"]
    assert ch["weight_valid"] is False and ch["weight_g"] is None


def test_REQ_WMQ_44_weight_path_median_under_one_ms(clock):
    samples = []
    for i in range(200):
        raw = json.dumps(weight_body(uptime_ms=1000 + i * 20, seq=i)).encode()
        clock.t += 0.02
        t0 = time.perf_counter()
        mqtt_bridge.enqueue_message(weight_topic(DEV), raw, False)
        samples.append(time.perf_counter() - t0)
    assert statistics.median(samples) < 0.001


def test_REQ_WMQ_44_weight_path_never_opens_db(clock, monkeypatch):
    def boom():
        raise AssertionError("db")
    monkeypatch.setattr(mqtt_bridge, "_db", boom)
    send_weight(DEV, weight_body())
    assert _channels()["CH1"]["weight_g"] == 1234


def test_REQ_WMQ_45_state_reports_inbound_age_and_fanout_failures(monkeypatch, clock):
    set_link(monkeypatch)
    mqtt_bridge.enqueue_message(weight_topic(DEV), json.dumps(weight_body()).encode(), False)
    state = mqtt_bridge.mqtt_state()
    assert state["state"] == "CONNECTED"
    assert state["last_inbound_age_s"] < 5
    assert state["fanout_failures"] == 0


def test_REQ_WMQ_45_connected_without_inbound_for_30s_is_degraded(monkeypatch, clock):
    set_link(monkeypatch)
    mqtt_bridge.enqueue_message(weight_topic(DEV), json.dumps(weight_body()).encode(), False)
    real_mono, real_time = time.monotonic, time.time
    monkeypatch.setattr(time, "monotonic", lambda: real_mono() + 31)
    monkeypatch.setattr(time, "time", lambda: real_time() + 31)
    state = mqtt_bridge.mqtt_state()
    assert state["state"] == "DEGRADED"
    assert state["last_inbound_age_s"] >= 30


def test_REQ_WMQ_45_disconnected_is_not_reported_degraded(monkeypatch, clock):
    set_link(monkeypatch, connected=False)
    assert mqtt_bridge.mqtt_state()["state"] == "DISCONNECTED"


def test_REQ_WMQ_50_client_id_defaults_to_unique_per_process_value(monkeypatch):
    monkeypatch.delenv("MQTT_CLIENT_ID", raising=False)
    assert re.fullmatch(r"dispense-server-\d+-[0-9a-f]{4}", settings.mqtt_client_id)


def test_REQ_WMQ_50_client_id_env_override(monkeypatch):
    monkeypatch.setenv("MQTT_CLIENT_ID", "worker-a")
    assert settings.mqtt_client_id == "worker-a"


def test_REQ_WMQ_39_scale_cmd_ttl_default_and_override(monkeypatch):
    monkeypatch.delenv("SCALE_CMD_TTL_MS", raising=False)
    assert settings.scale_cmd_ttl_ms == 3000
    assert mqtt_bridge.command_ttl_ms("ZERO") == 3000 and mqtt_bridge.command_ttl_ms("TARE") == 3000
    monkeypatch.setenv("SCALE_CMD_TTL_MS", "4500")
    assert mqtt_bridge.command_ttl_ms("ZERO") == 4500


def test_REQ_WMQ_39_other_transient_types_keep_their_ttl(monkeypatch):
    monkeypatch.delenv("TRANSIENT_COMMAND_TTL_SECONDS", raising=False)
    assert mqtt_bridge.command_ttl_ms("READY") == 10000
    assert mqtt_bridge.command_ttl_ms("ESTOP") == 0


def test_REQ_WMQ_32_bench_open_writes_defaults_false(monkeypatch):
    monkeypatch.delenv("BENCH_OPEN_WRITES", raising=False)
    assert settings.bench_open_writes is False
    monkeypatch.setenv("BENCH_OPEN_WRITES", "true")
    assert settings.bench_open_writes is True


def test_REQ_WMQ_33_whitelist_parses_csv_and_empty_means_none(monkeypatch):
    monkeypatch.delenv("LIVE_WS_DEVICE_WHITELIST", raising=False)
    assert list(settings.live_ws_device_whitelist) == []
    monkeypatch.setenv("LIVE_WS_DEVICE_WHITELIST", " a1 , b2 ,,")
    assert set(settings.live_ws_device_whitelist) == {"a1", "b2"}


def test_REQ_WMQ_28_tls_defaults_off(monkeypatch):
    monkeypatch.delenv("MQTT_TLS", raising=False)
    assert settings.mqtt_tls is False
    monkeypatch.setenv("MQTT_TLS", "true")
    assert settings.mqtt_tls is True


class _FakePahoClient:
    created = []

    def __init__(self, *args, **kwargs):
        self.kwargs = kwargs
        _FakePahoClient.created.append(self)

    def __getattr__(self, name):
        return lambda *a, **k: None


@pytest.fixture()
def fake_paho(monkeypatch):
    import paho.mqtt.client as paho_client
    _FakePahoClient.created = []
    monkeypatch.setattr(paho_client, "Client", _FakePahoClient)
    monkeypatch.setenv("MQTT_ENABLED", "true")
    yield _FakePahoClient
    mqtt_bridge.stop()


def test_REQ_WMQ_49_multiple_workers_refuse_the_bridge(monkeypatch, fake_paho):
    monkeypatch.setenv("WEB_CONCURRENCY", "2")
    monkeypatch.delenv("MQTT_CLIENT_ID", raising=False)
    mqtt_bridge.start()
    assert fake_paho.created == [] and mqtt_bridge._client is None
    assert mqtt_bridge._publisher is None


def test_REQ_WMQ_49_multiple_workers_allowed_with_explicit_client_id(monkeypatch, fake_paho):
    monkeypatch.setenv("WEB_CONCURRENCY", "2")
    monkeypatch.setenv("MQTT_CLIENT_ID", "worker-a")
    mqtt_bridge.start()
    assert len(fake_paho.created) == 1
    assert fake_paho.created[0].kwargs.get("client_id") == "worker-a"


def test_REQ_WMQ_49_single_worker_starts(monkeypatch, fake_paho):
    monkeypatch.setenv("WEB_CONCURRENCY", "1")
    monkeypatch.delenv("MQTT_CLIENT_ID", raising=False)
    mqtt_bridge.start()
    assert len(fake_paho.created) == 1


def test_REQ_WMQ_50_start_uses_configured_client_id(monkeypatch, fake_paho):
    monkeypatch.delenv("WEB_CONCURRENCY", raising=False)
    monkeypatch.setenv("MQTT_CLIENT_ID", "srv-x")
    mqtt_bridge.start()
    assert fake_paho.created[0].kwargs.get("client_id") == "srv-x"


def test_REQ_WMQ_50_default_client_id_is_not_the_fixed_legacy_string(monkeypatch, fake_paho):
    monkeypatch.delenv("WEB_CONCURRENCY", raising=False)
    monkeypatch.delenv("MQTT_CLIENT_ID", raising=False)
    mqtt_bridge.start()
    client_id = fake_paho.created[0].kwargs.get("client_id")
    assert client_id != "dispense-server"
    assert re.fullmatch(r"dispense-server-\d+-[0-9a-f]{4}", client_id)


def _seed_pending(sender):
    with Session(database.get_engine()) as db:
        db.add(DeviceStatus(device_id=DEVICE_ID, status_json=RELAY_STATUS,
                            command_transport="MQTT", updated_at=utcnow()))
        db.add(DeviceStatus(device_id=sender, status_json={"role": "weight_sender"},
                            command_transport="MQTT", updated_at=utcnow()))
        ids = {}
        for name, device, kind, channel in (("zero", sender, "ZERO", "CH1"),
                                            ("tare", sender, "TARE", "CH2"),
                                            ("clear", DEVICE_ID, "CLEAR", None)):
            row = DeviceCommand(device_id=device, command_type=kind, channel_id=channel,
                                state="PENDING", created_at=utcnow(), updated_at=utcnow())
            db.add(row)
            db.flush()
            ids[name] = row.id
        db.commit()
    return ids


def _state(command_id):
    with Session(database.get_engine()) as db:
        return db.get(DeviceCommand, command_id).state


def test_REQ_WMQ_37_startup_recovery_expires_pending_zero_tare_without_publishing(client, pub):
    mqtt_bridge.set_publisher(None)
    ids = _seed_pending("bits-sender-r001")
    mqtt_bridge.set_publisher(pub)
    mqtt_bridge.startup_recovery()
    assert _state(ids["zero"]) == "EXPIRED"
    assert _state(ids["tare"]) == "EXPIRED"
    assert [m for m in pub.sent if m[1].get("type") in ("ZERO", "TARE")] == []
    assert [m for m in pub.sent if m[0].startswith("cas/bits-sender-r001/")] == []


def test_REQ_WMQ_37_startup_recovery_still_republishes_other_pending_commands(client, pub):
    mqtt_bridge.set_publisher(None)
    ids = _seed_pending("bits-sender-r002")
    mqtt_bridge.set_publisher(pub)
    mqtt_bridge.startup_recovery()
    assert [m[1]["type"] for m in pub.sent] == ["CLEAR"]
    assert _state(ids["clear"]) != "EXPIRED"
