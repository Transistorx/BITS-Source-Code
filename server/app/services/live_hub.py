import asyncio
import hmac
import logging
import time
from collections import deque
from dataclasses import dataclass
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field

from ..config import settings
from .errors import ServiceError
from .live_state import live_state

log = logging.getLogger(__name__)

DEVICE_SETTLE_MS = 2000
LOCK_MARGIN_MS = 1000
ACK_SLOTS = 8
CMDS_PER_SECOND = 1
CMDS_PER_MINUTE = 10
REJECTIONS_PER_MINUTE = 20
ALLOWED_CMDS = ("ZERO", "TARE")
ALLOWED_CHANNELS = ("CH1", "CH2")
TERMINAL_ACK_STATES = ("APPLIED", "FAILED")
CLOSE_POLICY = 1008
CLOSE_INTERNAL = 1011


class WsAuthIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    type: Literal["auth"]
    api_key: str = Field(max_length=256)


class WsCmdIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    type: Literal["cmd"]
    device_id: str = Field(min_length=1, max_length=64)
    channel: Literal["CH1", "CH2"]
    cmd: Literal["ZERO", "TARE"]


class WsPingIn(BaseModel):
    model_config = ConfigDict(extra="forbid")
    type: Literal["ping"]


@dataclass
class WsCmdState:
    status: str
    reason: str
    device_id: str = ""
    channel: str = ""
    command_id: int | None = None

    def frame(self) -> dict:
        return {"type": "cmd_state", "command_id": self.command_id,
                "device_id": self.device_id, "channel": self.channel,
                "status": self.status, "reason": self.reason}


class RateWindow:
    def __init__(self, limits):
        self._limits = tuple(limits)
        self._span = max(w for _, w in self._limits) if self._limits else 0.0
        self._hits = deque()

    def _prune(self, now):
        while self._hits and now - self._hits[0] > self._span:
            self._hits.popleft()

    def count(self, now, window_s=None) -> int:
        self._prune(now)
        if window_s is None:
            return len(self._hits)
        return sum(1 for t in self._hits if now - t <= window_s)

    def would_allow(self, now) -> bool:
        self._prune(now)
        return all(self.count(now, w) < n for n, w in self._limits)

    def record(self, now) -> int:
        self._prune(now)
        self._hits.append(now)
        return len(self._hits)


class HubFull(RuntimeError):
    pass


class ClientSlot:
    def __init__(self, slot_id: int, ws, authed: bool, loopback: bool = False):
        self.id = slot_id
        self.ws = ws
        self.authed = authed
        self.loopback = loopback
        self.dirty: dict[str, None] = {}
        self.acks: deque = deque(maxlen=ACK_SLOTS)
        self.wake = asyncio.Event()
        self.rate = RateWindow(((CMDS_PER_SECOND, 1.0), (CMDS_PER_MINUTE, 60.0)))
        self.rejections = RateWindow(((REJECTIONS_PER_MINUTE, 60.0),))
        self.closed = False
        self.close_code: int | None = None
        self.close_reason = ""
        self.gone: asyncio.Future | None = None
        self.task: asyncio.Task | None = None


@dataclass
class InFlight:
    command_id: int | None
    device_id: str
    channel: str
    slot_id: int
    client_deadline: float = 0.0
    lock_deadline: float = 0.0
    unknown_sent: bool = False
    client_timer: asyncio.TimerHandle | None = None
    lock_timer: asyncio.TimerHandle | None = None

    @property
    def key(self):
        return (self.device_id, self.channel)


class LiveHub:
    def __init__(self):
        self._loop: asyncio.AbstractEventLoop | None = None
        self._slots: dict[int, ClientSlot] = {}
        self._next_id = 1
        self._inflight: dict[tuple[str, str], InFlight] = {}
        self._by_id: dict[int, InFlight] = {}
        self._fanout_failures = 0
        self._fanout_failures_total = 0
        self._fanout_unbound = 0
        self._ack_unknown = 0
        self._ack_unrouted = 0
        self._acks_dropped = 0
        self._sends_timed_out = 0
        self._sends_failed = 0
        self._pending_auth = 0
        self._refused_full = 0

    def bind_loop(self, loop) -> None:
        self._loop = loop
        self._fanout_failures = 0

    @property
    def loop_bound(self) -> bool:
        return self._loop is not None

    def health(self) -> dict:
        return {"loop_bound": self._loop is not None, "clients": len(self._slots),
                "in_flight": len(self._inflight),
                "fanout_failures": self._fanout_failures,
                "fanout_failures_total": self._fanout_failures_total,
                "fanout_unbound": self._fanout_unbound,
                "ack_unknown": self._ack_unknown, "ack_unrouted": self._ack_unrouted,
                "acks_dropped": self._acks_dropped,
                "sends_timed_out": self._sends_timed_out,
                "sends_failed": self._sends_failed,
                "pending_auth": self._pending_auth,
                "refused_full": self._refused_full}

    def _handoff(self, fn, *args) -> bool:
        loop = self._loop
        if loop is None:
            self._fanout_unbound += 1
            return False
        try:
            loop.call_soon_threadsafe(fn, *args)
            return True
        except Exception:
            self._fanout_failures += 1
            self._fanout_failures_total += 1
            return False

    def on_weight_threadsafe(self, device_id: str) -> None:
        self._handoff(self._mark_dirty, device_id)

    def on_ack_threadsafe(self, command_id: int, device_id: str, body: dict) -> None:
        self._handoff(self._on_ack, command_id, device_id, dict(body or {}))

    def on_reboot_threadsafe(self, device_id: str) -> None:
        self._handoff(self._on_reboot, device_id)

    def on_link_lost_threadsafe(self) -> None:
        self._handoff(self._on_link_lost)

    def on_link_up_threadsafe(self) -> None:
        self._handoff(self._broadcast, self._broker_frame("CONNECTED"))

    def bind_id_threadsafe(self, inf: "InFlight", command_id: int) -> None:
        if not self._handoff(self._bind_id, inf, command_id):
            log.warning("live ws command %d id handoff dropped: loop gone", command_id)

    @staticmethod
    def now() -> float:
        return time.monotonic()

    @staticmethod
    def _broker_frame(state: str | None = None) -> dict:
        if state is None:
            from .. import mqtt_bridge
            state = mqtt_bridge.mqtt_state().get("state", "UNKNOWN")
        return {"type": "broker", "state": state}

    def _mark_dirty(self, device_id: str) -> None:
        for slot in self._slots.values():
            if not slot.closed:
                slot.dirty[device_id] = None
                slot.wake.set()

    def push(self, slot: ClientSlot, frame: dict) -> None:
        if slot.closed:
            return
        if len(slot.acks) == slot.acks.maxlen:
            self._acks_dropped += 1
        slot.acks.append(frame)
        slot.wake.set()

    def _broadcast(self, frame: dict) -> None:
        for slot in list(self._slots.values()):
            self.push(slot, frame)

    def _release(self, inf: InFlight) -> None:
        for handle in (inf.client_timer, inf.lock_timer):
            if handle is not None:
                handle.cancel()
        inf.client_timer = inf.lock_timer = None
        if self._inflight.get(inf.key) is inf:
            del self._inflight[inf.key]
        if inf.command_id is not None and self._by_id.get(inf.command_id) is inf:
            del self._by_id[inf.command_id]

    def _notify(self, inf: InFlight, status: str, reason: str) -> None:
        slot = self._slots.get(inf.slot_id)
        if slot is not None:
            self.push(slot, WsCmdState(status, reason, inf.device_id, inf.channel,
                                         inf.command_id).frame())

    def _on_ack(self, command_id: int, device_id: str, body: dict) -> None:
        inf = self._by_id.get(command_id)
        if inf is None or inf.device_id != device_id:
            self._ack_unknown += 1
            log.info("live ws ack for unknown command %s from %s dropped", command_id, device_id)
            return
        state = body.get("state")
        if state not in TERMINAL_ACK_STATES:
            return
        late = self.now() > inf.client_deadline
        self._release(inf)
        frame = {"type": "cmd_ack", "command_id": command_id, "device_id": inf.device_id,
                 "channel": inf.channel, "state": state, "result": body.get("result"),
                 "reason": body.get("reason"), "weight_g": body.get("weight_g"),
                 "late": late}
        slot = self._slots.get(inf.slot_id)
        if slot is None:
            self._ack_unrouted += 1
            return
        self.push(slot, frame)

    def _on_reboot(self, device_id: str) -> None:
        for inf in [i for i in self._inflight.values() if i.device_id == device_id]:
            self._release(inf)
            self._notify(inf, "rebooted", "device rebooted; outcome unknown")

    def _on_link_lost(self) -> None:
        for inf in list(self._inflight.values()):
            self._release(inf)
            self._notify(inf, "link_lost", "link lost; outcome unknown")
        self._broadcast(self._broker_frame("DISCONNECTED"))

    def _client_timeout(self, inf: InFlight) -> None:
        if self._inflight.get(inf.key) is not inf:
            return
        inf.unknown_sent = True
        left = max(0, int((inf.lock_deadline - self.now()) * 1000))
        self._notify(inf, "unknown",
                     f"no ack within {settings.live_ws_client_timeout_ms} ms; outcome unknown; "
                     f"channel locked for {left} ms")

    def _lock_expired(self, inf: InFlight) -> None:
        if self._inflight.get(inf.key) is not inf:
            return
        self._release(inf)
        self._notify(inf, "lock_released", "ttl window elapsed without ack; outcome unknown")

    def _bind_id(self, inf: InFlight, command_id: int) -> None:
        if self._inflight.get(inf.key) is inf and inf.command_id is None:
            inf.command_id = command_id
            self._by_id[command_id] = inf

    def full(self) -> bool:
        return len(self._slots) + self._pending_auth >= settings.max_ws_clients

    def pending_begin(self) -> bool:
        if self.full():
            self._refused_full += 1
            return False
        self._pending_auth += 1
        return True

    def pending_end(self) -> None:
        self._pending_auth = max(0, self._pending_auth - 1)

    def register(self, ws, authed: bool = False, loopback: bool = False) -> ClientSlot:
        loop = self._loop
        if loop is None:
            raise RuntimeError("live hub loop not bound")
        if len(self._slots) >= settings.max_ws_clients:
            self._refused_full += 1
            raise HubFull("too many live ws clients")
        slot = ClientSlot(self._next_id, ws, authed, loopback)
        self._next_id += 1
        slot.gone = loop.create_future()
        self._slots[slot.id] = slot
        slot.acks.append({"type": "snapshot", **live_state.snapshot()})
        slot.acks.append(self._broker_frame())
        slot.wake.set()
        slot.task = loop.create_task(self._pump(slot))
        return slot

    def unregister(self, slot: ClientSlot) -> None:
        self._slots.pop(slot.id, None)
        slot.closed = True
        if slot.gone is not None and not slot.gone.done():
            slot.gone.set_result(None)
        task = slot.task
        if task is not None and not task.done():
            try:
                current = asyncio.current_task()
            except RuntimeError:
                current = None
            if task is not current:
                task.cancel()

    def request_close(self, slot: ClientSlot, code: int, reason: str) -> None:
        if slot.closed:
            return
        slot.close_code, slot.close_reason = code, reason
        self.unregister(slot)

    async def _close(self, slot: ClientSlot, code: int, reason: str) -> None:
        if slot.closed:
            return
        slot.close_code, slot.close_reason = code, reason
        self.unregister(slot)
        try:
            await asyncio.wait_for(slot.ws.close(code=code, reason=reason[:120]), 1.0)
        except Exception:
            pass

    async def _send(self, slot: ClientSlot, frame: dict) -> None:
        try:
            await asyncio.wait_for(slot.ws.send_json(frame),
                                   settings.live_ws_send_timeout_ms / 1000)
        except asyncio.TimeoutError:
            self._sends_timed_out += 1
            log.warning("live ws client %d send timeout; closing", slot.id)
            await self._close(slot, CLOSE_INTERNAL, "send timeout")
        except asyncio.CancelledError:
            raise
        except Exception:
            self._sends_failed += 1
            await self._close(slot, CLOSE_INTERNAL, "send failed")

    def _sender_frame(self, device_id: str) -> dict | None:
        view = live_state.sender_view(device_id)
        if view is None:
            return None
        revision, body = view
        return {"type": "sender", "device_id": device_id, "revision": revision, **body}

    async def _pump(self, slot: ClientSlot) -> None:
        try:
            while not slot.closed:
                await slot.wake.wait()
                slot.wake.clear()
                while not slot.closed and slot.acks:
                    await self._send(slot, slot.acks.popleft())
                while not slot.closed and slot.dirty:
                    device_id = next(iter(slot.dirty))
                    del slot.dirty[device_id]
                    frame = self._sender_frame(device_id)
                    if frame is not None:
                        await self._send(slot, frame)
        except asyncio.CancelledError:
            raise
        except Exception:
            log.exception("live ws pump failed for client %d", slot.id)
            await self._close(slot, CLOSE_INTERNAL, "internal error")

    def auth_allowed(self, slot: ClientSlot) -> bool:
        if slot.authed:
            return True
        return not settings.api_key and settings.bench_open_writes and slot.loopback

    @staticmethod
    def key_matches(api_key: str | None) -> bool:
        expected = settings.api_key
        if not expected or not isinstance(api_key, str):
            return False
        return hmac.compare_digest(api_key.encode("utf-8", "replace"),
                                   expected.encode("utf-8", "replace"))

    @staticmethod
    def reads_gated() -> bool:
        return settings.reads_require_key and bool(settings.api_key)

    def note_rejection(self, slot: ClientSlot) -> bool:
        return slot.rejections.record(self.now()) >= REJECTIONS_PER_MINUTE

    def _refuse(self, slot: ClientSlot, msg: WsCmdIn, reason: str) -> WsCmdState:
        self.note_rejection(slot)
        return WsCmdState("refused", reason, msg.device_id, msg.channel)

    @staticmethod
    def _create_row(device_id: str, channel: str, cmd: str, on_id) -> dict:
        from ..database import get_db
        from .control import ControlIn, scale_command
        from ..models import DeviceCommand
        gen = get_db()
        db = next(gen)
        try:
            result = scale_command(db, ControlIn(action=cmd, channel_id=channel,
                                                 device_id=device_id))
            command_id = int(result["command_id"])
            on_id(command_id)
            row = db.get(DeviceCommand, command_id)
            if row is None or row.published_at is None:
                if row is not None and row.state == "PENDING":
                    row.state, row.error_text = "FAILED", "not published"
                    db.commit()
                raise ServiceError("unavailable", "command not published", 503)
            return result
        finally:
            gen.close()

    async def submit_cmd(self, slot: ClientSlot, msg: WsCmdIn) -> WsCmdState:
        from .. import mqtt_bridge
        now = self.now()
        if not self.auth_allowed(slot):
            return self._refuse(slot, msg, "not authenticated")
        state = mqtt_bridge.mqtt_state().get("state")
        if state != "CONNECTED":
            return self._refuse(slot, msg, f"broker link {state}")
        if msg.device_id not in settings.live_ws_device_whitelist:
            return self._refuse(slot, msg, "device not whitelisted")
        if msg.cmd not in ALLOWED_CMDS or msg.channel not in ALLOWED_CHANNELS:
            return self._refuse(slot, msg, "command not allowed")
        if not slot.rate.would_allow(now):
            return self._refuse(slot, msg, "rate limited")
        key = (msg.device_id, msg.channel)
        if key in self._inflight:
            return self._refuse(slot, msg, "channel busy")
        loop = self._loop
        if loop is None or loop.is_closed():
            return self._refuse(slot, msg, "live hub stopping")
        inf = InFlight(None, msg.device_id, msg.channel, slot.id)
        self._inflight[key] = inf

        def on_id(command_id: int) -> None:
            self.bind_id_threadsafe(inf, command_id)

        try:
            result = await asyncio.to_thread(self._create_row, msg.device_id, msg.channel,
                                             msg.cmd, on_id)
        except ServiceError as exc:
            self._release(inf)
            return self._refuse(slot, msg, exc.message)
        except Exception:
            log.exception("live ws command row failed")
            self._release(inf)
            return self._refuse(slot, msg, "db error")
        command_id = int(result["command_id"])
        self._bind_id(inf, command_id)
        now = self.now()
        inf.client_deadline = now + settings.live_ws_client_timeout_ms / 1000
        inf.lock_deadline = now + (settings.scale_cmd_ttl_ms + DEVICE_SETTLE_MS
                                   + LOCK_MARGIN_MS) / 1000
        inf.client_timer = loop.call_later(inf.client_deadline - now, self._client_timeout, inf)
        inf.lock_timer = loop.call_later(inf.lock_deadline - now, self._lock_expired, inf)
        slot.rate.record(now)
        return WsCmdState("accepted", "published", msg.device_id, msg.channel, command_id)


live_hub = LiveHub()
