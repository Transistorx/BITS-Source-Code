"""Duplicates and replays: a command executes once, run batches are idempotent on
(run_id, batch_seq), an RPC corr_id is answered from the dedupe store."""
import json
import time

from app.models import DeviceCommand, DispenseEvent, RunBatch, WeightSample
from sim.mqttx import wait_for


def test_duplicate_command_delivery_executes_once(stack):
    cid = stack.create_job(5000)
    assert wait_for(lambda: stack.relay.executed, 15)
    raw = stack.command_wire(cid)
    assert raw
    # QoS 1 redelivery / replay of the very same message, three times, mid-run and after
    for _ in range(3):
        stack.relay._inbox.append(("cmd", raw, time.monotonic()))
        time.sleep(0.2)
    assert stack.wait_run(("COMPLETE",), 40)
    stack.relay._inbox.append(("cmd", raw, time.monotonic()))
    time.sleep(0.5)
    assert stack.relay.executed == [cid]
    assert stack.relay.duplicates == 4
    assert len(stack.runs()) == 1
    # every re-ACK carried the stored outcome, never a new QUEUED for a finished job
    assert stack.cmd(cid).state == "APPLIED"
    assert stack.plant.relay_on_time["CH1"] < 6.0          # one dispense worth of pump time


def test_run_batches_are_idempotent_on_run_id_and_batch_seq(stack):
    stack.relay.begin_adhoc("CH1", 5000)
    run = stack.wait_run(("COMPLETE",), 40)
    assert run
    assert wait_for(lambda: not stack.relay.ring, 10)
    n_s = len(stack.db.all(WeightSample, WeightSample.run_id == run.run_id))
    n_e = len(stack.db.all(DispenseEvent, DispenseEvent.run_id == run.run_id))
    n_b = len(stack.db.all(RunBatch, RunBatch.run_id == run.run_id))
    d = stack.relay.runs[run.run_id]["disp"]
    # replay EVERYTHING the relay ever sent for this run, twice (as after a flapping link)
    stored = [(tp, p) for _t, tp, p, _r in stack.probe.msgs() if tp.startswith("cas/rly1/run/")
              and tp.rsplit("/", 1)[1] in ("start", "samples", "events", "complete")]
    assert len(stored) >= n_b
    before_acks = len(stack.relay.run_acks)
    for _ in range(2):
        for tp, p in stored:
            assert stack.relay.link.publish(tp, p, 1)
    assert wait_for(lambda: len(stack.relay.run_acks) >= before_acks + 2 * len(stored), 15), \
        (len(stack.relay.run_acks), before_acks, len(stored))
    assert len(stack.db.all(WeightSample, WeightSample.run_id == run.run_id)) == n_s == d.samples
    assert len(stack.db.all(DispenseEvent, DispenseEvent.run_id == run.run_id)) == n_e
    assert len(stack.db.all(RunBatch, RunBatch.run_id == run.run_id)) == n_b
    assert len(stack.runs()) == 1 and stack.runs()[0].status == "COMPLETE"


def test_out_of_order_batch_is_stored_but_ack_stays_at_the_gap(stack):
    """batch 2 arrives before batch 1: stored, ack watermark stays 0; batch 1 closes the gap."""
    link = stack.relay.link
    rid = "rly1-ch1-j99-1-ab12"
    base = {"run_id": rid, "boot_id": stack.relay.boot_id}
    start = dict(base, message_id="x-1", material_id="M1", channel_id="CH1", device_id="rly1",
                 job_id=99, target_g=7000, priority=0, firmware="t", start_weight_g=0,
                 config={"kp": 0.1, "ki": 0.0, "kd": 0.0})

    def sample(i):
        return {"idx": i, "uptime_ms": i * 100, "elapsed_ms": i * 100, "seq": i, "weight_g": i * 10,
                "target_g": 7000, "error_g": 7000 - i * 10, "stable": False, "weight_age_ms": 20,
                "relay1": False, "relay2": False, "state": "COARSE_DISPENSE"}

    def acks():
        return [a for a in stack.relay.run_acks if a["run_id"] == rid]

    stack.relay.runs[rid] = {"disp": type("D", (), {"released": False, "unknown_repeats": 0})(),
                             "outcome": None}
    link.publish("cas/rly1/run/start", json.dumps(start), 1)
    assert wait_for(lambda: acks() and acks()[-1]["state"] == "START_OK", 10)
    link.publish("cas/rly1/run/samples", json.dumps(dict(base, batch_seq=2, samples=[sample(3), sample(4)])), 1)
    assert wait_for(lambda: len(acks()) >= 2, 10)
    assert acks()[-1]["acked_batch_seq"] == 0, acks()
    link.publish("cas/rly1/run/samples", json.dumps(dict(base, batch_seq=1, samples=[sample(1), sample(2)])), 1)
    assert wait_for(lambda: len(acks()) >= 3, 10)
    assert acks()[-1]["acked_batch_seq"] == 2, acks()
    assert len(stack.db.all(WeightSample, WeightSample.run_id == rid)) == 4


def test_unknown_run_is_acked_unknown_and_minus_one(stack):
    rid = "rly1-ch1-j98-1-ff00"
    stack.relay.runs[rid] = {"disp": type("D", (), {"released": False, "unknown_repeats": 0})(),
                             "outcome": None}
    stack.relay.link.publish("cas/rly1/run/samples", json.dumps(
        {"run_id": rid, "boot_id": stack.relay.boot_id, "batch_seq": 1, "samples": [
            {"idx": 1, "uptime_ms": 1, "elapsed_ms": 1, "weight_g": 1, "target_g": 5000}]}), 1)
    assert wait_for(lambda: any(a["run_id"] == rid for a in stack.relay.run_acks), 10)
    ack = [a for a in stack.relay.run_acks if a["run_id"] == rid][0]
    assert ack == {"run_id": rid, "acked_batch_seq": -1, "state": "UNKNOWN_RUN"}
    assert stack.db.all(WeightSample, WeightSample.run_id == rid) == []


def test_rpc_corr_id_retry_creates_one_job(stack):
    args = {"material_id": "M1", "target_g": 5000, "profile_version_id": stack.ids["profiles"][0]}
    c1 = stack.op.request("queue.job.create", args, corr="job-once")
    r1 = stack.op.wait(c1, 10)
    assert r1 and r1["ok"]
    stack.op.responses.pop("job-once")
    stack.op._waiters.pop("job-once")
    c2 = stack.op.request("queue.job.create", args, corr="job-once")        # client retry
    r2 = stack.op.wait(c2, 10)
    assert r2 and r2["ok"] and r2["data"]["command_id"] == r1["data"]["command_id"]
    jobs = stack.db.all(DeviceCommand, DeviceCommand.command_type == "JOB")
    assert len(jobs) == 1
    assert wait_for(lambda: stack.relay.executed, 15)
    assert stack.relay.executed == [jobs[0].id]
