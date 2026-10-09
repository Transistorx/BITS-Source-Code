"""Relay weight cache for ``cas/{sender}/weight/ctl`` (CONTRACT 8.3 and 9.3 notes).

Pure logic with an injectable clock, so the rules are unit-testable without a broker.
A frame is accepted per channel only when:
  * not retained, <= 256 B, strict JSON, integers are plain integers (no 1.0 / 1e3),
  * schema_version absent or 1, boot_id 8 hex (absent = legacy, its own boot),
  * channel CH1|CH2, weight_g 0..100000 (negative = fail safe), age_ms 0..5000,
  * same boot_id: seq strictly greater (wrap-safe); dup/older/uptime regression dropped
    WITHOUT moving the freshness timestamp,
  * new boot_id: only on the 2nd message with seq exactly +1 of the previous message of
    that boot on either channel (shared sender counter) and uptime not going backwards;
    the held message is never used, accepted data is forced stable=False once,
  * transit guard: (local receive ms - uptime_ms) more than 300 ms above the 40 s window
    minimum is rejected.
``get`` returns None when the effective age (age_ms + time since receipt) exceeds 5000 ms.
"""
import json
import re
import threading
import time
from collections import deque

BOOT_RE = re.compile(r"^[0-9a-fA-F]{8}$")
MAX_BYTES = 256
MAX_AGE_MS = 5000
TRANSIT_MS = 300
WINDOW_S = 40.0
MASK = 0xFFFFFFFF


def _no_float(_s):
    raise ValueError("float not allowed")


def _no_const(_s):
    raise ValueError("non-finite")


def _plain_int(v) -> bool:
    return type(v) is int


def _newer(a: int, b: int) -> bool:
    """serial-number arithmetic: a is strictly after b"""
    d = (a - b) & MASK
    return 0 < d < 0x80000000


class WeightCache:
    def __init__(self, clock=time.monotonic):
        self._clock = clock
        self._lock = threading.Lock()
        self._rec: dict[str, dict] = {}       # channel -> accepted record
        self._boot: str | None = None         # boot currently trusted
        self._last: dict | None = None        # last accepted message of the trusted boot (any channel)
        self._held: dict | None = None        # candidate message of an unaccepted boot
        self._window: deque = deque()         # (t, offset_ms)
        self.counts: dict[str, int] = {}

    def _drop(self, why: str) -> str:
        self.counts[why] = self.counts.get(why, 0) + 1
        return why

    def feed(self, raw: bytes, retained: bool = False, now: float | None = None) -> str:
        """-> 'accepted' | 'held' | drop reason."""
        if retained:
            return self._drop("retained")
        if len(raw) > MAX_BYTES:
            return self._drop("oversize")
        try:
            body = json.loads(raw, parse_float=_no_float, parse_constant=_no_const)
        except (ValueError, UnicodeDecodeError):
            return self._drop("malformed")
        if not isinstance(body, dict):
            return self._drop("malformed")
        sv = body.get("schema_version", 1)
        if not _plain_int(sv) or sv != 1:
            return self._drop("schema")
        boot = body.get("boot_id", "legacy")
        if boot != "legacy" and not (isinstance(boot, str) and BOOT_RE.match(boot)):
            return self._drop("boot_id")
        boot = boot.lower() if boot != "legacy" else boot
        seq, up = body.get("seq"), body.get("uptime_ms")
        ch, w, age = body.get("channel"), body.get("weight_g"), body.get("age_ms")
        if not (_plain_int(seq) and 0 <= seq <= MASK and _plain_int(up) and 0 <= up <= MASK):
            return self._drop("seq_uptime")
        if ch not in ("CH1", "CH2"):
            return self._drop("channel")
        if not (_plain_int(w) and 0 <= w <= 100000):
            return self._drop("weight")      # negative or non-integer: fail safe
        if not (_plain_int(age) and 0 <= age <= MAX_AGE_MS):
            return self._drop("age")
        stable = body.get("stable", False)
        if not isinstance(stable, bool):
            return self._drop("stable")
        t = self._clock() if now is None else now
        msg = dict(boot=boot, seq=seq, up=up, ch=ch, w=w, age=age, stable=stable, t=t)
        with self._lock:
            return self._accept(msg)

    def _transit_ok(self, msg) -> bool:
        offset = msg["t"] * 1000.0 - msg["up"]
        while self._window and msg["t"] - self._window[0][0] > WINDOW_S:
            self._window.popleft()
        floor = min((o for _, o in self._window), default=offset)
        if offset - floor > TRANSIT_MS:
            return False
        self._window.append((msg["t"], offset))
        return True

    def _accept(self, msg) -> str:
        if msg["boot"] == self._boot:
            rec = self._rec.get(msg["ch"])
            if rec is not None:
                if not _newer(msg["seq"], rec["seq"]):
                    return self._drop("dup_or_old")
                if not _newer(msg["up"], rec["up"]) and msg["up"] != rec["up"]:
                    return self._drop("uptime_regression")
            if self._last is not None and not _newer(msg["seq"], self._last["seq"]) \
                    and msg["ch"] == self._last["ch"]:
                return self._drop("dup_or_old")
            if not self._transit_ok(msg):
                return self._drop("transit")
            self._store(msg, force_unstable=False)
            return "accepted"
        # new boot: needs a consecutive predecessor of the same boot
        held = self._held
        if held and held["boot"] == msg["boot"] and ((msg["seq"] - held["seq"]) & MASK) == 1 \
                and (msg["up"] - held["up"]) & MASK < 0x80000000 and msg["boot"] != "legacy":
            self._boot, self._rec, self._last, self._held = msg["boot"], {}, None, None
            self._window.clear()
            self._transit_ok(msg)
            self._store(msg, force_unstable=True)
            return "accepted"
        if msg["boot"] == "legacy":              # no boot_id: legacy behaviour, own boot
            self._boot, self._rec, self._last, self._held = "legacy", {}, None, None
            self._transit_ok(msg)
            self._store(msg, force_unstable=False)
            return "accepted"
        self._held = msg
        return self._drop("held_new_boot")

    def _store(self, msg, force_unstable: bool) -> None:
        self._rec[msg["ch"]] = dict(seq=msg["seq"], up=msg["up"], w=msg["w"], age=msg["age"],
                                    stable=msg["stable"] and not force_unstable, t=msg["t"])
        self._last = msg

    def get(self, ch: str, now: float | None = None):
        """-> (weight_g, stable, effective_age_ms) or None when absent/stale (> 5000 ms)."""
        t = self._clock() if now is None else now
        with self._lock:
            rec = self._rec.get(ch)
            if rec is None:
                return None
            eff = rec["age"] + int((t - rec["t"]) * 1000)
            if eff > MAX_AGE_MS:
                return None
            return rec["w"], rec["stable"], eff

    def last_valid_age_s(self, ch: str, now: float | None = None) -> float | None:
        t = self._clock() if now is None else now
        with self._lock:
            rec = self._rec.get(ch)
            return None if rec is None else t - rec["t"]
