"""Operator queue, device command delivery and shared page smoke tests.

Every POST /queue/jobs now carries a pinned PID profile version â€” see
tests/test_profile_pinning.py for the pin contract itself. These tests keep
their original subject (claiming, ordering, hold/release/promote, delivery
priority) and just seed the profiles they need to get a job queued at all.
"""

PROFILE_BODY = {
    "kp": 0.0025, "ki": 0.0003, "kd": 0.0001,
    "tolerance_g": 20, "max_overshoot_g": 100, "max_duration_ms": 120000,
    "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40,
}

# Pin address -> profile_id. Mirrors the fixed material/channel map.
PINS = {
    ("M1", 5000): "tune-m1-5k",
    ("M1", 10000): "tune-m1-10k",
    ("M2", 5000): "tune-m2-5k",
    ("M2", 10000): "tune-m2-10k",
}


def _seed_profiles(client, *addresses):
    for material_id, target_g in addresses:
        body = dict(PROFILE_BODY,
                    profile_id=PINS[(material_id, target_g)],
                    material_id=material_id,
                    channel_id="CH1" if material_id == "M1" else "CH2",
                    target_g=target_g)
        response = client.post("/api/v1/profiles", json=body)
        assert response.status_code == 200, response.text
        # A job can only pin an ACTIVE profile.
        activated = client.post(
            f"/api/v1/profile-versions/{response.json()['profile_version_id']}/activate")
        assert activated.status_code == 200, activated.text


def _job(material_id="M1", target_g=5000, priority=0):
    return {"material_id": material_id, "target_g": target_g, "priority": priority,
            "profile_id": PINS[(material_id, target_g)], "profile_version": 1}


def test_global_queue_commands_are_claimed_and_reconciled(client):
    _seed_profiles(client, ("M1", 5000), ("M2", 10000))

    # No device has reported yet: the handler refuses to invent one.
    unavailable = client.post("/api/v1/queue/jobs", json=_job())
    assert unavailable.status_code == 503

    status = client.post("/api/v1/device/status", json={
        "device_id": "relay-a", "status": {
            "channels": [
                {"channel_id": "CH1", "state": "IDLE", "weight_valid": True},
                {"channel_id": "CH2", "state": "IDLE", "weight_valid": True},
            ], "queue": [],
        },
    })
    assert status.status_code == 200
    normal = client.post("/api/v1/queue/jobs", json=_job("M1", 5000, 0)).json()
    priority = client.post("/api/v1/queue/jobs", json=_job("M2", 10000, 1)).json()
    queue = client.get("/api/v1/queue").json()
    assert [item["command_id"] for item in queue["waiting_commands"]] == [
        priority["command_id"], normal["command_id"]
    ]
    assert priority["material_id"] == "M2" and normal["material_id"] == "M1"

    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-a"})
    assert delivered.status_code == 200
    assert [item["command_type"] for item in delivered.json()] == ["JOB", "JOB"]
    first = delivered.json()[0]
    ack = client.post(f"/api/v1/device/commands/{first['command_id']}/ack", json={
        "state": "QUEUED", "local_job_id": 37,
    })
    assert ack.status_code == 200

    client.post("/api/v1/device/status", json={
        "device_id": "relay-a", "status": {
            "channels": [{"channel_id": "CH1", "active_job_id": 0}],
            "queue": [{"id": 37, "channel_id": 0, "state": "QUEUED", "target_g": 10000}],
        },
    })
    queue = client.get("/api/v1/queue").json()
    linked = next(x for x in queue["waiting_commands"] if x["command_id"] == first["command_id"])
    assert linked["state"] == "QUEUED"
    assert linked["local_job_id"] == 37


def test_server_queue_validates_job_and_control_payloads(client):
    _seed_profiles(client, ("M1", 5000))
    assert client.post("/api/v1/queue/jobs",
                       json={"material_id": "M1", "target_g": 0}).status_code == 422
    assert client.post("/api/v1/queue/control", json={"action": "PAUSE"}).status_code == 422
    assert client.get("/api/v1/device/status").json() == []


def test_material_mapping_is_fixed_and_only_labels_are_editable(client):
    _seed_profiles(client, ("M1", 5000))
    materials = client.get("/api/v1/materials").json()
    assert [(m["material_id"], m["channel_id"], m["pump_id"], m["relay_id"], m["scale_id"])
            for m in materials] == [
        ("M1", "CH1", "Pump 1", "Relay 1", "Scale 1"),
        ("M2", "CH2", "Pump 2", "Relay 2", "Scale 2"),
    ]
    assert client.patch("/api/v1/materials/M1", json={"name": "Resin"}).status_code == 200
    assert client.get("/api/v1/materials").json()[0]["name"] == "Resin"
    assert client.patch("/api/v1/materials/M1", json={"name": "X", "channel_id": "CH2"}).status_code == 422
    assert client.post("/api/v1/queue/jobs",
                       json={"material_id": "MX", "target_g": 5000}).status_code == 422
    assert client.post("/api/v1/queue/jobs", json=dict(_job(), channel_id="CH2")).status_code == 422
    invalid_status = client.post("/api/v1/device/status", json={"device_id": "bad-map", "status": {
        "channels": [{"channel_id": "CH1", "material_id": "M2", "pump_id": "Pump 2"}], "queue": []}})
    assert invalid_status.status_code == 422


def test_cancel_racing_a_delivered_job_targets_remote_command_id(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-cancel", "status": {
        "channels": [], "queue": [],
    }})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-cancel"}).json()
    assert delivered[0]["command_id"] == created["command_id"]
    cancelled = client.post(f"/api/v1/queue/jobs/{created['command_id']}/cancel")
    assert cancelled.status_code == 200 and cancelled.json()["state"] == "CANCELLING"
    commands = client.get("/api/v1/device/commands", params={"device_id": "relay-cancel"}).json()
    cancel_command = next(command for command in commands if command["command_type"] == "CANCEL")
    assert cancel_command["remote_command_id"] == created["command_id"]


def test_control_path_cancel_marks_only_the_named_job(client):
    """CANCEL via /queue/control must drop the row out of the waiting list now.

    It used to leave the JOB row in PENDING/QUEUED until the device reconciled
    it, so the UI kept showing a job the operator had already cancelled.
    Sibling rows â€” including ones with the same target_g â€” stay waiting.
    """
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-cctl", "status": {
        "channels": [], "queue": [],
    }})
    first = client.post("/api/v1/queue/jobs", json=_job()).json()
    second = client.post("/api/v1/queue/jobs", json=_job()).json()
    client.get("/api/v1/device/commands", params={"device_id": "relay-cctl"})
    client.post(f"/api/v1/device/commands/{first['command_id']}/ack",
                json={"state": "QUEUED", "local_job_id": 37})

    cancelled = client.post("/api/v1/queue/control",
                            json={"action": "CANCEL", "local_job_id": 37})
    assert cancelled.status_code == 200, cancelled.text

    queue = client.get("/api/v1/queue").json()["waiting_commands"]
    assert [row["command_id"] for row in queue] == [second["command_id"]]


def test_queue_control_accepts_hold_release_promote_and_requires_a_target(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-q", "status": {
        "channels": [], "queue": [],
    }})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    for action in ("HOLD", "RELEASE", "PROMOTE"):
        accepted = client.post("/api/v1/queue/control",
                               json={"action": action, "command_id": created["command_id"]})
        assert accepted.status_code == 200, accepted.text
        assert client.post("/api/v1/queue/control", json={"action": action}).status_code == 422


def test_hold_on_a_pending_job_keeps_it_out_of_device_delivery(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-hold", "status": {
        "channels": [], "queue": [],
    }})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    held = client.post("/api/v1/queue/control", json={"action": "HOLD",
                                                      "command_id": created["command_id"]})
    assert held.status_code == 200, held.text

    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-hold"}).json()
    assert [item["command_type"] for item in delivered] == []

    queue = client.get("/api/v1/queue").json()
    row = next(x for x in queue["waiting_commands"] if x["command_id"] == created["command_id"])
    assert row["held"] is True


def test_release_returns_a_held_pending_job_to_delivery(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-rel", "status": {
        "channels": [], "queue": [],
    }})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": created["command_id"]})
    released = client.post("/api/v1/queue/control", json={"action": "RELEASE",
                                                          "command_id": created["command_id"]})
    assert released.status_code == 200, released.text

    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-rel"}).json()
    assert [item["command_type"] for item in delivered] == ["JOB"]
    assert client.get("/api/v1/queue").json()["waiting_commands"][0]["held"] is False


def test_promote_delivers_a_pending_job_ahead_of_its_peers(client):
    _seed_profiles(client, ("M1", 5000), ("M2", 10000))
    client.post("/api/v1/device/status", json={"device_id": "relay-promo", "status": {
        "channels": [], "queue": [],
    }})
    first = client.post("/api/v1/queue/jobs", json=_job("M1", 5000)).json()
    second = client.post("/api/v1/queue/jobs", json=_job("M2", 10000)).json()
    promoted = client.post("/api/v1/queue/control", json={"action": "PROMOTE",
                                                          "command_id": second["command_id"]})
    assert promoted.status_code == 200, promoted.text

    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-promo"}).json()
    assert [item["command_id"] for item in delivered] == [
        second["command_id"], first["command_id"]
    ]
    queue = client.get("/api/v1/queue").json()["waiting_commands"]
    assert queue[0]["command_id"] == second["command_id"]
    assert queue[0]["promoted"] is True


def test_hold_on_a_device_owned_job_forwards_a_hold_command(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-dev", "status": {
        "channels": [], "queue": [],
    }})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    client.get("/api/v1/device/commands", params={"device_id": "relay-dev"})
    client.post(f"/api/v1/device/commands/{created['command_id']}/ack",
                json={"state": "QUEUED", "local_job_id": 37})

    held = client.post("/api/v1/queue/control", json={"action": "HOLD", "local_job_id": 37})
    assert held.status_code == 200, held.text

    forwarded = client.get("/api/v1/device/commands", params={"device_id": "relay-dev"}).json()
    hold = next(item for item in forwarded if item["command_type"] == "HOLD")
    assert hold["local_job_id"] == 37


def test_queue_reports_held_and_promoted_for_waiting_rows(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-flag", "status": {
        "channels": [], "queue": [],
    }})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    queue = client.get("/api/v1/queue").json()["waiting_commands"]
    assert queue[0]["held"] is False and queue[0]["promoted"] is False

    client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": created["command_id"]})
    client.post("/api/v1/queue/control", json={"action": "PROMOTE", "command_id": created["command_id"]})
    queue = client.get("/api/v1/queue").json()["waiting_commands"]
    assert queue[0]["held"] is True and queue[0]["promoted"] is True


def test_a_hold_command_is_delivered_before_the_job_backlog(client):
    """A control command must not sit behind a full poll window of JOBs.

    /device/commands hands out at most 10 rows per poll. If HOLD sorts purely
    by age it lands last, so the job it is meant to park can start first.
    """
    _seed_profiles(client, ("M1", 5000), ("M2", 10000))
    client.post("/api/v1/device/status", json={"device_id": "relay-starve", "status": {
        "channels": [], "queue": [],
    }})
    # One job already lives on the device; ten more are still server-side.
    # Duplicate targets are independent rows â€” that is the point.
    victim = client.post("/api/v1/queue/jobs", json=_job("M2", 10000)).json()
    client.get("/api/v1/device/commands", params={"device_id": "relay-starve"})
    client.post(f"/api/v1/device/commands/{victim['command_id']}/ack",
                json={"state": "QUEUED", "local_job_id": 37})
    for _ in range(10):
        client.post("/api/v1/queue/jobs", json=_job("M1", 5000))

    held = client.post("/api/v1/queue/control", json={"action": "HOLD", "local_job_id": 37})
    assert held.status_code == 200, held.text

    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-starve"}).json()
    assert len(delivered) == 10
    assert delivered[0]["command_type"] == "HOLD"
    assert delivered[0]["local_job_id"] == 37


def test_three_operational_pages_render_shared_navigation(client):
    for path, title in (("/queue", "Global dispensing queue"),
                        ("/tuning", "PID tuning profiles"),
                        ("/graphs", "Graphs / Test History")):
        response = client.get(path)
        assert response.status_code == 200
        assert title in response.text
        assert 'href="/queue"' in response.text
        assert 'href="/tuning"' in response.text
        assert 'href="/graphs"' in response.text
        if path == "/graphs":
            assert 'id="compare-runs"' in response.text
            assert 'id="comparison-chart"' in response.text


def _held_job(client, device="relay-reid"):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": device, "status": {"channels": [], "queue": []}})
    created = client.post("/api/v1/queue/jobs", json=_job()).json()
    client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": created["command_id"]})
    return created["command_id"]


def test_release_of_parked_job_reissues_under_fresh_monotonic_id(client):
    old = _held_job(client)
    for _ in range(3):  # newer commands advance the device ledger floor
        client.post("/api/v1/queue/control", json={"action": "CLEAR"})
    released = client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": old})
    assert released.status_code == 200, released.text
    new = released.json()["command_id"]
    assert new > old and released.json()["held"] is False

    delivered = client.get("/api/v1/device/commands", params={"device_id": "relay-reid"}).json()
    ids = [d["command_id"] for d in delivered if d["command_type"] == "JOB"]
    assert ids == [new]  # old id is never served again

    waiting = client.get("/api/v1/queue").json()["waiting_commands"]
    assert [w["command_id"] for w in waiting] == [new]  # no duplicate in the queue


def test_old_id_still_resolves_after_release_for_promote_and_cancel(client):
    old = _held_job(client, "relay-reid2")
    client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": old})
    promoted = client.post("/api/v1/queue/control", json={"action": "PROMOTE", "command_id": old})
    assert promoted.status_code == 200, promoted.text
    assert promoted.json()["promoted"] is True and promoted.json()["command_id"] > old
    assert client.post(f"/api/v1/queue/jobs/{old}/cancel").status_code == 200
    assert client.get("/api/v1/queue").json()["waiting_commands"] == []


def test_stale_failed_job_is_surfaced_and_can_be_resubmitted(client):
    _seed_profiles(client, ("M1", 5000))
    client.post("/api/v1/device/status", json={"device_id": "relay-stale", "status": {"channels": [], "queue": []}})
    old = client.post("/api/v1/queue/jobs", json=_job()).json()["command_id"]
    client.get("/api/v1/device/commands", params={"device_id": "relay-stale"})
    client.post(f"/api/v1/device/commands/{old}/ack", json={
        "state": "FAILED", "error": "stale: older than ledger floor"})

    failed = client.get("/api/v1/queue").json()["failed_commands"]
    assert [f["command_id"] for f in failed] == [old]
    assert "stale" in failed[0]["error"]

    again = client.post(f"/api/v1/queue/jobs/{old}/resubmit")
    assert again.status_code == 200, again.text
    new = again.json()["command_id"]
    assert new > old
    queue = client.get("/api/v1/queue").json()
    assert queue["failed_commands"] == []
    assert [w["command_id"] for w in queue["waiting_commands"]] == [new]
    assert client.post(f"/api/v1/queue/jobs/{old}/resubmit").status_code == 409
