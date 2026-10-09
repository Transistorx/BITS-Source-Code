"""Overload and contract-vs-firmware checks on the run lifecycle."""
import json

import pytest

from app.models import DispenseEvent, RunBatch, WeightSample
from sim.mqttx import wait_for


def _size(ranges):
    return sum(b - a + 1 for a, b in ranges)


def _overflow_run(make_stack, max_inflight, wait_s):
    """Backend down for the whole dispense (nothing is acked), then back: the relay sample byte budget
    (4000 B, run_log.c RUNLOG_SAMPLE_BYTES scaled down) has overflowed and must report the loss."""
    s = make_stack(relay_kw={"sample_bytes": 4000, "max_inflight": max_inflight, "resend_s": 1.0,
                             "sample_period_s": 0.02}).up().ready()
    s.backend.kill9()
    s.relay.begin_adhoc("CH1", 5000)
    assert wait_for(lambda: s.relay.runs and not s.relay.busy(), 30), "dispense did not finish"
    s.start_backend()
    return s, s.wait_run(("COMPLETE", "COMPLETE_PARTIAL", "FAILED", "INTERRUPTED"), wait_s)


@pytest.mark.slow
def test_overflow_gap_does_not_starve_the_complete_item_with_a_window_of_2(make_stack):
    s, run = _overflow_run(make_stack, 2, 40)
    assert run, f"run never completed; ring={[(i.kind, i.seq) for i in s.relay.ring]}"


@pytest.mark.slow
def test_ring_overflow_is_counted_and_the_run_ends_complete_partial_with_the_gaps(make_stack):
    """Ring overflow with a window as wide as the ring (so the stall above cannot occur): the
    relay drops the OLDEST samples, counts them in dropped_samples, and the backend reports the
    exact gaps instead of calling the run COMPLETE."""
    s, run = _overflow_run(make_stack, 5, 60)
    assert run, "run never finished\n" + s.backend.log_tail()
    d = s.relay.runs[run.run_id]["disp"]
    assert s.relay.dropped_samples > 0 and s.relay.ring_overflow > 0
    assert run.status == "COMPLETE_PARTIAL", (run.status, run.missing_ranges)
    assert run.ingest_json["dropped_samples"] == s.relay.dropped_samples == d.dropped
    assert run.ingest_json["total_samples"] == d.samples
    rng = run.missing_ranges
    assert rng and rng.get("sample_idx") and rng.get("batch_seq")
    stored = len(s.db.all(WeightSample, WeightSample.run_id == run.run_id))
    assert _size(rng["sample_idx"]) == d.samples - stored                 # exactly the lost samples
    assert stored + s.relay.dropped_samples == d.samples
    assert wait_for(lambda: s.relay.outcome(run.run_id) == "COMPLETE_PARTIAL", 10)
    assert wait_for(lambda: not s.relay.ring, 10)
    # start, every event and the completion were never dropped
    assert len(s.db.all(DispenseEvent, DispenseEvent.run_id == run.run_id)) == d.events
    assert 0 in {b.batch_seq for b in s.db.all(RunBatch, RunBatch.run_id == run.run_id)}


def test_rejected_run_is_released_by_a_firmware_parsed_ack(make_stack):
    s = make_stack(relay_kw={"tamper_applied": True, "strict_acks": True}).up().ready()
    cid = s.create_job(5000)
    assert wait_for(lambda: any(a["state"] == "REJECTED" for a in s.relay.run_acks), 15)
    minus_one = [a for a in s.relay.run_acks if a["state"] == "REJECTED"]
    assert minus_one and minus_one[0]["acked_batch_seq"] == -1       # what the backend really sends
    assert wait_for(lambda: not s.relay.ring, 5), \
        f"ack ignored by firmware parser ({s.relay.ack_ignored} ignored); ring={len(s.relay.ring)}"
