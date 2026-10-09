"""CONTRACT 9.3 across the real pipeline: the hash the server computes is the hash the relay
verifies from the received text, a mismatch makes the job not dispensable."""
import json
import time

from sim.mqttx import wait_for
from app.models import DispenseRun


FLOATY = [{"profile_id": "floaty", "kp": 1.0, "ki": 0.00001, "kd": 0.0},      # 1.0, 1e-05, 0.0
          {"profile_id": "floaty2", "kp": 0.5, "ki": 0.001, "kd": 0.10}]      # 0.5, 1e-3, 0.10


def test_server_hashed_text_equals_relay_verified_text_for_float_formatted_gains(make_stack):
    s = make_stack(profiles=FLOATY).up().ready()
    seen = []
    for pv in s.ids["profiles"]:
        before = len(s.relay.acks_sent)
        s.activate_only(pv)
        cid = s.create_job(5000, pv=pv)
        assert wait_for(lambda: cid in s.relay.executed, 15), (s.relay.refused, s.backend.log_tail())
        assert s.relay.refused == [], s.relay.refused        # relay verified profile_hash
        queued = next(a for a in s.relay.acks_sent[before:] if a["state"] == "QUEUED")
        wire_hash = queued["applied_profile"]["hash"]
        # the relay hashed the TEXT it received; the server hashes the object it sent
        sent = s.command_wire(cid).decode()
        from sim import canon
        assert canon.hash_command_profile(sent)[1] == wire_hash == json.loads(sent)["profile_hash"]
        assert s.wait_run(("COMPLETE",), 40, index=-1)
        assert wait_for(lambda: s.cmd(cid).state == "APPLIED", 10)
        assert s.cmd(cid).error_text is None
        seen.append(wire_hash)
        assert wait_for(lambda: not s.relay.busy() and not s.relay.ring, 10)
        s.plant.set_weight("CH1", 0)
        time.sleep(0.5)
    assert len(set(seen)) == 2
    assert [r.status for r in s.runs()] == ["COMPLETE", "COMPLETE"]


def test_profile_mismatch_marks_command_failed_and_run_is_not_accepted(make_stack):
    """The relay applies a different profile than the pin (tampered hash in applied_profile):
    FAILED PROFILE_MISMATCH, and the run it then starts is REJECTED, never stored."""
    s = make_stack(relay_kw={"tamper_applied": True}).up().ready()
    cid = s.create_job(5000)
    assert wait_for(lambda: cid in s.relay.executed, 15)
    assert wait_for(lambda: s.cmd(cid).state == "FAILED", 10), s.cmd(cid).state
    row = s.cmd(cid)
    assert row.error_text == "PROFILE_MISMATCH" and row.state == "FAILED"
    # job is not dispensable: the run/start is refused and no run exists
    assert wait_for(lambda: any(a["state"] == "REJECTED" for a in s.relay.run_acks), 10), s.relay.run_acks
    assert s.runs() == []
    assert wait_for(lambda: not s.relay.ring, 10)          # REJECTED released the run
    # the late APPLIED ack from the relay cannot resurrect the command
    time.sleep(1.5)
    assert s.cmd(cid).state == "FAILED"


def test_relay_refuses_hash_mismatch_and_malformed_pin_without_dispensing(stack):
    """Forged/corrupted commands delivered to the relay: refused before any relay activity."""
    from sim import canon  # noqa: F401
    real = {"profile_id": "default", "version": 1, "kp": 0.5, "ki": 0.001, "kd": 0.1,
            "tolerance_g": 60, "max_overshoot_g": 150, "max_duration_ms": 60000, "window_ms": 500,
            "min_on_ms": 40, "min_off_ms": 40}

    def deliver(cid, text):
        stack.relay._inbox.append(("cmd", text.encode(), time.monotonic()))
        assert wait_for(lambda: any(c == cid for c, _ in stack.relay.cmd_log), 5), cid

    base = {"type": "JOB", "command_type": "JOB", "material_id": "M1", "target_g": 5000,
            "channel_id": "CH1", "profile_id": "default", "profile_version": 1, "profile": real}
    from app.services import profile_pin
    good_hash = profile_pin.profile_hash(real)
    # 1. corrupted hash
    deliver(9001, json.dumps(dict(base, command_id=9001, profile_hash="0123456789abcdef")))
    # 2. duplicate key inside profile{}
    dup = json.dumps(dict(base, command_id=9002, profile_hash=good_hash))
    dup = dup.replace('"kp": 0.5,', '"kp": 0.5, "kp": 0.6,')
    deliver(9002, dup)
    # 3. no profile at all
    deliver(9003, json.dumps({k: v for k, v in dict(base, command_id=9003).items()
                              if k not in ("profile",)}))
    # 4. float where an integer is required
    deliver(9004, json.dumps(dict(base, command_id=9004, profile=dict(real, window_ms=500.5))))
    why = dict(stack.relay.refused)
    assert why[9001] == "PROFILE_HASH_MISMATCH"
    assert why[9002].startswith("PIN_DUPLICATE_KEY")
    assert why[9003] == "PROFILE_REQUIRED"
    assert why[9004].startswith("PIN_NOT_INTEGER")
    time.sleep(0.5)
    assert stack.relay.executed == [] and not stack.relay.queue
    assert not stack.plant.any_relay_on() and stack.runs() == []
