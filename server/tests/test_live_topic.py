"""cas/{device}/telemetry/live: compact live weight, live memory only."""
import json
from pathlib import Path

import pytest

from app import mqtt_bridge
from app.services.live_state import live_state
from conftest import DEVICE_ID

TOPIC = f"cas/{DEVICE_ID}/telemetry/live"


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


@pytest.fixture(autouse=True)
def no_db(monkeypatch):
    def boom():
        raise AssertionError("telemetry/live must not open a DB session")
    monkeypatch.setattr(mqtt_bridge, "_db", boom)


def _ch(cid="CH1", g=1234, valid=True, age=20, stable=True):
    return {"channel_id": cid, "weight_g": g, "weight_valid": valid,
            "weight_age_ms": age, "stable": stable}


def _live(uptime=1000, channels=None, **kw):
    body = {"uptime_ms": uptime, "channels": channels if channels is not None else [_ch()]}
    mqtt_bridge.handle_message(kw.pop("topic", TOPIC), json.dumps(body).encode(),
                               kw.pop("retain", False))


def _status(uptime, g=500, ts=None):
    return {"role": "relay_controller", "uptime_ms": uptime,
            "channels": [{"channel_id": "CH1", "state": "IDLE", "weight_g": g,
                          "weight_valid": True, "weight_age_ms": 10},
                         {"channel_id": "CH2", "state": "IDLE", "weight_g": 7,
                          "weight_valid": True, "weight_age_ms": 10}]}


def _ch1(snap):
    return snap["controller"]["status"]["channels"][0]


def test_subscribed_qos0():
    assert ("cas/+/telemetry/live", 0) in mqtt_bridge.SUBSCRIPTIONS


def test_valid_live_updates_state_without_db(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000)
    ch = _ch1(live_state.snapshot())
    assert ch["weight_g"] == 1234 and ch["weight_valid"] is True and ch["stable"] is True
    assert ch["weight_age_ms"] == 20


def test_age_grows_and_invalidates(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(age=100)])
    clock.t += 3.0
    ch = _ch1(live_state.snapshot())
    assert ch["weight_age_ms"] == 3100 and ch["weight_valid"] is False


def test_null_weight_stays_null_and_invalid(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=None, valid=False, age=None, stable=True)])
    ch = _ch1(live_state.snapshot())
    assert ch["weight_g"] is None and ch["weight_valid"] is False and ch["stable"] is False


def test_valid_true_with_null_weight_is_invalid(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=None, valid=True)])
    ch = _ch1(live_state.snapshot())
    assert ch["weight_g"] is None and ch["weight_valid"] is False


@pytest.mark.parametrize("bad", [12.5, 12.0, True, "12", [1]])
def test_bad_weight_type_rejected(clock, bad):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=bad)])
    assert _ch1(live_state.snapshot())["weight_g"] == 500  # untouched status value


@pytest.mark.parametrize("body", [
    {"uptime_ms": -1, "channels": [_ch()]},
    {"uptime_ms": 4294967296, "channels": [_ch()]},
    {"uptime_ms": 1.5, "channels": [_ch()]},
    {"uptime_ms": True, "channels": [_ch()]},
    {"uptime_ms": 5, "channels": []},
    {"uptime_ms": 5, "channels": [_ch("CH1"), _ch("CH1")]},
    {"uptime_ms": 5, "channels": [_ch("CH1"), _ch("CH2"), _ch("CH1")]},
    {"uptime_ms": 5, "channels": [_ch("CH3")]},
    {"uptime_ms": 5, "channels": [_ch(age=-1)]},
    {"uptime_ms": 5, "channels": [_ch(age=1.5)]},
    {"uptime_ms": 5, "channels": [dict(_ch(), weight_valid="yes")]},
])
def test_malformed_never_reaches_state(clock, body):
    mqtt_bridge.handle_message(TOPIC, json.dumps(body).encode())
    assert live_state.snapshot()["diagnostics"]["received"] == 0
    assert live_state._live == {}


def test_non_json_never_reaches_state(clock):
    mqtt_bridge.handle_message(TOPIC, b"{not json")
    mqtt_bridge.handle_message(TOPIC, b"[1,2]")
    assert live_state._live == {}


def test_retained_ignored(clock):
    _live(1000, retain=True)
    assert live_state._live == {}


def test_foreign_device_ignored(clock):
    body = {"device_id": "someone-else", "uptime_ms": 1, "channels": [_ch()]}
    mqtt_bridge.handle_message(TOPIC, json.dumps(body).encode())
    assert live_state._live == {}


def test_matching_payload_device_id_accepted(clock):
    body = {"device_id": DEVICE_ID, "uptime_ms": 1, "channels": [_ch()]}
    mqtt_bridge.handle_message(TOPIC, json.dumps(body).encode())
    assert DEVICE_ID in live_state._live


def test_uptime_must_be_strictly_greater(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=1000)])
    _live(1000, [_ch(g=2000)])      # duplicate
    clock.t += 1
    _live(900, [_ch(g=3000)])       # regression within 10 s
    assert _ch1(live_state.snapshot())["weight_g"] == 1000
    _live(1001, [_ch(g=4000)])
    assert _ch1(live_state.snapshot())["weight_g"] == 4000


def test_reboot_regression_accepted_after_10s(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(50000, [_ch(g=1000)])
    clock.t += 11
    _live(5, [_ch(g=2000)])
    assert live_state._live[DEVICE_ID]["CH1"][0] == 2000   # status is offline: snapshot nulls


def test_forged_huge_uptime_rejected_and_does_not_poison(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=1000)])
    stamp = live_state._order[DEVICE_ID]
    clock.t += 1
    _live(2_000_000_000, [_ch(g=9999)])
    assert live_state._order[DEVICE_ID] == stamp           # no stamp refresh
    assert _ch1(live_state.snapshot())["weight_g"] == 1000
    _live(1100, [_ch(g=1100)])                              # genuine still accepted
    assert _ch1(live_state.snapshot())["weight_g"] == 1100


def test_plausible_jump_after_silence_accepted(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=1000)])
    clock.t += 11
    _live(500_000, [_ch(g=2000)])
    assert live_state._live[DEVICE_ID]["CH1"][0] == 2000   # status is offline: snapshot nulls


def test_slow_plausible_jump_accepted(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=1000)])
    clock.t += 4
    _live(1000 + 4000 + 4900, [_ch(g=3000)])
    assert _ch1(live_state.snapshot())["weight_g"] == 3000


def test_idle_device_evicted_after_60s(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000)
    clock.t += 61
    live_state.snapshot()
    assert DEVICE_ID not in live_state._order and DEVICE_ID not in live_state._live


def test_device_cap_recovers_after_eviction(clock):
    for i in range(16):
        assert live_state.update_live(f"d{i}", 100, [_ch()])
    assert not live_state.update_live("d16", 100, [_ch()])
    clock.t += 61
    assert live_state.update_live("d16", 100, [_ch()])
    assert live_state.update_live("d0", 5, [_ch()])         # evicted: first message


def test_aged_out_live_slot_nulls_weight(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=1234)])
    clock.t += 4
    ch = _ch1(live_state.snapshot())
    assert ch["weight_valid"] is False and ch["weight_g"] is None and ch["stable"] is False


def test_same_tick_status_and_live_duplicate_keeps_slot(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(g=1234)])
    live_state.update_status(DEVICE_ID, _status(1000, g=1))  # same tick, same uptime
    assert _ch1(live_state.snapshot())["weight_g"] == 1234
    _live(1001, [_ch(g=1500)])
    assert _ch1(live_state.snapshot())["weight_g"] == 1500


def test_live_does_not_refresh_controller_stamp(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    for i in range(1, 15):
        clock.t += 1
        _live(1000 + i)
    snap = live_state.snapshot()
    assert snap["controller"]["online"] is False
    ch = _ch1(snap)
    assert ch["state"] == "OFFLINE" and ch["weight_valid"] is False


def test_status_cannot_overwrite_fresher_live(clock):
    live_state.update_status(DEVICE_ID, _status(900, g=500))
    _live(1000, [_ch(g=1234)])
    live_state.update_status(DEVICE_ID, _status(990, g=999))   # older uptime
    assert _ch1(live_state.snapshot())["weight_g"] == 1234
    live_state.update_status(DEVICE_ID, _status(1100, g=777))  # newer status wins
    assert _ch1(live_state.snapshot())["weight_g"] == 777


def test_live_older_than_status_is_dropped(clock):
    live_state.update_status(DEVICE_ID, _status(2000, g=777))
    _live(1500, [_ch(g=1234)])
    assert _ch1(live_state.snapshot())["weight_g"] == 777


def test_age_sentinel_is_unknown(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch(age=4294967295)])
    ch = _ch1(live_state.snapshot())
    assert ch["weight_age_ms"] is None and ch["weight_valid"] is False
    _live(1001, [_ch(age=4294967294)])
    assert _ch1(live_state.snapshot())["weight_valid"] is False  # age too old


def test_other_channel_untouched(clock):
    live_state.update_status(DEVICE_ID, _status(900))
    _live(1000, [_ch("CH2", g=42)])
    chans = live_state.snapshot()["controller"]["status"]["channels"]
    assert chans[0]["weight_g"] == 500 and chans[1]["weight_g"] == 42


def test_live_js_poll_does_not_overwrite_streamed_weight():
    src = (Path(__file__).resolve().parents[1] / "app" / "static" / "js" / "live.js").read_text(
        encoding="utf-8")
    start = src.index("function render(")
    body = src[start:src.index("function renderLiveWeight")]
    idx = body.index("T.el('live-weight').textContent")
    guard = body[:idx].rstrip().splitlines()[-1]
    assert "liveSnapshot" in guard and "controller" in guard


# --- sender telemetry/weight in the live snapshot (CONTRACT 9.5) ------------------

def _sender(uptime, ch="CH1", g=1234, age=20, stable=True, device="snd1", **kw):
    body = {"uptime_ms": uptime, "channel": ch, "weight_g": g, "stable": stable, "age_ms": age}
    mqtt_bridge.handle_message(f"cas/{device}/telemetry/weight", json.dumps(body).encode(),
                               kw.pop("retain", False))


def _devices(snap):
    from datetime import datetime, timezone
    from app.rpc.state import build_live
    out = build_live(snap, server_time=datetime.now(timezone.utc), seq=1, epoch="e")
    return {d["device_id"]: d for d in out["devices"]}


def test_sender_weight_appears_per_channel_and_goes_stale(clock):
    _sender(1000, "CH1", 1234)
    _sender(1000, "CH2", 77, device="snd1")           # same uptime on the other channel: no collision
    snd = _devices(live_state.snapshot())["snd1"]
    assert snd["role"] == "weight_sender" and snd["stale"] is False
    got = {c["channel_id"]: c for c in snd["channels"]}
    assert got["CH1"]["weight_g"] == 1234 and got["CH1"]["weight_valid"] and got["CH2"]["weight_g"] == 77
    assert "snd1/CH1" not in live_state._live          # no longer keyed into the relay slots
    _sender(1500, "CH1", 1300)                         # newer frame replaces only its channel
    clock.t += 2
    got = {c["channel_id"]: c for c in _devices(live_state.snapshot())["snd1"]["channels"]}
    assert got["CH1"]["weight_g"] == 1300 and got["CH1"]["weight_age_ms"] >= 2000
    clock.t += 2                                       # > 3 s since the last frame: null, not 0
    snd = _devices(live_state.snapshot())["snd1"]
    assert snd["stale"] is True
    assert all(c["weight_g"] is None and c["weight_valid"] is False for c in snd["channels"])


def test_sender_weight_ignores_retained_and_reorders_and_never_touches_the_relay_slot(clock):
    _sender(1000, g=500, retain=True)
    assert "snd1" not in _devices(live_state.snapshot())
    _sender(2000, g=900)
    _sender(1900, g=1)                                 # older uptime: dropped
    got = _devices(live_state.snapshot())["snd1"]["channels"][0]
    assert got["weight_g"] == 900
    assert live_state.snapshot()["controller"] is None


def test_weight_ctl_topic_never_feeds_live_state(clock):
    mqtt_bridge.handle_message("cas/snd1/weight/ctl", b'{"uptime_ms":1,"channel":"CH1",'
                               b'"weight_g":5,"age_ms":1}', False)
    assert live_state.snapshot()["senders"] == {}


def test_idle_sender_is_evicted(clock):
    _sender(1000)
    clock.t += 61
    assert live_state.snapshot()["senders"] == {}
