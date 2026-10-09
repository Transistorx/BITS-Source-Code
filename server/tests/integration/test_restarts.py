"""Restarts and crashes of one component at a time (CONTRACT 9.2, 9.5, 9.8). All slow: they
kill -9 / restart real processes."""
import json
import time

import pytest

from app.models import DeviceCommand, DispenseRun, RunBatch, WeightSample
from . import invariants
from sim.mqttx import wait_for

pytestmark = pytest.mark.slow

LONG = [{}, {"target_g": 15000}]          # profiles: [5000 g, 15000 g]; the 15000 g job runs ~6 s


def _running_with_data(s):
    def ok():
        runs = s.runs()
        return runs and runs[-1].status == "RUNNING" and s.db.scalar(
            "select count(*) from weight_samples where run_id = :r", r=runs[-1].run_id) > 0
    return wait_for(ok, 20, 0.1)


def _commands_published(s, cid):
    return [1 for _t, _tp, p, _r in s.probe.msgs(topic="cas/rly1/commands")
            if json.loads(p).get("command_id") == cid]


def test_backend_kill9_mid_run_no_replay_and_run_completes(make_stack):
    s = make_stack(profiles=LONG, relay_kw={"resend_s": 1.0},
                   backend_env={"RUN_STALE_SECONDS": 60}).up().ready()
    cid = s.create_job(15000, pv=s.ids["profiles"][1])
    assert _running_with_data(s)
    s.backend.kill9()
    time.sleep(1.5)                                   # relay keeps dispensing and buffering
    s.start_backend()
    run = s.wait_run(("COMPLETE", "COMPLETE_PARTIAL", "FAILED", "INTERRUPTED"), 60)
    assert run and run.status in ("COMPLETE", "COMPLETE_PARTIAL"), (run and run.status, run and run.error_text)
    assert wait_for(lambda: not s.relay.ring, 20)
    # the command was never resent by the restarted backend and never ran twice
    assert len(_commands_published(s, cid)) == 1
    assert s.relay.executed == [cid]
    assert not [c for c in s.db.all(DeviceCommand) if c.state in ("DELIVERED", "PENDING")]
    seqs = sorted(b.batch_seq for b in s.db.all(RunBatch, RunBatch.run_id == run.run_id))
    assert seqs == list(range(0, len(seqs)))          # gap-free, nothing double stored
    n = s.db.scalar("select count(*) from weight_samples where run_id = :r", r=run.run_id)
    d = s.relay.runs[run.run_id]["disp"]
    assert n == d.samples if run.status == "COMPLETE" else n <= d.samples
    assert invariants.check(s) == []


def test_backend_down_longer_than_stale_window_does_not_interrupt_a_healthy_run(make_stack):
    s = make_stack(profiles=LONG, relay_kw={"resend_s": 1.0},
                   backend_env={"RUN_STALE_SECONDS": 2}).up().ready()
    s.create_job(15000, pv=s.ids["profiles"][1])
    assert _running_with_data(s)
    s.backend.kill9()
    assert wait_for(lambda: not s.relay.busy(), 30)    # relay finished (or failed) on its own
    time.sleep(2.5)                                    # longer than RUN_STALE_SECONDS
    s.start_backend()
    # CONTRACT 9.8: staleness alone never finalises a run; the relay resends and it completes
    run = s.wait_run(("INTERRUPTED", "COMPLETE", "COMPLETE_PARTIAL", "FAILED"), 30)
    assert run and run.status in ("COMPLETE", "COMPLETE_PARTIAL", "FAILED"), (run and run.status)
    assert wait_for(lambda: s.relay.outcome(run.run_id) in ("COMPLETE", "COMPLETE_PARTIAL"), 20)
    time.sleep(2)
    assert s.runs()[-1].status == run.status
    assert invariants.check(s) == []


def test_broker_kill9_mid_dispense_relays_off_within_5s_then_everything_recovers(make_stack):
    s = make_stack(profiles=LONG, relay_kw={"resend_s": 1.0}).up().ready()
    mon = invariants.SafetyMonitor(s)
    mon.start()
    cid = s.create_job(15000, pv=s.ids["profiles"][1])
    assert wait_for(lambda: s.plant.relay("CH1") or s.plant.weight("CH1") > 500, 15)
    last_frame = time.monotonic() - (s.relay.cache.last_valid_age_s("CH1") or 0)
    s.broker.kill9()
    t_off = None
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if not s.plant.relay("CH1") and s.relay.disp["CH1"] is None:
            t_off = time.monotonic()
            break
        time.sleep(0.02)
    assert t_off is not None, "relay stayed energised after the broker died"
    assert t_off - last_frame <= 5.0 + 0.5, f"relay OFF only {t_off - last_frame:.2f}s after the last weight"
    assert not s.plant.any_relay_on()
    mon.stop()
    assert mon.violations == [], mon.violations
    # broker back: every client reconnects by itself; the failed job is NOT resumed or resent
    before = {n: c.link.connects for n, c in (("relay", s.relay), ("sender", s.sender))}
    s.broker.start()
    assert wait_for(lambda: s.relay.link.is_ready and s.sender.link.is_ready and s.op.link.is_ready, 30)
    assert s.relay.link.connects > before["relay"] and s.sender.link.connects > before["sender"]
    assert s.wait_backend_online(prev=None, timeout=30)
    run = s.wait_run(("FAILED", "COMPLETE_PARTIAL", "COMPLETE", "INTERRUPTED"), 40)
    assert run and run.status == "FAILED", (run.status, run.error_text)       # ended by weight loss
    assert wait_for(lambda: not s.relay.ring, 30)
    assert len(_commands_published(s, cid)) == 1                              # not replayed
    assert s.relay.executed == [cid] and not s.plant.any_relay_on()
    assert [r for _t, _tp, _p, r in s.probe.msgs(topic="cas/rly1/commands") if r] == []
    assert invariants.check(s) == []


def test_retained_state_after_broker_restart_is_never_shown_as_live(make_stack):
    s = make_stack().up().ready()
    assert wait_for(lambda: s.op.view.status() == "LIVE", 20)
    s.backend.stop()                                   # graceful: retained state/live stays behind
    assert wait_for(lambda: s.op.view.status() == "OFFLINE", 10), s.op.view.status()
    assert s.op.view.weight("rly1", "CH1") is None
    s.broker.restart(hard=False)                       # persistence keeps the retained messages
    assert wait_for(lambda: s.op.view.status() == "RECONNECTING" or s.op.link.is_ready, 5)
    assert wait_for(lambda: s.op.link.is_ready, 30)
    end = time.monotonic() + 3.0
    seen = set()
    while time.monotonic() < end:
        seen.add(s.op.view.status())
        assert s.op.view.weight("rly1", "CH1") is None
        time.sleep(0.05)
    assert "LIVE" not in seen, seen
    assert any(t.endswith("state/live") for t in s.op.retained_seen), "retained snapshot not delivered"
    assert s.op.view.last_known_unverified()
    s.start_backend()                                  # fresh non-retained data makes it LIVE again
    assert wait_for(lambda: s.op.view.status() == "LIVE", 30), s.op.view.status()
    assert wait_for(lambda: s.op.view.weight("rly1", "CH1") is not None, 10)


def test_relay_reboot_mid_job_interrupts_the_run_and_never_resumes(make_stack):
    s = make_stack(profiles=LONG, relay_kw={"resend_s": 1.0}).up().ready()
    cid = s.create_job(15000, pv=s.ids["profiles"][1])
    assert _running_with_data(s)
    old_boot = s.relay.boot_id
    on_before = s.plant.relay_on_time["CH1"]
    new = s.reboot_relay()
    assert not s.plant.any_relay_on()                  # boots with relays OFF
    assert new.boot_id != old_boot and new.executed == [] and not new.queue
    run = s.wait_run(("INTERRUPTED", "COMPLETE", "COMPLETE_PARTIAL", "FAILED"), 20)
    assert run and run.status == "INTERRUPTED", (run.status if run else None)
    t_check = s.plant.relay_on_time["CH1"]
    time.sleep(6)                                      # no auto-resume, ever
    assert s.plant.relay_on_time["CH1"] - t_check < 0.1 and not s.plant.any_relay_on()
    assert new.executed == [] and new.disp == {"CH1": None, "CH2": None}
    assert len(_commands_published(s, cid)) == 1       # the backend did not resend the job
    assert s.runs()[-1].status == "INTERRUPTED" and len(s.runs()) == 1
    assert invariants.check(s) == []


def test_unacked_command_has_unknown_outcome_and_a_lost_write_is_unknown_not_success(make_stack):
    """The relay never ACKs (lost on the way). The command must stay unresolved, then become
    FAILED/DELIVERED_NO_ACK_OUTCOME_UNKNOWN on recovery; never APPLIED. A write sent while
    the backend is down raises Unknown at the client and creates nothing."""
    from sim.opclient import Unknown
    s = make_stack(relay_kw={"drop_acks": True}, backend_env={"COMMAND_TTL_MS": 1000}).up().ready()
    cid = s.create_job(5000)
    assert wait_for(lambda: cid in s.relay.executed, 15)
    time.sleep(2.0)
    got = s.op.call("command.get", {"command_id": cid})["data"]
    assert got["state"] == "DELIVERED"                 # unresolved: not QUEUED/APPLIED
    s.backend.stop()
    assert wait_for(lambda: s.op.view.status() == "OFFLINE", 10)
    with pytest.raises(Unknown):
        s.op.call("queue.job.create", {"material_id": "M1", "target_g": 5000,
                                       "profile_version_id": s.ids["profiles"][0]},
                  timeout=2, corr="lost-write")
    s.start_backend()                                  # startup recovery closes the unknown outcome
    assert wait_for(lambda: s.cmd(cid).state == "FAILED", 20), s.cmd(cid).state
    assert s.cmd(cid).error_text == "DELIVERED_NO_ACK_OUTCOME_UNKNOWN"
    assert len([c for c in s.db.all(DeviceCommand) if c.command_type == "JOB"]) == 1   # the lost write never ran
    # the client resolves its unknown write by retrying the SAME corr_id: executed exactly once
    r = s.op.call("queue.job.create", {"material_id": "M1", "target_g": 5000,
                                       "profile_version_id": s.ids["profiles"][0]}, corr="lost-write")
    assert r["ok"]
    assert len([c for c in s.db.all(DeviceCommand) if c.command_type == "JOB"]) == 2
    assert [c for c in s.db.all(DeviceCommand) if c.state == "APPLIED"] == []
