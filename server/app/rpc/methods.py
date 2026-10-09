"""Method table for the operator plane (CONTRACT 9.4 catalogue).

method -> Method(min role, pydantic arg schema, handler). Handlers are thin: they
call app/services/* and return ``(data, next_cursor)``. They never touch the broker
and never drive a relay; control.cmd only creates the same DeviceCommand row the
HTTP route creates, and the firmware stays the safety authority.
"""

import base64
import json
from dataclasses import dataclass, field
from datetime import datetime
from typing import Annotated, Any, Callable, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictBool, StrictInt, StrictStr
from sqlalchemy import select
from sqlalchemy.orm import Session

from ..models import DeviceCommand
from ..schemas import TuningProfileBulkIn, TuningProfileIn
from ..services import control as control_service
from ..services import history as history_service
from ..services import materials as materials_service
from ..services import profiles as profile_service
from ..services import queue as queue_service
from ..services import runs as run_service
from ..services.auth import Principal
from ..services.control import ControlIn
from ..services.materials import MaterialLabelIn
from ..services.queue import QueueJobIn, ResubmitIn
from .envelope import RpcError

Pos = Annotated[StrictInt, Field(ge=1)]
Limit50 = Annotated[StrictInt, Field(ge=1, le=50)]
Mat = Annotated[StrictStr, Field(pattern="^(M1|M2)$")]
Chan = Annotated[StrictStr, Field(pattern="^(CH1|CH2)$")]
RunId = Annotated[StrictStr, Field(min_length=1, max_length=64)]
ProfId = Annotated[StrictStr, Field(min_length=1, max_length=64, pattern=r"^[A-Za-z0-9._-]+$")]
Grams = Annotated[StrictInt, Field(gt=0, le=1_000_000)]


class Args(BaseModel):
    model_config = ConfigDict(extra="forbid", populate_by_name=True)


class NoArgs(Args):
    pass


class PageArgs(Args):
    limit: Limit50 = 50
    cursor: StrictStr | None = Field(default=None, max_length=64)


class LoginArgs(Args):
    username: StrictStr = Field(min_length=1, max_length=64)
    password: StrictStr = Field(min_length=1, max_length=256, repr=False)


class RunsLatestArgs(Args):
    material_id: Mat | None = None
    channel_id: Chan | None = None
    target_g: Grams | None = None
    limit: Limit50 = 10


class RunsSearchArgs(PageArgs):
    material_id: Mat | None = None
    channel_id: Chan | None = None
    target_g: Grams | None = None
    from_time: datetime | None = Field(default=None, alias="from")
    to_time: datetime | None = Field(default=None, alias="to")
    run_id: RunId | None = None
    status: StrictStr | None = Field(default=None, max_length=16)
    result: StrictStr | None = Field(default=None, max_length=16)
    profile_id: ProfId | None = None
    profile_version: Pos | None = None
    profile_version_id: Pos | None = None


class RunIdArgs(Args):
    run_id: RunId


class RunEventsArgs(PageArgs):
    run_id: RunId


class RunSamplesArgs(Args):
    run_id: RunId
    from_seq: Annotated[StrictInt, Field(ge=0)] = 0
    limit: Annotated[StrictInt, Field(ge=1, le=2000)] = 500
    max_points: Annotated[StrictInt, Field(ge=4, le=2000)] | None = None
    cursor: StrictStr | None = Field(default=None, max_length=64)


class JobCreateArgs(Args):
    # Any positive weight; the per-pump maximum (config PUMPn_MAX_TARGET_G, default
    # 20000 g) is enforced by the service. The queue is not a Test Graph. With no
    # pin the service resolves the single active compatible profile or refuses.
    material_id: Mat
    target_g: Grams
    priority: Annotated[StrictInt, Field(ge=0, le=1)] = 0
    profile_id: ProfId | None = None
    profile_version: Pos | None = None
    profile_version_id: Pos | None = None


class CommandIdArgs(Args):
    command_id: Pos


class ResubmitArgs(CommandIdArgs):
    confirm_unknown: StrictBool = False


class ControlArgs(ControlIn):
    model_config = ConfigDict(extra="forbid")


class MaterialUpdateArgs(Args):
    material_id: Mat
    name: StrictStr = Field(min_length=1, max_length=64)


class ProfileVersionArgs(Args):
    profile_version_id: Pos


class ProfilesListArgs(PageArgs):
    material_id: Mat | None = None
    channel_id: Chan | None = None
    target_g: StrictInt | None = None
    status: Literal["draft", "validated", "active", "deprecated"] | None = None


class ProfilesActiveArgs(Args):
    material_id: Mat
    channel_id: Chan
    target_g: Grams


class ProfilesCoverageArgs(Args):
    material_id: Mat | None = None


class ProfilesResolveArgs(Args):
    material_id: Mat
    target_g: Grams


class ProfileValidateArgs(ProfileVersionArgs):
    validation_ref: StrictStr = Field(min_length=1, max_length=190)
    validated_min_g: Annotated[StrictInt, Field(ge=1, le=100_000)]
    validated_max_g: Annotated[StrictInt, Field(ge=1, le=100_000)]


class NextVersionArgs(Args):
    profile_id: ProfId
    materials: StrictStr = Field(default="M1,M2", max_length=8)


class GraphArgs(PageArgs):
    target_g: StrictInt
    material_id: Mat | None = None
    status: StrictStr = Field(default="COMPLETE", max_length=16)


class ConfirmBeginArgs(Args):
    method: StrictStr = Field(min_length=1, max_length=48)
    args: dict = Field(default_factory=dict)


@dataclass
class Ctx:
    db: Session
    principal: Principal | None
    client_id: str
    now: datetime
    dispatcher: Any = None


Handler = Callable[[Ctx, Any], tuple[Any, str | None]]


@dataclass(frozen=True)
class Method:
    name: str
    min_role: str | None            # None = no session needed (auth.login)
    schema: type[Args]
    handler: Handler
    write: bool = False             # audited + response persisted for dedupe
    history: bool = False           # counts toward the in-flight history cap
    dangerous: Callable[[Session, dict], bool] | None = None
    store: bool = True              # False: response holds a secret, never stored


# ---- cursor helpers -------------------------------------------------------

def enc_cursor(**kw) -> str:
    return base64.urlsafe_b64encode(json.dumps(kw, separators=(",", ":")).encode()).decode().rstrip("=")


def dec_cursor(cursor: str | None, key: str = "o") -> int:
    if not cursor:
        return 0
    try:
        value = json.loads(base64.urlsafe_b64decode(cursor + "=" * (-len(cursor) % 4)))[key]
        if type(value) is int and 0 <= value <= 10_000_000:
            return value
    except Exception:
        pass
    raise RpcError("INVALID", "bad cursor")


def _page_list(items: list, a: PageArgs) -> tuple[list, str | None]:
    off = dec_cursor(a.cursor)
    page = items[off:off + a.limit]
    return page, (enc_cursor(o=off + a.limit) if off + a.limit < len(items) else None)


# ---- read handlers --------------------------------------------------------

def h_live_resync(c: Ctx, a):
    return c.dispatcher.snapshot_bundle(c.db, c.now), None


def h_dashboard(c: Ctx, a):
    return run_service.dashboard(c.db), None


def h_runs_latest(c: Ctx, a: RunsLatestArgs):
    items = run_service.latest_runs(c.db, a.material_id, a.channel_id, a.target_g, a.limit)
    return {"items": items}, None


def h_targets(c: Ctx, a: PageArgs):
    page, nxt = _page_list(run_service.targets(c.db), a)
    return {"items": page}, nxt


def _search(c: Ctx, a: RunsSearchArgs, **over):
    off = dec_cursor(a.cursor)
    f = dict(material_id=a.material_id, channel_id=a.channel_id, target_g=a.target_g,
             from_time=a.from_time, to_time=a.to_time, run_id=a.run_id, status=a.status,
             result=a.result, profile_id=a.profile_id, profile_version=a.profile_version,
             profile_version_id=a.profile_version_id)
    f.update(over)
    out = run_service.search_runs(c.db, limit=a.limit, offset=off, **f)
    nxt = enc_cursor(o=off + a.limit) if off + a.limit < out.total else None
    return {"items": out.items, "total": out.total}, nxt


def h_runs_search(c: Ctx, a: RunsSearchArgs):
    return _search(c, a)


def h_run_get(c: Ctx, a: RunIdArgs):
    return run_service.run_detail(c.db, a.run_id), None


def h_run_events(c: Ctx, a: RunEventsArgs):
    off = dec_cursor(a.cursor)
    page = history_service.run_events_page(c.db, a.run_id, offset=off, limit=a.limit)
    more = page.pop("more")
    return page, (enc_cursor(o=off + a.limit) if more else None)


def h_run_samples(c: Ctx, a: RunSamplesArgs):
    start = dec_cursor(a.cursor, "s") if a.cursor else a.from_seq
    page = history_service.run_samples_page(c.db, a.run_id, from_seq=start, limit=a.limit,
                                            max_points=a.max_points or 0)
    nxt = page.pop("next_seq")
    return page, (enc_cursor(s=nxt) if nxt is not None else None)


def h_queue_list(c: Ctx, a: PageArgs):
    q = queue_service.read_queue(c.db)
    off = dec_cursor(a.cursor)
    waiting = q["waiting_commands"]
    q["waiting_commands"] = waiting[off:off + a.limit]
    if off:
        q["failed_commands"] = []
    return q, (enc_cursor(o=off + a.limit) if off + a.limit < len(waiting) else None)


def h_command_get(c: Ctx, a: CommandIdArgs):
    return control_service.read_control_command(c.db, a.command_id), None


def h_materials_list(c: Ctx, a):
    return {"items": materials_service.read_materials(c.db)}, None


def h_profiles_list(c: Ctx, a: ProfilesListArgs):
    rows = profile_service.list_profiles(c.db, a.material_id, a.channel_id, a.target_g, a.status)
    page, nxt = _page_list(rows, a)
    return {"items": page}, nxt


def h_profiles_active(c: Ctx, a: ProfilesActiveArgs):
    """Every ACTIVE profile whose range contains the target (0, 1 or several)."""
    items = profile_service.active_profiles(c.db, a.material_id, a.channel_id, a.target_g)
    # Response cap (64 KB): at most 20 profiles are listed; "count" is the true total.
    return {"target_g": a.target_g, "count": len(items), "items": items[:20]}, None


def h_profiles_coverage(c: Ctx, a: ProfilesCoverageArgs):
    return profile_service.coverage(c.db, a.material_id), None


def h_profiles_resolve(c: Ctx, a: ProfilesResolveArgs):
    return profile_service.resolve_preview(c.db, a.material_id, a.target_g), None


def h_profiles_nextversion(c: Ctx, a: NextVersionArgs):
    return profile_service.next_version_preview(c.db, a.profile_id, a.materials), None


def h_graphs_category(c: Ctx, a: GraphArgs):
    history_service.check_graph_target(a.target_g)
    sa = RunsSearchArgs(limit=a.limit, cursor=a.cursor, material_id=a.material_id,
                        target_g=a.target_g, status=a.status)
    return _search(c, sa)


# ---- write handlers -------------------------------------------------------

def h_job_create(c: Ctx, a: JobCreateArgs):
    if a.profile_version_id is None and (a.profile_id is None) != (a.profile_version is None):
        raise RpcError("INVALID", "pin with profile_version_id, or profile_id and profile_version together")
    # model_construct: the RPC layer already validated the args; the service checks the
    # per-pump maximum and resolves/validates the profile for this exact target.
    payload = QueueJobIn.model_construct(**a.model_dump())
    return queue_service.create_job(c.db, payload), None


def _job(db: Session, command_id: int) -> DeviceCommand | None:
    job = db.get(DeviceCommand, command_id)
    if job is not None and job.command_type == "JOB":
        return queue_service.follow_successor(db, job) or job
    return None


def job_cancel_is_dangerous(db: Session, args: dict) -> bool:
    cid = args.get("command_id")
    job = _job(db, cid) if type(cid) is int else None
    return job is not None and job.state == "RUNNING"


def h_job_cancel(c: Ctx, a: CommandIdArgs):
    job = _job(c.db, a.command_id)
    if job is not None and job.state == "RUNNING":
        # Active job: same CANCEL the control route issues (gated by confirm.begin here).
        if job.local_job_id is None and not job.channel_id:
            raise RpcError("CONFLICT", "active job has no local_job_id or channel to cancel")
        return control_service.control_command(c.db, ControlIn(
            action="CANCEL", local_job_id=job.local_job_id,
            channel_id=None if job.local_job_id else job.channel_id)), None
    return queue_service.cancel_job(c.db, a.command_id), None


def h_job_resubmit(c: Ctx, a: ResubmitArgs):
    return queue_service.resubmit_job(c.db, a.command_id,
                                      ResubmitIn(confirm_unknown=a.confirm_unknown)), None


def control_is_dangerous(db: Session, args: dict) -> bool:
    return args.get("action") in ("CLEAR", "PUMP_START")   # ESTOP/PUMP_STOP/CANCEL never


def h_control(c: Ctx, a: ControlArgs):
    return control_service.control_command(c.db, ControlIn(**a.model_dump())), None


def h_material_update(c: Ctx, a: MaterialUpdateArgs):
    return materials_service.rename_material(c.db, a.material_id, MaterialLabelIn(name=a.name)), None


def h_run_delete(c: Ctx, a: RunIdArgs):
    return run_service.delete_run(c.db, a.run_id), None


def _actor(c: Ctx) -> str | None:
    return c.principal.username if c.principal else None


def h_profile_create(c: Ctx, a: dict):
    return profile_service.create_profile(c.db, a, actor=_actor(c)), None


def h_profile_bulk(c: Ctx, a):
    return profile_service.create_profiles_bulk(c.db, a, actor=_actor(c)), None


def _row(c: Ctx, a: ProfileVersionArgs):
    return profile_service.row_by_id(c.db, a.profile_version_id)


def h_profile_activate(c: Ctx, a):
    return profile_service.activate(c.db, _row(c, a), actor=_actor(c)), None


def h_profile_deactivate(c: Ctx, a):
    return profile_service.deactivate(c.db, _row(c, a), actor=_actor(c)), None


def h_profile_validate(c: Ctx, a: ProfileValidateArgs):
    return profile_service.validate(
        c.db, _row(c, a), validation_ref=a.validation_ref, validated_min_g=a.validated_min_g,
        validated_max_g=a.validated_max_g, actor=_actor(c)), None


def h_profile_deprecate(c: Ctx, a):
    return profile_service.deprecate(c.db, _row(c, a), actor=_actor(c)), None


def h_profile_delete(c: Ctx, a):
    return profile_service.delete(c.db, _row(c, a)), None


def h_confirm_begin(c: Ctx, a: ConfirmBeginArgs):
    return c.dispatcher.confirm_begin(c, a), None


def _always(db, args):
    return True


class ProfileCreateArgs(TuningProfileIn):
    model_config = ConfigDict(extra="forbid")


class ProfileBulkArgs(TuningProfileBulkIn):
    model_config = ConfigDict(extra="forbid")


def _m(name, role, schema, handler, **kw) -> Method:
    return Method(name, role, schema, handler, **kw)


METHODS: dict[str, Method] = {m.name: m for m in (
    _m("auth.login", None, LoginArgs, None, write=True, store=False),  # handled by dispatcher
    _m("live.resync", "viewer", NoArgs, h_live_resync),
    _m("dashboard.get", "viewer", NoArgs, h_dashboard, history=True),
    _m("runs.latest", "viewer", RunsLatestArgs, h_runs_latest, history=True),
    _m("targets.list", "viewer", PageArgs, h_targets, history=True),
    _m("runs.search", "viewer", RunsSearchArgs, h_runs_search, history=True),
    _m("run.get", "viewer", RunIdArgs, h_run_get, history=True),
    _m("run.events", "viewer", RunEventsArgs, h_run_events, history=True),
    _m("run.samples", "viewer", RunSamplesArgs, h_run_samples, history=True),
    _m("run.delete", "admin", RunIdArgs, h_run_delete, write=True, dangerous=_always),
    _m("queue.list", "viewer", PageArgs, h_queue_list),
    _m("queue.job.create", "operator", JobCreateArgs, h_job_create, write=True),
    _m("queue.job.cancel", "operator", CommandIdArgs, h_job_cancel, write=True,
       dangerous=job_cancel_is_dangerous),
    _m("queue.job.resubmit", "operator", ResubmitArgs, h_job_resubmit, write=True),
    _m("control.cmd", "operator", ControlArgs, h_control, write=True, dangerous=control_is_dangerous),
    _m("command.get", "viewer", CommandIdArgs, h_command_get),
    _m("materials.list", "viewer", NoArgs, h_materials_list),
    _m("materials.update", "admin", MaterialUpdateArgs, h_material_update, write=True),
    _m("profiles.list", "viewer", ProfilesListArgs, h_profiles_list),
    _m("profiles.active", "viewer", ProfilesActiveArgs, h_profiles_active),
    _m("profiles.nextversion", "viewer", NextVersionArgs, h_profiles_nextversion),
    _m("profiles.create", "operator", ProfileCreateArgs, h_profile_create, write=True),
    _m("profiles.bulk", "operator", ProfileBulkArgs, h_profile_bulk, write=True, dangerous=_always),
    _m("profiles.coverage", "viewer", ProfilesCoverageArgs, h_profiles_coverage),
    _m("profiles.resolve", "viewer", ProfilesResolveArgs, h_profiles_resolve),
    _m("profiles.validate", "admin", ProfileValidateArgs, h_profile_validate, write=True),
    _m("profiles.activate", "operator", ProfileVersionArgs, h_profile_activate, write=True,
       dangerous=_always),
    _m("profiles.deprecate", "admin", ProfileVersionArgs, h_profile_deprecate, write=True,
       dangerous=_always),
    _m("profiles.deactivate", "admin", ProfileVersionArgs, h_profile_deactivate, write=True,
       dangerous=_always),
    _m("profiles.delete", "admin", ProfileVersionArgs, h_profile_delete, write=True,
       dangerous=_always),
    _m("graphs.category", "viewer", GraphArgs, h_graphs_category, history=True),
    _m("confirm.begin", "operator", ConfirmBeginArgs, h_confirm_begin, store=False),
)}

# STOP-class control actions are never rate limited, never gated.
UNLIMITED_ACTIONS = ("ESTOP", "PUMP_STOP", "CANCEL")


def is_unlimited(method: str, args: dict) -> bool:
    return method == "control.cmd" and args.get("action") in UNLIMITED_ACTIONS
