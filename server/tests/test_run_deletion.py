"""Deleting one stored run removes that run and its own telemetry, nothing else.

The History page offers a per-run Delete action. The authoritative identity is
``dispense_runs.run_id`` — the primary key — never the displayed ``test_number``,
which is a per-target counter and is not unique across materials or targets.

Deletion is scoped hard: only ``weight_samples`` and ``dispense_events`` rows
that carry this exact ``run_id`` go with it. Tuning profiles, materials, device
configuration and every other run stay put, so a 5 kg Material A deletion can
never disturb a 10 kg or Material B record.
"""

from sqlalchemy import func, select

from app.database import get_engine
from app.models import DispenseEvent, DispenseRun, WeightSample


def _sample(idx: int, weight_g: int, channel_id: str = "CH1",
            material_id: str = "M1") -> dict:
    return {"idx": idx, "elapsed_ms": idx * 100, "weight_g": weight_g,
            "target_g": 5000, "error_g": 5000 - weight_g, "seq": idx,
            "stable": False, "relay1": True, "relay2": False,
            "state": "COARSE_DISPENSE",
            "channel_id": channel_id, "material_id": material_id}


def _event(idx: int) -> dict:
    return {"idx": idx, "event": "COARSE_STARTED", "elapsed_ms": idx * 100,
            "state": "COARSE_DISPENSE", "weight_g": 120, "detail": None}


def _seed(client, run_id: str, push_samples, samples: int = 3,
          events: int = 2, channel_id: str = "CH1", material_id: str = "M1") -> None:
    # idx is 1-based: the contract rejects idx < 1.
    if samples:
        response = push_samples(run_id, [
            _sample(i, 100 + i * 50, channel_id, material_id)
            for i in range(1, samples + 1)])
        assert response.status_code == 200, response.text
    if events:
        # Posted directly: the push_events fixture pins channel_id=CH1, and a
        # Material B run must carry CH2/M2 events to pass the channel guard.
        response = client.post("/api/v1/events", json={
            "run_id": run_id, "material_id": material_id, "channel_id": channel_id,
            "device_id": "device-test-1",
            "events": [_event(i) for i in range(1, events + 1)],
        })
        assert response.status_code == 200, response.text


def _count(model, run_id: str | None = None) -> int:
    stmt = select(func.count()).select_from(model)
    if run_id is not None:
        stmt = stmt.where(model.run_id == run_id)
    with get_engine().connect() as conn:
        return conn.execute(stmt).scalar() or 0


def test_delete_unknown_run_returns_404(client):
    response = client.delete("/api/v1/runs/no-such-run-at-all")

    assert response.status_code == 404
    assert "no-such-run-at-all" in response.text


def test_delete_run_removes_the_run_and_its_child_telemetry(
        client, start_run, push_samples):
    run_id = "bits-del-basic-j7-1834-0001"
    start_run(run_id, target_g=5000)
    _seed(client, run_id, push_samples)

    assert _count(DispenseRun, run_id) == 1
    assert _count(WeightSample, run_id) == 3
    assert _count(DispenseEvent, run_id) == 2

    response = client.delete(f"/api/v1/runs/{run_id}")

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["ok"] is True
    assert body["run_id"] == run_id
    assert body["deleted"]["weight_samples"] == 3
    assert body["deleted"]["dispense_events"] == 2

    assert _count(DispenseRun, run_id) == 0
    assert _count(WeightSample, run_id) == 0
    assert _count(DispenseEvent, run_id) == 0


def test_delete_is_addressed_by_run_id_not_by_displayed_test_number(
        client, start_run):
    """Two runs can share a test_number (different target, different material)."""
    five = "bits-del-5kg-aaaa-j7-1834-0005"
    ten = "bits-del-10kg-bbbb-j7-1834-0005"
    start_run(five, target_g=5000)
    start_run(ten, target_g=10000, job_id=8)

    # Deleting by the primary key removes exactly that row, even though both
    # carry the same displayed test_number.
    response = client.delete(f"/api/v1/runs/{five}")
    assert response.status_code == 200, response.text

    assert client.get(f"/api/v1/runs/{five}").status_code == 404
    assert client.get(f"/api/v1/runs/{ten}").status_code == 200


def test_delete_one_target_leaves_other_targets_and_materials_untouched(
        client, start_run, push_samples):
    doomed = "bits-del-doomed-aaaa-j7-1834-0010"
    five_other = "bits-del-5kg-other-bbbb-j7-1834-0011"
    ten = "bits-del-10kg-keep-cccc-j7-1834-0012"
    twenty = "bits-del-20kg-keep-dddd-j7-1834-0013"
    material_b = "bits-del-m2-keep-eeee-j7-1834-0014"

    start_run(doomed, target_g=5000)
    start_run(five_other, target_g=5000, job_id=8)
    start_run(ten, target_g=10000, job_id=9)
    start_run(twenty, target_g=20000, job_id=10)
    start_run(material_b, target_g=5000, job_id=11, material_id="M2", channel_id="CH2")

    _seed(client, doomed, push_samples)
    _seed(client, material_b, push_samples, channel_id="CH2", material_id="M2")

    response = client.delete(f"/api/v1/runs/{doomed}")
    assert response.status_code == 200, response.text

    assert client.get(f"/api/v1/runs/{doomed}").status_code == 404
    for survivor in (five_other, ten, twenty, material_b):
        assert client.get(f"/api/v1/runs/{survivor}").status_code == 200, survivor

    # Material B's own telemetry is still there.
    material_b_samples = client.get(f"/api/v1/runs/{material_b}/samples").json()
    assert material_b_samples["count"] == 3

    # Target separation is preserved in the search index too.
    five_kg = client.get("/api/v1/runs", params={"target_g": 5000}).json()
    assert five_kg["total"] == 2
    assert {item["run_id"] for item in five_kg["items"]} == {five_other, material_b}

    ten_kg = client.get("/api/v1/runs", params={"target_g": 10000}).json()
    assert ten_kg["total"] == 1 and ten_kg["items"][0]["run_id"] == ten


def test_delete_leaves_tuning_profiles_and_materials_untouched(client, start_run):
    created = client.post("/api/v1/profiles", json={
        "profile_id": "m1-keep", "material_id": "M1", "channel_id": "CH1",
        "target_g": 5000, "kp": 0.0025, "ki": 0.0003, "kd": 0.0001,
        "tolerance_g": 20, "max_overshoot_g": 100, "max_duration_ms": 120000,
        "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40,
    })
    assert created.status_code == 200, created.text

    run_id = "bits-del-profile-safe-j7-1834-0020"
    start_run(run_id, target_g=5000)

    response = client.delete(f"/api/v1/runs/{run_id}")
    assert response.status_code == 200, response.text

    profiles = client.get("/api/v1/profiles").json()
    assert [p["profile_id"] for p in profiles] == ["m1-keep"]
    materials = client.get("/api/v1/materials").json()
    assert [m["material_id"] for m in materials] == ["M1", "M2"]


def test_deleting_already_deleted_run_returns_404(client, start_run):
    run_id = "bits-del-twice-j7-1834-0030"
    start_run(run_id, target_g=5000)

    assert client.delete(f"/api/v1/runs/{run_id}").status_code == 200
    assert client.delete(f"/api/v1/runs/{run_id}").status_code == 404


def test_delete_never_leaves_orphaned_child_rows(
        client, start_run, push_samples):
    """Children go in the same transaction as their parent run."""
    run_id = "bits-del-orphan-j7-1834-0040"
    start_run(run_id, target_g=15000)
    _seed(client, run_id, push_samples)

    assert client.delete(f"/api/v1/runs/{run_id}").status_code == 200

    assert _count(WeightSample, run_id) == 0
    assert _count(DispenseEvent, run_id) == 0


def test_delete_only_touches_rows_carried_by_that_run_id(
        client, start_run, push_samples):
    """A child row for another run is never swept up by a delete."""
    keep = "bits-del-keep-aaaa-j7-1834-0050"
    drop = "bits-del-drop-bbbb-j7-1834-0051"
    start_run(keep, target_g=5000)
    start_run(drop, target_g=5000, job_id=8)
    _seed(client, keep, push_samples)
    _seed(client, drop, push_samples)

    assert client.delete(f"/api/v1/runs/{drop}").status_code == 200

    assert _count(WeightSample, keep) == 3
    assert _count(DispenseEvent, keep) == 2
    assert _count(WeightSample, drop) == 0
