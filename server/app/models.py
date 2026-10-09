"""SQLAlchemy models — dispense_runs, weight_samples, dispense_events.

Contract: docs/telemetry/CONTRACT.md §2. All timestamps are naive UTC.

Telemetry is append-only on the write path: the device never overwrites a
sample, and there are no purge jobs or cascade deletes configured on these
tables. The single exception is an explicit operator action —
``DELETE /api/v1/runs/{run_id}`` — which removes exactly one run together with
its own ``weight_samples`` and ``dispense_events`` rows in one transaction.
That route is the only place anything here is deleted, and it is scoped by the
``run_id`` primary key so it can never reach another run's rows.
"""

from datetime import datetime

from sqlalchemy import (
    event,
    BigInteger,
    Boolean,
    DateTime,
    Float,
    ForeignKey,
    Index,
    Integer,
    JSON,
    SmallInteger,
    String,
    UniqueConstraint,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column
from sqlalchemy.dialects.mysql import DATETIME as MYSQL_DATETIME

# Millisecond precision on MySQL; plain DATETIME elsewhere (tests/SQLite).
TS = DateTime().with_variant(MYSQL_DATETIME(fsp=3), "mysql")
BIG_ID = BigInteger().with_variant(Integer, "sqlite")


class Base(DeclarativeBase):
    pass


class Material(Base):
    """Editable operator labels over a fixed, safety-critical hardware map."""
    __tablename__ = "materials"

    material_id: Mapped[str] = mapped_column(String(8), primary_key=True)
    name: Mapped[str] = mapped_column(String(64), nullable=False)
    channel_id: Mapped[str] = mapped_column(String(8), unique=True, nullable=False)
    pump_id: Mapped[str] = mapped_column(String(16), unique=True, nullable=False)
    relay_id: Mapped[str] = mapped_column(String(16), unique=True, nullable=False)
    scale_id: Mapped[str] = mapped_column(String(16), unique=True, nullable=False)
    enabled: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)


class DispenseRun(Base):
    __tablename__ = "dispense_runs"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    material_id: Mapped[str] = mapped_column(String(8), default="M1", index=True)
    channel_id: Mapped[str] = mapped_column(String(8), default="CH1", index=True)
    pump_id: Mapped[str] = mapped_column(String(16), default="Pump 1", index=True)
    relay_id: Mapped[str] = mapped_column(String(16), default="Relay 1", index=True)
    scale_id: Mapped[str] = mapped_column(String(16), default="Scale 1", index=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True, default="")
    job_id: Mapped[int] = mapped_column(Integer, default=0)
    test_number: Mapped[int] = mapped_column(Integer, default=0)
    target_g: Mapped[int] = mapped_column(Integer, index=True)
    priority: Mapped[int] = mapped_column(SmallInteger, default=0)
    status: Mapped[str] = mapped_column(
        String(16), default="RUNNING", index=True
    )
    assigned_at: Mapped[datetime | None] = mapped_column(TS, nullable=True, index=True)
    started_at: Mapped[datetime] = mapped_column(TS, index=True)
    completed_at: Mapped[datetime | None] = mapped_column(TS, nullable=True)
    duration_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)

    # PID gains actually used for this run
    kp: Mapped[float | None] = mapped_column(Float, nullable=True)
    ki: Mapped[float | None] = mapped_column(Float, nullable=True)
    kd: Mapped[float | None] = mapped_column(Float, nullable=True)
    integral_max: Mapped[float | None] = mapped_column(Float, nullable=True)

    # Configuration snapshot
    tolerance_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    coarse_transition_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    max_overshoot_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    settle_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    max_duration_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    window_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    min_on_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    min_off_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    correction_limit: Mapped[int | None] = mapped_column(Integer, nullable=True)
    completion_mode: Mapped[str | None] = mapped_column(String(32), nullable=True)
    profile_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    profile_version: Mapped[int | None] = mapped_column(Integer, nullable=True)
    # Authoritative link to the exact tuning_profiles row (NOT a foreign key, so
    # history survives a deleted profile). profile_id / profile_version /
    # material_id above stay as the immutable human-readable snapshot.
    profile_version_id: Mapped[int | None] = mapped_column(BIG_ID, nullable=True, index=True)
    firmware: Mapped[str | None] = mapped_column(String(64), nullable=True)
    config_snapshot: Mapped[dict | None] = mapped_column(JSON, nullable=True)

    # Results
    start_weight_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    final_weight_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    final_error_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    overshoot_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    max_weight_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    corrections: Mapped[int | None] = mapped_column(Integer, nullable=True)
    error_text: Mapped[str | None] = mapped_column(String(120), nullable=True)

    sample_count: Mapped[int] = mapped_column(Integer, default=0)
    event_count: Mapped[int] = mapped_column(Integer, default=0)
    created_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)
    # CONTRACT 9.2 (MQTT run lifecycle). boot_id of the relay boot that produced the
    # run; missing_ranges = gaps found at completion; ingest_json = the relay's
    # end_seq/total_* so a late batch can re-evaluate COMPLETE_PARTIAL. All NULL for
    # runs written over HTTP.
    boot_id: Mapped[str | None] = mapped_column(String(16), nullable=True)
    missing_ranges: Mapped[dict | None] = mapped_column(JSON, nullable=True)
    ingest_json: Mapped[dict | None] = mapped_column(JSON, nullable=True)

    __table_args__ = (
        Index("ix_runs_target_started", "target_g", "started_at"),
        Index("ix_runs_channel_target_started", "channel_id", "target_g", "started_at"),
        Index("ix_runs_profile_version", "profile_id", "profile_version"),
    )


class WeightSample(Base):
    __tablename__ = "weight_samples"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    run_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("dispense_runs.run_id"), index=True
    )
    material_id: Mapped[str] = mapped_column(String(8), default="M1", index=True)
    channel_id: Mapped[str] = mapped_column(String(8), default="CH1", index=True)
    idx: Mapped[int] = mapped_column(Integer)
    timestamp: Mapped[datetime] = mapped_column(TS, index=True)
    elapsed_ms: Mapped[int] = mapped_column(Integer, default=0)
    uptime_ms: Mapped[int | None] = mapped_column(BigInteger, nullable=True)
    seq: Mapped[int | None] = mapped_column(Integer, nullable=True)
    source: Mapped[str] = mapped_column(String(16), default="CAS")
    weight_g: Mapped[int] = mapped_column(Integer)
    target_g: Mapped[int] = mapped_column(Integer)
    error_g: Mapped[int] = mapped_column(Integer, default=0)
    stable: Mapped[bool] = mapped_column(Boolean, default=False)
    weight_age_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    p_term: Mapped[float | None] = mapped_column(Float, nullable=True)
    i_term: Mapped[float | None] = mapped_column(Float, nullable=True)
    d_term: Mapped[float | None] = mapped_column(Float, nullable=True)
    pid_output: Mapped[float | None] = mapped_column(Float, nullable=True)
    relay1: Mapped[bool] = mapped_column(Boolean, default=False)
    relay2: Mapped[bool] = mapped_column(Boolean, default=False)
    state: Mapped[str] = mapped_column(String(24), default="")

    __table_args__ = (
        # (run_id, idx) is the client idempotency key: retried batches never
        # duplicate samples.
        UniqueConstraint("run_id", "idx", name="uq_sample_run_idx"),
        Index("ix_samples_run_elapsed", "run_id", "elapsed_ms"),
        Index("ix_samples_channel_timestamp", "channel_id", "timestamp"),
    )


class DispenseEvent(Base):
    __tablename__ = "dispense_events"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    run_id: Mapped[str] = mapped_column(
        String(64), ForeignKey("dispense_runs.run_id"), index=True
    )
    material_id: Mapped[str] = mapped_column(String(8), default="M1", index=True)
    idx: Mapped[int] = mapped_column(Integer)
    timestamp: Mapped[datetime] = mapped_column(TS, index=True)
    elapsed_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    event: Mapped[str] = mapped_column(String(32), index=True)
    state: Mapped[str | None] = mapped_column(String(24), nullable=True)
    weight_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    detail: Mapped[str | None] = mapped_column(String(240), nullable=True)

    __table_args__ = (
        UniqueConstraint("run_id", "idx", name="uq_event_run_idx"),
    )


class RunBatch(Base):
    """One stored MQTT run item (CONTRACT 9.2). Primary key (run_id, batch_seq) is
    the idempotency key: start = 0, samples and events share the counter. Written in
    the same transaction as the data it describes, so a row here means "committed"."""
    __tablename__ = "run_batches"

    run_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    batch_seq: Mapped[int] = mapped_column(Integer, primary_key=True)
    kind: Mapped[str] = mapped_column(String(8))          # start / samples / events
    item_count: Mapped[int] = mapped_column(Integer, default=0)
    received_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)


class ServerMeta(Base):
    """Tiny key/value table (last telemetry receipt, etc.)."""

    __tablename__ = "server_meta"

    key: Mapped[str] = mapped_column(String(64), primary_key=True)
    value: Mapped[str] = mapped_column(String(190), default="")


class SchemaMigrationAudit(Base):
    """Append-only audit of schema migration steps (never updated or deleted).

    Written on a SEPARATE connection by ``migration_state`` so rows survive the
    implicit commits of MySQL DDL and the rollback of a failed step.
    """
    __tablename__ = "schema_migration_audit"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    ts: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)
    migration: Mapped[str] = mapped_column(String(64), index=True)
    step: Mapped[str] = mapped_column(String(64))
    outcome: Mapped[str] = mapped_column(String(8))   # START / OK / FAIL / SKIP
    detail: Mapped[str] = mapped_column(String(500), default="")


class TuningProfile(Base):
    """One immutable PID version for one fixed pump.

    ``profile_version_id`` is the identity. ``profile_id`` is only a label that
    several pumps may share, and ``version`` is numbered independently per
    (profile_id, material_id): Material A and Material B can both own
    "default v8" as two distinct rows.
    """
    __tablename__ = "tuning_profiles"

    profile_version_id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    profile_id: Mapped[str] = mapped_column(String(64))
    version: Mapped[int] = mapped_column(Integer)
    material_id: Mapped[str] = mapped_column(String(8), default="M1", index=True)
    channel_id: Mapped[str] = mapped_column(String(8), index=True)
    # NOTE: nullable at the DB level only to avoid a migration; the API layer
    # (schemas.TuningProfileIn) requires target_g to be exactly one of the four
    # canonical targets (5000/10000/15000/20000) and never stores NULL/0.
    # Do not read a NULL target_g as a "channel-wide fallback".
    target_g: Mapped[int | None] = mapped_column(Integer, nullable=True, index=True)
    kp: Mapped[float] = mapped_column(Float)
    ki: Mapped[float] = mapped_column(Float)
    kd: Mapped[float] = mapped_column(Float)
    tolerance_g: Mapped[int] = mapped_column(Integer)
    max_overshoot_g: Mapped[int] = mapped_column(Integer)
    max_duration_ms: Mapped[int] = mapped_column(Integer)
    window_ms: Mapped[int] = mapped_column(Integer)
    min_on_ms: Mapped[int] = mapped_column(Integer)
    min_off_ms: Mapped[int] = mapped_column(Integer)
    active: Mapped[bool] = mapped_column(Boolean, default=False, index=True)
    created_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)

    # --- ranged profiles (CONTRACT 9.3). All additive; a schema-1 row keeps its
    # exact legacy meaning: applicability 'exact', range [target_g, target_g].
    # ``target_g`` stays for legacy readers (= target_min_g for a ranged row).
    applicability: Mapped[str] = mapped_column(String(8), default="exact", server_default="exact")
    target_min_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    target_max_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    # draft | validated | active | deprecated. ``active`` above is the derived mirror.
    status: Mapped[str] = mapped_column(String(12), default="draft", server_default="draft")
    coarse_threshold_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    fine_threshold_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    micro_threshold_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    coarse_min_on_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    fine_min_on_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    micro_min_on_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    settle_time_ms: Mapped[int | None] = mapped_column(Integer, nullable=True)
    inflight_comp_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    validated_min_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    validated_max_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    validation_ref: Mapped[str | None] = mapped_column(String(190), nullable=True)
    flow_regime_note: Mapped[str | None] = mapped_column(String(255), nullable=True)
    # 1 = legacy canonical (11-field wire), 2 = ranged (staged fields in the wire).
    profile_schema: Mapped[int] = mapped_column(SmallInteger, default=1, server_default="1")
    status_changed_at: Mapped[datetime | None] = mapped_column(TS, nullable=True)
    status_changed_by: Mapped[str | None] = mapped_column(String(64), nullable=True)

    __table_args__ = (
        UniqueConstraint("profile_id", "material_id", "version",
                         name="uq_tuning_profile_material_version"),
        Index("ix_tuning_profiles_mat_chan_status", "material_id", "channel_id", "status"),
        # SQLite (tests/dev) would otherwise reuse the id of a deleted last row.
        {"sqlite_autoincrement": True},
    )


@event.listens_for(TuningProfile, "before_insert")
def _tuning_profile_defaults(_mapper, _connection, row: TuningProfile) -> None:
    """Keep the additive columns coherent for rows built without them (legacy
    callers and tests): the range of an unranged row is [target_g, target_g], and
    ``status`` follows ``active`` (active -> 'active', else 'draft'). A row that
    already names its status is never changed; ``active`` is then its mirror."""
    if row.applicability is None:
        row.applicability = "exact"
    if row.profile_schema is None:
        row.profile_schema = 1
    if row.target_min_g is None and row.target_max_g is None and row.target_g is not None:
        row.target_min_g = row.target_max_g = row.target_g
    if row.status is None:
        row.status = "active" if row.active else "draft"
    row.active = row.status == "active"


class ProfileStatusLog(Base):
    """Append-only status history of a tuning profile (never updated or deleted)."""
    __tablename__ = "profile_status_log"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    profile_version_id: Mapped[int] = mapped_column(BIG_ID, index=True)
    ts: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)
    from_status: Mapped[str | None] = mapped_column(String(12), nullable=True)
    to_status: Mapped[str] = mapped_column(String(12))
    actor: Mapped[str | None] = mapped_column(String(64), nullable=True)
    detail: Mapped[str | None] = mapped_column(String(190), nullable=True)


class DeviceStatus(Base):
    __tablename__ = "device_status"

    device_id: Mapped[str] = mapped_column(String(64), primary_key=True)
    status_json: Mapped[dict] = mapped_column(JSON, default=dict)
    updated_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow, index=True)
    # CONTRACT 8.4: command_transport HTTP (default) or MQTT; MQTT only if the
    # birth caps contain cmd_mqtt. boot_id / caps_json come from the birth and
    # are never taken from the polled status body.
    command_transport: Mapped[str] = mapped_column(String(8), default="HTTP",
                                                   server_default="HTTP")
    boot_id: Mapped[str | None] = mapped_column(String(16), nullable=True)
    caps_json: Mapped[list | None] = mapped_column(JSON, nullable=True)


class DeviceCommand(Base):
    __tablename__ = "device_commands"

    id: Mapped[int] = mapped_column(BIG_ID, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(String(64), index=True, default="")
    command_type: Mapped[str] = mapped_column(String(16), default="JOB", index=True)
    target_g: Mapped[int | None] = mapped_column(Integer, nullable=True)
    material_id: Mapped[str | None] = mapped_column(String(8), nullable=True, index=True)
    priority: Mapped[int | None] = mapped_column(SmallInteger, nullable=True)
    local_job_id: Mapped[int | None] = mapped_column(Integer, nullable=True)
    channel_id: Mapped[str | None] = mapped_column(String(8), nullable=True)
    pump_id: Mapped[str | None] = mapped_column(String(16), nullable=True)
    assigned_at: Mapped[datetime | None] = mapped_column(TS, nullable=True)
    payload_json: Mapped[dict | None] = mapped_column(JSON, nullable=True)
    # Exact immutable tuning version this job is pinned to. Not a foreign key,
    # for the same reason dispense_runs.profile_* is not: history and the
    # delivery record must stay readable after a profile version is deleted.
    profile_id: Mapped[str | None] = mapped_column(String(64), nullable=True, index=True)
    profile_version: Mapped[int | None] = mapped_column(Integer, nullable=True)
    # Surrogate id of the exact tuning_profiles row (also not a foreign key).
    # profile_id / profile_version / material_id stay as the readable snapshot.
    profile_version_id: Mapped[int | None] = mapped_column(BIG_ID, nullable=True, index=True)
    state: Mapped[str] = mapped_column(String(16), default="PENDING", index=True)
    # Operator queue controls. For a JOB the server still owns (PENDING, never
    # delivered) these are the source of truth: held keeps the row out of
    # /device/commands and promoted puts it ahead of its peers. For a job the
    # device already owns they only mirror the ESP32, which is the one that
    # decides what actually starts. Never applied to a RUNNING job.
    held: Mapped[bool] = mapped_column(Boolean, default=False)
    promoted: Mapped[bool] = mapped_column(Boolean, default=False)
    error_text: Mapped[str | None] = mapped_column(String(120), nullable=True)
    # Extra ACK fields (result, reason, weight_g, stable) from ZERO/TARE ACKs.
    ack_json: Mapped[dict | None] = mapped_column(JSON, nullable=True)
    # Set (atomically, before the publish) when the MQTT hook hands the row to
    # the device. With state DELIVERED it is the "handed out by either
    # transport" record: a PENDING row with published_at set may already be in
    # the device queue, so it is never reissued or cancelled server-side.
    published_at: Mapped[datetime | None] = mapped_column(TS, nullable=True)
    # Which transport won the PENDING -> DELIVERED claim (HTTP / MQTT) and the
    # device boot_id known at that moment (CONTRACT 8.4). NULL on legacy rows.
    delivered_via: Mapped[str | None] = mapped_column(String(8), nullable=True)
    delivered_boot_id: Mapped[str | None] = mapped_column(String(16), nullable=True)
    created_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow, index=True)
    updated_at: Mapped[datetime] = mapped_column(TS, default=datetime.utcnow)

    __table_args__ = (
        Index("ix_device_commands_local_job", "local_job_id"),
        Index("ix_device_commands_channel_state", "channel_id", "state"),
        Index("ix_device_commands_device_state_held", "device_id", "state", "held"),
    )
