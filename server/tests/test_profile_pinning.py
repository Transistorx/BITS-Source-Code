"""Every queued job pins the exact PID profile version it will run with.

The tuning table is already immutable-INSERT (composite PK
``(profile_id, version)``, ``create_profile`` always writes ``max+1``). What was
missing is the link from a queued job to the version the operator chose: a job
recorded only a target, and the device resolved "the active profile" at start
time, so a version saved between enqueue and start silently changed what ran.

These tests pin the opposite contract: a job cannot be queued without a pin, the
pin is validated server-side against the profile's own material/target and the
firmware's safety bounds, the pin travels to the device, and an edit produces a
new immutable version that leaves old jobs and runs untouched.
"""

from app.schemas import CANONICAL_TARGETS_G

PROFILE_BODY = {
    "profile_id": "tune-a",
    "material_id": "M1",
    "channel_id": "CH1",
    "target_g": 5000,
    "kp": 0.0025, "ki": 0.0003, "kd": 0.0001,
    "tolerance_g": 20, "max_overshoot_g": 100, "max_duration_ms": 120000,
    "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40,
}


def _seed_device(client, device_id="relay-pin"):
    response = client.post("/api/v1/device/status", json={
        "device_id": device_id,
        "status": {"channels": [], "queue": []},
    })
    assert response.status_code == 200, response.text
    return device_id


def _create_profile(client, activate=True, **overrides):
    """Save a profile (always a draft) and, by default, activate it: a job may only
    pin an ACTIVE profile."""
    body = dict(PROFILE_BODY, **overrides)
    response = client.post("/api/v1/profiles", json=body)
    assert response.status_code == 200, response.text
    created = response.json()
    assert created["status"] == "draft" and created["active"] is False
    if activate:
        done = client.post(f"/api/v1/profile-versions/{created['profile_version_id']}/activate")
        assert done.status_code == 200, done.text
        created = dict(created, active=True, status="active")
    return created


def _queue_job(client, **overrides):
    body = {
        "material_id": "M1",
        "target_g": 5000,
        "priority": 0,
        "profile_id": "tune-a",
        "profile_version": 1,
    }
    body.update(overrides)
    return client.post("/api/v1/queue/jobs", json=body)


# --------------------------------------------------------------- required ----


def test_queue_job_without_pin_needs_a_compatible_active_profile(client):
    """No pin is allowed, but then the server must find exactly one active profile;
    it never invents one. A half pin (id without version) is still refused."""
    _seed_device(client)

    missing = client.post("/api/v1/queue/jobs",
                          json={"material_id": "M1", "target_g": 5000, "priority": 0})
    assert missing.status_code == 422
    assert missing.json()["detail"]["code"] == "NO_COMPATIBLE_PROFILE"

    no_version = client.post("/api/v1/queue/jobs", json={
        "material_id": "M1", "target_g": 5000, "priority": 0, "profile_id": "tune-a",
    })
    assert no_version.status_code == 422


def test_queue_job_rejects_a_profile_version_that_does_not_exist(client):
    _seed_device(client)
    _create_profile(client)  # v1 only

    for payload in ({"profile_version": 2}, {"profile_id": "absent"}):
        refused = _queue_job(client, **payload)
        assert refused.status_code == 422
        assert "no PID profile" in refused.text, refused.text


def test_queue_job_rejects_a_profile_pinned_to_the_wrong_target(client):
    """Compatibility check: the pin's declared range must contain the target."""
    _seed_device(client)
    _create_profile(client)  # 5000 g

    refused = _queue_job(client, target_g=10000)
    assert refused.status_code == 422
    assert refused.json()["detail"]["code"] == "TARGET_OUT_OF_PROFILE_RANGE", refused.text
    assert "covers 5000..5000 g, not 10000 g" in refused.text, refused.text


def test_queue_job_rejects_a_profile_pinned_to_the_wrong_material(client):
    _seed_device(client)
    _create_profile(client)  # M1 / CH1

    refused = _queue_job(client, material_id="M2", target_g=5000)
    assert refused.status_code == 422
    assert "is for M1, not M2" in refused.text, refused.text


def test_queue_job_accepts_non_canonical_target_but_never_borrows_a_canonical_profile(client):
    """Any weight 1..20000 may be queued, but a 5000 g profile never serves 12000 g
    (no nearest-canonical fallback), with or without a pin."""
    _seed_device(client)
    _create_profile(client)

    assert 12000 not in CANONICAL_TARGETS_G
    pinned = _queue_job(client, target_g=12000)
    assert pinned.status_code == 422
    assert pinned.json()["detail"]["code"] == "TARGET_OUT_OF_PROFILE_RANGE"
    unpinned = client.post("/api/v1/queue/jobs", json={"material_id": "M1", "target_g": 12000})
    assert unpinned.status_code == 422
    assert unpinned.json()["detail"]["code"] == "NO_COMPATIBLE_PROFILE"
    assert unpinned.json()["detail"]["nearest"][0]["target_min_g"] == 5000
    for bad in (0, 20001):
        assert client.post("/api/v1/queue/jobs", json={
            "material_id": "M1", "target_g": bad}).status_code == 422


def test_queue_job_refuses_a_profile_whose_own_bounds_are_unsafe(client):
    """Re-validate at queue time, not only at profile-create time.

    A row that somehow violates the firmware clamps (gain outside [0,1],
    minimum relay time longer than the control window) must never be sent to
    the device as a job's pin.
    """
    from app import database, models

    _seed_device(client)
    created = _create_profile(client)
    engine = database.get_engine()
    with engine.begin() as conn:
        conn.execute(
            models.TuningProfile.__table__.update()
            .where(models.TuningProfile.profile_id == created["profile_id"])
            .values(kp=9.0, min_on_ms=5000, window_ms=500)
        )

    refused = _queue_job(client)
    assert refused.status_code == 422
    assert "unsafe" in refused.text, refused.text


# ----------------------------------------------------------------- storage ----


def test_queue_job_stores_and_returns_the_pinned_profile_version(client):
    _seed_device(client)
    _create_profile(client)
    _create_profile(client, version=None, profile_id="tune-a")  # becomes v2

    created = _queue_job(client, profile_id="tune-a", profile_version=2)
    assert created.status_code == 200, created.text
    body = created.json()
    assert body["profile_id"] == "tune-a"
    assert body["profile_version"] == 2

    queue = client.get("/api/v1/queue").json()
    row = queue["waiting_commands"][0]
    assert row["profile_id"] == "tune-a"
    assert row["profile_version"] == 2


def test_device_command_delivery_carries_the_pinned_profile_version(client):
    """The pin must reach the ESP32, not just the operator UI."""
    device_id = _seed_device(client)
    _create_profile(client)
    created = _queue_job(client).json()

    delivered = client.get("/api/v1/device/commands", params={"device_id": device_id}).json()
    job = next(item for item in delivered if item["command_id"] == created["command_id"])
    assert job["profile_id"] == "tune-a"
    assert job["profile_version"] == 1

    # The gains travel with the job too. The firmware must never substitute
    # "whatever is active on the channel" for the version the operator pinned.
    assert job["profile"]["kp"] == 0.0025
    assert job["profile"]["tolerance_g"] == 20
    assert job["profile"]["min_on_ms"] == 40


def test_cancel_one_job_leaves_its_duplicate_target_sibling(client):
    """Cancellation is addressed by command id, never by target weight.

    Two jobs with the same target_g are independent rows; cancelling one must
    leave the other queued.
    """
    _seed_device(client)
    _create_profile(client)
    first = _queue_job(client).json()
    second = _queue_job(client).json()
    assert first["command_id"] != second["command_id"]

    cancelled = client.post(f"/api/v1/queue/jobs/{first['command_id']}/cancel")
    assert cancelled.status_code == 200

    queue = client.get("/api/v1/queue").json()
    remaining = [row["command_id"] for row in queue["waiting_commands"]]
    assert remaining == [second["command_id"]]


# --------------------------------------------------------------- versions ----


def test_profiles_can_be_filtered_by_target_for_the_new_job_dropdown(client):
    """The dropdown must offer only versions compatible with the chosen target."""
    _seed_device(client)
    _create_profile(client, profile_id="tune-5k", target_g=5000)
    _create_profile(client, profile_id="tune-10k", target_g=10000,
                    material_id="M1", channel_id="CH1")
    _create_profile(client, profile_id="tune-5k-m2", target_g=5000,
                    material_id="M2", channel_id="CH2")

    for_5k_m1 = client.get("/api/v1/profiles",
                           params={"material_id": "M1", "target_g": 5000}).json()
    assert {(row["profile_id"], row["target_g"]) for row in for_5k_m1} == {("tune-5k", 5000)}

    for_10k_m1 = client.get("/api/v1/profiles",
                            params={"material_id": "M1", "target_g": 10000}).json()
    assert {(row["profile_id"], row["target_g"]) for row in for_10k_m1} == {("tune-10k", 10000)}


def test_editing_a_profile_creates_a_new_immutable_version(client):
    """Edit loads the values; saving writes v2 and never mutates v1.

    Historical jobs and runs must stay readable against the version they
    actually ran with.
    """
    _seed_device(client)
    v1 = _create_profile(client)

    edited = client.post("/api/v1/profiles", json=dict(PROFILE_BODY, kp=0.0040))
    assert edited.status_code == 200, edited.text
    v2 = edited.json()
    assert v2["profile_id"] == v1["profile_id"]
    assert v2["version"] == 2
    assert v2["kp"] == 0.0040

    # v1 is untouched: the same composite key still returns the old gains.
    history = client.get("/api/v1/profiles").json()
    original = next(row for row in history if row["version"] == 1)
    assert original["kp"] == 0.0025
    assert original["profile_id"] == v1["profile_id"]


def test_a_job_queued_against_an_older_version_stays_on_that_version(client):
    """Queuing against v1 must not follow a later v2 edit."""
    _seed_device(client)
    _create_profile(client)                              # v1, active
    job = _queue_job(client, profile_version=1).json()
    assert job["profile_version"] == 1
    _create_profile(client, kp=0.0040)                   # v2 supersedes v1
    _create_profile(client, kp=0.0099)                   # v3 supersedes v2

    # An unpinned job now follows the single active profile (v3); v1 is no longer active.
    assert _queue_job(client, profile_version=1).status_code == 422

    queue = client.get("/api/v1/queue").json()
    assert queue["waiting_commands"][0]["profile_version"] == 1


# ---------------------------------------------------------------- history ----


def test_run_history_records_the_pinned_profile_version(client):
    _seed_device(client)
    _create_profile(client)

    started = client.post("/api/v1/runs/start", json={
        "run_id": "pin-run-1",
        "material_id": "M1",
        "device_id": "relay-pin",
        "job_id": 3,
        "target_g": 5000,
        "profile_id": "tune-a",
        "profile_version": 1,
    })
    assert started.status_code == 200, started.text

    detail = client.get("/api/v1/runs/pin-run-1").json()
    assert detail["profile_id"] == "tune-a"
    assert detail["profile_version"] == 1


def test_editing_a_profile_leaves_historical_runs_on_their_own_version(client):
    """The point of immutability: history does not follow a later edit."""
    _seed_device(client)
    _create_profile(client)
    client.post("/api/v1/runs/start", json={
        "run_id": "pin-run-old",
        "material_id": "M1",
        "device_id": "relay-pin",
        "job_id": 4,
        "target_g": 5000,
        "profile_id": "tune-a",
        "profile_version": 1,
    })
    _create_profile(client, kp=0.0040)  # v2

    detail = client.get("/api/v1/runs/pin-run-old").json()
    assert detail["profile_version"] == 1
