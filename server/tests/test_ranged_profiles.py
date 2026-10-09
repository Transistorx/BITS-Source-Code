"""Ranged / staged / versioned PID profiles (CONTRACT 9.3, "Ranged profiles").

Covers: wire + profile_hash stability for legacy rows, create-time consistency
(worst case target = target_min_g, stall rule), the status machine, job
resolution (pin / no pin / ambiguous / none), coverage and resolve previews, the
RPC surface and roles, the legacy-command flag, Test Graph categories, and the
additive migration of the existing rows.
"""

import hashlib
import json
import sqlite3

import pytest
from pydantic import ValidationError
from sqlalchemy import select, text
from sqlalchemy.orm import Session

from app import database, models
from app.config import settings
from app.models import DeviceCommand, ProfileStatusLog, TuningProfile
from app.schemas import TuningProfileBulkIn, TuningProfileIn
from app.services import control, history, profile_pin, profile_rules, profiles, queue
from app.services.errors import ServiceError
from test_rpc import h  # noqa: F401  (fixture)

LEGACY = {"kp": 0.0025, "ki": 0.0003, "kd": 0.0001, "tolerance_g": 20,
          "max_overshoot_g": 100, "max_duration_ms": 120000, "window_ms": 500,
          "min_on_ms": 40, "min_off_ms": 40}
RANGE = dict(LEGACY, applicability="range", target_min_g=1000, target_max_g=8000,
             inflight_comp_g=20, flow_regime_note="viscous, 1-8 kg")


@pytest.fixture()
def db(client):
    with Session(database.get_engine(), expire_on_commit=False) as session:
        yield session


def _relay(db):
    control.update_device_status(db, control.DeviceStatusIn(
        device_id="relay-rg", status={"role": "relay_controller", "channels": [], "queue": []}))


def _ranged(db, material="M1", profile_id="wide", **over):
    body = dict(RANGE, profile_id=profile_id, material_id=material,
                channel_id="CH1" if material == "M1" else "CH2", **over)
    return profiles.create_profile(db, TuningProfileIn(**body), actor="tester")


def _ready(db, row, vmin=None, vmax=None):
    """validate (admin step) + activate (operator step) a ranged draft."""
    orm = profiles.row_by_id(db, row.profile_version_id)
    profiles.validate(db, orm, validation_ref="BENCH-1",
                      validated_min_g=vmin or row.target_min_g,
                      validated_max_g=vmax or row.target_max_g, actor="admin")
    profiles.activate(db, profiles.row_by_id(db, row.profile_version_id), actor="operator")
    return profiles.row_by_id(db, row.profile_version_id)


# ------------------------------------------------------------- hash stability ----

# Captured from the code as it was BEFORE ranged profiles existed.
GOLDEN = [
    (dict(profile_id="default", version=1, kp=0.0025, ki=0.0003, kd=0.0001, tolerance_g=20,
          max_overshoot_g=100, max_duration_ms=120000, window_ms=500, min_on_ms=40, min_off_ms=40),
     "0ed532ec8a62e7ad"),
    (dict(profile_id="fast-5kg", version=12, kp=0.1, ki=0.0, kd=0.0, tolerance_g=10,
          max_overshoot_g=50, max_duration_ms=90000, window_ms=250, min_on_ms=20, min_off_ms=20),
     "14d27d7d71659dac"),
    (dict(profile_id="p2.cal", version=3, kp=0.00123, ki=1e-05, kd=0.5, tolerance_g=25,
          max_overshoot_g=200, max_duration_ms=300000, window_ms=1000, min_on_ms=100, min_off_ms=60),
     "1f02951e2f804665"),
]


@pytest.mark.parametrize("fields,expected", GOLDEN)
def test_schema_1_profile_hash_is_byte_identical_to_the_pre_ranged_value(fields, expected):
    row = TuningProfile(material_id="M1", channel_id="CH1", target_g=5000, active=False, **fields)
    wire = queue.gains_wire(row)
    assert set(wire) == set(fields)          # exactly the 11 legacy members
    assert profile_pin.profile_hash(wire) == expected


def test_schema_2_wire_adds_range_and_staged_members_and_the_hash_covers_them(db):
    row = _ready(db, _ranged(db))
    wire = queue.gains_wire(row)
    assert {"target_min_g", "target_max_g", *profile_rules.STAGED_FIELDS} <= set(wire)
    assert wire["target_min_g"] == 1000 and wire["target_max_g"] == 8000
    canon = json.dumps(wire, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    assert profile_pin.profile_hash(wire) == hashlib.sha256(canon.encode()).hexdigest()[:16]
    for key, value in (("target_max_g", 8001), ("target_min_g", 999), ("inflight_comp_g", 21),
                       ("settle_time_ms", 1501)):
        assert profile_pin.profile_hash(dict(wire, **{key: value})) != profile_pin.profile_hash(wire)


def test_a_schema_2_row_missing_a_member_has_no_wire(db):
    row = _ready(db, _ranged(db))
    row.settle_time_ms = None
    with pytest.raises(ValueError):
        queue.gains_wire(row)
    assert queue.safe_gains_wire(row) is None


# ------------------------------------------------------------- create rules ----

def test_create_is_always_a_draft_and_never_active(db):
    out = _ranged(db)
    assert (out.status, out.active, out.profile_schema) == ("draft", False, 2)
    assert (out.target_min_g, out.target_max_g, out.target_g) == (1000, 8000, 1000)
    assert out.coarse_threshold_g == 500 and out.inflight_comp_g == 20   # firmware defaults filled
    legacy = profiles.create_profile(db, TuningProfileIn(
        **LEGACY, profile_id="old", material_id="M1", channel_id="CH1", target_g=5000))
    assert (legacy.status, legacy.active, legacy.profile_schema, legacy.applicability) == (
        "draft", False, 1, "exact")
    assert (legacy.target_min_g, legacy.target_max_g) == (5000, 5000)
    log = db.scalars(select(ProfileStatusLog).order_by(ProfileStatusLog.id)).all()
    assert [(r.from_status, r.to_status, r.actor) for r in log] == [
        (None, "draft", "tester"), (None, "draft", "api")]


@pytest.mark.parametrize("over,needle", [
    ({"inflight_comp_g": None}, "inflight_comp_g is required"),
    ({"flow_regime_note": None}, "flow_regime_note is required"),
    ({"flow_regime_note": "  "}, "flow_regime_note is required"),
    ({"tolerance_g": 1000, "inflight_comp_g": 1000}, "tolerance_g >= target_g"),   # worst case = min
    ({"max_overshoot_g": 1001}, "max_overshoot_g > min(target_g, cap)"),
    ({"max_overshoot_g": 6000, "target_min_g": 6000, "target_max_g": 9000}, "max_overshoot_g"),
    ({"inflight_comp_g": 19}, "inflight_comp_g < tolerance_g"),       # the stall rule
    ({"coarse_threshold_g": 10, "fine_threshold_g": 100}, "micro<=fine<=coarse"),
    ({"fine_min_on_ms": 300}, "PIN_STAGE_CANNOT_PULSE fine"),
    ({"min_off_ms": 300, "min_on_ms": 40}, "PIN_MIN_OFF_SATURATES"),
    ({"target_min_g": 9000, "target_max_g": 8000}, "target_min_g must be <="),
    ({"applicability": "exact"}, "exact profile needs"),
])
def test_ranged_create_refuses_unsafe_or_incomplete_profiles(over, needle):
    body = dict(RANGE, profile_id="x", material_id="M1", channel_id="CH1")
    body.update(over)
    with pytest.raises(ValidationError) as exc:
        TuningProfileIn(**body)
    assert needle in str(exc.value)


def test_stall_rule_boundary_is_inflight_ge_tolerance():
    def with_comp(n):
        return {**LEGACY, **profile_rules.FW_STAGED_DEFAULTS, "inflight_comp_g": n}

    assert profile_rules.consistency_error(with_comp(20), 1000) is None
    assert "PIN_STALL" in profile_rules.consistency_error(with_comp(19), 1000)
    # Kconfig defaults (comp 15, tol 20) are exactly the known 5 g stall window.
    assert "PIN_STALL" in profile_rules.consistency_error(
        dict(LEGACY, **profile_rules.FW_STAGED_DEFAULTS), 1000)


def test_legacy_body_keeps_canonical_only_but_any_other_weight_needs_the_ranged_form():
    with pytest.raises(ValidationError):
        TuningProfileIn(**LEGACY, profile_id="x", material_id="M1", channel_id="CH1", target_g=7000)
    exact = TuningProfileIn(**dict(RANGE, profile_id="x", material_id="M1", channel_id="CH1",
                                   applicability="exact", target_min_g=7000, target_max_g=7000,
                                   flow_regime_note=None))
    assert (exact.applicability, exact.profile_schema, exact.target_g) == ("exact", 2, 7000)


def test_general_profile_needs_a_note_and_may_declare_a_wide_range(db):
    out = _ranged(db, applicability="general", target_min_g=1, target_max_g=20000,
                  tolerance_g=0, inflight_comp_g=0, max_overshoot_g=1)
    assert (out.applicability, out.target_min_g, out.target_max_g) == ("general", 1, 20000)


def test_target_max_is_bounded_by_the_pump_maximum(db, monkeypatch):
    monkeypatch.setenv("PUMP1_MAX_TARGET_G", "5000")
    with pytest.raises(ServiceError) as exc:
        _ranged(db)
    assert exc.value.http_status == 422 and "exceeds the M1 maximum" in exc.value.message
    assert _ranged(db, material="M2").target_max_g == 8000      # pump 2 keeps 20000


def test_bulk_of_a_range_profile_needs_per_pump_validation(db):
    base = dict(RANGE, profile_id="bulk", materials=["M1", "M2"])
    with pytest.raises(ValidationError) as exc:
        TuningProfileBulkIn(**base)
    assert "per_pump_validation" in str(exc.value)
    with pytest.raises(ValidationError):                   # only one pump covered
        TuningProfileBulkIn(**dict(base, per_pump_validation={
            "M1": {"validation_ref": "B1", "validated_min_g": 1000, "validated_max_g": 8000}}))
    both = {m: {"validation_ref": f"B-{m}", "validated_min_g": 1000, "validated_max_g": 8000}
            for m in ("M1", "M2")}
    out = profiles.create_profiles_bulk(db, TuningProfileBulkIn(**base, per_pump_validation=both))
    assert {p.material_id: (p.status, p.validation_ref) for p in out.profiles} == {
        "M1": ("draft", "B-M1"), "M2": ("draft", "B-M2")}
    # An exact legacy bulk is unchanged.
    legacy = profiles.create_profiles_bulk(db, TuningProfileBulkIn(
        **LEGACY, profile_id="lb", target_g=5000, materials=["M1", "M2"]))
    assert all(p.status == "draft" and not p.active for p in legacy.profiles)


# ------------------------------------------------------------- status machine ----

def test_draft_cannot_be_activated_and_validate_needs_a_reference(db):
    row = _ranged(db)
    orm = profiles.row_by_id(db, row.profile_version_id)
    with pytest.raises(ServiceError) as exc:
        profiles.activate(db, orm)
    assert exc.value.http_status == 409 and "validated" in exc.value.message
    with pytest.raises(ServiceError):
        profiles.validate(db, orm, validation_ref=" ", validated_min_g=1000, validated_max_g=8000)
    ok = profiles.validate(db, orm, validation_ref="BENCH-7", validated_min_g=1000,
                           validated_max_g=8000, actor="admin")
    assert (ok.status, ok.validation_ref, ok.active) == ("validated", "BENCH-7", False)
    with pytest.raises(ServiceError):                       # only a draft can be validated
        profiles.validate(db, orm, validation_ref="again", validated_min_g=1, validated_max_g=2)


def test_activation_refused_when_the_declared_range_exceeds_the_validated_range(db):
    row = _ranged(db)
    orm = profiles.row_by_id(db, row.profile_version_id)
    profiles.validate(db, orm, validation_ref="BENCH-2", validated_min_g=2000,
                      validated_max_g=8000)
    with pytest.raises(ServiceError) as exc:
        profiles.activate(db, profiles.row_by_id(db, row.profile_version_id))
    assert exc.value.http_status == 409 and "only validated for 2000..8000" in exc.value.message
    assert profiles.row_by_id(db, row.profile_version_id).status == "validated"


def test_identical_active_range_twin_is_rejected_but_overlaps_are_legal(db):
    first = _ready(db, _ranged(db, profile_id="a"))
    twin = _ranged(db, profile_id="b")
    profiles.validate(db, profiles.row_by_id(db, twin.profile_version_id), validation_ref="B",
                      validated_min_g=1000, validated_max_g=8000)
    with pytest.raises(ServiceError) as exc:
        profiles.activate(db, profiles.row_by_id(db, twin.profile_version_id))
    assert exc.value.http_status == 409 and "exactly 1000..8000" in exc.value.message
    assert profiles.row_by_id(db, first.profile_version_id).status == "active"
    overlap = _ranged(db, profile_id="c", target_min_g=5000, target_max_g=12000)
    assert _ready(db, overlap).status == "active"           # overlap is the operator's choice


def test_deactivate_returns_to_validated_and_deprecate_is_terminal(db):
    row = _ready(db, _ranged(db))
    out = profiles.deactivate(db, profiles.row_by_id(db, row.profile_version_id), actor="admin")
    assert (out["status"], out["active"]) == ("validated", False)
    profiles.activate(db, profiles.row_by_id(db, row.profile_version_id))
    dep = profiles.deprecate(db, profiles.row_by_id(db, row.profile_version_id), actor="admin")
    assert (dep["status"], dep["active"]) == ("deprecated", False)
    with pytest.raises(ServiceError):
        profiles.activate(db, profiles.row_by_id(db, row.profile_version_id))
    with pytest.raises(ServiceError):
        profiles.deprecate(db, profiles.row_by_id(db, row.profile_version_id))
    states = [(r.from_status, r.to_status) for r in db.scalars(
        select(ProfileStatusLog).where(ProfileStatusLog.profile_version_id == row.profile_version_id)
        .order_by(ProfileStatusLog.id))]
    assert states == [(None, "draft"), ("draft", "validated"), ("validated", "active"),
                      ("active", "validated"), ("validated", "active"), ("active", "deprecated")]


# ------------------------------------------------------------- job resolution ----

def test_unpinned_job_uses_the_single_compatible_profile_at_any_weight(db):
    _relay(db)
    row = _ready(db, _ranged(db))
    for target in (1000, 2345, 8000):
        job = queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=target))
        assert job["target_g"] == target and job["profile_version_id"] == row.profile_version_id
        wired = control.command_wire(db.get(DeviceCommand, job["command_id"]), db)
        assert wired["target_g"] == target and wired["profile"]["target_min_g"] == 1000


def test_no_compatible_profile_lists_nearest_for_display_but_never_applies_one(db):
    _relay(db)
    row = _ready(db, _ranged(db))
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=9000))
    err = exc.value
    assert (err.code, err.http_status) == ("no_compatible_profile", 422)
    assert err.data["nearest"][0]["profile_version_id"] == row.profile_version_id
    assert db.scalars(select(DeviceCommand).where(DeviceCommand.command_type == "JOB")).first() is None
    # A 5000 g canonical profile is never "nearest canonical" for 5100 g either.
    canon = profiles.create_profile(db, TuningProfileIn(
        **LEGACY, profile_id="c5", material_id="M2", channel_id="CH2", target_g=5000))
    profiles.activate(db, profiles.row_by_id(db, canon.profile_version_id))
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M2", target_g=5100))
    assert exc.value.code == "no_compatible_profile"


def test_overlap_makes_an_unpinned_job_ambiguous_and_a_pin_resolves_it(db):
    _relay(db)
    a = _ready(db, _ranged(db, profile_id="a"))
    b = _ready(db, _ranged(db, profile_id="b", target_min_g=5000, target_max_g=12000))
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=6000))
    assert (exc.value.code, exc.value.http_status) == ("ambiguous_profile", 409)
    got = {c["profile_version_id"]: c for c in exc.value.data["candidates"]}
    assert set(got) == {a.profile_version_id, b.profile_version_id}
    assert got[b.profile_version_id]["target_min_g"] == 5000 and got[b.profile_version_id][
        "validated_max_g"] == 12000
    assert {"profile_id", "version", "validated_min_g"} <= set(got[a.profile_version_id])
    pinned = queue.create_job(db, queue.QueueJobIn(
        material_id="M1", target_g=6000, profile_version_id=b.profile_version_id))
    assert pinned["profile_version_id"] == b.profile_version_id
    # Outside the overlap only one profile applies.
    assert queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=3000))[
        "profile_version_id"] == a.profile_version_id


def test_explicit_pin_must_be_active_in_range_and_consistent_at_the_target(db):
    _relay(db)
    draft = _ranged(db)
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=2000,
                                              profile_version_id=draft.profile_version_id))
    assert exc.value.http_status == 422 and "draft" in exc.value.message
    row = _ready(db, draft)
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=8001,
                                              profile_version_id=row.profile_version_id))
    assert (exc.value.code, exc.value.http_status) == ("target_out_of_profile_range", 422)
    assert exc.value.data["target_min_g"] == 1000 and exc.value.data["target_max_g"] == 8000
    with pytest.raises(ServiceError):                        # wrong pump
        queue.create_job(db, queue.QueueJobIn(material_id="M2", target_g=2000,
                                              profile_version_id=row.profile_version_id))
    # A row damaged after activation (staged member lost) is refused at queue time.
    row.settle_time_ms = None
    db.commit()
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=2000,
                                              profile_version_id=row.profile_version_id))
    assert exc.value.http_status == 422


def test_job_target_bounds_and_half_pins(db):
    _relay(db)
    with pytest.raises(ValidationError):
        queue.QueueJobIn(material_id="M1", target_g=0)
    with pytest.raises(ValidationError):
        queue.QueueJobIn(material_id="M1", target_g=5000, profile_id="x")
    with pytest.raises(ServiceError) as exc:
        queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=20001))
    assert exc.value.http_status == 422 and "1..20000" in exc.value.message


def test_resubmit_revalidates_the_range_and_activeness(db):
    _relay(db)
    row = _ready(db, _ranged(db))
    job = queue.create_job(db, queue.QueueJobIn(material_id="M1", target_g=2000))
    control.acknowledge_command(db, job["command_id"], control.DeviceCommandAck(
        state="FAILED", error="stale: older than ledger floor"))
    profiles.deprecate(db, profiles.row_by_id(db, row.profile_version_id))
    with pytest.raises(ServiceError) as exc:
        queue.resubmit_job(db, job["command_id"], queue.ResubmitIn())
    assert exc.value.http_status == 409 and "no longer valid" in exc.value.message


# ------------------------------------------------------------- read helpers ----

def test_coverage_reports_gaps_and_overlaps(db):
    _ready(db, _ranged(db, profile_id="a", target_min_g=1000, target_max_g=8000))
    _ready(db, _ranged(db, profile_id="b", target_min_g=5000, target_max_g=12000))
    _ready(db, _ranged(db, profile_id="z", target_min_g=15000, target_max_g=20000))
    m1 = profiles.coverage(db, "M1")["pumps"][0]
    assert m1["max_target_g"] == 20000 and m1["range_count"] == 3
    assert m1["gaps"] == [{"from_g": 1, "to_g": 999}, {"from_g": 12001, "to_g": 14999}]
    assert [(o["from_g"], o["to_g"]) for o in m1["overlaps"]] == [(5000, 8000)]
    both = profiles.coverage(db)["pumps"]
    assert [p["material_id"] for p in both] == ["M1", "M2"]
    assert both[1]["gaps"] == [{"from_g": 1, "to_g": 20000}]


def test_resolve_preview_is_read_only_and_reports_every_outcome(db):
    _ready(db, _ranged(db, profile_id="a"))
    _ready(db, _ranged(db, profile_id="b", target_min_g=5000, target_max_g=12000))
    before = db.scalar(select(text("count(*)")).select_from(TuningProfile))
    assert profiles.resolve_preview(db, "M1", 2000)["outcome"] == "ok"
    amb = profiles.resolve_preview(db, "M1", 6000)
    assert amb["outcome"] == "ambiguous" and len(amb["candidates"]) == 2
    none = profiles.resolve_preview(db, "M1", 15000)
    assert none["outcome"] == "none" and none["nearest"]
    assert db.scalar(select(text("count(*)")).select_from(TuningProfile)) == before
    assert db.scalars(select(DeviceCommand)).first() is None


def test_list_filters_by_range_and_status(db):
    _ready(db, _ranged(db, profile_id="a"))
    _ranged(db, profile_id="draft-only", target_min_g=9000, target_max_g=10000)
    assert [p.profile_id for p in profiles.list_profiles(db, "M1", target_g=2000)] == ["a"]
    assert [p.profile_id for p in profiles.list_profiles(db, "M1", status="draft")] == ["draft-only"]
    assert len(profiles.list_profiles(db, "M1")) == 2


# ------------------------------------------------------------- legacy commands ----

def test_legacy_profile_commands_flag(db, monkeypatch):
    _relay(db)
    canon = profiles.create_profile(db, TuningProfileIn(
        **LEGACY, profile_id="lg", material_id="M1", channel_id="CH1", target_g=5000))

    def kinds():
        return sorted(c.command_type for c in db.scalars(select(DeviceCommand)) if c.command_type
                      in ("PROFILE", "PROFILE_CLEAR"))

    profiles.activate(db, profiles.row_by_id(db, canon.profile_version_id))
    assert kinds() == ["PROFILE"]                               # default ON
    monkeypatch.setenv("LEGACY_PROFILE_COMMANDS", "0")
    profiles.deactivate(db, profiles.row_by_id(db, canon.profile_version_id))
    assert kinds() == ["PROFILE"]                               # no PROFILE_CLEAR while OFF
    monkeypatch.delenv("LEGACY_PROFILE_COMMANDS")
    profiles.activate(db, profiles.row_by_id(db, canon.profile_version_id))
    n = len(kinds())
    _ready(db, _ranged(db))                                     # a ranged row never emits one
    assert len(kinds()) == n


# ------------------------------------------------------------- graphs / history ----

def test_graph_categories_stay_exactly_the_four_canonical_targets():
    assert history.GRAPH_TARGETS_G == (5000, 10000, 15000, 20000)
    for target in (5000, 10000, 15000, 20000):
        history.check_graph_target(target)
    for target in (1, 7000, 12000, 19999):
        with pytest.raises(ServiceError) as exc:
            history.check_graph_target(target)
        assert exc.value.http_status == 403


def test_run_history_filters_by_exact_target_and_a_ranged_run_links_its_profile(client, db):
    _relay(db)
    row = _ready(db, _ranged(db))
    for rid, target in (("odd-1", 7000), ("odd-2", 7001), ("canon", 5000)):
        body = {"run_id": rid, "material_id": "M1", "device_id": "relay-rg", "job_id": 1,
                "target_g": target, "profile_id": "wide", "profile_version": 1}
        assert client.post("/api/v1/runs/start", json=body).status_code == 200
    one = client.get("/api/v1/runs", params={"target_g": 7000}).json()
    assert [r["run_id"] for r in one["items"]] == ["odd-1"]
    assert one["items"][0]["profile_version_id"] == row.profile_version_id    # 7000 is in range
    canon = client.get("/api/v1/runs", params={"target_g": 5000}).json()["items"][0]
    assert canon["profile_version_id"] == row.profile_version_id
    out_of_range = {"run_id": "far", "material_id": "M1", "device_id": "relay-rg", "job_id": 1,
                    "target_g": 9000, "profile_id": "wide", "profile_version": 1}
    client.post("/api/v1/runs/start", json=out_of_range)
    assert client.get("/api/v1/runs/far").json()["profile_version_id"] is None


# ------------------------------------------------------------- RPC ----

def _rpc_range_body(**over):
    body = dict(RANGE, profile_id="wide", material_id="M1", channel_id="CH1")
    body.update(over)
    return body


def test_rpc_roles_confirm_and_full_lifecycle(h):
    op, ad = h.login("opr1", "app_o"), h.login("adm1", "app_a")
    created = h.call("profiles.create", _rpc_range_body(), token=op, client="app_o")
    assert created["ok"] and created["data"]["status"] == "draft"
    pvid = created["data"]["profile_version_id"]
    val = {"profile_version_id": pvid, "validation_ref": "BENCH-9",
           "validated_min_g": 1000, "validated_max_g": 8000}
    assert h.call("profiles.validate", val, token=op, client="app_o")["code"] == "FORBIDDEN"
    # Not activatable before validation, even with a confirm token.
    ct = h.confirm(op, "profiles.activate", {"profile_version_id": pvid}, client="app_o")
    early = h.call("profiles.activate", {"profile_version_id": pvid, "confirm_token": ct},
                   token=op, client="app_o")
    assert early["code"] == "CONFLICT" and "validated" in early["error"]
    ok = h.call("profiles.validate", val, token=ad)
    assert ok["ok"] and ok["data"]["status"] == "validated"
    assert h.call("profiles.activate", {"profile_version_id": pvid}, token=op,
                  client="app_o")["code"] == "CONFIRM_REQUIRED"
    ct = h.confirm(op, "profiles.activate", {"profile_version_id": pvid}, client="app_o")
    done = h.call("profiles.activate", {"profile_version_id": pvid, "confirm_token": ct},
                  token=op, client="app_o")
    assert done["ok"] and done["data"]["status"] == "active"
    # No legacy PROFILE command for a ranged profile.
    assert h.rows(DeviceCommand, DeviceCommand.command_type == "PROFILE") == []
    # profiles.active lists the compatible active profiles for a target.
    act = h.call("profiles.active", {"material_id": "M1", "channel_id": "CH1", "target_g": 4321}, token=op,
                 client="app_o")
    assert act["ok"] and act["data"]["count"] == 1 and act["data"]["items"][0][
        "profile_version_id"] == pvid
    none = h.call("profiles.active", {"material_id": "M1", "channel_id": "CH1", "target_g": 9000},
                  token=op, client="app_o")
    assert none["ok"] and none["data"]["count"] == 0 and none["data"]["items"] == []
    cov = h.call("profiles.coverage", {"material_id": "M1"}, token=op, client="app_o")
    assert cov["ok"] and cov["data"]["pumps"][0]["ranges"][0]["profile_version_id"] == pvid
    res = h.call("profiles.resolve", {"material_id": "M1", "target_g": 4321}, token=op, client="app_o")
    assert res["ok"] and res["data"]["outcome"] == "ok"
    # Job at an arbitrary weight, no pin.
    job = h.call("queue.job.create", {"material_id": "M1", "target_g": 4321}, token=op, client="app_o")
    assert job["ok"] and job["data"]["target_g"] == 4321
    # deprecate: admin only, confirm required.
    assert h.call("profiles.deprecate", {"profile_version_id": pvid}, token=op,
                  client="app_o")["code"] == "FORBIDDEN"
    assert h.call("profiles.deprecate", {"profile_version_id": pvid}, token=ad)["code"] == "CONFIRM_REQUIRED"
    ct = h.confirm(ad, "profiles.deprecate", {"profile_version_id": pvid})
    dep = h.call("profiles.deprecate", {"profile_version_id": pvid, "confirm_token": ct}, token=ad)
    assert dep["ok"] and dep["data"]["status"] == "deprecated"
    gone = h.call("queue.job.create", {"material_id": "M1", "target_g": 4321}, token=op, client="app_o")
    assert gone["code"] == "NO_COMPATIBLE_PROFILE"


def test_rpc_ambiguous_and_out_of_range_codes_with_data(h):
    ad, op = h.login("adm1", "app_a"), h.login("opr1", "app_o")
    ids = []
    for pid, lo, hi in (("a", 1000, 8000), ("b", 5000, 12000)):
        made = h.call("profiles.create", _rpc_range_body(profile_id=pid, target_min_g=lo,
                                                          target_max_g=hi), token=op, client="app_o")
        pvid = made["data"]["profile_version_id"]
        h.call("profiles.validate", {"profile_version_id": pvid, "validation_ref": "B",
                                     "validated_min_g": lo, "validated_max_g": hi}, token=ad)
        ct = h.confirm(op, "profiles.activate", {"profile_version_id": pvid}, client="app_o")
        assert h.call("profiles.activate", {"profile_version_id": pvid, "confirm_token": ct},
                      token=op, client="app_o")["ok"]
        ids.append(pvid)
    amb = h.call("queue.job.create", {"material_id": "M1", "target_g": 6000}, token=op, client="app_o")
    assert amb["code"] == "AMBIGUOUS_PROFILE" and not amb["ok"]
    assert {c["profile_version_id"] for c in amb["data"]["candidates"]} == set(ids)
    oor = h.call("queue.job.create", {"material_id": "M1", "target_g": 12500, "profile_version_id": ids[1]},
                 token=op, client="app_o")
    assert oor["code"] == "TARGET_OUT_OF_PROFILE_RANGE" and oor["data"]["target_max_g"] == 12000
    none = h.call("queue.job.create", {"material_id": "M1", "target_g": 15000}, token=op, client="app_o")
    assert none["code"] == "NO_COMPATIBLE_PROFILE" and none["data"]["nearest"]
    big = h.call("queue.job.create", {"material_id": "M1", "target_g": 20001}, token=op, client="app_o")
    assert big["code"] == "INVALID"
    half = h.call("queue.job.create", {"material_id": "M1", "target_g": 100, "profile_id": "a"},
                  token=op, client="app_o")
    assert half["code"] == "INVALID"


def test_rpc_bulk_range_needs_per_pump_validation(h):
    op = h.login("opr1", "app_o")
    bulk = {k: v for k, v in _rpc_range_body().items() if k not in ("material_id", "channel_id")}
    bulk["materials"] = ["M1", "M2"]
    refused = h.call("profiles.bulk", bulk, token=op, client="app_o")
    assert refused["code"] == "INVALID" and "per_pump_validation" in refused["error"]
    assert h.rows(TuningProfile) == []


def test_every_method_is_in_the_catalogue_with_the_right_role():
    from app.rpc.methods import METHODS
    want = {"profiles.coverage": "viewer", "profiles.resolve": "viewer",
            "profiles.validate": "admin", "profiles.activate": "operator",
            "profiles.deprecate": "admin"}
    for name, role in want.items():
        assert METHODS[name].min_role == role, name
    assert METHODS["profiles.activate"].dangerous and METHODS["profiles.deprecate"].dangerous
    assert METHODS["profiles.validate"].dangerous is None and METHODS["profiles.validate"].write


# ------------------------------------------------------------- migration ----

RANGED_COLS = ("applicability", "target_min_g", "target_max_g", "status", "coarse_threshold_g",
               "fine_threshold_g", "micro_threshold_g", "coarse_min_on_ms", "fine_min_on_ms",
               "micro_min_on_ms", "settle_time_ms", "inflight_comp_g", "validated_min_g",
               "validated_max_g", "validation_ref", "flow_regime_note", "profile_schema",
               "status_changed_at", "status_changed_by")


@pytest.fixture()
def pre_ranged_db(tmp_path, monkeypatch):
    """A fully versioned database as it is in production BEFORE this change:
    tuning_profiles has no ranged columns, no index, no profile_status_log."""
    path = tmp_path / "pre.db"
    monkeypatch.setenv("DATABASE_URL", "sqlite:///" + path.as_posix())
    database.reset_engine()
    database.init_db()
    engine = database.get_engine()
    with engine.begin() as conn:
        conn.execute(text("DROP INDEX ix_tuning_profiles_mat_chan_status"))
        for col in RANGED_COLS:
            conn.execute(text(f"ALTER TABLE tuning_profiles DROP COLUMN {col}"))
        conn.execute(text("DROP TABLE profile_status_log"))
        conn.execute(text("DELETE FROM server_meta WHERE key LIKE 'ranged_profiles%'"))
        conn.execute(text("DELETE FROM schema_migration_audit WHERE migration = 'ranged_profiles'"))
    database.reset_engine()
    yield path
    database.reset_engine()


def _legacy_row(path, pid, version, material, target, kp, active):
    con = sqlite3.connect(path)
    channel = "CH1" if material == "M1" else "CH2"
    con.execute(
        "INSERT INTO tuning_profiles (profile_id, version, material_id, channel_id, target_g, kp, "
        "ki, kd, tolerance_g, max_overshoot_g, max_duration_ms, window_ms, min_on_ms, min_off_ms, "
        "active, created_at) VALUES (?,?,?,?,?,?,0.0003,0.0001,20,100,120000,500,40,40,?,"
        "'2026-01-01 00:00:00')", (pid, version, material, channel, target, kp, active))
    con.commit()
    con.close()


def _snapshot(path):
    con = sqlite3.connect(path)
    rows = con.execute(
        "SELECT profile_version_id, profile_id, version, material_id, channel_id, target_g, kp, ki, kd, "
        "tolerance_g, max_overshoot_g, max_duration_ms, window_ms, min_on_ms, min_off_ms, active, "
        "created_at FROM tuning_profiles ORDER BY profile_version_id").fetchall()
    con.close()
    return rows


def _hashes():
    with Session(database.get_engine()) as db:
        return {r.profile_version_id: profile_pin.profile_hash(queue.gains_wire(r))
                for r in db.scalars(select(TuningProfile))}


def test_migration_backfills_exact_ranges_and_never_changes_hashes_or_identity(pre_ranged_db):
    rows = [("a", 1, "M1", 5000, 0.011, 1), ("a", 2, "M1", 5000, 0.012, 0),
            ("b", 1, "M2", 10000, 0.02, 1), ("b", 2, "M2", 15000, 0.021, 0),
            ("c", 1, "M1", 20000, 0.03, 0), ("d", 1, "M2", None, 0.04, 0)]
    for r in rows:
        _legacy_row(pre_ranged_db, *r)
    before = _snapshot(pre_ranged_db)

    database.init_db()

    assert _snapshot(pre_ranged_db) == before                      # identity, gains, active: untouched
    con = sqlite3.connect(pre_ranged_db)
    got = con.execute("SELECT profile_id, version, material_id, applicability, target_min_g, "
                      "target_max_g, status, profile_schema, inflight_comp_g, validation_ref "
                      "FROM tuning_profiles ORDER BY profile_version_id").fetchall()
    assert got == [
        ("a", 1, "M1", "exact", 5000, 5000, "active", 1, None, None),
        ("a", 2, "M1", "exact", 5000, 5000, "validated", 1, None, None),
        ("b", 1, "M2", "exact", 10000, 10000, "active", 1, None, None),
        ("b", 2, "M2", "exact", 15000, 15000, "validated", 1, None, None),
        ("c", 1, "M1", "exact", 20000, 20000, "validated", 1, None, None),
        ("d", 1, "M2", "exact", None, None, "deprecated", 1, None, None)]
    assert con.execute("SELECT COUNT(*) FROM tuning_profiles WHERE (status='active') <> (active=1)"
                       ).fetchone()[0] == 0
    assert con.execute(
        "SELECT COUNT(*) FROM sqlite_master WHERE tbl_name='tuning_profiles' "
        "AND sql LIKE '%uq_tuning_profile_material_version%'").fetchone()[0] == 1   # UNIQUE intact
    idx = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='index'")}
    assert "ix_tuning_profiles_mat_chan_status" in idx
    assert con.execute("SELECT COUNT(*) FROM profile_status_log").fetchone()[0] == 0
    audit = [r for r in con.execute(
        "SELECT step, outcome FROM schema_migration_audit WHERE migration='ranged_profiles'")]
    assert ("backfill", "OK") in audit and ("run", "OK") in audit
    con.close()

    # Hash of every migrated row equals the pre-ranged formula over the OLD columns.
    hashes = _hashes()
    for (pvid, pid, version, mat, ch, target, kp, ki, kd, tol, over, dur, win, on, off, *_r) in before:
        wire = dict(profile_id=pid, version=version, kp=kp, ki=ki, kd=kd, tolerance_g=tol,
                    max_overshoot_g=over, max_duration_ms=dur, window_ms=win, min_on_ms=on,
                    min_off_ms=off)
        canon = json.dumps(wire, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        assert hashes[pvid] == hashlib.sha256(canon.encode()).hexdigest()[:16]


def test_migration_is_idempotent_and_never_rewrites_later_status_changes(pre_ranged_db):
    _legacy_row(pre_ranged_db, "a", 1, "M1", 5000, 0.011, 1)
    database.init_db()
    con = sqlite3.connect(pre_ranged_db)
    con.execute("UPDATE tuning_profiles SET status='deprecated', active=0")
    con.commit()
    con.close()
    first = _snapshot(pre_ranged_db)
    database.reset_engine()
    database.init_db()                                              # second run: nothing to do
    from app import ranged_profile_migration as mig
    assert mig.needs_migration(database.get_engine()) is False
    out = mig.migrate_ranged_profiles(database.get_engine())        # forced rerun is a no-op
    assert out["columns_added"] == [] and out["backfill"] == {"skipped": True}
    assert _snapshot(pre_ranged_db) == first
    con = sqlite3.connect(pre_ranged_db)
    assert con.execute("SELECT status FROM tuning_profiles").fetchone() == ("deprecated",)
    con.close()


def test_migration_refuses_inconsistent_active_rows_and_changes_nothing(pre_ranged_db):
    _legacy_row(pre_ranged_db, "a", 1, "M1", 5000, 0.011, 1)
    _legacy_row(pre_ranged_db, "a", 2, "M1", 5000, 0.012, 1)        # two ACTIVE rows, same pump+target
    before = _snapshot(pre_ranged_db)
    with pytest.raises(RuntimeError, match="REFUSED"):
        database.init_db()
    database.reset_engine()
    cols = {r[1] for r in sqlite3.connect(pre_ranged_db).execute("PRAGMA table_info(tuning_profiles)")}
    assert "status" not in cols and _snapshot(pre_ranged_db) == before
    audit = sqlite3.connect(pre_ranged_db).execute(
        "SELECT step, outcome FROM schema_migration_audit WHERE migration='ranged_profiles'").fetchall()
    assert ("preflight", "FAIL") in audit


def test_migration_resumes_after_a_partial_column_add(pre_ranged_db):
    _legacy_row(pre_ranged_db, "a", 1, "M1", 5000, 0.011, 1)
    con = sqlite3.connect(pre_ranged_db)
    con.execute("ALTER TABLE tuning_profiles ADD COLUMN target_min_g INTEGER NULL")   # half-done
    con.commit()
    con.close()
    database.init_db()
    row = sqlite3.connect(pre_ranged_db).execute(
        "SELECT status, target_min_g, target_max_g FROM tuning_profiles").fetchone()
    assert row == ("active", 5000, 5000)
