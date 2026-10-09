"""Relay-controller simulator (firmware-faithful where the contract is precise).

Safety model mirrored from the firmware, none of it depends on the broker or backend:
  * relays are OFF at construction (boot), before any networking; a reboot is a NEW
    instance (new boot_id, empty queue, local job ids restart, nothing is resumed),
  * weight comes ONLY from cas/{sender}/weight/ctl through WeightCache (8.3 rules); 5 s
    without a valid weight turns the relays OFF and fails the job,
  * a JOB is staged only with a pinned profile whose hash, computed from the received
    text by sim.canon (members sorted, value tokens verbatim), equals profile_hash;
    ``applied_profile`` in the ACK is read back from what it staged,
  * commands are deduped by command_id (32-deep ledger) and executed at most once,
    retained commands are ignored,
  * run lifecycle (9.2): bounded ring (40), <= 2 items in flight, unacked item resent
    after ``resend_s`` and after every reconnect, run/ack trims cumulatively
    (acked_batch_seq == -1 trims nothing), COMPLETE/COMPLETE_PARTIAL/INTERRUPTED/
    REJECTED release the run, UNKNOWN_RUN resends from start (3 repeats max).
"""
import collections
import itertools
import json
import os
import re
import threading
import time
from dataclasses import dataclass, field

from . import canon
from .mqttx import BrokerInfo, Link
from .wcache import WeightCache

CH_OF = {"M1": "CH1", "M2": "CH2"}
MAT_OF = {"CH1": "M1", "CH2": "M2"}
RELEASE_STATES = ("COMPLETE", "COMPLETE_PARTIAL", "INTERRUPTED", "REJECTED")
ACK_STATES = ("START_OK", "BATCH_OK", "UNKNOWN_RUN") + RELEASE_STATES
LEDGER = 32
QUEUE_DEPTH = 8
SAMPLES_PER_BATCH = 5
# run_log.h
RUNLOG_SAMPLE_BYTES = 16384
RUNLOG_TOTAL_BYTES = 24576
RUNLOG_CRITICAL_RESERVE = 4096
RUNLOG_ITEM_MAX = 4864
RUNLOG_RESERVE_PER_CLASS = 2      # start / terminal event / complete: one per channel each
RUNLOG_SLOT_RESERVE = 3 * RUNLOG_RESERVE_PER_CLASS   # slots samples/events never take
RUNLOG_WINDOW = 2
TOMB_BODY_MAX = 256                 # every sample/event body is allocated at least this big
RUNLOG_MAX_RESENDS = 6
RUNLOG_MAX_UNKNOWN = 3

PROFILE_BOUNDS = {          # key: (lo, hi, integer)
    "kp": (0, 1, False), "ki": (0, 1, False), "kd": (0, 1, False),
    "tolerance_g": (0, 100000, True), "max_overshoot_g": (0, 100000, True),
    "max_duration_ms": (1, 3600000, True), "window_ms": (50, 10000, True),
    "min_on_ms": (1, 10000, True), "min_off_ms": (1, 10000, True),
}


def _u32(ms: float) -> int:
    return int(ms) & 0xFFFFFFFF


@dataclass
class Item:
    run_id: str
    seq: int
    kind: str                 # start | samples | events | complete
    topic: str
    payload: str
    n_samples: int = 0
    sent_at: float | None = None
    sends: int = 0
    tomb: bool = False        # dropped batch kept as a tiny placeholder (batch_seq stays)
    ever_sent: bool = False   # published at least once: only such items can be acked
    tries: int = 0            # counted timed-out sends (backend alive) while oldest of its run
    silent: int = 0           # timed-out sends with no run/ack of any kind since: backoff only
    resp_gen: int = 0         # relay response generation when this item was last sent
    terminal: bool = False    # a run's terminal event: topic run/events, deeper slot reserve
    unknown: int = 0          # UNKNOWN_RUN cycles seen
    ord: int = 0

    @property
    def size(self) -> int:    # real byte size of the stored body (tombstones shrink)
        return len(self.payload.encode())

    @property
    def alloc(self) -> int:   # heap really held: samples/events bodies are allocated >= 256 B
        n = self.size + 1
        return max(n, TOMB_BODY_MAX) if self.kind in ("samples", "events") and not self.tomb else n


@dataclass
class Job:
    cid: int | None
    channel: str
    target_g: int
    local_id: int
    profile: dict | None
    phash: str | None
    received: float = field(default_factory=time.monotonic)


class Disp:
    """One run in progress on a channel."""

    def __init__(self, job: Job, run_id: str, now: float):
        self.job, self.run_id, self.t0 = job, run_id, now
        self.seq = 0                  # last batch_seq produced (start = 0)
        self.samples = 0              # idx high-water mark
        self.events = 0
        self.pending: list[dict] = []
        self.next_sample = now
        self.next_pulse = now
        self.pulse_off = 0.0
        self.in_band_since: float | None = None
        self.stage = "ASSIGNED"
        self.dropped = 0
        self.released = False
        self.added_seq = 0            # highest batch_seq the ring accepted (firmware batch_seq counter)
        self.max_w = 0
        self.first_w: int | None = None


class RelaySim:
    def __init__(self, relay_id: str, sender_id: str, plant, broker: BrokerInfo, *,
                 boot_id: str | None = None, weight_timeout_s: float = 5.0,
                 ring_slots: int = 40, resend_s: float = 10.0, max_inflight: int = 2,
                 tamper_applied: bool = False, run_mqtt: bool = True, drop_acks: bool = False, strict_acks: bool = False,
                 sample_period_s: float = 0.1, settle_s: float = 0.35,
                 sample_bytes: int = RUNLOG_SAMPLE_BYTES, total_bytes: int = RUNLOG_TOTAL_BYTES):
        self.id, self.sender, self.plant = relay_id, sender_id, plant
        self.boot_id = boot_id or os.urandom(4).hex()
        self.t0 = time.monotonic()
        self.weight_timeout_s, self.ring_slots = weight_timeout_s, ring_slots
        self.resend_s, self.max_inflight = resend_s, max_inflight
        self.tamper_applied, self.sample_period_s, self.settle_s = tamper_applied, sample_period_s, settle_s
        self.strict_acks = strict_acks             # mirror run_log.c u32_of(): malformed token => whole ack ignored
        self.ack_ignored = 0
        self.sample_bytes, self.total_bytes = sample_bytes, total_bytes
        self.stats: collections.Counter = collections.Counter()   # run_log_stats_t counters
        self.backlog: list[tuple] = []             # adds refused by the ring: the caller keeps and retries them
        self._ord = itertools.count(1)
        self.resp_gen = 0                          # bumps on every valid run/ack (backend demonstrably answered)
        self.unk_owed: dict[str, int] = {}         # run_id -> UNKNOWN_RUN replies still owed by the last cycle
        self.drop_acks = drop_acks                 # ACKs lost on the way: outcome unknown to the backend
        # boot: relays OFF before networking starts
        for ch in ("CH1", "CH2"):
            plant.set_relay(ch, False)
        self.cache = WeightCache()
        self._inbox: collections.deque = collections.deque()
        self.ledger: collections.OrderedDict[int, dict] = collections.OrderedDict()
        self.queue: list[Job] = []
        self.disp: dict[str, Disp | None] = {"CH1": None, "CH2": None}
        self.ring: list[Item] = []
        self._local = itertools.count(1)
        # evidence for tests
        self.executed: list[int | None] = []       # command ids whose dispense STARTED
        self.cmd_log: list[tuple[int, str]] = []   # (command_id, reply state) per ack sent
        self.acks_sent: list[dict] = []
        self.run_acks: list[dict] = []             # run/ack bodies received
        self.runs: dict[str, dict] = {}            # run_id -> {"outcome", "channel", "target"...}
        self.duplicates = 0
        self.refused: list[tuple[int, str]] = []
        self.ring_overflow = 0
        self.dropped_samples = 0
        self.max_ring = 0
        self.relays_off_at: dict[str, float] = {}  # channel -> monotonic when watchdog cut it
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.link = Link(broker, f"dev_{relay_id}", on_message=self._on_msg, on_ready=self._ready,
                         subs=[(f"cas/{sender_id}/weight/ctl", 0), (f"cas/{relay_id}/commands", 1),
                               (f"cas/{relay_id}/run/ack", 1)],
                         will=(f"cas/{relay_id}/status", json.dumps({"online": False}), 1, True))
        self._run_mqtt = run_mqtt
        self._last_status = self._last_live = 0.0

    # -- lifecycle -----------------------------------------------------------
    def start(self) -> "RelaySim":
        self.link.start()
        self._thread = threading.Thread(target=self._loop, name=f"relay-{self.id}", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        self.link.close()
        for ch in ("CH1", "CH2"):
            self.plant.set_relay(ch, False)

    def crash(self) -> None:
        """Power loss: no DISCONNECT, relays drop (hardware default OFF), RAM is gone."""
        self._stop.set()
        self.link.crash()
        for ch in ("CH1", "CH2"):
            self.plant.set_relay(ch, False)

    def uptime_ms(self) -> int:
        return _u32((time.monotonic() - self.t0) * 1000)

    # -- mqtt (paho thread) ------------------------------------------------------
    def _ready(self) -> None:
        self._inbox.append(("ready", None))

    def _on_msg(self, topic: str, payload: bytes, retained: bool) -> None:
        if topic.endswith("/weight/ctl"):
            self.cache.feed(payload, retained)
        elif retained:
            return                                   # retained control is never acted on
        elif topic.endswith("/commands"):
            self._inbox.append(("cmd", payload, time.monotonic()))
        elif topic.endswith("/run/ack"):
            self._inbox.append(("runack", payload))

    # -- control loop ---------------------------------------------------------------
    def _loop(self) -> None:
        while not self._stop.is_set():
            now = time.monotonic()
            try:
                while self._inbox:
                    item = self._inbox.popleft()
                    if item[0] == "ready":
                        self._on_ready(now)
                    elif item[0] == "cmd":
                        self._on_command(item[1], item[2])
                    elif item[0] == "adhoc":
                        self._adhoc(item[1], item[2])
                    else:
                        self._on_run_ack(item[1])
                self._control(now)
                self._telemetry(now)
                self._pump(now)
            except Exception as exc:                 # a sim bug must be loud, not silent
                self.refused.append((-1, f"sim error {exc!r}"))
            time.sleep(0.02)
        # (instance is dead; a reboot is a new instance)

    def _on_ready(self, now: float) -> None:
        self.link.publish(f"cas/{self.id}/status", json.dumps(
            {"online": True, "boot_id": self.boot_id, "caps": ["cmd_mqtt", "weight_mqtt"]}), 1, True)
        for it in self.ring:
            if it.sent_at is not None:
                self.stats["resent"] += 1
            it.sent_at = None                        # resend oldest unacked first after reconnect

    # -- commands ------------------------------------------------------------------
    def _ack(self, cid: int, state: str, job: Job | None = None, error: str | None = None) -> None:
        body = {"command_id": cid, "state": state, "boot_id": self.boot_id}
        if job is not None:
            body.update(local_job_id=job.local_id, channel_id=job.channel)
            if job.phash is not None and state in ("QUEUED", "APPLIED"):
                h = job.phash
                if self.tamper_applied:
                    h = h[:-1] + ("0" if h[-1] != "0" else "1")
                body["applied_profile"] = {"profile_id": job.profile["profile_id"],
                                           "version": job.profile["version"], "hash": h}
        if error:
            body["error"] = error[:79]
            body["reason"] = error[:79]
        self.ledger[cid] = body
        while len(self.ledger) > LEDGER:
            self.ledger.popitem(last=False)
        self.acks_sent.append(body)
        self.cmd_log.append((cid, state))
        if not self.drop_acks:
            self.link.publish(f"cas/{self.id}/commands/ack", json.dumps(body, separators=(",", ":")), 1)

    def _on_command(self, payload: bytes, received: float) -> None:
        try:
            text = payload.decode("utf-8")
            body = json.loads(text)
            cid = body["command_id"]
        except (ValueError, KeyError, TypeError, UnicodeDecodeError):
            return
        if type(cid) is not int or cid < 1:
            return
        if cid in self.ledger:                       # duplicate: re-ACK stored outcome, never re-run
            self.duplicates += 1
            self.link.publish(f"cas/{self.id}/commands/ack",
                              json.dumps(self.ledger[cid], separators=(",", ":")), 1)
            self.cmd_log.append((cid, "DUP_REACK"))
            return
        ctype = body.get("type") or body.get("command_type")
        if ctype == "JOB":
            self._accept_job(cid, body, text)
        elif ctype in ("ESTOP", "PUMP_STOP", "CANCEL", "PAUSE"):
            ch = body.get("channel_id")
            for c in (("CH1", "CH2") if ctype == "ESTOP" else (ch,)):
                if c in self.disp and ctype != "PAUSE":
                    self.plant.set_relay(c, False)
                    if self.disp[c] is not None and ctype in ("ESTOP", "CANCEL"):
                        self._finish(c, "CANCELLED", "JOB_CANCELLED", "operator")
            self._ack(cid, "APPLIED")
        else:
            self._ack(cid, "FAILED", error=f"unsupported in sim: {ctype}")

    def _refuse(self, cid: int, why: str) -> None:
        self.refused.append((cid, why))
        self._ack(cid, "FAILED", error=why)

    def _accept_job(self, cid: int, body: dict, text: str) -> None:
        mat, ch = body.get("material_id"), body.get("channel_id")
        if mat not in CH_OF or (ch is not None and ch != CH_OF[mat]):
            return self._refuse(cid, "channel does not match material")
        ch = CH_OF[mat]
        target = body.get("target_g")
        if type(target) is not int or target <= 0:
            return self._refuse(cid, "PIN_NOT_INTEGER target_g")
        if not isinstance(body.get("profile"), dict):
            return self._refuse(cid, "PROFILE_REQUIRED")
        try:
            canon_text, phash = canon.hash_command_profile(text)
        except canon.CanonError as exc:
            return self._refuse(cid, str(exc.args[0]))
        sent = body.get("profile_hash")
        if sent is not None and (not isinstance(sent, str) or sent.lower() != phash):
            return self._refuse(cid, "PROFILE_HASH_MISMATCH")
        prof = body["profile"]
        if body.get("profile_id") != prof.get("profile_id") or body.get("profile_version") != prof.get("version"):
            return self._refuse(cid, "PIN_INCONSISTENT profile_id/version")
        for key, (lo, hi, integer) in PROFILE_BOUNDS.items():
            v = prof.get(key)
            if type(v) is bool or not isinstance(v, (int, float)):
                return self._refuse(cid, f"PIN_NOT_A_NUMBER {key}")
            if integer and type(v) is not int and v != int(v):
                return self._refuse(cid, f"PIN_NOT_INTEGER {key}")
            if not lo <= v <= hi:
                return self._refuse(cid, f"PIN_OUT_OF_RANGE {key}")
        if not isinstance(prof.get("version"), int) or prof["version"] < 1:
            return self._refuse(cid, "PIN_MALFORMED version")
        if len(self.queue) >= QUEUE_DEPTH:
            return self._refuse(cid, "queue full")
        job = Job(cid, ch, target, next(self._local), prof, phash)
        self.queue.append(job)
        self._ack(cid, "QUEUED", job)

    # -- weight-driven dispensing ----------------------------------------------------
    def _control(self, now: float) -> None:
        for ch in ("CH1", "CH2"):
            w = self.cache.get(ch)
            d = self.disp[ch]
            if w is None:
                if self.plant.relay(ch):             # weight silent > timeout: relays OFF
                    self.plant.set_relay(ch, False)
                    self.relays_off_at.setdefault(ch, now)
                if d is not None:
                    self._finish(ch, "FAILED", "WEIGHT_LINK_LOST", "weight stale")
                continue
            self.relays_off_at.pop(ch, None)
            if d is None:
                if self.plant.relay(ch):
                    self.plant.set_relay(ch, False)
                job = next((j for j in self.queue if j.channel == ch), None)
                if job is not None:
                    self.queue.remove(job)
                    self._begin(job, w[0], now)
                continue
            self._step(ch, d, w, now)

    def begin_adhoc(self, channel: str, target_g: int) -> None:
        """Operator-less manual run (arbitrary target) for history tests; runs on the
        control thread."""
        self._inbox.append(("adhoc", channel, target_g))

    def _adhoc(self, channel: str, target_g: int) -> None:
        w = self.cache.get(channel)
        if w is None or self.disp[channel] is not None:
            self.refused.append((0, "adhoc not started"))
            return
        self._begin(Job(None, channel, target_g, next(self._local), None, None), w[0], time.monotonic())

    def _begin(self, job: Job, w0: int, now: float) -> None:
        up_s = int(now - self.t0)
        run_id = f"{self.id[:24]}-{job.channel.lower()}-j{job.local_id}-{up_s}-{os.urandom(2).hex()}"
        self._supersede(job.channel)
        d = self.disp[job.channel] = Disp(job, run_id, now)
        d.first_w = w0
        self.executed.append(job.cid)
        self.runs[run_id] = {"channel": job.channel, "target": job.target_g, "cid": job.cid,
                             "outcome": None, "disp": d}
        if job.cid is not None:
            self._ack(job.cid, "APPLIED", job)
        prof = job.profile or {}
        cfg = {k: prof[k] for k in ("kp", "ki", "kd", "tolerance_g", "max_overshoot_g",
                                    "max_duration_ms", "window_ms", "min_on_ms", "min_off_ms") if k in prof}
        cfg.update(inflight_comp_g=0, settle_time_ms=int(self.settle_s * 1000))
        start = {"run_id": run_id, "boot_id": self.boot_id, "message_id": f"{self.boot_id}-{run_id[-4:]}",
                 "material_id": MAT_OF[job.channel], "channel_id": job.channel, "device_id": self.id,
                 "job_id": job.local_id, "target_g": job.target_g, "priority": 0,
                 "firmware": "relay-sim", "start_weight_g": w0, "config": cfg}
        if job.cid is not None:
            start["command_id"] = job.cid
        if prof:
            start.update(profile_id=prof.get("profile_id"), profile_version=prof.get("version"))
        self._enqueue(d, "start", 0, start)
        self._event(d, "JOB_ASSIGNED", now, w0)
        self._event(d, "JOB_STARTED", now, w0)

    def _prof(self, d: Disp, key: str, default):
        return (d.job.profile or {}).get(key, default)

    def _step(self, ch: str, d: Disp, w: tuple, now: float) -> None:
        grams = w[0]
        tol = self._prof(d, "tolerance_g", 60)
        over = self._prof(d, "max_overshoot_g", 150)
        max_dur = self._prof(d, "max_duration_ms", 60000) / 1000.0
        d.max_w = max(d.max_w, grams)
        if now >= d.next_sample:
            d.next_sample = now + self.sample_period_s
            d.samples += 1
            d.pending.append({
                "idx": d.samples, "uptime_ms": self.uptime_ms(), "elapsed_ms": int((now - d.t0) * 1000),
                "seq": d.samples, "weight_g": grams, "target_g": d.job.target_g,
                "error_g": d.job.target_g - grams, "stable": bool(w[1]), "weight_age_ms": w[2],
                "p_term": 0.0, "i_term": 0.0, "d_term": 0.0, "pid_output": 0.0,
                "relay1": ch == "CH1" and self.plant.relay(ch), "relay2": ch == "CH2" and self.plant.relay(ch),
                "state": d.stage[:24], "stage": d.stage[:12], "scale_owner": "SENDER", "source": "CAS"})
            if len(d.pending) >= SAMPLES_PER_BATCH:
                self._flush_samples(d)
        if d.pulse_off and now >= d.pulse_off:
            d.pulse_off = 0.0
            self.plant.set_relay(ch, False)
        if grams > d.job.target_g + over:
            self.plant.set_relay(ch, False)
            return self._finish(ch, "FAILED", "OVERWEIGHT", "overweight")
        if now - d.t0 > max_dur:
            self.plant.set_relay(ch, False)
            return self._finish(ch, "FAILED", "JOB_FAILED", "max duration")
        if grams >= d.job.target_g - tol:
            self.plant.set_relay(ch, False)
            if d.in_band_since is None:
                d.in_band_since = now
                d.stage = "SETTLING"
                self._event(d, "TARGET_REACHED", now, grams)
            elif now - d.in_band_since >= self.settle_s:
                self._finish(ch, "COMPLETE", "JOB_COMPLETE", None)
            return
        d.in_band_since = None
        if grams < d.job.target_g - 300:
            if d.stage != "COARSE":
                d.stage = "COARSE"
                self._event(d, "COARSE_STARTED", now, grams)
            self.plant.set_relay(ch, True)
        else:
            if d.stage != "FINE":
                d.stage = "FINE"
                self.plant.set_relay(ch, False)
                self._event(d, "FINE_STARTED", now, grams)
            if now >= d.next_pulse and not d.pulse_off:
                d.next_pulse = now + 0.15
                d.pulse_off = now + 0.02
                self.plant.set_relay(ch, True)

    def _finish(self, ch: str, status: str, event: str, error: str | None) -> None:
        d = self.disp[ch]
        if d is None:
            return
        now = time.monotonic()
        self.plant.set_relay(ch, False)
        self._flush_samples(d)
        self._event(d, event, now, self.plant.weight(ch), terminal=True)
        final = self.cache.get(ch)
        final_w = final[0] if final else self.plant.weight(ch)
        end_seq = d.seq
        body = {"run_id": d.run_id, "boot_id": self.boot_id, "message_id": f"{self.boot_id}-c{end_seq + 1}",
                "batch_seq": end_seq + 1, "status": status, "final_weight_g": final_w,
                "max_weight_g": d.max_w, "duration_ms": int((now - d.t0) * 1000),
                "end_seq": end_seq, "total_batches": end_seq, "total_samples": d.samples,
                "total_events": d.events, "dropped_samples": d.dropped}
        if error:
            body["error"] = error[:120]
        self._enqueue(d, "complete", end_seq + 1, body)
        self.runs[d.run_id]["local_status"] = status
        self.disp[ch] = None

    def _flush_samples(self, d: Disp) -> None:
        if not d.pending:
            return
        d.seq += 1
        body = {"run_id": d.run_id, "boot_id": self.boot_id, "batch_seq": d.seq, "samples": d.pending}
        n, d.pending = len(d.pending), []
        self._enqueue(d, "samples", d.seq, body, n)

    def _event(self, d: Disp, name: str, now: float, grams: int, terminal: bool = False) -> None:
        self._flush_samples(d)                      # keep stored order: samples before events
        d.events += 1
        d.seq += 1
        body = {"run_id": d.run_id, "boot_id": self.boot_id, "batch_seq": d.seq,
                "events": [{"idx": d.events, "event": name, "elapsed_ms": int((now - d.t0) * 1000),
                            "state": d.stage[:24], "weight_g": grams}]}
        self._enqueue(d, "events", d.seq, body, terminal=terminal)

    def _supersede(self, ch: str) -> None:
        """telemetry_client.c run_supersede(): a new job takes the slot of a run whose complete is still
        in the caller backlog (ring was full). Its un-added items are lost, the run is closed as
        FAILED / RUN_SUPERSEDED through the complete reserve (end_seq = what the ring really took)."""
        old = next((d for d, i in self.backlog if i.kind == "complete" and d.job.channel == ch), None)
        if old is None:
            return
        lost = [i for d, i in self.backlog if d is old]
        self.backlog = [(d, i) for d, i in self.backlog if d is not old]
        dropped = old.dropped + sum(i.n_samples for i in lost)
        end = old.added_seq
        body = {"run_id": old.run_id, "boot_id": self.boot_id, "message_id": f"{self.boot_id}-s{end + 1}",
                "batch_seq": end + 1, "status": "FAILED", "final_weight_g": old.first_w or 0,
                "max_weight_g": old.max_w, "duration_ms": int((time.monotonic() - old.t0) * 1000),
                "error": "RUN_SUPERSEDED", "end_seq": end, "total_batches": end,
                "total_samples": old.samples, "total_events": old.events, "dropped_samples": dropped}
        item = Item(old.run_id, end + 1, "complete", f"cas/{self.id}/run/complete",
                    json.dumps(body, separators=(",", ":")))
        ok = self._add(item)
        if ok:
            old.added_seq = end + 1
        self.stats["superseded_runs"] += 1
        if not ok:
            self.stats["superseded_unclosed"] += 1
        self.runs[old.run_id]["local_status"] = "FAILED" if ok else "UNCLOSED"

    # -- ring (mirrors run_log.c) -------------------------------------------------------
    def _enqueue(self, d: Disp, kind: str, seq: int, body: dict, n_samples: int = 0,
                 terminal: bool = False) -> None:
        if d.released or not self._run_mqtt:
            return
        self.backlog.append((d, Item(d.run_id, seq, kind, f"cas/{self.id}/run/{kind}",
                                     json.dumps(body, separators=(",", ":")), n_samples, terminal=terminal)))
        self._feed_ring()

    def _feed_ring(self) -> None:
        """The caller's own ring: a refused add stays here and is retried every tick. Per-run
        FIFO (a refused item holds back only the later items of its own run)."""
        blocked: set[str] = set()
        keep = []
        for d, item in self.backlog:
            if d.released:
                continue
            if item.run_id in blocked or not self._add(item):
                blocked.add(item.run_id)
                keep.append((d, item))
            else:
                d.added_seq = max(d.added_seq, item.seq)
        self.backlog = keep

    def _sample_bytes(self) -> int:
        return sum(i.size for i in self.ring if i.kind == "samples" and not i.tomb)

    def _total_bytes(self) -> int:
        return sum(i.size for i in self.ring)

    def _make_tomb(self, it: Item) -> None:
        """Body shrinks in place to {run_id, boot_id?, batch_seq, dropped, samples}; batch_seq stays."""
        try:
            boot = json.loads(it.payload).get("boot_id")
        except ValueError:
            boot = None
        tomb = {"run_id": it.run_id}
        if boot is not None:
            tomb["boot_id"] = boot
        tomb.update(batch_seq=it.seq, dropped=True, samples=it.n_samples)
        it.payload = json.dumps(tomb, separators=(",", ":"))     # size is the real new length
        it.tomb = True
        self.stats["tombstones"] += 1

    def _drop_oldest_sample(self) -> bool:
        cand = [i for i in self.ring if i.kind == "samples" and not i.tomb]
        if not cand:
            return False
        oldest = min(cand, key=lambda i: i.ord)
        self._count_dropped(oldest)
        self._make_tomb(oldest)
        return True

    def _count_dropped(self, it: Item) -> None:
        self.ring_overflow += 1
        self.stats["dropped_batches"] += 1
        self.dropped_samples += it.n_samples
        owner = self.runs.get(it.run_id, {}).get("disp")
        if owner is not None:
            owner.dropped += it.n_samples

    def _add(self, item: Item) -> bool:
        if item.size > RUNLOG_ITEM_MAX:
            self.stats["add_oversize"] += 1
            self.stats["add_refused"] += 1
            return False
        critical = item.kind in ("start", "complete") or item.terminal
        # staggered reserves: samples/events leave 6 free, start down to 4, terminal event 2, complete 0
        keep = RUNLOG_SLOT_RESERVE
        if item.kind == "complete":
            keep = 0
        elif item.terminal:
            keep = RUNLOG_RESERVE_PER_CLASS
        elif item.kind == "start":
            keep = 2 * RUNLOG_RESERVE_PER_CLASS
        if self.ring_slots - len(self.ring) <= keep:
            self.stats["add_refused"] += 1
            return False
        if item.kind == "samples":
            while self._sample_bytes() + item.size > self.sample_bytes and self._drop_oldest_sample():
                pass
            if self._sample_bytes() + item.size > self.sample_bytes:
                self.stats["add_refused"] += 1
                return False
        cap = self.total_bytes + (RUNLOG_CRITICAL_RESERVE if critical else 0)
        while self._total_bytes() + item.size > cap and self._drop_oldest_sample():
            pass
        if self._total_bytes() + item.size > cap:
            self.stats["refused_total_cap"] += 1
            self.stats["add_refused"] += 1
            return False
        if len(self.ring) >= self.ring_slots:
            self.stats["add_refused"] += 1
            return False
        item.ord = next(self._ord)
        self.ring.append(item)
        self.stats["added"] += 1
        self.max_ring = max(self.max_ring, len(self.ring))
        return True

    def _is_run_head(self, it: Item) -> bool:
        return not any(o is not it and o.ord < it.ord and o.run_id == it.run_id for o in self.ring)

    def _abandon(self, it: Item) -> None:
        """Samples/events only (a tombstone, start and complete are NEVER released by a timeout)."""
        self.stats["resend_abandoned"] += 1
        if it.kind == "samples":
            self._count_dropped(it)
        self._make_tomb(it)
        it.tries = it.silent = 0
        it.sent_at = None

    def _release_run(self, run_id: str, abandoned: bool) -> None:
        keep = []
        for i in self.ring:
            if i.run_id == run_id:
                self.stats["abandoned_items" if abandoned else "acked_items"] += 1
            else:
                keep.append(i)
        self.ring = keep
        self.unk_owed.pop(run_id, None)

    def _pump(self, now: float) -> None:
        """runlog_pump. An unacked item times out after resend_s << min(max(tries, silent), 3) (10/20/40/80 s,
        capped). The timeout is a counted try only when some valid run/ack (any run) arrived since it was
        sent AND the item is its run's oldest sample/event (6 tries => tombstone); otherwise it is
        silence and only backs off. Next item: oldest unsent of the run with the fewest in flight."""
        self._feed_ring()
        if not self.link.is_ready:
            return
        inflight = 0
        for it in list(self.ring):
            if it.sent_at is None:
                continue
            step = max(it.tries, it.silent)
            if now - it.sent_at >= self.resend_s * (1 << min(step, 3)):
                it.sent_at = None
                self.stats["resent"] += 1
                self.unk_owed.pop(it.run_id, None)
                if self.resp_gen == it.resp_gen:
                    it.silent = min(it.silent + 1, 255)
                elif self._is_run_head(it) and it.kind in ("samples", "events") and not it.tomb:
                    it.tries += 1
                    if it.tries >= RUNLOG_MAX_RESENDS:
                        self._abandon(it)
            else:
                inflight += 1
        while inflight < self.max_inflight:
            nxt, nxt_in = None, 0
            for it in self.ring:
                if it.sent_at is not None:
                    continue
                same = [o for o in self.ring if o.run_id == it.run_id]
                if any(o.sent_at is None and o.ord < it.ord for o in same):
                    continue                                   # not the oldest unsent of its run
                n_in = sum(1 for o in same if o.sent_at is not None)
                if nxt is None or n_in < nxt_in or (n_in == nxt_in and it.ord < nxt.ord):
                    nxt, nxt_in = it, n_in
            if nxt is None or not self.link.publish(nxt.topic, nxt.payload, 1):
                break
            nxt.sent_at, nxt.ever_sent, nxt.sends = now, True, nxt.sends + 1
            nxt.resp_gen = self.resp_gen
            self.stats["published"] += 1
            inflight += 1

    def _on_run_ack(self, payload: bytes) -> None:
        strict_bad = False
        if self.strict_acks:
            # run_log.c u32_of(), on the raw token: literal -1 = "nothing stored"; otherwise 1..10
            # plain digits, <= 0xFFFFFFFF, anything else (-01, -1.5, -2, 1.5, overflow) makes the
            # WHOLE ack malformed and ignored.
            m = re.search(rb'"acked_batch_seq"\s*:\s*([^,}\s]*)', payload)
            if m:
                tok = m.group(1)
                if tok != b"-1" and not (tok.isdigit() and tok.isascii() and len(tok) <= 10
                                         and int(tok) <= 0xFFFFFFFF):
                    strict_bad = True
        try:
            body = json.loads(payload) if not strict_bad else {}
            run_id, state = body["run_id"], body["state"]
        except (ValueError, KeyError, TypeError):
            if strict_bad:
                self.ack_ignored += 1
            return
        acked = body.get("acked_batch_seq", ...)
        if not isinstance(run_id, str) or state not in ACK_STATES or \
                (acked is not ... and (type(acked) is not int or acked < -1)):
            return
        self.resp_gen += 1                                # a valid ack: the backend is alive (any run)
        self.run_acks.append(body)
        info = self.runs.get(run_id)
        if state in ("START_OK", "BATCH_OK"):
            # ABSENT field: START_OK means 0, BATCH_OK is ignored. -1 ("nothing stored") never
            # trims, in ANY state (START_OK included).
            if acked is ... and state == "START_OK":
                acked = 0
            elif acked is ... or acked == -1:
                self.ack_ignored += 1
                return
            for i in list(self.ring):
                # ever_sent: a huge ack can never release an item that was not published yet
                if i.run_id == run_id and i.ever_sent and i.kind != "complete" and i.seq <= acked:
                    self.ring.remove(i)
                    self.stats["acked_items"] += 1
        elif state in RELEASE_STATES:
            if info is None and not any(i.run_id == run_id for i in self.ring):
                return
            self._release_run(run_id, state == "REJECTED")
            if state == "REJECTED":
                self.stats["rejected_runs"] += 1
            if info is not None:
                info["outcome"] = state
                info["disp"].released = True
                info["missing_ranges"] = body.get("missing_ranges")
        elif state == "UNKNOWN_RUN":
            if self.unk_owed.get(run_id, 0) > 0:
                self.unk_owed[run_id] -= 1                # late reply of the previous cycle: not a new cycle
                return
            abandon, n_sent = False, 0
            for i in self.ring:
                if i.run_id != run_id:
                    continue
                # one count per resend cycle: only an item sent since the last UNKNOWN_RUN counts
                if i.sent_at is not None:
                    n_sent += 1
                    i.unknown += 1
                    abandon = abandon or i.unknown > RUNLOG_MAX_UNKNOWN
                i.sent_at = None
            if abandon:
                self._release_run(run_id, True)
                if info is not None:
                    info["disp"].released = True
                    info["outcome"] = "ABANDONED"
            elif n_sent > 1:
                self.unk_owed[run_id] = n_sent - 1        # replies still owed by this cycle

    # -- telemetry ----------------------------------------------------------------------
    def _channels(self, full: bool) -> list[dict]:
        out = []
        for ch in ("CH1", "CH2"):
            w = self.cache.get(ch)
            c = {"channel_id": ch, "weight_g": w[0] if w else None, "weight_valid": w is not None,
                 "weight_age_ms": w[2] if w else None, "stable": bool(w and w[1])}
            if full:
                d = self.disp[ch]
                c.update(material_id=MAT_OF[ch], state="DISPENSING" if d else "IDLE",
                         relay_on=self.plant.relay(ch), active_job_id=d.job.local_id if d else 0)
            out.append(c)
        return out

    def _telemetry(self, now: float) -> None:
        if not self.link.is_ready:
            return
        if now - self._last_live >= 0.2:
            self._last_live = now
            self.link.publish(f"cas/{self.id}/telemetry/live", json.dumps(
                {"uptime_ms": self.uptime_ms(), "channels": self._channels(False)},
                separators=(",", ":")), 0)
        if now - self._last_status >= 1.0:
            self._last_status = now
            self.link.publish(f"cas/{self.id}/telemetry/status", json.dumps(
                {"role": "relay_controller", "firmware": "relay-sim", "boot_id": self.boot_id,
                 "uptime_ms": self.uptime_ms(), "estop": False, "channels": self._channels(True),
                 "queue": [{"local_job_id": j.local_id, "channel_id": j.channel} for j in self.queue]},
                separators=(",", ":")), 0)

    # -- helpers for tests ------------------------------------------------------------------
    def busy(self) -> bool:
        return any(self.disp.values()) or bool(self.queue)

    def outcome(self, run_id: str) -> str | None:
        return self.runs.get(run_id, {}).get("outcome")

    def run_ids(self) -> list[str]:
        return list(self.runs)
