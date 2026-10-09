"""Pydantic request/response models — contract §3 and §4.

Strict where the ESP32 writes (bounds catch firmware bugs and hostile input),
permissive on reads. Every write model rejects unknown fields is deliberately
NOT enabled: additive firmware fields must not break ingestion, so extra keys
are ignored (pydantic default).
"""

from datetime import datetime
from enum import Enum
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

RUN_ID_MAX = 64
RUN_ID_PATTERN = r"^[A-Za-z0-9._-]{1,64}$"

# The only targets a tuning profile may address. The ESP32 refuses any other
# target (including 0, which used to be the "channel-wide fallback" bleed key),
# so the server rejects them at the API layer before they can be stored.
CANONICAL_TARGETS_G = (5000, 10000, 15000, 20000)


# ---------------------------------------------------------------------------
# Writes (ESP32 -> server)
# ---------------------------------------------------------------------------

class RunConfigIn(BaseModel):
    kp: float | None = Field(default=None, allow_inf_nan=False)
    ki: float | None = Field(default=None, allow_inf_nan=False)
    kd: float | None = Field(default=None, allow_inf_nan=False)
    integral_max: float | None = Field(default=None, ge=0, allow_inf_nan=False)
    tolerance_g: int | None = Field(default=None, ge=0)
    coarse_transition_g: int | None = Field(default=None, ge=0)
    settle_ms: int | None = Field(default=None, ge=0)
    max_duration_ms: int | None = Field(default=None, ge=0)
    max_overshoot_g: int | None = Field(default=None, ge=0)
    window_ms: int | None = Field(default=None, ge=0)
    min_on_ms: int | None = Field(default=None, ge=0)
    min_off_ms: int | None = Field(default=None, ge=0)
    correction_limit: int | None = Field(default=None, ge=0)
    completion_mode: str | None = Field(default=None, max_length=32)


class RunStartIn(BaseModel):
    run_id: str = Field(pattern=RUN_ID_PATTERN)
    material_id: Literal["M1", "M2"]
    pump_id: Literal["Pump 1", "Pump 2"] | None = None
    channel_id: Literal["CH1", "CH2"] = "CH1"
    relay_id: Literal["Relay 1", "Relay 2"] | None = None
    scale_id: Literal["Scale 1", "Scale 2"] | None = None
    device_id: str = Field(default="", max_length=RUN_ID_MAX)
    job_id: int = Field(default=0, ge=0)
    target_g: int = Field(gt=0, le=1_000_000)
    priority: int = Field(default=0, ge=0, le=1)
    firmware: str | None = Field(default=None, max_length=RUN_ID_MAX)
    start_weight_g: int | None = Field(default=None, ge=0, le=1_000_000)
    profile_id: str | None = Field(default=None, max_length=64)
    profile_version: int | None = Field(default=None, ge=1)
    config: RunConfigIn | None = None

    @model_validator(mode="after")
    def fixed_material_mapping(self):
        channel = "CH1" if self.material_id == "M1" else "CH2"
        relay = "Relay 1" if channel == "CH1" else "Relay 2"
        scale = "Scale 1" if channel == "CH1" else "Scale 2"
        pump = "Pump 1" if channel == "CH1" else "Pump 2"
        if self.channel_id != channel:
            raise ValueError("material_id does not match the fixed channel mapping")
        if self.relay_id is not None and self.relay_id != relay:
            raise ValueError("relay override conflicts with the fixed material mapping")
        if self.scale_id is not None and self.scale_id != scale:
            raise ValueError("scale override conflicts with the fixed material mapping")
        if self.pump_id is not None and self.pump_id != pump:
            raise ValueError("pump override conflicts with the fixed material mapping")
        return self


class RunStartOut(BaseModel):
    run_id: str
    test_number: int
    status: str
    assigned_at: datetime | None = None
    started_at: datetime


class SampleIn(BaseModel):
    material_id: Literal["M1", "M2"]
    channel_id: Literal["CH1", "CH2"] = "CH1"
    idx: int = Field(ge=1)
    uptime_ms: int | None = Field(default=None, ge=0)
    elapsed_ms: int = Field(ge=0)
    seq: int | None = Field(default=None, ge=0, le=4_294_967_295)
    source: Literal["CAS"] = "CAS"
    simulated: bool = False
    weight_g: int = Field(ge=0, le=1_000_000)
    target_g: int = Field(gt=0, le=1_000_000)
    error_g: int | None = None
    stable: bool = False
    weight_age_ms: int | None = Field(default=None, ge=0)
    p_term: float | None = Field(default=None, allow_inf_nan=False)
    i_term: float | None = Field(default=None, allow_inf_nan=False)
    d_term: float | None = Field(default=None, allow_inf_nan=False)
    pid_output: float | None = Field(default=None, ge=0, le=1, allow_inf_nan=False)
    relay1: bool = False
    relay2: bool = False
    state: str = Field(default="", max_length=24)

    @model_validator(mode="after")
    def cas_only(self):
        if self.simulated:
            raise ValueError("simulated samples are not accepted for tuning")
        if self.error_g is not None and self.error_g != self.target_g - self.weight_g:
            raise ValueError("error_g must equal target_g - weight_g")
        return self


class TelemetryBatchIn(BaseModel):
    run_id: str = Field(pattern=RUN_ID_PATTERN)
    material_id: Literal["M1", "M2"]
    channel_id: Literal["CH1", "CH2"] = "CH1"
    device_id: str = Field(default="", max_length=RUN_ID_MAX)
    samples: list[SampleIn] = Field(min_length=1, max_length=500)

    @field_validator("samples")
    @classmethod
    def unique_sample_indices(cls, samples):
        idxs = [sample.idx for sample in samples]
        if len(idxs) != len(set(idxs)):
            raise ValueError("sample idx values must be unique within a batch")
        return samples

    @model_validator(mode="after")
    def one_channel_per_batch(self):
        if any(sample.channel_id != self.channel_id or sample.material_id != self.material_id
               for sample in self.samples):
            raise ValueError("sample material_id and channel_id must match batch")
        if self.channel_id != ("CH1" if self.material_id == "M1" else "CH2"):
            raise ValueError("material_id does not match the fixed channel mapping")
        return self


class EventName(str, Enum):
    JOB_CREATED = "JOB_CREATED"
    JOB_ASSIGNED = "JOB_ASSIGNED"
    JOB_STARTED = "JOB_STARTED"
    COARSE_STARTED = "COARSE_STARTED"
    COARSE_STOPPED = "COARSE_STOPPED"
    FINE_STARTED = "FINE_STARTED"
    TARGET_REACHED = "TARGET_REACHED"
    SETTLING_STARTED = "SETTLING_STARTED"
    FINE_CORRECTION = "FINE_CORRECTION"
    JOB_COMPLETE = "JOB_COMPLETE"
    JOB_FAILED = "JOB_FAILED"
    JOB_CANCELLED = "JOB_CANCELLED"
    OVERWEIGHT = "OVERWEIGHT"
    WEIGHT_LINK_LOST = "WEIGHT_LINK_LOST"
    PAUSE = "PAUSE"
    RESUME = "RESUME"
    CANCEL = "CANCEL"
    EMERGENCY_STOP = "EMERGENCY_STOP"
    STATE_CHANGE = "STATE_CHANGE"


class EventIn(BaseModel):
    idx: int = Field(ge=1)
    event: EventName
    elapsed_ms: int | None = Field(default=None, ge=0)
    state: str | None = Field(default=None, max_length=24)
    weight_g: int | None = None
    detail: str | None = Field(default=None, max_length=240)


class EventsIn(BaseModel):
    run_id: str = Field(pattern=RUN_ID_PATTERN)
    material_id: Literal["M1", "M2"]
    channel_id: Literal["CH1", "CH2"] = "CH1"
    device_id: str = Field(default="", max_length=RUN_ID_MAX)
    events: list[EventIn] = Field(min_length=1, max_length=100)

    @model_validator(mode="after")
    def fixed_material_mapping(self):
        if self.channel_id != ("CH1" if self.material_id == "M1" else "CH2"):
            raise ValueError("material_id does not match the fixed channel mapping")
        return self

    @field_validator("events")
    @classmethod
    def unique_event_indices(cls, events):
        idxs = [event.idx for event in events]
        if len(idxs) != len(set(idxs)):
            raise ValueError("event idx values must be unique within a batch")
        return events


class BatchOut(BaseModel):
    run_id: str | None = None
    accepted: int
    inserted: int
    duplicates: int


class RunCompleteIn(BaseModel):
    status: Literal["COMPLETE", "FAILED", "CANCELLED"]
    final_weight_g: int | None = Field(default=None, ge=-1_000_000, le=1_000_000)
    max_weight_g: int | None = Field(default=None, ge=-1_000_000, le=1_000_000)
    overshoot_g: int | None = None
    duration_ms: int | None = Field(default=None, ge=0)
    corrections: int | None = Field(default=None, ge=0)
    error: str | None = Field(default=None, max_length=120)


# ---------------------------------------------------------------------------
# Reads (server -> dashboard)
# ---------------------------------------------------------------------------

class RunCard(BaseModel):
    run_id: str
    material_id: Literal["M1", "M2"] = "M1"
    pump_id: str = "Pump 1"
    channel_id: Literal["CH1", "CH2"] = "CH1"
    relay_id: str = "Relay 1"
    scale_id: str = "Scale 1"
    test_number: int
    device_id: str
    job_id: int
    target_g: int
    priority: int
    status: str
    assigned_at: datetime | None = None
    started_at: datetime
    completed_at: datetime | None = None
    duration_ms: int | None = None
    kp: float | None = None
    ki: float | None = None
    kd: float | None = None
    profile_id: str | None = None
    profile_version: int | None = None
    # Exact tuning_profiles row (null for legacy/unresolvable runs, which keep
    # profile_id + profile_version as their readable snapshot).
    profile_version_id: int | None = None
    start_weight_g: int | None = None
    final_weight_g: int | None = None
    final_error_g: int | None = None
    final_error_pct: float | None = None
    overshoot_g: int | None = None
    max_weight_g: int | None = None
    corrections: int | None = None
    error_text: str | None = None
    sample_count: int = 0
    event_count: int = 0


class RunDetail(RunCard):
    integral_max: float | None = None
    tolerance_g: int | None = None
    coarse_transition_g: int | None = None
    max_overshoot_g: int | None = None
    settle_ms: int | None = None
    max_duration_ms: int | None = None
    window_ms: int | None = None
    min_on_ms: int | None = None
    min_off_ms: int | None = None
    correction_limit: int | None = None
    completion_mode: str | None = None
    firmware: str | None = None
    created_at: datetime | None = None
    config_snapshot: dict | None = None


class TuningProfileFields(BaseModel):
    """Gains/limits shared by the single and the save-both request, and the read model."""
    profile_id: str = Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$")
    # Legacy exact target. For a ranged row it mirrors target_min_g (legacy readers).
    target_g: int | None = None
    kp: float = Field(ge=0, le=1, allow_inf_nan=False)
    ki: float = Field(ge=0, le=1, allow_inf_nan=False)
    kd: float = Field(ge=0, le=1, allow_inf_nan=False)
    tolerance_g: int = Field(ge=0, le=100_000)
    max_overshoot_g: int = Field(ge=0, le=100_000)
    max_duration_ms: int = Field(gt=0, le=3_600_000)
    window_ms: int = Field(ge=50, le=10_000)
    min_on_ms: int = Field(ge=1, le=5_000)
    min_off_ms: int = Field(ge=1, le=5_000)
    # Ranged profiles (CONTRACT 9.3). Omit ALL of these for a legacy canonical
    # (schema 1) profile.
    applicability: Literal["exact", "range", "general"] | None = None
    target_min_g: int | None = Field(default=None, ge=1, le=100_000)
    target_max_g: int | None = Field(default=None, ge=1, le=100_000)
    coarse_threshold_g: int | None = Field(default=None, ge=0, le=100_000)
    fine_threshold_g: int | None = Field(default=None, ge=0, le=100_000)
    micro_threshold_g: int | None = Field(default=None, ge=0, le=100_000)
    coarse_min_on_ms: int | None = Field(default=None, ge=1, le=10_000)
    fine_min_on_ms: int | None = Field(default=None, ge=1, le=10_000)
    micro_min_on_ms: int | None = Field(default=None, ge=1, le=10_000)
    settle_time_ms: int | None = Field(default=None, ge=1, le=60_000)
    inflight_comp_g: int | None = Field(default=None, ge=0, le=10_000)
    flow_regime_note: str | None = Field(default=None, max_length=255)

    @model_validator(mode="after")
    def times_fit_window(self):
        if self.min_on_ms > self.window_ms or self.min_off_ms > self.window_ms:
            raise ValueError("minimum relay times must not exceed the control window")
        return self


class RangedInput(TuningProfileFields):
    """Create-time rules. Legacy body (no range/staged members): schema 1, one of
    the four canonical targets. Anything else: schema 2, checked at worst case
    target = target_min_g against the firmware consistency rules + the stall rule."""
    profile_schema: int = 1

    @model_validator(mode="after")
    def resolve_range(self):
        from .services import profile_rules as rules   # pure helpers, no DB

        staged_given = any(getattr(self, n) is not None for n in rules.STAGED_FIELDS)
        ranged = (self.applicability is not None or self.target_min_g is not None
                  or self.target_max_g is not None or staged_given)
        if not ranged:
            if self.target_g not in CANONICAL_TARGETS_G:
                raise ValueError(
                    f"target_g must be one of {CANONICAL_TARGETS_G} grams for a legacy "
                    f"profile (got {self.target_g}); send applicability and "
                    "target_min_g/target_max_g for any other weight")
            self.applicability, self.profile_schema = "exact", 1
            self.target_min_g = self.target_max_g = self.target_g
            return self
        lo, hi = self.target_min_g, self.target_max_g
        if self.target_g is not None:
            lo = self.target_g if lo is None else lo
            hi = self.target_g if hi is None else hi
        if lo is None or hi is None:
            raise ValueError("target_min_g and target_max_g are required (or target_g for exact)")
        if lo > hi:
            raise ValueError("target_min_g must be <= target_max_g")
        kind = self.applicability or ("exact" if lo == hi else "range")
        if kind == "exact" and lo != hi:
            raise ValueError("an exact profile needs target_min_g == target_max_g")
        if self.target_g is not None and self.target_g != lo:
            raise ValueError("target_g must equal target_min_g when both are given")
        if kind != "exact" and not (self.flow_regime_note or "").strip():
            raise ValueError("flow_regime_note is required for a range or general profile")
        if self.inflight_comp_g is None:
            raise ValueError("inflight_comp_g is required for a ranged (schema 2) profile")
        for name, value in rules.FW_STAGED_DEFAULTS.items():
            if getattr(self, name) is None:
                setattr(self, name, value)
        why = rules.consistency_error(self, lo)
        if why:
            raise ValueError(f"{why} (checked at the lowest target {lo} g)")
        self.applicability, self.profile_schema = kind, 2
        self.target_g, self.target_min_g, self.target_max_g = lo, lo, hi
        return self


class TuningProfileIn(RangedInput):
    material_id: Literal["M1", "M2"]
    channel_id: Literal["CH1", "CH2"]

    @model_validator(mode="after")
    def fixed_material_mapping(self):
        if self.channel_id != ("CH1" if self.material_id == "M1" else "CH2"):
            raise ValueError("material_id does not match the fixed channel mapping")
        return self


class PumpValidation(BaseModel):
    validation_ref: str = Field(min_length=1, max_length=190)
    validated_min_g: int = Field(ge=1, le=100_000)
    validated_max_g: int = Field(ge=1, le=100_000)

    @model_validator(mode="after")
    def ordered(self):
        if self.validated_min_g > self.validated_max_g:
            raise ValueError("validated_min_g must be <= validated_max_g")
        return self


class TuningProfileBulkIn(RangedInput):
    """Atomic save of the same gains for several pumps (one row per material).

    A range/general profile may only be saved for several pumps when each selected
    pump has its own validation evidence in ``per_pump_validation`` (recorded on the
    draft; the row still has to pass profiles.validate). Otherwise it is refused."""
    materials: list[Literal["M1", "M2"]] = Field(min_length=1, max_length=2)
    per_pump_validation: dict[Literal["M1", "M2"], PumpValidation] | None = None

    @field_validator("materials")
    @classmethod
    def unique_materials(cls, v):
        if len(set(v)) != len(v):
            raise ValueError("materials must not repeat")
        return v

    @model_validator(mode="after")
    def ranged_needs_per_pump_validation(self):
        if self.profile_schema == 2 and self.applicability != "exact":
            have = self.per_pump_validation or {}
            missing = [m for m in self.materials if m not in have]
            if missing:
                raise ValueError(
                    "a range/general profile can only be bulk-saved with per_pump_validation "
                    f"for every selected pump (missing {', '.join(missing)})")
        return self


class TuningProfileOut(TuningProfileFields):
    model_config = ConfigDict(from_attributes=True)
    material_id: Literal["M1", "M2"]
    channel_id: Literal["CH1", "CH2"]
    # Immutable identity of this exact row. profile_id + version is NOT unique
    # across pumps: Material A and Material B may both own "default v8".
    profile_version_id: int
    pump_id: str | None = None
    version: int
    active: bool
    created_at: datetime
    profile_schema: int = 1
    status: Literal["draft", "validated", "active", "deprecated"] = "draft"
    validated_min_g: int | None = None
    validated_max_g: int | None = None
    validation_ref: str | None = None
    status_changed_at: datetime | None = None
    status_changed_by: str | None = None
    # Device-side apply state. applied_version is what the ESP32 last reported
    # for this channel; sync_state is IN-SYNC when it matches `version`,
    # OUT-OF-SYNC when it differs, and UNKNOWN when no device has reported.
    applied_version: int | None = None
    sync_state: Literal["IN-SYNC", "OUT-OF-SYNC", "UNKNOWN"] = "UNKNOWN"

class BulkProfilesOut(BaseModel):
    profiles: list[TuningProfileOut]


class SampleOut(BaseModel):
    idx: int
    channel_id: Literal["CH1", "CH2"] = "CH1"
    timestamp: datetime
    elapsed_ms: int
    uptime_ms: int | None = None
    seq: int | None = None
    source: str = "CAS"
    weight_g: int
    target_g: int
    error_g: int
    stable: bool
    weight_age_ms: int | None = None
    p_term: float | None = None
    i_term: float | None = None
    d_term: float | None = None
    pid_output: float | None = None
    relay1: bool
    relay2: bool
    state: str


class EventOut(BaseModel):
    idx: int
    material_id: Literal["M1", "M2"] = "M1"
    timestamp: datetime
    elapsed_ms: int | None = None
    event: str
    state: str | None = None
    weight_g: int | None = None
    detail: str | None = None


class SamplesOut(BaseModel):
    run_id: str
    count: int
    decimated: bool
    samples: list[SampleOut]


class EventsOut(BaseModel):
    run_id: str
    events: list[EventOut]


class TargetSummary(BaseModel):
    avg_final_error_g: float | None = None
    avg_abs_error_g: float | None = None
    avg_overshoot_g: float | None = None
    avg_duration_ms: float | None = None


class TargetGroup(BaseModel):
    material_id: Literal["M1", "M2"] = "M1"
    channel_id: Literal["CH1", "CH2"] = "CH1"
    target_g: int
    run_count: int
    last_run_at: datetime | None = None
    summary: TargetSummary | None = None
    runs: list[RunCard] = []


class DashboardOut(BaseModel):
    generated_at: datetime
    targets: list[TargetGroup]


class RunSearchOut(BaseModel):
    items: list[RunCard]
    total: int
    limit: int
    offset: int


class HealthOut(BaseModel):
    status: str
    database: Literal["up", "down"]
    last_telemetry_at: datetime | None = None
    run_count: int = 0


class LiveOut(BaseModel):
    server_time: datetime
    database: Literal["up", "down"]
    last_telemetry_at: datetime | None = None
    active_run: RunCard | None = None
    is_live: bool = False
    samples: list[SampleOut] = []
    last_sample: SampleOut | None = None
    channels: list["LiveChannelOut"] = []


class LiveChannelOut(BaseModel):
    channel_id: Literal["CH1", "CH2"]
    active_run: RunCard | None = None
    is_live: bool = False
    samples: list[SampleOut] = []
    last_sample: SampleOut | None = None
