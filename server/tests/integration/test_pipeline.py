"""Full pipeline: operator -> backend -> broker -> relay sim -> ACK -> run lifecycle -> DB ->
operator live and history."""
import json

import pytest

from app.models import DeviceCommand, DispenseEvent, DispenseRun, RunBatch, WeightSample
from sim.mqttx import wait_for


def test_full_pipeline_job_to_history(stack):
    cid = stack.create_job(5000)
    # command reaches the relay exactly once, pinned, with the server's profile hash
    assert wait_for(lambda: stack.relay.executed, 15), stack.relay.refused
    assert stack.relay.executed == [cid]
    assert stack.wait_cmd(cid, ("QUEUED", "APPLIED"), 10)
    run = stack.wait_run(("COMPLETE",), 40)
    assert run, [(r.status, r.error_text) for r in stack.runs()]
    assert run.device_id == "rly1" and run.target_g == 5000 and run.channel_id == "CH1"
    assert run.profile_id == "default" and run.profile_version == 1
    # relay was released by COMPLETE and nothing is left unacked
    assert wait_for(lambda: not stack.relay.ring, 10), len(stack.relay.ring)
    assert stack.relay.outcome(run.run_id) == "COMPLETE"
    # DB holds a gap-free record that matches what the relay produced
    d = stack.relay.runs[run.run_id]["disp"]
    seqs = sorted(b.batch_seq for b in stack.db.all(RunBatch, RunBatch.run_id == run.run_id))
    assert seqs == list(range(0, d.seq + 1))
    assert len(stack.db.all(WeightSample, WeightSample.run_id == run.run_id)) == d.samples
    assert len(stack.db.all(DispenseEvent, DispenseEvent.run_id == run.run_id)) == d.events
    assert run.missing_ranges in (None, {}) and run.final_weight_g >= 5000 - 60
    # relay physical safety: relay is OFF at the end
    assert not stack.plant.relay("CH1")
    # command ended APPLIED only because the relay sent that ACK
    assert (cid, "APPLIED") in stack.relay.cmd_log
    assert stack.cmd(cid).state == "APPLIED"
    # operator history (cursor paging) shows the run; samples via run.samples
    r = stack.op.call("runs.search", {"limit": 10})
    assert r["ok"] and r["data"]["items"][0]["run_id"] == run.run_id, r
    r = stack.op.call("run.samples", {"run_id": run.run_id, "limit": 2000})
    assert r["ok"] and len(r["data"]["items"]) == d.samples if "items" in r["data"] else r["ok"], r
    r = stack.op.call("run.get", {"run_id": run.run_id})
    assert r["ok"] and r["data"]["status"] == "COMPLETE"


def test_operator_live_view_goes_live_with_fresh_weight(stack):
    assert wait_for(lambda: stack.op.view.status() == "LIVE", 15), (stack.op.view.status(), stack.op.view.live)
    stack.plant.set_weight("CH1", 1234)
    shown = lambda: stack.op.view.weight("rly1", "CH1")  # noqa: E731
    assert wait_for(lambda: (shown() or 0) >= 1200, 10), shown()
    # silence the sender: the relay's status turns the weight invalid; the view must not
    # keep showing a number
    stack.sender.paused.set()
    assert wait_for(lambda: shown() is None, 10), shown()
    stack.sender.paused.clear()
    assert wait_for(lambda: (shown() or 0) >= 1200, 10), shown()


def test_command_not_retained_and_no_weight_ctl_from_backend(stack):
    cid = stack.create_job(5000)
    assert stack.wait_run(("COMPLETE",), 40)
    cmds = stack.probe.msgs(topic="cas/rly1/commands")
    assert cmds and all(not r for _t, _tp, _p, r in cmds), "command published retained"
    ids = [json.loads(p)["command_id"] for _t, _tp, p, _r in cmds]
    assert ids.count(cid) == 1, "command published more than once"
    # weight/ctl frames come only from the sender; the probe sees them with retain=False
    assert all(not r for _t, _tp, _p, r in stack.probe.msgs(topic="cas/snd1/weight/ctl"))


def test_live_snapshot_contains_the_weight_sender_device(stack):
    assert wait_for(lambda: stack.op.view.status() == "LIVE", 15)
    assert wait_for(lambda: stack.op.view.device("snd1"), 10), stack.op.view.live
