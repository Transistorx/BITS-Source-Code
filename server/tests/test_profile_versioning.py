"""Per-material PID profile versioning (surrogate profile_version_id).

Contract: docs/telemetry/CONTRACT.md "PID profile versioning". Each
(profile_id, material_id) has its own version counter; profile_id + version is
no longer unique across pumps; history is linked by profile_version_id.
"""

import pytest
from sqlalchemy.orm import Session

from app import database, models
from app.services import profiles as profile_service
from test_profile_pinning import _seed_device
from test_tuning_profiles import profile_body

M = {"M1": "CH1", "M2": "CH2"}


def save(client, material="M1", profile_id="default", **overrides):
    response = client.post("/api/v1/profiles", json=profile_body(
        profile_id=profile_id, material_id=material, channel_id=M[material], **overrides))
    assert response.status_code == 200, response.text
    return response.json()


def activate(client, *rows):
    """Activate saved drafts (a job may only pin an ACTIVE profile)."""
    for row in rows:
        done = client.post(f"/api/v1/profile-versions/{row['profile_version_id']}/activate")
        assert done.status_code == 200, done.text


def bulk_body(profile_id="default", materials=("M1", "M2"), **overrides):
    body = profile_body(profile_id=profile_id, **overrides)
    del body["material_id"], body["channel_id"]
    body["materials"] = list(materials)
    return body


def save_both(client, **kwargs):
    response = client.post("/api/v1/profiles/bulk", json=bulk_body(**kwargs))
    assert response.status_code == 200, response.text
    return {row["material_id"]: row for row in response.json()["profiles"]}


def preview(client, profile_id="default", materials="M1,M2"):
    response = client.get("/api/v1/profiles/next-version",
                          params={"profile_id": profile_id, "materials": materials})
    assert response.status_code == 200, response.text
    return response.json()["next_version"]


def make_versions(client, material, upto, profile_id="default"):
    return [save(client, material, profile_id) for _ in range(upto)]


def versions_of(client, material, profile_id="default"):
    rows = client.get("/api/v1/profiles", params={"material_id": material}).json()
    return sorted(r["version"] for r in rows if r["profile_id"] == profile_id)


# ------------------------------------------------------------ counters ----


def test_same_profile_id_and_version_can_exist_on_both_pumps(client):
    a, b = save(client, "M1"), save(client, "M2")
    assert (a["version"], b["version"]) == (1, 1)
    assert a["profile_version_id"] != b["profile_version_id"]
    assert (a["material_id"], a["channel_id"], a["pump_id"]) == ("M1", "CH1", "Pump 1")
    assert (b["material_id"], b["channel_id"], b["pump_id"]) == ("M2", "CH2", "Pump 2")
    listed = client.get("/api/v1/profiles").json()
    assert {r["profile_version_id"] for r in listed} == {a["profile_version_id"], b["profile_version_id"]}


def test_counters_are_independent_per_material(client):
    make_versions(client, "M1", 3)
    assert save(client, "M2")["version"] == 1          # M1 saves did not advance M2
    assert save(client, "M2")["version"] == 2
    assert save(client, "M1")["version"] == 4          # M2 saves did not advance M1
    assert save(client, "M1", profile_id="other")["version"] == 1  # per profile_id too


def test_uneven_histories_stay_uneven_on_save_both(client):
    make_versions(client, "M1", 7)
    make_versions(client, "M2", 4)
    assert preview(client) == {"M1": 8, "M2": 5}
    rows = save_both(client)
    assert (rows["M1"]["version"], rows["M2"]["version"]) == (8, 5)
    assert versions_of(client, "M2") == [1, 2, 3, 4, 5]   # nothing skipped to synchronise


def test_even_histories_then_single_edit_diverges(client):
    make_versions(client, "M1", 7)
    make_versions(client, "M2", 7)
    rows = save_both(client)
    assert (rows["M1"]["version"], rows["M2"]["version"]) == (8, 8)
    assert save(client, "M1")["version"] == 9
    assert versions_of(client, "M2")[-1] == 8
    assert preview(client) == {"M1": 10, "M2": 9}


def test_save_both_returns_distinct_rows_with_ids(client):
    rows = save_both(client, kp=0.05)
    assert rows["M1"]["profile_version_id"] != rows["M2"]["profile_version_id"]
    assert rows["M1"]["channel_id"] == "CH1" and rows["M2"]["channel_id"] == "CH2"
    assert rows["M1"]["kp"] == rows["M2"]["kp"] == 0.05
    assert rows["M1"]["active"] is False and rows["M2"]["active"] is False


def test_save_both_can_target_a_single_pump_and_validates_materials(client):
    only = save_both(client, materials=("M2",))
    assert list(only) == ["M2"]
    for bad in (["M3"], [], ["M1", "M1"]):
        response = client.post("/api/v1/profiles/bulk", json=bulk_body(materials=bad))
        assert response.status_code == 422, (bad, response.text)
    bad_gains = client.post("/api/v1/profiles/bulk", json=bulk_body(min_on_ms=900, window_ms=500))
    assert bad_gains.status_code == 422


def test_next_version_validates_input(client):
    assert client.get("/api/v1/profiles/next-version",
                      params={"profile_id": "x", "materials": "M9"}).status_code == 422
    assert client.get("/api/v1/profiles/next-version",
                      params={"profile_id": "a b"}).status_code == 422
    assert preview(client, "never-saved", "M2") == {"M2": 1}


@pytest.mark.parametrize("m1_n,m2_n", [(0, 0), (3, 0), (0, 5), (4, 4), (2, 6)])
def test_preview_equals_what_the_server_creates(client, m1_n, m2_n):
    pid = f"prop-{m1_n}-{m2_n}"
    make_versions(client, "M1", m1_n, pid)
    make_versions(client, "M2", m2_n, pid)
    predicted_both = preview(client, pid)
    predicted_m2 = preview(client, pid, "M2")
    assert predicted_m2 == {"M2": predicted_both["M2"]}
    created = save_both(client, profile_id=pid)
    assert {m: r["version"] for m, r in created.items()} == predicted_both
    nxt = preview(client, pid, "M1")
    assert save(client, "M1", pid)["version"] == nxt["M1"]


def test_preview_and_server_use_the_same_service_query(client):
    save(client, "M1")
    with Session(database.get_engine()) as db:
        assert profile_service.next_versions(db, "default", ["M1", "M2"]) == {"M1": 2, "M2": 1}


# ------------------------------------------------- atomicity / collisions ----


def _stale_once(monkeypatch, stale, times=1):
    real = profile_service._max_versions
    calls = {"n": 0}

    def fake(db, profile_id, materials, lock=False):
        calls["n"] += 1
        if calls["n"] <= times:
            return {m: stale.get(m, 0) for m in materials}
        return real(db, profile_id, materials, lock)

    monkeypatch.setattr(profile_service, "_max_versions", fake)
    return calls


def test_save_both_unique_collision_retries_and_never_half_saves(client, monkeypatch):
    save(client, "M2")                                  # M2 v1 already exists
    calls = _stale_once(monkeypatch, {})                # first attempt computes v1/v1
    rows = save_both(client)
    assert calls["n"] == 2                              # collided once, retried
    assert (rows["M1"]["version"], rows["M2"]["version"]) == (1, 2)
    assert versions_of(client, "M1") == [1] and versions_of(client, "M2") == [1, 2]


def test_save_both_is_atomic_when_the_second_insert_keeps_failing(client, monkeypatch):
    save(client, "M2")                                  # makes M2 v1 collide every time
    _stale_once(monkeypatch, {}, times=99)
    response = client.post("/api/v1/profiles/bulk", json=bulk_body())
    assert response.status_code == 409, response.text   # not a 500
    assert "nothing was saved" in response.text
    monkeypatch.undo()
    # The M1 row that was valid on its own must not have been persisted.
    assert versions_of(client, "M1") == []
    assert versions_of(client, "M2") == [1]


def test_single_save_collision_returns_409_not_500(client, monkeypatch):
    save(client, "M1")
    _stale_once(monkeypatch, {}, times=99)
    response = client.post("/api/v1/profiles", json=profile_body(profile_id="default"))
    assert response.status_code == 409, response.text


def test_unique_key_is_enforced_by_the_database(client):
    from sqlalchemy.exc import IntegrityError
    save(client, "M1")
    with Session(database.get_engine()) as db:
        db.add(models.TuningProfile(
            profile_id="default", version=1, material_id="M1", channel_id="CH1",
            target_g=5000, kp=0.01, ki=0.0, kd=0.0, tolerance_g=20, max_overshoot_g=100,
            max_duration_ms=120000, window_ms=500, min_on_ms=40, min_off_ms=40))
        with pytest.raises(IntegrityError):
            db.commit()


# ------------------------------------------------ immutable history ----


def _job(material="M1", target=5000, **extra):
    return dict({"material_id": material, "target_g": target, "priority": 0}, **extra)


def _start_run(client, run_id, material, profile_id, version, target=5000):
    response = client.post("/api/v1/runs/start", json={
        "run_id": run_id, "material_id": material, "channel_id": M[material],
        "device_id": "relay-pin", "job_id": 3, "target_g": target,
        "profile_id": profile_id, "profile_version": version})
    assert response.status_code == 200, response.text


def test_job_and_run_stay_linked_to_the_old_row_after_new_versions(client):
    _seed_device(client)
    old = save(client, "M1")
    activate(client, old)
    job = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=old["profile_version_id"])).json()
    _start_run(client, "run-old", "M1", "default", 1)
    for _ in range(3):
        save(client, "M1")
    save_both(client)

    queue = client.get("/api/v1/queue").json()["waiting_commands"]
    assert queue[0]["profile_version_id"] == old["profile_version_id"]
    assert (queue[0]["profile_id"], queue[0]["profile_version"]) == ("default", 1)
    run = client.get("/api/v1/runs/run-old").json()
    assert run["profile_version_id"] == old["profile_version_id"]
    assert job["profile_version_id"] == old["profile_version_id"]
    listed = client.get("/api/v1/runs", params={"profile_version_id": old["profile_version_id"]}).json()
    assert [r["run_id"] for r in listed["items"]] == ["run-old"]


def test_delete_refused_when_a_job_or_run_references_the_row(client):
    _seed_device(client)
    with_job = save(client, "M1")
    with_run = save(client, "M1")
    free = save(client, "M1")
    activate(client, with_job)
    job = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=with_job["profile_version_id"])).json()
    _start_run(client, "run-ref", "M1", "default", 2)
    # Deactivated (so the active guard is not what refuses): the job reference does.
    client.post(f"/api/v1/profile-versions/{with_job['profile_version_id']}/deactivate")

    for row in (with_job, with_run):
        refused = client.delete(f"/api/v1/profile-versions/{row['profile_version_id']}")
        assert refused.status_code == 409, refused.text
        assert "referenced" in refused.text
    # Any command state still counts: a cancelled job is history too.
    assert client.post(f"/api/v1/queue/jobs/{job['command_id']}/cancel").status_code == 200
    again = client.delete(f"/api/v1/profiles/default/{with_job['version']}", params={"material_id": "M1"})
    assert again.status_code == 409

    assert client.delete(f"/api/v1/profile-versions/{free['profile_version_id']}").status_code == 200
    assert versions_of(client, "M1") == [1, 2]
    # History still points at the surviving rows, and a re-save never re-points it.
    new = save(client, "M1")
    assert new["version"] == 3 and new["profile_version_id"] != free["profile_version_id"]
    assert client.get("/api/v1/runs/run-ref").json()["profile_version_id"] == with_run["profile_version_id"]


def test_legacy_null_id_reference_also_blocks_delete(client):
    row = save(client, "M2", "legacy")
    with Session(database.get_engine()) as db:
        db.add(models.DeviceCommand(device_id="d", command_type="JOB", material_id="M2",
                                    target_g=5000, state="COMPLETE", profile_id="legacy",
                                    profile_version=1))
        db.commit()
    refused = client.delete(f"/api/v1/profile-versions/{row['profile_version_id']}")
    assert refused.status_code == 409 and "1 queue job" in refused.text
    # Same id+version on the other pump is a different row and is not blocked.
    other = save(client, "M1", "legacy")
    assert client.delete(f"/api/v1/profile-versions/{other['profile_version_id']}").status_code == 200


def test_active_guard_is_kept_and_unknown_id_is_404(client):
    row = save(client, "M1")
    assert client.post(f"/api/v1/profile-versions/{row['profile_version_id']}/activate").status_code == 200
    assert client.delete(f"/api/v1/profile-versions/{row['profile_version_id']}").status_code == 409
    assert client.post(f"/api/v1/profile-versions/{row['profile_version_id']}/deactivate").status_code == 200
    assert client.delete(f"/api/v1/profile-versions/{row['profile_version_id']}").status_code == 200
    assert client.delete("/api/v1/profile-versions/99999").status_code == 404


# ------------------------------------------------- legacy routes ----


def test_legacy_routes_require_material_when_the_pair_is_ambiguous(client):
    m1, m2 = save(client, "M1"), save(client, "M2")
    for path in ("/api/v1/profiles/default/1/activate", "/api/v1/profiles/default/1/deactivate"):
        ambiguous = client.post(path)
        assert ambiguous.status_code == 409, ambiguous.text
        assert "material_id" in ambiguous.text
    assert client.delete("/api/v1/profiles/default/1").status_code == 409

    ok = client.post("/api/v1/profiles/default/1/activate", params={"material_id": "M2"})
    assert ok.status_code == 200 and ok.json()["profile_version_id"] == m2["profile_version_id"]
    rows = {r["profile_version_id"]: r for r in client.get("/api/v1/profiles").json()}
    assert rows[m2["profile_version_id"]]["active"] is True
    assert rows[m1["profile_version_id"]]["active"] is False
    assert client.post("/api/v1/profiles/default/1/activate",
                       params={"material_id": "M3"}).status_code == 422
    assert client.post("/api/v1/profiles/default/9/activate").status_code == 404


def test_legacy_routes_still_work_unambiguously(client):
    row = save(client, "M1")
    done = client.post("/api/v1/profiles/default/1/activate")
    assert done.status_code == 200 and done.json()["profile_version_id"] == row["profile_version_id"]


# ------------------------------------------------- queue selection ----


def test_job_resolves_shared_id_and_version_by_job_material(client):
    _seed_device(client)
    m1, m2 = save(client, "M1"), save(client, "M2")
    activate(client, m1, m2)
    for material, row in (("M1", m1), ("M2", m2)):
        created = client.post("/api/v1/queue/jobs", json=_job(
            material, profile_id="default", profile_version=1))
        assert created.status_code == 200, created.text
        assert created.json()["profile_version_id"] == row["profile_version_id"]


def test_job_pin_by_id_must_match_job_material_target_and_pair(client):
    _seed_device(client)
    m1, m2 = save(client, "M1"), save(client, "M2")
    activate(client, m1, m2)
    wrong_pump = client.post("/api/v1/queue/jobs", json=_job(
        "M2", profile_version_id=m1["profile_version_id"]))
    assert wrong_pump.status_code == 422 and "is for M1, not M2" in wrong_pump.text
    wrong_target = client.post("/api/v1/queue/jobs", json=_job(
        "M1", target=10000, profile_version_id=m1["profile_version_id"]))
    assert wrong_target.status_code == 422
    assert wrong_target.json()["detail"]["code"] == "TARGET_OUT_OF_PROFILE_RANGE"
    inconsistent = client.post("/api/v1/queue/jobs", json=_job(
        "M1", profile_version_id=m1["profile_version_id"], profile_id="default", profile_version=5))
    assert inconsistent.status_code == 422
    missing = client.post("/api/v1/queue/jobs", json=_job("M1", profile_version_id=424242))
    assert missing.status_code == 422 and "profile_version_id" in missing.text
    consistent = client.post("/api/v1/queue/jobs", json=_job(
        "M2", profile_version_id=m2["profile_version_id"], profile_id="default", profile_version=1))
    assert consistent.status_code == 200


def test_job_without_any_pin_needs_an_active_profile_and_half_pins_are_rejected(client):
    _seed_device(client)
    row = save(client, "M1")
    # A draft is not a candidate: nothing compatible, nothing queued.
    assert client.post("/api/v1/queue/jobs", json=_job("M1")).status_code == 422
    assert client.post("/api/v1/queue/jobs", json=_job("M1", profile_id="default")).status_code == 422
    activate(client, row)
    ok = client.post("/api/v1/queue/jobs", json=_job("M1"))
    assert ok.status_code == 200 and ok.json()["profile_version_id"] == row["profile_version_id"]


def test_device_wire_is_unchanged_and_gains_come_from_the_exact_row(client):
    device_id = _seed_device(client)
    make_versions(client, "M1", 1)
    rows = save_both(client, kp=0.07)                      # M1 v2, M2 v1 (different gains)
    m2_other = save(client, "M2", kp=0.09)                 # M2 v2
    activate(client, rows["M1"], rows["M2"])
    job1 = client.post("/api/v1/queue/jobs", json=_job("M1", profile_version_id=rows["M1"]["profile_version_id"])).json()
    job2 = client.post("/api/v1/queue/jobs", json=_job("M2", profile_version_id=rows["M2"]["profile_version_id"])).json()
    activate(client, m2_other)                             # supersedes M2 v1; job2 keeps its pin
    job3 = client.post("/api/v1/queue/jobs", json=_job("M2", profile_id="default", profile_version=2)).json()
    assert m2_other["profile_version_id"] == job3["profile_version_id"]

    wire = {c["command_id"]: c for c in
            client.get("/api/v1/device/commands", params={"device_id": device_id}).json()}
    for job, material, version, kp in ((job1, "M1", 2, 0.07), (job2, "M2", 1, 0.07), (job3, "M2", 2, 0.09)):
        item = wire[job["command_id"]]
        assert item["profile_id"] == "default" and item["profile_version"] == version
        assert item["profile"]["version"] == version and item["profile"]["kp"] == kp
        assert item["material_id"] == material
        # Nothing new for the firmware to understand.
        assert "profile_version_id" not in item
        assert "profile_version_id" not in item["profile"]
        assert set(item["profile"]) == {
            "profile_id", "version", "kp", "ki", "kd", "tolerance_g", "max_overshoot_g",
            "max_duration_ms", "window_ms", "min_on_ms", "min_off_ms"}


def test_legacy_job_without_id_still_gets_gains_by_its_material(client):
    device_id = _seed_device(client)
    save(client, "M1", kp=0.01)
    m2 = save(client, "M2", kp=0.02)
    with Session(database.get_engine()) as db:
        db.add(models.DeviceCommand(device_id=device_id, command_type="JOB", material_id="M2",
                                    target_g=5000, state="PENDING", profile_id="default",
                                    profile_version=1))
        db.commit()
    (item,) = [c for c in client.get("/api/v1/device/commands", params={"device_id": device_id}).json()
               if c["command_type"] == "JOB"]
    assert item["profile"]["kp"] == 0.02 and m2["kp"] == 0.02


def test_reissue_and_resubmit_copy_profile_version_id(client):
    device = _seed_device(client)
    row = save(client, "M1")
    activate(client, row)
    pid = row["profile_version_id"]

    held = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=pid)).json()["command_id"]
    assert client.post("/api/v1/queue/control", json={"action": "HOLD", "command_id": held}).status_code == 200
    released = client.post("/api/v1/queue/control", json={"action": "RELEASE", "command_id": held}).json()
    assert released["command_id"] != held

    failing = client.post("/api/v1/queue/jobs", json=_job(profile_version_id=pid)).json()["command_id"]
    client.get("/api/v1/device/commands", params={"device_id": device})
    client.post(f"/api/v1/device/commands/{failing}/ack",
                json={"state": "FAILED", "error": "stale: older than ledger floor"})
    resubmitted = client.post(f"/api/v1/queue/jobs/{failing}/resubmit")
    assert resubmitted.status_code == 200, resubmitted.text

    with Session(database.get_engine()) as db:
        for command_id in (released["command_id"], resubmitted.json()["command_id"]):
            copy = db.get(models.DeviceCommand, command_id)
            assert copy.profile_version_id == pid
            assert (copy.profile_id, copy.profile_version) == ("default", 1)


# ------------------------------------------------- run start identity ----


def test_run_start_resolves_the_surrogate_id_with_the_run_material(client):
    m1, m2 = save(client, "M1"), save(client, "M2")
    _start_run(client, "run-m1", "M1", "default", 1)
    _start_run(client, "run-m2", "M2", "default", 1)
    assert client.get("/api/v1/runs/run-m1").json()["profile_version_id"] == m1["profile_version_id"]
    assert client.get("/api/v1/runs/run-m2").json()["profile_version_id"] == m2["profile_version_id"]
    cards = client.get("/api/v1/runs", params={"profile_version_id": m2["profile_version_id"]}).json()
    assert [r["run_id"] for r in cards["items"]] == ["run-m2"]
    both = client.get("/api/v1/runs", params={"profile_id": "default", "profile_version": 1}).json()
    assert both["total"] == 2


def test_run_start_with_unresolvable_profile_keeps_snapshot_and_null_id(client):
    save(client, "M1")
    _start_run(client, "run-ghost", "M2", "default", 1)        # only M1 owns default v1
    _start_run(client, "run-missing", "M1", "gone", 4)
    for run_id, pid, ver in (("run-ghost", "default", 1), ("run-missing", "gone", 4)):
        body = client.get(f"/api/v1/runs/{run_id}").json()
        assert body["profile_version_id"] is None
        assert (body["profile_id"], body["profile_version"]) == (pid, ver)


def test_profile_list_and_active_expose_the_surrogate_and_pump(client):
    row = save(client, "M2")
    client.post(f"/api/v1/profile-versions/{row['profile_version_id']}/activate")
    active = client.get("/api/v1/profiles/active", params={
        "material_id": "M2", "channel_id": "CH2", "target_g": 5000}).json()
    assert active["profile_version_id"] == row["profile_version_id"] and active["pump_id"] == "Pump 2"
    (listed,) = client.get("/api/v1/profiles").json()
    assert listed["profile_version_id"] == row["profile_version_id"]


# ------------------------------------------------- UI contract ----


def _js(name):
    from pathlib import Path
    return (Path(__file__).resolve().parents[1] / "app" / "static" / "js" / name).read_text(encoding="utf-8")


def test_tuning_button_label_regression_guards():
    js = _js("tuning.js")
    code = js
    # One renderer derives the label; it uses the server's preview endpoint.
    assert "function renderSaveButton" in code
    assert "'/profiles/next-version?profile_id='" in code
    # Fallback shows no number when the preview is missing/failed.
    body = code[code.index("function renderSaveButton"):code.index("function previewKey")]
    assert "if (!busy) button.textContent = 'Save New Version';" in body
    assert "button.textContent = 'Save as v' + next;" in body
    # Recomputed after the busy state is released (common.js restores the old label).
    fin = code[code.index("} finally {\n        T.setButtonBusy(button, false);"):]
    assert "renderSaveButton();" in fin.split("updateEditBanner();", 1)[0]
    # ... and on every input that feeds it.
    assert "T.el('profile-id').addEventListener('input', updateEditBanner);" in code
    assert "T.el('apply-both').addEventListener('change', updateEditBanner);" in code
    material_change = code[code.index("T.el('material').addEventListener('change'"):]
    assert "updateEditBanner();" in material_change.split("});", 1)[0]
    assert "T.el('target').addEventListener('change', function () { updateActiveSummary(); updateEditBanner(); });" in code
    assert code.count("startEdit(source)") >= 1 and "updateEditBanner();" in code[code.index("function startEdit"):code.index("async function selectProfile")]
    refresh = code[code.index("async function refresh"):code.index("async function deleteProfile")]
    assert "nextCache = {};" in refresh and "renderSaveButton();" in refresh
    # Only the server's own prediction is displayed; never a client-side max+1.
    nv = code[code.index("function nextVersionFor"):code.index("function schedulePreview")]
    assert "nextCache" in nv and "max" not in nv
    # After a save the label/banner/selection come from the response rows.
    assert "savedRows = bulk.profiles" in code and "profileKey(created)" in code
    assert "post('/profiles/bulk'" in code and "post('/profiles', payload)" in code


def test_tuning_and_queue_use_the_surrogate_key():
    tuning, queue = _js("tuning.js"), _js("queue.js")
    assert "function profileKey(profile) { return String(profile.profile_version_id); }" in tuning
    assert "'/profile-versions/'" in tuning
    assert "profile_version_id: String(profile.profile_version_id)" in tuning
    assert "split(':')" not in queue
    assert "'<option value=\"' + esc(row.profile_version_id) + '\">'" in queue
    assert "profile_version_id: Number(pin.profile_version_id)" in queue
    assert "pump_id" in queue and "profileContext(row)" in queue


# ------------------------------------------- run start links only provable rows ----


def _cfg(row, **over):
    cfg = {k: row[k] for k in ("kp", "ki", "kd", "tolerance_g", "max_overshoot_g",
                               "window_ms", "min_on_ms", "min_off_ms", "max_duration_ms")}
    cfg.update(over)
    return cfg


def _start_with(client, run_id, row, target=5000, **cfg_over):
    response = client.post("/api/v1/runs/start", json={
        "run_id": run_id, "material_id": "M1", "channel_id": "CH1",
        "device_id": "relay-pin", "job_id": 3, "target_g": target,
        "profile_id": row["profile_id"], "profile_version": row["version"],
        "config": _cfg(row, **cfg_over)})
    assert response.status_code == 200, response.text
    return client.get(f"/api/v1/runs/{run_id}").json()


def test_run_start_links_when_target_gains_and_time_match(client):
    row = save(client, "M1")
    run = _start_with(client, "link-ok", row)
    assert run["profile_version_id"] == row["profile_version_id"]


@pytest.mark.parametrize("over", [{"kp": 0.5}, {"ki": 0.9}, {"tolerance_g": 21},
                                  {"window_ms": 501}, {"max_duration_ms": 1}])
def test_run_start_leaves_null_when_reported_gains_differ(client, over):
    row = save(client, "M1")
    run = _start_with(client, "link-gains", row, **over)
    assert run["profile_version_id"] is None
    assert (run["profile_id"], run["profile_version"]) == (row["profile_id"], row["version"])


def test_run_start_leaves_null_when_target_differs(client):
    row = save(client, "M1", target_g=5000)
    run = _start_with(client, "link-target", row, target=10000)
    assert run["profile_version_id"] is None


def test_run_start_leaves_null_when_row_is_newer_than_the_run(client):
    from datetime import datetime, timedelta
    row = save(client, "M1")
    with Session(database.get_engine()) as db:
        db.get(models.TuningProfile, row["profile_version_id"]).created_at = (
            datetime.utcnow() + timedelta(hours=1))
        db.commit()
    run = _start_with(client, "link-time", row)
    assert run["profile_version_id"] is None