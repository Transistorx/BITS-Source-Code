"""Canonical-target tuning profile enforcement.

The four independent canonical targets (5000/10000/15000/20000 g) must never
bleed into each other. A NULL/0 target_g is the historical bleed key and must
be rejected at the API layer: the ESP32 now refuses non-canonical targets.
"""

import pytest

CANONICAL = (5000, 10000, 15000, 20000)


def profile_body(target_g=5000, profile_id="m1-base", material_id="M1",
                 channel_id="CH1", **overrides):
    body = {
        "profile_id": profile_id,
        "material_id": material_id,
        "channel_id": channel_id,
        "target_g": target_g,
        "kp": 0.02, "ki": 0.001, "kd": 0.0,
        "tolerance_g": 20, "max_overshoot_g": 100, "max_duration_ms": 120000,
        "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40,
    }
    body.update(overrides)
    return body


def register_device(client, device_id="relay-canon", channels=None):
    if channels is None:
        channels = [
            {"channel_id": "CH1", "state": "IDLE"},
            {"channel_id": "CH2", "state": "IDLE"},
        ]
    response = client.post("/api/v1/device/status", json={
        "device_id": device_id, "status": {"channels": channels, "queue": []},
    })
    assert response.status_code == 200, response.text
    return device_id


def test_create_profile_rejects_zero_target(client):
    response = client.post("/api/v1/profiles", json=profile_body(target_g=0))
    assert response.status_code == 422, response.text
    assert "target_g" in response.text


@pytest.mark.parametrize("bad_id", ["Pump 1", "m1 5kg", "", "a/b"])
def test_create_profile_rejects_bad_profile_id(client, bad_id):
    response = client.post("/api/v1/profiles", json=profile_body(profile_id=bad_id))
    assert response.status_code == 422, response.text
    assert "profile_id" in response.text


def test_tuning_ui_validates_profile_id_before_post():
    # The HTML pattern "[A-Za-z0-9._-]+" is an invalid regex under the browser's
    # `v` flag, so native validation was silently skipped. The hyphen must be
    # escaped and the JS must pre-check before POST /profiles.
    from pathlib import Path
    app = Path(__file__).resolve().parents[1] / "app"
    html = (app / "templates" / "tuning.html").read_text(encoding="utf-8")
    js = (app / "static" / "js" / "tuning.js").read_text(encoding="utf-8")
    assert r'pattern="[A-Za-z0-9._\-]+"' in html
    assert "A-Za-z0-9._-]{1,64}" in js


def test_tuning_ui_apply_to_both_pumps_guards():
    from pathlib import Path
    app = Path(__file__).resolve().parents[1] / "app"
    html = (app / "templates" / "tuning.html").read_text(encoding="utf-8")
    js = (app / "static" / "js" / "tuning.js").read_text(encoding="utf-8")
    assert 'id="apply-both"' in html and "Apply to both pumps (M1 + M2)" in html
    assert r'pattern="[A-Za-z0-9._\-]+"' in html
    # Changed deliberately: "both" is now ONE atomic request (POST /profiles/bulk)
    # instead of two POSTs, so there is no partial-save path any more.
    submit_start = js.index("tuning-form').addEventListener('submit'")
    assert js.index("A-Za-z0-9._-]{1,64}", submit_start) < js.index("var both")
    assert js.index("var both") < js.index("post('/profiles/bulk'")
    assert "materials: ['M1', 'M2']" in js
    assert "Partial save" not in js
    assert "Nothing was saved for either pump" in js
    assert "post('/profiles', payloads[i])" not in js
    submit = js[js.index("var both"):]
    assert "/activate" not in submit.split("updateEditBanner();\n    renderResults")[0]


def test_same_profile_id_saved_per_pump_and_pins_stay_per_pump(client):
    register_device(client)
    m1 = client.post("/api/v1/profiles", json=profile_body(profile_id="both-5k"))
    m2 = client.post("/api/v1/profiles", json=profile_body(
        profile_id="both-5k", material_id="M2", channel_id="CH2"))
    assert m1.status_code == 200, m1.text
    assert m2.status_code == 200, m2.text
    # Independent per-material counters: both pumps own "both-5k v1".
    assert (m1.json()["version"], m2.json()["version"]) == (1, 1)
    assert m1.json()["profile_version_id"] != m2.json()["profile_version_id"]
    assert m1.json()["active"] is False and m2.json()["active"] is False

    bad = client.post("/api/v1/profiles", json=profile_body(
        profile_id="both-5k", material_id="M2", channel_id="CH1"))
    assert bad.status_code == 422, bad.text

    def job(material, version):
        return client.post("/api/v1/queue/jobs", json={
            "material_id": material, "target_g": 5000, "priority": 0,
            "profile_id": "both-5k", "profile_version": version})

    for made in (m1, m2):   # a job can only pin an ACTIVE profile
        assert client.post(
            f"/api/v1/profile-versions/{made.json()['profile_version_id']}/activate").status_code == 200

    # The same id+version resolves by the job's material to that pump's row.
    j1, j2 = job("M1", 1), job("M2", 1)
    assert j1.status_code == 200 and j2.status_code == 200
    assert j1.json()["profile_version_id"] == m1.json()["profile_version_id"]
    assert j2.json()["profile_version_id"] == m2.json()["profile_version_id"]
    # A version only the other pump owns is still rejected.
    assert client.post("/api/v1/profiles", json=profile_body(
        profile_id="both-5k", material_id="M2", channel_id="CH2")).json()["version"] == 2
    cross = job("M1", 2)
    assert cross.status_code == 422 and "is for M2, not M1" in cross.text


def test_create_profile_rejects_null_target(client):
    body = profile_body()
    body["target_g"] = None
    response = client.post("/api/v1/profiles", json=body)
    assert response.status_code == 422, response.text


def test_create_profile_rejects_missing_target(client):
    body = profile_body()
    del body["target_g"]
    response = client.post("/api/v1/profiles", json=body)
    assert response.status_code == 422, response.text


def test_create_profile_rejects_non_canonical_target(client):
    response = client.post("/api/v1/profiles", json=profile_body(target_g=7000))
    assert response.status_code == 422, response.text
    assert "5000" in response.text and "20000" in response.text
    # A plausible-looking but non-canonical value is also refused.
    response = client.post("/api/v1/profiles", json=profile_body(target_g=12000))
    assert response.status_code == 422, response.text


def test_create_profiles_for_all_four_canonical_targets(client):
    created = {}
    for target in CANONICAL:
        body = profile_body(target_g=target, profile_id=f"m1-{target}",
                            material_id="M1", channel_id="CH1")
        response = client.post("/api/v1/profiles", json=body)
        assert response.status_code == 200, response.text
        assert response.json()["target_g"] == target
        created[target] = response.json()
    listed = client.get("/api/v1/profiles", params={"material_id": "M1"}).json()
    assert sorted(item["target_g"] for item in listed) == list(CANONICAL)
    for target in CANONICAL:
        assert created[target]["profile_id"] == f"m1-{target}"


def test_editing_one_target_does_not_change_another(client):
    # Two independent targets on the same material/channel.
    for target, gains in ((5000, 0.01), (10000, 0.02)):
        body = profile_body(target_g=target, profile_id=f"m1-{target}", kp=gains)
        assert client.post("/api/v1/profiles", json=body).status_code == 200
        assert client.post(f"/api/v1/profiles/m1-{target}/1/activate").status_code == 200

    # Editing (adding a new version of) the 5000 g profile...
    updated = client.post("/api/v1/profiles",
                          json=profile_body(target_g=5000, profile_id="m1-5000",
                                            kp=0.09, ki=0.009, kd=0.009))
    assert updated.status_code == 200
    assert updated.json()["version"] == 2
    assert client.post("/api/v1/profiles/m1-5000/2/activate").status_code == 200

    five = client.get("/api/v1/profiles/active",
                      params={"material_id": "M1", "channel_id": "CH1",
                              "target_g": 5000}).json()
    ten = client.get("/api/v1/profiles/active",
                     params={"material_id": "M1", "channel_id": "CH1",
                             "target_g": 10000}).json()
    # ...must not change the other target's profile.
    assert five["kp"] == 0.09 and five["version"] == 2
    assert ten["kp"] == 0.02 and ten["version"] == 1
    assert ten["profile_id"] == "m1-10000"


def test_active_profile_is_per_target_with_no_null_fallback(client):
    # Only the 5000 g target has a profile.
    assert client.post("/api/v1/profiles",
                       json=profile_body(target_g=5000)).status_code == 200
    assert client.post("/api/v1/profiles/m1-base/1/activate").status_code == 200

    assert client.get("/api/v1/profiles/active",
                      params={"material_id": "M1", "channel_id": "CH1",
                              "target_g": 5000}).status_code == 200

    # A canonical target with no profile must be 404, never a NULL fallback.
    for target in (10000, 15000, 20000):
        response = client.get("/api/v1/profiles/active",
                              params={"material_id": "M1", "channel_id": "CH1",
                                      "target_g": target})
        assert response.status_code == 404, (target, response.text)


def test_active_profile_rejects_zero_and_missing_target(client):
    assert client.post("/api/v1/profiles", json=profile_body(target_g=5000)).status_code == 200
    assert client.post("/api/v1/profiles/m1-base/1/activate").status_code == 200

    response = client.get("/api/v1/profiles/active",
                          params={"material_id": "M1", "channel_id": "CH1",
                                  "target_g": 0})
    assert response.status_code == 422, response.text

    response = client.get("/api/v1/profiles/active",
                          params={"material_id": "M1", "channel_id": "CH1"})
    assert response.status_code == 422, response.text


def test_profile_command_payload_carries_explicit_target(client):
    device_id = register_device(client)
    for target in (10000, 20000):
        body = profile_body(target_g=target, profile_id=f"m1-{target}")
        assert client.post("/api/v1/profiles", json=body).status_code == 200
        assert client.post(f"/api/v1/profiles/m1-{target}/1/activate").status_code == 200

    commands = client.get("/api/v1/device/commands",
                          params={"device_id": device_id}).json()
    profile_commands = [c for c in commands if c["command_type"] == "PROFILE"]
    by_id = {c["profile_id"]: c for c in profile_commands}
    assert sorted(c["target_g"] for c in profile_commands) == [10000, 20000]
    # Explicit target only: 0 is the ESP-side bleed key and must never appear.
    assert by_id["m1-10000"]["target_g"] == 10000
    assert by_id["m1-20000"]["target_g"] == 20000
    for command in profile_commands:
        assert command["target_g"] in CANONICAL
        assert command["target_g"] not in (0, None)


def test_activate_deactivate_keep_other_targets_active(client):
    for target in (5000, 10000):
        assert client.post("/api/v1/profiles",
                           json=profile_body(target_g=target,
                                             profile_id=f"m1-{target}")).status_code == 200
        assert client.post(f"/api/v1/profiles/m1-{target}/1/activate").status_code == 200

    # Deactivating one target leaves the other active.
    assert client.post("/api/v1/profiles/m1-5000/1/deactivate").status_code == 200
    assert client.get("/api/v1/profiles/active",
                      params={"material_id": "M1", "channel_id": "CH1",
                              "target_g": 5000}).status_code == 404
    assert client.get("/api/v1/profiles/active",
                      params={"material_id": "M1", "channel_id": "CH1",
                              "target_g": 10000}).status_code == 200


def test_sync_state_unknown_without_device_report(client):
    assert client.post("/api/v1/profiles", json=profile_body(target_g=5000)).status_code == 200
    created = client.post("/api/v1/profiles/m1-base/1/activate")
    assert created.status_code == 200
    body = created.json()
    assert body["sync_state"] == "UNKNOWN"
    assert body["applied_version"] is None

    active = client.get("/api/v1/profiles/active",
                        params={"material_id": "M1", "channel_id": "CH1",
                                "target_g": 5000}).json()
    assert active["sync_state"] == "UNKNOWN"
    assert active["applied_version"] is None


def test_sync_state_out_of_sync_when_device_reports_other_version(client):
    device_id = register_device(client, channels=[
        {"channel_id": "CH1", "state": "IDLE", "profile_id": "m1-base",
         "profile_version": 1},
        {"channel_id": "CH2", "state": "IDLE"},
    ])
    assert client.post("/api/v1/profiles", json=profile_body(target_g=5000)).status_code == 200
    two = client.post("/api/v1/profiles", json=profile_body(target_g=5000))
    assert two.status_code == 200 and two.json()["version"] == 2
    activated = client.post("/api/v1/profiles/m1-base/2/activate")
    assert activated.status_code == 200
    # Device still reports version 1 -> saved version 2 is out of sync.
    assert activated.json()["applied_version"] == 1
    assert activated.json()["sync_state"] == "OUT-OF-SYNC"

    # Device reports the matching version -> in sync.
    register_device(client, device_id=device_id, channels=[
        {"channel_id": "CH1", "state": "IDLE", "profile_id": "m1-base",
         "profile_version": 2},
        {"channel_id": "CH2", "state": "IDLE"},
    ])
    active = client.get("/api/v1/profiles/active",
                        params={"material_id": "M1", "channel_id": "CH1",
                                "target_g": 5000}).json()
    assert active["applied_version"] == 2
    assert active["sync_state"] == "IN-SYNC"


def test_legacy_null_target_row_is_never_queued_or_activated(client):
    """A NULL-target row (legacy) must refuse to activate and never emit target_g=0."""
    from app.database import get_engine
    from app.models import TuningProfile
    from sqlalchemy.orm import Session

    register_device(client)
    with Session(get_engine()) as db:
        db.add(TuningProfile(profile_id="legacy-null", version=1,
                             material_id="M1", channel_id="CH1", target_g=None,
                             kp=0.01, ki=0.001, kd=0.0,
                             tolerance_g=20, max_overshoot_g=100,
                             max_duration_ms=120000, window_ms=500,
                             min_on_ms=40, min_off_ms=40, active=True))
        db.commit()

    response = client.post("/api/v1/profiles/legacy-null/1/activate")
    assert response.status_code == 422, response.text

    commands = client.get("/api/v1/device/commands",
                          params={"device_id": "relay-canon"}).json()
    for command in commands:
        assert command.get("target_g") not in (0, None)

    # The active lookup must not surface the NULL row as a fallback.
    response = client.get("/api/v1/profiles/active",
                          params={"material_id": "M1", "channel_id": "CH1",
                                  "target_g": 5000})
    assert response.status_code == 404, response.text


def test_delete_non_active_profile_version(client):
    assert client.post("/api/v1/profiles",
                       json=profile_body(target_g=5000, profile_id="m1-del")).status_code == 200
    assert client.post("/api/v1/profiles",
                       json=profile_body(target_g=5000, profile_id="m1-del")).status_code == 200
    response = client.delete("/api/v1/profiles/m1-del/1")
    assert response.status_code == 200, response.text
    assert response.json() == {"ok": True}
    listed = client.get("/api/v1/profiles", params={"material_id": "M1"}).json()
    assert [item["version"] for item in listed if item["profile_id"] == "m1-del"] == [2]


def test_delete_active_profile_version_is_rejected(client):
    assert client.post("/api/v1/profiles",
                       json=profile_body(target_g=5000, profile_id="m1-act")).status_code == 200
    assert client.post("/api/v1/profiles/m1-act/1/activate").status_code == 200
    response = client.delete("/api/v1/profiles/m1-act/1")
    assert response.status_code == 409, response.text
    listed = client.get("/api/v1/profiles", params={"material_id": "M1"}).json()
    assert any(item["profile_id"] == "m1-act" for item in listed)


def test_delete_missing_profile_version_returns_404(client):
    response = client.delete("/api/v1/profiles/no-such/9")
    assert response.status_code == 404, response.text


def test_delete_last_version_removes_profile_from_active_lookup(client):
    assert client.post("/api/v1/profiles",
                       json=profile_body(target_g=5000, profile_id="m1-last")).status_code == 200
    assert client.post("/api/v1/profiles/m1-last/1/activate").status_code == 200
    assert client.post("/api/v1/profiles/m1-last/1/deactivate").status_code == 200
    response = client.delete("/api/v1/profiles/m1-last/1")
    assert response.status_code == 200, response.text
    active = client.get("/api/v1/profiles/active",
                        params={"material_id": "M1", "channel_id": "CH1",
                                "target_g": 5000})
    assert active.status_code == 404, active.text


def test_run_history_survives_profile_version_delete(client, start_run):
    """Changed from the pre-versioning test: a version that a run references can
    no longer be deleted (409), because the number could then be reused and
    silently re-point history. An unreferenced version still deletes, and a run
    whose profile row is gone keeps its snapshot columns."""
    run_id = "run-after-profile-delete"
    created = client.post("/api/v1/profiles",
                          json=profile_body(target_g=5000, profile_id="m1-hist")).json()
    # The run reports the gains it really used (== the saved row); a run whose
    # reported gains differ is deliberately left unlinked (see test_run_link_*).
    start_run(run_id, target_g=5000, profile_id="m1-hist", profile_version=1,
              config={"kp": 0.02, "ki": 0.001, "kd": 0.0, "tolerance_g": 20,
                      "max_overshoot_g": 100, "max_duration_ms": 120000,
                      "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40})
    assert client.post("/api/v1/profiles",
                       json=profile_body(target_g=5000, profile_id="m1-hist")).status_code == 200
    refused = client.delete("/api/v1/profiles/m1-hist/1")
    assert refused.status_code == 409, refused.text
    assert "referenced" in refused.text
    assert client.delete("/api/v1/profiles/m1-hist/2").status_code == 200
    run = client.get(f"/api/v1/runs/{run_id}").json()
    assert run["profile_id"] == "m1-hist"
    assert run["profile_version"] == 1
    assert run["profile_version_id"] == created["profile_version_id"]


def test_resave_same_profile_id_creates_next_version_and_never_overwrites(client):
    v1 = client.post("/api/v1/profiles", json=profile_body(profile_id="ed-1", kp=0.02))
    v2 = client.post("/api/v1/profiles", json=profile_body(profile_id="ed-1", kp=0.05))
    assert (v1.json()["version"], v2.json()["version"]) == (1, 2)
    assert v2.json()["material_id"] == "M1" and v2.json()["channel_id"] == "CH1"
    assert v2.json()["active"] is False
    listed = [i for i in client.get("/api/v1/profiles").json() if i["profile_id"] == "ed-1"]
    assert [(i["version"], i["kp"]) for i in listed] == [(2, 0.05), (1, 0.02)]
    assert client.post("/api/v1/profiles/ed-1/1/activate").status_code == 200
    v3 = client.post("/api/v1/profiles", json=profile_body(profile_id="ed-1", kp=0.07))
    assert v3.json()["version"] == 3
    flags = {i["version"]: i["active"] for i in client.get("/api/v1/profiles").json()
             if i["profile_id"] == "ed-1"}
    assert flags == {3: False, 2: False, 1: True}


def test_tuning_ui_edit_flow_guards():
    from pathlib import Path
    js = (Path(__file__).resolve().parents[1] / "app" / "static" / "js" / "tuning.js").read_text(encoding="utf-8")
    # Next version comes from the server's next-version preview, not source.version + 1.
    assert "function nextVersionFor" in js
    assert "Number(editingSource.version) + 1" not in js
    # Inspect drops the stale edit source.
    sel = js[js.index("async function selectProfile"):js.index("function metric")]
    assert "editingSource = null;" in sel and "updateEditBanner();" in sel
    # Banner refresh must not wipe the post-save status message.
    banner = js[js.index("function updateEditBanner"):js.index("function nextVersionFor")]
    assert "banner-edit') >= 0" in banner
    # Apply-both resets after a save so the next edit is not silently doubled.
    assert "if (both) T.el('apply-both').checked = false;" in js


def test_tuning_ui_save_label_is_rederived_from_state():
    """Regression: setButtonBusy(false) restored the pre-save dual label
    ("Save as v8 / v9") over the freshly computed one, and the checkbox/profile
    id/target listeners only refreshed it while editing."""
    from pathlib import Path
    js = (Path(__file__).resolve().parents[1] / "app" / "static" / "js" / "tuning.js").read_text(encoding="utf-8")
    assert "function renderSaveButton" in js
    # Label recomputed after the busy state is released.
    fin = js[js.index("} finally {\n        T.setButtonBusy(button, false);"):]
    assert "renderSaveButton();" in fin.split("updateEditBanner();", 1)[0]
    # Every state input re-renders unconditionally.
    assert "T.el('profile-id').addEventListener('input', updateEditBanner);" in js
    assert "T.el('apply-both').addEventListener('change', updateEditBanner);" in js
    assert "if (editingSource) updateEditBanner()" not in js
    # Changed deliberately: each pump has its own counter, so dual mode labels
    # per pump ("Save as M1 v8 / M2 v5") instead of "v8 / v9".
    assert "'Save as M1 v' + next + ' / M2 v' + nextM2" in js
    assert "' / v' + (next + 1)" not in js
    assert "mojibake" not in js and "â€" not in js and "Â" not in js


def test_server_version_counter_is_independent_per_material(client):
    """Replaces test_server_version_counter_is_shared_across_materials: the
    counter is now max(version for profile_id AND material_id) + 1."""
    m1 = client.post("/api/v1/profiles", json=profile_body(profile_id="shared-1"))
    m2 = client.post("/api/v1/profiles", json=profile_body(
        profile_id="shared-1", material_id="M2", channel_id="CH2"))
    assert m1.status_code == 200 and m2.status_code == 200, (m1.text, m2.text)
    assert (m1.json()["version"], m2.json()["version"]) == (1, 1)
