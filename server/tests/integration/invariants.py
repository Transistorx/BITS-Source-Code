"""System invariants checked after fault injection (CONTRACT 8.4, 9.2, 9.3, 9.8)."""
import collections
import threading
import time
from datetime import datetime

from app.models import DeviceCommand, DispenseEvent, DispenseRun, RunBatch, WeightSample


class SafetyMonitor(threading.Thread):
    """Samples the plant: a relay may never stay energised while its channel has had no
    valid weight (CONTRACT 8.3: 5 s without a valid weight => relays OFF). `slack_s` is the
    sampling/tick allowance on top of the sim's own 5 s limit."""

    def __init__(self, stack, slack_s: float = 0.3):
        super().__init__(name="safety-monitor", daemon=True)
        self.stack, self.slack = stack, slack_s
        self.violations: list[str] = []
        self.max_blind_s = 0.0
        self._stop_ev = threading.Event()
        self._blind: dict[str, float] = {}

    def run(self) -> None:
        while not self._stop_ev.wait(0.05):
            relay, plant = self.stack.relay, self.stack.plant
            if relay is None or relay.link.dead and not relay._stop.is_set():
                continue
            now = time.monotonic()
            for ch in ("CH1", "CH2"):
                if plant.relay(ch) and relay.cache.get(ch) is None:
                    t0 = self._blind.setdefault(ch, now)
                    self.max_blind_s = max(self.max_blind_s, now - t0)
                    if now - t0 > self.slack:
                        self.violations.append(f"{ch} energised {now - t0:.2f}s with no valid weight")
                else:
                    self._blind.pop(ch, None)

    def stop(self) -> None:
        self._stop_ev.set()
        self.join(2)


def _ranges_size(ranges) -> int:
    return sum(b - a + 1 for a, b in ranges)


def check(stack, ttl_s: float = 2.0) -> list[str]:
    """Return the list of violated invariants (empty = all hold)."""
    bad: list[str] = []
    db = stack.db
    # 1. no command executed twice (across every relay incarnation)
    execs = collections.Counter(c for r in stack.relays for c in r.executed if c is not None)
    bad += [f"command {c} executed {n}x" for c, n in execs.items() if n > 1]
    # 2. no APPLIED without an ACK actually sent by a device
    sent = {(c, st) for r in stack.relays for c, st in r.cmd_log}
    sent |= {(a["command_id"], a["state"]) for sn in stack.senders for a in sn.acks}
    commands = db.all(DeviceCommand)
    for c in commands:
        if c.state == "APPLIED" and (c.id, "APPLIED") not in sent:
            bad.append(f"command {c.id} {c.command_type} APPLIED without an APPLIED ACK")
        # 3. ZERO/TARE never APPLIED
        if c.command_type in ("ZERO", "TARE") and c.state == "APPLIED":
            bad.append(f"{c.command_type} {c.id} APPLIED")
    # 4. no COMPLETE with gaps; COMPLETE_PARTIAL says what is missing
    for run in db.all(DispenseRun):
        meta = run.ingest_json or {}
        seqs = {b.batch_seq for b in db.all(RunBatch, RunBatch.run_id == run.run_id)}
        if run.status == "COMPLETE":
            end = meta.get("end_seq")
            if end is None:
                bad.append(f"run {run.run_id} COMPLETE without end_seq")
                continue
            if not set(range(0, end + 1)) <= seqs:
                bad.append(f"run {run.run_id} COMPLETE with batch gaps {sorted(set(range(end + 1)) - seqs)}")
            idx = {r.idx for r in db.all(WeightSample, WeightSample.run_id == run.run_id)}
            if idx != set(range(1, meta.get("total_samples", 0) + 1)):
                bad.append(f"run {run.run_id} COMPLETE with sample gaps")
            ev = {r.idx for r in db.all(DispenseEvent, DispenseEvent.run_id == run.run_id)}
            if ev != set(range(1, meta.get("total_events", 0) + 1)):
                bad.append(f"run {run.run_id} COMPLETE with event gaps")
            if run.missing_ranges:
                bad.append(f"run {run.run_id} COMPLETE but missing_ranges={run.missing_ranges}")
        elif run.status == "COMPLETE_PARTIAL" and not run.missing_ranges:
            bad.append(f"run {run.run_id} COMPLETE_PARTIAL without missing_ranges")
    # 5. no orphan DELIVERED (older than the command TTL plus margin) on an MQTT device
    now = datetime.utcnow()
    for c in commands:
        if c.state == "DELIVERED":
            age = (now - c.updated_at).total_seconds()
            if age > ttl_s + 3:
                bad.append(f"orphan DELIVERED command {c.id} ({age:.0f}s old)")
    return bad


def _flat(ranges) -> set[int]:
    return {n for a, b in (ranges or []) for n in range(a, b + 1)}


def check_run_history(stack, run_ids=None, *, allow_unstored: bool = False) -> list[str]:
    """Run-history invariants (CONTRACT 9.2) for every stored run (or ``run_ids``):
      * no duplicate (run_id, batch_seq) / sample idx / event idx rows,
      * data is ordered: elapsed_ms never goes back with idx (ARRIVAL order is not required: per-item
        resend backoff lets a later batch overtake an earlier one, the backend stores it out of order),
      * no gap in 0..end_seq unless it is stored as a tombstone ('dropped'),
      * COMPLETE has nothing missing; COMPLETE_PARTIAL reports EXACTLY the missing batch_seq / sample idx /
        event idx (tombstoned seqs included), nothing more, nothing less,
      * nothing is left RUNNING.
    """
    bad: list[str] = []
    db = stack.db
    for run in db.all(DispenseRun):
        if run_ids is not None and run.run_id not in run_ids:
            continue
        rid, meta = run.run_id, run.ingest_json or {}
        batches = db.all(RunBatch, RunBatch.run_id == rid)
        seqs = [b.batch_seq for b in batches]
        if len(seqs) != len(set(seqs)):
            bad.append(f"{rid}: duplicate batch rows")
        real = {b.batch_seq for b in batches if b.kind != "dropped"}
        samples = db.all(WeightSample, WeightSample.run_id == rid, order=WeightSample.id)
        events = db.all(DispenseEvent, DispenseEvent.run_id == rid, order=DispenseEvent.id)
        for name, rows in (("sample", samples), ("event", events)):
            idx = [r.idx for r in rows]
            if len(idx) != len(set(idx)):
                bad.append(f"{rid}: duplicate {name} idx")
            el = [r.elapsed_ms or 0 for r in sorted(rows, key=lambda r: r.idx)]
            if el != sorted(el):
                bad.append(f"{rid}: {name} elapsed_ms goes backwards")
        if run.status == "RUNNING":
            bad.append(f"{rid}: still RUNNING (wedged)")
        end = meta.get("end_seq")
        if run.status not in ("COMPLETE", "COMPLETE_PARTIAL", "FAILED", "CANCELLED") or end is None:
            continue
        want = set(range(0, end + 1))
        unstored = want - set(seqs)
        if unstored and not allow_unstored:
            bad.append(f"{rid}: batch gap without tombstone {sorted(unstored)}")
        if run.status in ("FAILED", "CANCELLED"):
            continue
        missing = want - real
        got = run.missing_ranges or {}
        if run.status == "COMPLETE":
            if missing or got:
                bad.append(f"{rid}: COMPLETE but missing={sorted(missing)} ranges={got}")
            continue
        if _flat(got.get("batch_seq")) != missing:
            bad.append(f"{rid}: batch_seq ranges {got.get('batch_seq')} != missing {sorted(missing)}")
        for key, rows, total in (("sample_idx", samples, meta.get("total_samples")),
                                 ("event_idx", events, meta.get("total_events"))):
            have = {r.idx for r in rows}
            last = max([total or 0, *have]) if (have or total) else 0
            if _flat(got.get(key)) != set(range(1, last + 1)) - have:
                bad.append(f"{rid}: {key} ranges {got.get(key)} != real gaps")
    return bad