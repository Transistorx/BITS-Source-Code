"""Relay run-history semantics of run_log.c (third fix round) against the real backend.

Time is accelerated by a small relay ``resend_s``: the firmware backoff is 10/20/40/80/80/80 s
(310 s to give up on a run's oldest item); the sim uses ``resend_s`` << min(tries, 3), so
resend_s=0.3 is 9.3 s of real time (a 1/33 scale).
"""
import json
import time

import pytest

from app.models import DispenseRun, RunBatch
from sim.mqttx import wait_for
from sim.relay import Item

pytestmark = pytest.mark.slow


def _item(relay, kind, run_id, seq, payload):
    return Item(run_id, seq, kind, f"cas/{relay.id}/run/{kind}", payload, 0)


def test_never_acked_batch_blocks_neither_later_items_nor_another_runs_complete(make_stack):
    """An undecodable batch of a foreign run is never acked. It holds at most one window slot at
    a time; the real run still delivers every item and its complete (the ring is not a FIFO
    barrier)."""
    s = make_stack(relay_kw={"resend_s": 0.2}).up().ready()
    ghost = _item(s.relay, "samples", "ghost-run-1", 1, '{"run_id":"ghost-run-1","batch_seq":1,"samples":[')
    assert s.relay._add(ghost)
    s.create_job(5000)
    run = s.wait_run(("COMPLETE", "COMPLETE_PARTIAL", "FAILED"), 40)
    assert run and run.status == "COMPLETE", (run and run.status, s.backend.log_tail())
    assert wait_for(lambda: s.relay.outcome(run.run_id) == "COMPLETE", 10)
    assert ghost.sends >= 1 and ghost.ever_sent        # published, never acked\n    assert [i for i in s.relay.ring if i.run_id == run.run_id] == []
    assert s.relay.stats["tombstones"] == int(ghost.tomb)      # nothing of the real run was tombstoned


def _settling_with_clean_ring(s):
    d = s.relay.disp["CH1"]
    return bool(d and d.stage == "SETTLING"
                and all(i.kind == "samples" for i in s.relay.ring))


def test_backend_down_over_310s_equivalent_tombstones_then_backend_acks_them_complete_partial(make_stack):
    """Backend down while the ring really overflows (50 samples/s: byte budget, NOT timeouts, since total
    silence never counts a try): the oldest sample batches become tombstones that keep their batch_seq. When the backend
    returns, the tombstone is stored as a batch (kind 'dropped'), the cumulative ack passes it and
    the run ends COMPLETE_PARTIAL with the dropped samples reported."""
    s = make_stack(relay_kw={"resend_s": 0.3, "sample_period_s": 0.02, "settle_s": 2.0}).up().ready()
    s.create_job(5000)
    assert wait_for(lambda: _settling_with_clean_ring(s), 30, 0.005), "never reached a clean SETTLING window"
    s.backend.kill9()
    assert wait_for(lambda: s.relay.stats["tombstones"] >= 1, 40), dict(s.relay.stats)
    s.start_backend()                         # tombstone survives another full backoff cycle (9.3 s)
    run = s.wait_run(("COMPLETE", "COMPLETE_PARTIAL", "FAILED", "INTERRUPTED"), 60)
    assert run, "run never finished\n" + s.backend.log_tail()
    assert run.status == "COMPLETE_PARTIAL", (run.status, run.missing_ranges, dict(s.relay.stats))
    d = s.relay.runs[run.run_id]["disp"]
    assert s.relay.dropped_samples > 0 and d.dropped == s.relay.dropped_samples
    assert run.ingest_json["dropped_samples"] == d.dropped
    kinds = {b.kind for b in s.db.all(RunBatch, RunBatch.run_id == run.run_id)}
    assert "dropped" in kinds
    assert wait_for(lambda: s.relay.outcome(run.run_id) == "COMPLETE_PARTIAL", 15)
    assert wait_for(lambda: not s.relay.ring, 15)


def test_start_is_never_released_by_a_timeout_so_the_run_is_stored_when_the_backend_returns(make_stack):
    """Fourth round: a START (like a COMPLETE or a tombstone) is NEVER released by a timeout. With the
    backend down for far longer than the old 6-timeout window (resend_s=0.05: 310 s at firmware speed
    is 15 s) the START is still in the ring, nothing was counted, and when the backend returns the run
    is stored (no UNKNOWN_RUN, no abandon)."""
    s = make_stack(relay_kw={"resend_s": 0.05}).up().ready()
    s.backend.kill9()
    s.relay.begin_adhoc("CH1", 5000)
    assert wait_for(lambda: s.relay.runs, 10)
    rid = s.relay.run_ids()[0]
    time.sleep(17)
    assert [i for i in s.relay.ring if i.kind == "start"], dict(s.relay.stats)
    assert s.relay.stats["abandoned_items"] == 0 and s.relay.stats["resend_abandoned"] == 0
    s.start_backend()
    assert wait_for(lambda: s.relay.outcome(rid) in ("COMPLETE", "COMPLETE_PARTIAL", "ABANDONED"), 90)
    assert s.relay.outcome(rid) in ("COMPLETE", "COMPLETE_PARTIAL"), (s.relay.outcome(rid), dict(s.relay.stats))
    assert not any(a["state"] == "UNKNOWN_RUN" for a in s.relay.run_acks)
    assert len(s.db.all(DispenseRun, DispenseRun.run_id == rid)) == 1
    assert s.relay.ring == []


def test_events_tombstone_from_firmware_is_stored_by_backend(make_stack):
    s = make_stack(profiles=[{}, {"target_g": 15000}], relay_kw={"resend_s": 1.0}).up().ready()
    s.create_job(15000, pv=s.ids["profiles"][1])
    assert wait_for(lambda: s.runs() and s.runs()[-1].status == "RUNNING", 20, 0.1)
    run = s.runs()[-1]
    body = {"run_id": run.run_id, "boot_id": s.relay.boot_id, "batch_seq": 900, "dropped": True, "samples": 0}
    assert s.relay.link.publish(f"cas/{s.relay.id}/run/events", json.dumps(body, separators=(",", ":")), 1)
    stored = wait_for(lambda: [b for b in s.db.all(RunBatch, RunBatch.run_id == run.run_id)
                               if b.batch_seq == 900], 8)
    assert stored, "events tombstone not stored: " + str([a for a in s.relay.run_acks[-5:]])
