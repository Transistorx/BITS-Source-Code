from datetime import datetime, timedelta, timezone

from sqlalchemy import func, select


def test_run_start_is_idempotent_and_assigns_target_number(client, start_run):
    first = start_run("device-j1-10-a001", target_g=5000)
    second = start_run("device-j1-10-a001", target_g=5000)
    third = start_run("device-j2-20-a002", target_g=5000)
    assert first == second
    assert third["test_number"] == first["test_number"] + 1


def test_batch_insert_is_retry_safe_and_preserves_cas_samples(
        client, start_run, push_samples):
    run_id = "device-j1-10-a003"
    start_run(run_id)
    samples = [
        {"idx": 1, "elapsed_ms": 100, "weight_g": 250, "target_g": 5000,
         "error_g": 4750, "seq": 9, "stable": True,
         "relay1": True, "relay2": False, "state": "COARSE_DISPENSE"},
        {"idx": 2, "elapsed_ms": 200, "weight_g": 400, "target_g": 5000,
         "error_g": 4600, "seq": 10, "stable": False,
         "relay1": True, "relay2": False, "state": "COARSE_DISPENSE"},
    ]
    first = push_samples(run_id, samples).json()
    retry = push_samples(run_id, samples).json()
    assert (first["inserted"], first["duplicates"]) == (2, 0)
    assert (retry["inserted"], retry["duplicates"]) == (0, 2)
    rows = client.get(f"/api/v1/runs/{run_id}/samples").json()["samples"]
    assert [r["weight_g"] for r in rows] == [250, 400]
    assert rows[0]["source"] == "CAS"


def test_simulated_or_malformed_samples_are_rejected(client, start_run):
    run_id = "device-j1-10-a004"
    start_run(run_id)
    base = {"idx": 1, "elapsed_ms": 1, "weight_g": 100,
            "target_g": 5000, "error_g": 4900}
    assert client.post("/api/v1/telemetry/batch", json={
        "run_id": run_id, "samples": [{**base, "simulated": True}]
    }).status_code == 422
    assert client.post("/api/v1/telemetry/batch", json={
        "run_id": run_id, "samples": [{**base, "error_g": -1}]
    }).status_code == 422
    assert client.post("/api/v1/telemetry/batch", json={
        "run_id": run_id, "samples": [{**base, "idx": 0}]
    }).status_code == 422


def test_eleven_runs_keep_history_while_dashboard_shows_latest_ten(
        client, start_run):
    ids = [f"device-j{i}-10-a{i:03}" for i in range(1, 12)]
    for run_id in ids:
        start_run(run_id, target_g=5000)

    latest = client.get("/api/v1/runs/latest?target_g=5000&limit=10").json()
    assert len(latest) == 10
    assert ids[0] not in {row["run_id"] for row in latest}
    assert client.get(f"/api/v1/runs/{ids[0]}").status_code == 200
    history = client.get(f"/api/v1/runs?target_g=5000&run_id={ids[0]}").json()
    assert history["total"] == 1

    from app.database import get_engine
    with get_engine().connect() as conn:
        assert conn.execute(select(func.count()).select_from(
            __import__("app.models", fromlist=["DispenseRun"]).DispenseRun
        )).scalar_one() == 11


def test_timestamp_search_csv_and_complete_are_consistent(client, start_run,
                                                           push_samples):
    run_id = "device-j1-10-a005"
    start_run(run_id)
    push_samples(run_id, [{"idx": 1, "elapsed_ms": 500,
                           "weight_g": 5040, "target_g": 5000,
                           "error_g": -40, "state": "SETTLING"}])
    complete = client.post(f"/api/v1/runs/{run_id}/complete", json={
        "status": "COMPLETE", "final_weight_g": 5040,
        "max_weight_g": 5040, "duration_ms": 1500,
    })
    assert complete.status_code == 200
    assert complete.json()["final_error_g"] == 40
    assert complete.json()["overshoot_g"] == 40
    start = datetime.fromisoformat(complete.json()["started_at"].replace("Z", "+00:00"))
    results = client.get("/api/v1/runs", params={
        "from": (start - timedelta(seconds=1)).isoformat(),
        "to": (start + timedelta(seconds=1)).isoformat(),
        "status": "COMPLETE",
    })
    assert results.status_code == 200
    assert results.json()["total"] == 1
    csv = client.get(f"/api/v1/runs/{run_id}/csv")
    assert csv.status_code == 200
    assert "weight_g" in csv.text and "5040" in csv.text


def test_channel_scoped_run_samples_history_and_latest_ten(client, start_run):
    ids = []
    for channel, suffix in (("CH1", "one"), ("CH2", "two")):
        for i in range(11):
            run_id = f"dual-{suffix}-{i:02d}"
            ids.append(run_id)
            start_run(run_id, target_g=5000 if channel == "CH1" else 10000,
                      channel_id=channel)
    ch1 = client.get("/api/v1/runs/latest", params={
        "channel_id": "CH1", "target_g": 5000, "limit": 10,
    }).json()
    ch2 = client.get("/api/v1/runs/latest", params={
        "channel_id": "CH2", "target_g": 10000, "limit": 10,
    }).json()
    assert len(ch1) == len(ch2) == 10
    assert all(row["channel_id"] == "CH1" for row in ch1)
    assert all(row["channel_id"] == "CH2" for row in ch2)
    assert client.get("/api/v1/runs?channel_id=CH2&target_g=10000&run_id=dual-two-00").json()["total"] == 1
    assert client.get("/api/v1/runs/dual-two-00").json()["channel_id"] == "CH2"


def test_versioned_channel_profiles_activate_without_mutating_runs(client, start_run):
    run_id = "profile-ch2-run"
    start_run(run_id, target_g=12000, channel_id="CH2", profile_id="ch2-12kg", profile_version=4)
    payload = {
        "profile_id": "ch2-10kg", "material_id":"M2", "channel_id":"CH2", "target_g": 10000,
        "kp": 0.05, "ki": 0.001, "kd": 0.002, "tolerance_g": 25,
        "max_overshoot_g": 90, "max_duration_ms": 240000,
        "window_ms": 1000, "min_on_ms": 50, "min_off_ms": 50,
    }
    one = client.post("/api/v1/profiles", json=payload)
    assert one.status_code == 200, one.text
    two = client.post("/api/v1/profiles", json=payload)
    assert two.status_code == 200 and two.json()["version"] == one.json()["version"] + 1
    client.post("/api/v1/device/status", json={"device_id": "relay-profile", "status": {
        "channels": [{"channel_id": "CH1", "state": "IDLE"}, {"channel_id": "CH2", "state": "IDLE"}],
        "queue": [],
    }})
    activated = client.post(f"/api/v1/profiles/ch2-10kg/{two.json()['version']}/activate")
    assert activated.status_code == 200
    command = client.get("/api/v1/device/commands", params={"device_id": "relay-profile"}).json()[0]
    assert command["command_type"] == "PROFILE"
    assert command["profile_id"] == "ch2-10kg" and command["target_g"] == 10000
    assert command["kp"] == payload["kp"] and command["version"] == two.json()["version"]
    client.post(f"/api/v1/device/commands/{command['command_id']}/ack", json={"state": "APPLIED"})
    active = client.get("/api/v1/profiles/active", params={"material_id":"M2", "channel_id":"CH2", "target_g": 10000})
    assert active.json()["version"] == two.json()["version"]
    assert active.json()["active"] is True
    client.post(f"/api/v1/profiles/ch2-10kg/{two.json()['version']}/deactivate")
    clear = client.get("/api/v1/device/commands", params={"device_id": "relay-profile"}).json()[0]
    assert clear["command_type"] == "PROFILE_CLEAR"
    assert clear["channel_id"] == "CH2" and clear["target_g"] == 10000
    assert client.get(f"/api/v1/runs/{run_id}").json()["profile_version"] == 4


def test_material_scopes_same_target_profiles_and_dashboard_groups(client, start_run):
    profile_ids = {}
    for material, channel, run_id in (("M1", "CH1", "material-one-test"),
                                      ("M2", "CH2", "material-two-test")):
        start_run(run_id, target_g=5000, material_id=material, channel_id=channel)
        run = client.get(f"/api/v1/runs/{run_id}").json()
        assert run["material_id"] == material and run["pump_id"] == f"Pump {material[-1]}"
        assert run["assigned_at"] and run["channel_id"] == channel
        profile_id = material.lower() + "-profile"
        profile_ids[material] = profile_id
        body = {"profile_id":profile_id, "material_id":material, "channel_id":channel,
                "target_g":5000, "kp":0.02, "ki":0.001, "kd":0.0,
                "tolerance_g":20, "max_overshoot_g":100, "max_duration_ms":120000,
                "window_ms":500, "min_on_ms":40, "min_off_ms":40}
        assert client.post("/api/v1/profiles", json=body).status_code == 200
        assert client.post(f"/api/v1/profiles/{profile_id}/1/activate").status_code == 200
    m1 = client.get("/api/v1/profiles/active", params={"material_id":"M1", "channel_id":"CH1", "target_g":5000})
    m2 = client.get("/api/v1/profiles/active", params={"material_id":"M2", "channel_id":"CH2", "target_g":5000})
    assert m1.json()["profile_id"] == profile_ids["M1"]
    assert m2.json()["profile_id"] == profile_ids["M2"]
    for i in range(11):
        start_run(f"material-two-extra-{i}", target_g=5000, material_id="M2", channel_id="CH2")
    groups = client.get("/api/v1/dashboard").json()["targets"]
    same_target = [g for g in groups if g["target_g"] == 5000 and g["material_id"] in ("M1", "M2")]
    assert {g["material_id"] for g in same_target} == {"M1", "M2"}
    assert next(g for g in same_target if g["material_id"] == "M2")["runs"] and len(
        next(g for g in same_target if g["material_id"] == "M2")["runs"]) == 10
    assert client.get("/api/v1/runs?material_id=M2&target_g=5000").json()["total"] == 12
    csv = client.get("/api/v1/runs/material-two-test/csv").text
    assert "# material_id=M2" in csv and "material_id" in csv.splitlines()[-1]


def test_assignment_event_is_persisted_with_material_and_channel(client, start_run,
                                                                  push_events):
    run_id = "material-two-assigned-event"
    start_run(run_id, target_g=5000, material_id="M2", channel_id="CH2")
    response = client.post("/api/v1/events", json={
        "run_id": run_id, "material_id": "M2", "channel_id": "CH2",
        "events": [{"idx": 1, "event": "JOB_ASSIGNED", "elapsed_ms": 0,
                    "state": "QUEUED", "detail": "Pump 2 / Relay 2 / Scale 2"}],
    })
    assert response.status_code == 200, response.text
    event = client.get(f"/api/v1/runs/{run_id}/events").json()["events"][0]
    assert event["event"] == "JOB_ASSIGNED"
    assert event["material_id"] == "M2"


def test_run_channel_mismatch_is_rejected_and_csv_identifies_channel(client, start_run, push_samples):
    run_id = "run-on-channel-one"
    start_run(run_id, target_g=5000, channel_id="CH1")
    bad = client.post("/api/v1/telemetry/batch", json={
        "run_id": run_id, "material_id":"M2", "channel_id": "CH2", "samples": [{
            "idx": 1, "elapsed_ms": 100, "weight_g": 100, "target_g": 5000,
            "error_g": 4900, "channel_id": "CH2", "material_id":"M2",
        }],
    })
    assert bad.status_code == 409
    assert push_samples(run_id, [{"idx": 1, "elapsed_ms": 100,
        "weight_g": 100, "target_g": 5000, "error_g": 4900,
        "channel_id": "CH1", "material_id":"M1"}]).status_code == 200
    csv = client.get(f"/api/v1/runs/{run_id}/csv")
    assert "CH1" in csv.text
    assert client.post("/api/v1/runs/start", json={
        "run_id":"invalid-material-channel", "material_id":"M1", "channel_id":"CH2",
        "target_g":5000,
    }).status_code == 422
    assert client.post("/api/v1/runs/start", json={
        "run_id":"invalid-material-pump", "material_id":"M1", "channel_id":"CH1",
        "pump_id":"Pump 2", "target_g":5000,
    }).status_code == 422

