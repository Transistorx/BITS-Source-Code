"""Live telemetry must remain fresh independently of history persistence."""
from concurrent.futures import ThreadPoolExecutor
from threading import Event


def test_live_store_rejects_old_and_duplicate_samples():
    from app.services.live_state import LiveState
    clock = [10.0]
    state = LiveState(clock=lambda: clock[0])
    assert state.update_weight({"weight_g": 1000, "seq": 20, "age_ms": 25})
    clock[0] += .1
    assert not state.update_weight({"weight_g": 500, "seq": 19, "age_ms": 25})
    assert not state.update_weight({"weight_g": 500, "seq": 20, "age_ms": 25})
    snap = state.snapshot()
    assert snap["weight"]["weight_g"] == 1000
    assert 124 <= snap["weight"]["age_ms"] <= 126
    assert snap["diagnostics"]["rejected"] == 2


def test_live_store_silence_invalidates_weight_without_zero_fallback():
    from app.services.live_state import LiveState
    clock = [10.0]
    state = LiveState(clock=lambda: clock[0])
    assert state.update_weight({"weight_g": 1234, "seq": 1, "age_ms": 0})
    clock[0] += 3.1
    snap = state.snapshot()
    assert snap["weight"]["weight_valid"] is False
    assert snap["weight"]["weight_g"] == 1234


def test_live_state_accepts_restart_only_after_explicit_session_reset():
    from app.services.live_state import LiveState
    state = LiveState()
    assert state.update_weight({"weight_g": 1000, "seq": 100, "age_ms": 0})
    assert not state.update_weight({"weight_g": 500, "seq": 1, "age_ms": 0})
    state.reset_weight_session()
    assert state.update_weight({"weight_g": 500, "seq": 1, "age_ms": 0})


def test_status_reaches_live_state_before_slow_database_commit(client, monkeypatch):
    from app.services.live_state import live_state
    from sqlalchemy.orm import Session
    live_state.clear()
    entered, release = Event(), Event()
    original = Session.commit

    def slow_commit(session):
        entered.set()
        assert release.wait(5)
        return original(session)

    monkeypatch.setattr(Session, "commit", slow_commit)
    payload = {"device_id": "relay-test", "status": {"role": "relay_controller",
        "uptime_ms": 1000, "channels": [{"channel_id": "CH2", "weight_g": 4321,
            "weight_valid": True, "weight_age_ms": 20, "relay_on": False}]}}
    with ThreadPoolExecutor() as pool:
        future = pool.submit(client.post, "/api/v1/device/status", json=payload)
        try:
            assert entered.wait(2)
            response = client.get("/api/v1/live/telemetry")
            assert response.status_code == 200
            assert response.json()["controller"]["status"]["channels"][0]["weight_g"] == 4321
        finally:
            release.set()
        assert future.result().status_code == 200


def test_status_uptime_regression_does_not_replace_newer_live_state():
    from app.services.live_state import LiveState
    state = LiveState()
    assert state.update_status("relay", {"role": "relay_controller", "uptime_ms": 5000})
    assert not state.update_status("relay", {"role": "relay_controller", "uptime_ms": 4000})
    assert state.snapshot()["controller"]["status"]["uptime_ms"] == 5000


def test_live_http_burst_never_touches_database(client, monkeypatch):
    from app.services.live_state import live_state
    from sqlalchemy.orm import Session
    live_state.clear()
    def forbidden_commit(_session):
        raise AssertionError("live ingress must never write history")
    monkeypatch.setattr(Session, "commit", forbidden_commit)
    for index in range(100):
        response = client.post("/api/v1/live/status", json={"device_id": "burst",
            "status": {"role": "relay_controller", "uptime_ms": index,
                "channels": [{"channel_id": "CH2", "weight_g": 1000+index,
                    "weight_age_ms": 0, "weight_valid": True}]}})
        assert response.status_code == 200 and response.json()["accepted"]
    snapshot = client.get("/api/v1/live/telemetry").json()
    assert snapshot["controller"]["status"]["channels"][0]["weight_g"] == 1099
    assert snapshot["diagnostics"]["received"] == 100


def test_invalid_live_channels_do_not_break_stream(client):
    for channels in ({}, [None], [{"weight_age_ms": "bad"}], [{"weight_age_ms": -1}]):
        response = client.post("/api/v1/live/status", json={"device_id": "bad",
            "status": {"role": "relay_controller", "channels": channels}})
        assert response.status_code == 422
    assert client.get("/api/v1/live/telemetry").status_code == 200


def _channel_snapshot(age_ms):
    from app.services.live_state import LiveState
    state = LiveState(clock=lambda: 10.0)
    assert state.update_status("relay", {"role": "relay_controller", "uptime_ms": 1,
        "channels": [{"channel_id": "CH1", "weight_g": 5, "weight_valid": True,
                      "weight_age_ms": age_ms}]})
    return state.snapshot()["controller"]["status"]["channels"][0]


def test_uint32_max_weight_age_is_unknown_not_a_real_age():
    channel = _channel_snapshot(4294967295)
    assert channel["weight_age_ms"] is None
    assert channel["weight_age_s"] is None
    assert channel["weight_valid"] is False


def test_normal_weight_age_is_unchanged():
    channel = _channel_snapshot(20)
    assert channel["weight_age_ms"] == 20
    assert channel["weight_age_s"] == 0.02
    assert channel["weight_valid"] is True


def test_static_js_treats_age_sentinel_as_no_reading():
    from pathlib import Path
    js = (Path(__file__).resolve().parents[1] / "app" / "static" / "js" / "common.js").read_text(encoding="utf-8")
    assert js.count("4294967295") >= 2
    assert "No reading yet" in js
    assert "Stale — last reading" in js
