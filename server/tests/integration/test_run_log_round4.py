"""Relay run history after the fourth run_log.c fix round, against the real backend (slow).

Time is accelerated by a small relay ``resend_s``: firmware backoff is 10/20/40/80 s capped at 80 s;
the sim uses ``resend_s << min(step, 3)``, so resend_s=0.1 is a 1/100 scale (700 s = 7 s, 3600 s = 36 s).

Proven here: a backend that is silent for 700 s / 3600 s (before START_OK and mid-run) costs NOTHING
(no counted try, no tombstone, no released START/COMPLETE); after recovery every batch is stored exactly
once, in order, the run ends COMPLETE (or COMPLETE_PARTIAL with the exact missing ranges when the ring
really overflowed), nothing stays RUNNING and the ring is empty.
"""
import collections
import json
import sqlite3
import time

import pytest

from app.models import DeviceCommand, DispenseEvent, DispenseRun, RunBatch, WeightSample
from sim.mqttx import wait_for
from . import invariants

pytestmark = pytest.mark.slow

SCALE = 0.01                       # real seconds per firmware second at resend_s=0.1
KW = {"resend_s": 0.1}
KW_FEW = {**KW, "sample_period_s": 0.2}   # <= ~6 sample batches per 15000 g run: fits the 16 KB sample budget
FINAL = ("COMPLETE", "COMPLETE_PARTIAL", "FAILED", "CANCELLED", "INTERRUPTED")
LONG = [{}, {"target_g": 15000}]   # profiles: [5000 g, 15000 g]; the 15000 g job runs ~6 s


def _has_data(s, n=1):
    def ok():
        runs = s.runs()
        return runs and runs[-1].status == "RUNNING" and s.db.scalar(
            "select count(*) from weight_samples where run_id = :r", r=runs[-1].run_id) >= n
    return wait_for(ok, 30, 0.05)


def _publishes(s) -> collections.Counter:
    """How often each (kind, run_id, batch_seq) went over the wire (QoS1 duplicates)."""
    c: collections.Counter = collections.Counter()
    for kind in ("start", "samples", "events", "complete"):
        for body in s.probe.bodies(f"/run/{kind}"):
            c[(kind, body.get("run_id"), body.get("batch_seq", 0))] += 1
    return c


def _settled(s, rids, timeout=90):
    """Every run is final in the DB, the relay released it, its ring and caller backlog are empty."""
    def done():
        rows = {r.run_id: r for r in s.runs()}
        return (all(rid in rows and rows[rid].status in FINAL for rid in rids)
                and all(s.relay.outcome(rid) is not None for rid in rids)
                and not s.relay.ring and not s.relay.backlog)
    ok = wait_for(done, timeout, 0.2)
    assert ok, ([(r.run_id, r.status) for r in s.runs()], {k: s.relay.outcome(k) for k in rids},
                len(s.relay.ring), len(s.relay.backlog), dict(s.relay.stats), s.backend.log_tail())
    return {r.run_id: r for r in s.runs()}


def _first_publish_order(s, rid) -> list[int]:
    """batch_seq of each item of the run in the order it FIRST went over the wire."""
    seen: list[int] = []
    for _t, topic, payload, _r in s.probe.msgs():
        if not topic.startswith("cas/rly1/run/") or topic.endswith("/ack"):
            continue
        try:
            body = json.loads(payload)
        except ValueError:
            continue
        if body.get("run_id") == rid and body.get("batch_seq", 0) not in seen:
            seen.append(body.get("batch_seq", 0))
    return seen


def _verify_exact(s, rid, expect_status="COMPLETE"):
    """Exactly once, in order, nothing missing, and the DB equals what the relay produced."""
    run = s.db.get(DispenseRun, rid)
    d = s.relay.runs[rid]["disp"]
    assert run.status == expect_status, (run.status, run.missing_ranges, dict(s.relay.stats))
    batches = s.db.all(RunBatch, RunBatch.run_id == rid)
    seqs = sorted(b.batch_seq for b in batches)
    end = (run.ingest_json or {})["end_seq"]
    assert seqs == list(range(0, end + 1)), seqs                       # no gap, no duplicate row
    assert all(b.kind != "dropped" for b in batches)
    n_samples = s.db.scalar("select count(*) from weight_samples where run_id = :r", r=rid)
    n_events = s.db.scalar("select count(*) from dispense_events where run_id = :r", r=rid)
    assert (n_samples, n_events) == (d.samples, d.events)
    assert run.missing_ranges is None
    order = _first_publish_order(s, rid)
    assert order == list(range(0, end + 2)), order      # items (and the complete, end_seq + 1) leave the relay in order


def _history_ok(s, rids=None):
    assert invariants.check_run_history(s, rids) == []
    assert invariants.check(s) == []


# ---- silence before START_OK and mid-run -----------------------------------------------------------

@pytest.mark.parametrize("silent_s", [700, 3600])
def test_backend_silent_before_start_ok_nothing_is_lost_or_released(make_stack, silent_s):
    s = make_stack(relay_kw=KW).up().ready()
    s.backend.kill9()
    s.relay.begin_adhoc("CH1", 5000)                 # the run starts while nobody can ack its START
    assert wait_for(lambda: s.relay.runs, 10)
    rid = s.relay.run_ids()[0]
    time.sleep(silent_s * SCALE)
    assert not s.relay.busy()                        # the relay finished the dispense on its own
    st = s.relay.stats
    assert st["tombstones"] == 0 and st["resend_abandoned"] == 0 and st["abandoned_items"] == 0, dict(st)
    kinds = [i.kind for i in s.relay.ring if i.run_id == rid]
    assert "start" in kinds and "complete" in kinds, kinds        # START and COMPLETE are never released
    assert all(i.tries == 0 for i in s.relay.ring), [(i.kind, i.tries) for i in s.relay.ring]
    sent_before = _publishes(s)
    assert max(sent_before.values()) >= 2            # it kept re-sending (QoS1 duplicates are on the wire)
    s.start_backend()
    _settled(s, [rid])
    _verify_exact(s, rid)
    assert s.relay.outcome(rid) == "COMPLETE" and s.relay.stats["tombstones"] == 0
    _history_ok(s)


@pytest.mark.parametrize("silent_s", [700, 3600])
def test_backend_silent_mid_run_then_recovery_every_batch_once_in_order(make_stack, silent_s):
    s = make_stack(profiles=LONG, relay_kw=KW, backend_env={"RUN_STALE_SECONDS": 60}).up().ready()
    s.create_job(15000, pv=s.ids["profiles"][1])
    assert _has_data(s, 5)
    rid = s.runs()[-1].run_id
    s.backend.kill9()
    time.sleep(silent_s * SCALE)
    assert not s.relay.busy()
    st = s.relay.stats
    assert st["tombstones"] == 0 and st["resend_abandoned"] == 0 and st["abandoned_items"] == 0, dict(st)
    assert any(i.kind == "complete" and i.run_id == rid for i in s.relay.ring)
    s.start_backend()
    _settled(s, [rid])
    _verify_exact(s, rid)
    assert s.relay.outcome(rid) == "COMPLETE"
    assert max(_publishes(s).values()) >= 2               # QoS1 duplicates were on the wire, stored once
    _history_ok(s)


def test_ring_overflow_during_silence_ends_complete_partial_with_exact_missing_ranges(make_stack):
    """The ONLY legitimate loss: bytes really overflowed (tombstones, oldest samples first)."""
    s = make_stack(profiles=LONG, relay_kw={**KW, "sample_period_s": 0.01}).up().ready()
    s.create_job(15000, pv=s.ids["profiles"][1])
    assert _has_data(s, 3)
    rid = s.runs()[-1].run_id
    s.backend.kill9()
    assert wait_for(lambda: not s.relay.busy(), 30)
    assert s.relay.stats["tombstones"] > 0, dict(s.relay.stats)
    time.sleep(700 * SCALE)
    assert s.relay.stats["resend_abandoned"] == 0                 # tombstones came from bytes, not timeouts
    s.start_backend()
    _settled(s, [rid])
    run = s.db.get(DispenseRun, rid)
    assert run.status == "COMPLETE_PARTIAL" and run.missing_ranges, (run.status, run.missing_ranges)
    assert s.relay.outcome(rid) == "COMPLETE_PARTIAL"
    assert "dropped" in {b.kind for b in s.db.all(RunBatch, RunBatch.run_id == rid)}
    _history_ok(s)


# ---- two pumps at once ------------------------------------------------------------------------------

def test_two_interleaved_runs_both_pumps_complete_even_across_a_backend_outage(make_stack):
    s = make_stack(profiles=LONG, relay_kw={**KW, "sample_period_s": 0.25},    # two runs share the 16 KB sample budget
                   backend_env={"RUN_STALE_SECONDS": 60}).up().ready()
    s.plant.set_weight("CH2", 0)
    s.sender.also = ["CH2"]
    assert wait_for(lambda: s.relay.cache.get("CH2") is not None, 10)
    c1 = s.create_job(15000, pv=s.ids["profiles"][1])
    c2 = s.create_job(5000, material="M2")
    assert wait_for(lambda: len(s.relay.runs) == 2, 20)
    rids = s.relay.run_ids()
    assert wait_for(lambda: s.db.scalar("select count(*) from weight_samples") >= 3, 20, 0.05)
    s.backend.kill9()                                   # both runs in flight, then 700 s of silence
    time.sleep(700 * SCALE)
    assert not s.relay.busy()
    assert s.relay.stats["tombstones"] == 0 and s.relay.stats["abandoned_items"] == 0
    s.start_backend()
    rows = _settled(s, rids)
    assert {rows[r].channel_id for r in rids} == {"CH1", "CH2"}
    assert sorted(x for x in s.relay.executed) == sorted([c1, c2])
    for rid in rids:
        _verify_exact(s, rid)
    _history_ok(s)


# ---- infrastructure combos --------------------------------------------------------------------------

@pytest.mark.parametrize("order", ["broker_first", "backend_first"])
def test_broker_outage_and_backend_restart_combo_everything_arrives_once(make_stack, order):
    s = make_stack(profiles=LONG, relay_kw=KW, backend_env={"RUN_STALE_SECONDS": 60}).up().ready()
    cid = s.create_job(15000, pv=s.ids["profiles"][1])
    assert _has_data(s, 5)
    rid = s.runs()[-1].run_id
    if order == "broker_first":
        s.broker.kill9()
        s.backend.kill9()
    else:
        s.backend.kill9()
        s.broker.kill9()
    time.sleep(300 * SCALE + 6.0)                       # the weight stream is gone: the relay fails the job itself
    assert not s.plant.any_relay_on()
    s.broker.start()
    assert wait_for(lambda: s.relay.link.is_ready, 30)
    time.sleep(300 * SCALE)                              # broker alive, backend still dead
    assert s.relay.stats["tombstones"] == 0 and s.relay.stats["abandoned_items"] == 0, dict(s.relay.stats)
    s.start_backend()
    rows = _settled(s, [rid])
    assert rows[rid].status == "FAILED", (rows[rid].status, rows[rid].error_text)
    assert s.relay.executed == [cid]
    batches = s.db.all(RunBatch, RunBatch.run_id == rid)
    assert sorted(b.batch_seq for b in batches) == list(range(0, (rows[rid].ingest_json or {})["end_seq"] + 1))
    _history_ok(s)


def test_backend_db_down_while_broker_alive_no_ack_no_loss(make_stack):
    s = make_stack(profiles=LONG, relay_kw=KW, backend_env={"RUN_STALE_SECONDS": 60}).up().ready()
    s.create_job(15000, pv=s.ids["profiles"][1])
    assert _has_data(s, 5)
    rid = s.runs()[-1].run_id
    lock = sqlite3.connect(str(s.tmp / "it.db"), timeout=1, isolation_level=None)
    try:
        lock.execute("BEGIN EXCLUSIVE")                   # the backend is alive, its database is not
        assert wait_for(lambda: not s.relay.busy(), 40)
        time.sleep(20.0)                                  # ~ 2000 firmware seconds
        assert s.backend.alive()
    finally:
        lock.execute("ROLLBACK")
        lock.close()
    rows = _settled(s, [rid], timeout=120)
    assert rows[rid].status in ("COMPLETE", "COMPLETE_PARTIAL")
    if s.relay.stats["tombstones"] == 0:
        _verify_exact(s, rid)
    _history_ok(s)


# ---- storms -------------------------------------------------------------------------------------------

def test_ack_storm_and_duplicate_publish_storm_change_nothing(make_stack):
    s = make_stack(profiles=LONG, relay_kw=KW).up().ready()
    s.create_job(15000, pv=s.ids["profiles"][1])
    assert _has_data(s, 3)
    rid = s.runs()[-1].run_id
    storm = []
    for k in range(60):                                   # stale / foreign / malformed acks, all harmless
        storm += [{"run_id": rid, "acked_batch_seq": 0, "state": "BATCH_OK"},
                  {"run_id": rid, "acked_batch_seq": 0, "state": "START_OK"},
                  {"run_id": f"foreign-{k}", "acked_batch_seq": -1, "state": "UNKNOWN_RUN"},
                  {"run_id": f"foreign-{k}", "acked_batch_seq": 5, "state": "BATCH_OK"},
                  {"run_id": f"foreign-{k}", "acked_batch_seq": -1, "state": "REJECTED"},
                  {"run_id": rid, "state": "BATCH_OK"}]
    for ack in storm:
        s.relay._inbox.append(("runack", json.dumps(ack).encode()))
    for _ in range(12):                                   # QoS1 duplicates of whatever is in the ring
        for it in list(s.relay.ring):
            if it.ever_sent and it.run_id == rid:
                s.relay.link.publish(it.topic, it.payload, 1)
        time.sleep(0.1)
    _settled(s, [rid])
    _verify_exact(s, rid)
    assert s.relay.outcome(rid) == "COMPLETE" and s.relay.stats["tombstones"] == 0
    dup = _publishes(s)
    assert max(dup.values()) >= 5                         # duplicates reached the backend ...
    _history_ok(s)                                        # ... and were stored once


# ---- superseded run -----------------------------------------------------------------------------------

def test_new_job_closes_the_unclosed_old_run_as_failed_run_superseded_and_the_backend_accepts_it(make_stack):
    s = make_stack(relay_kw={**KW, "ring_slots": 8}).up().ready()
    s.backend.kill9()
    s.relay.begin_adhoc("CH1", 5000)
    assert wait_for(lambda: s.relay.runs and not s.relay.busy(), 20)
    old = s.relay.run_ids()[0]
    assert any(i.kind == "complete" for _d, i in s.relay.backlog)         # the ring was too full to close it
    s.plant.set_weight("CH1", 0)
    s.relay.begin_adhoc("CH1", 5000)                                      # a new job takes the slot
    assert wait_for(lambda: len(s.relay.runs) == 2, 10)
    assert s.relay.stats["superseded_runs"] == 1 and s.relay.stats["superseded_unclosed"] == 0
    new = [r for r in s.relay.run_ids() if r != old][0]
    s.start_backend()
    rows = _settled(s, [old, new], timeout=120)
    assert rows[old].status in ("FAILED", "COMPLETE_PARTIAL"), (rows[old].status, rows[old].error_text)
    assert rows[old].error_text == "RUN_SUPERSEDED", rows[old].error_text
    assert s.relay.outcome(old) in ("COMPLETE", "COMPLETE_PARTIAL")
    _history_ok(s)
