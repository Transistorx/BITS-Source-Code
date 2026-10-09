"""Operator plane over the real broker: roles, confirm flow, safety-negative checks (the
backend only expresses intent), history vs graphs, command lifecycle honesty."""
import time
from datetime import datetime, timedelta

from app.models import DeviceCommand
from app.models_ops import OperatorAudit
from sim.mqttx import wait_for


def test_operator_without_role_or_session_is_refused(make_stack):
    s = make_stack().up(login=None).ready()
    op = s.op
    assert op.call("queue.list")["code"] == "UNAUTHENTICATED"                      # no session
    assert op.call("auth.login", {"username": "opr1", "password": "wrong-wrong-1"}, token=None)["code"] \
        == "AUTH_FAILED"
    assert op.login("norole", "correct-horse-1")["ok"]                              # account exists...
    for method, args in (("queue.list", {}), ("runs.search", {}), ("live.resync", {}),
                         ("queue.job.create", {"material_id": "M1", "target_g": 5000,
                                               "profile_version_id": s.ids["profiles"][0]}),
                         ("control.cmd", {"action": "ESTOP"})):
        r = op.call(method, args)
        assert not r["ok"] and r["code"] == "UNAUTHENTICATED", (method, r)         # ...but no valid role
    assert s.db.all(DeviceCommand) == []                                            # nothing was created
    # a viewer reads but cannot write; nothing reaches the relay
    assert op.login("vue1", "correct-horse-1")["ok"]
    assert op.call("queue.list")["ok"]
    for method, args in (("control.cmd", {"action": "ESTOP"}),
                         ("queue.job.create", {"material_id": "M1", "target_g": 5000,
                                               "profile_version_id": s.ids["profiles"][0]})):
        r = op.call(method, args)
        assert r["code"] == "FORBIDDEN", (method, r)
    assert s.db.all(DeviceCommand) == []
    # an operator is still not an admin
    assert op.login("opr1", "correct-horse-1")["ok"]
    assert op.call("run.delete", {"run_id": "x"})["code"] == "FORBIDDEN"
    audits = s.db.all(OperatorAudit)
    assert audits and all(a.args_hash for a in audits)


def test_dangerous_actions_need_a_single_use_confirm_stop_class_is_never_gated(stack):
    op = stack.op
    r = op.call("control.cmd", {"action": "CLEAR"})
    assert r["code"] == "CONFIRM_REQUIRED"
    assert stack.db.all(DeviceCommand) == []
    b = op.call("confirm.begin", {"method": "control.cmd", "args": {"action": "CLEAR"}})
    assert b["ok"]
    token = b["data"]["confirm_token"]
    assert op.call("control.cmd", {"action": "CLEAR", "confirm_token": token})["ok"]
    assert op.call("control.cmd", {"action": "CLEAR", "confirm_token": token})["code"] == "CONFIRM_REQUIRED"
    assert op.confirm_call("control.cmd", {"action": "CLEAR"})["ok"]                # full two-step flow
    # ESTOP/PUMP_STOP/CANCEL: never gated, never rate limited
    assert op.call("control.cmd", {"action": "ESTOP"})["ok"]
    assert op.call("control.cmd", {"action": "PUMP_STOP", "channel_id": "CH1"})["ok"]
    types = sorted(c.command_type for c in stack.db.all(DeviceCommand))
    assert types == ["CLEAR", "CLEAR", "ESTOP", "PUMP_STOP"]


def test_estop_is_executed_by_the_relay_not_by_the_server(stack):
    """The backend queues intent; the relay turns itself OFF and ends the run."""
    stack.relay.begin_adhoc("CH1", 15000)                   # long dispense (~6 s)
    assert wait_for(lambda: stack.plant.relay("CH1"), 5)
    r = stack.op.call("control.cmd", {"action": "ESTOP"})
    assert r["ok"]
    assert wait_for(lambda: not stack.plant.relay("CH1"), 3)
    run = stack.wait_run(("CANCELLED", "FAILED", "COMPLETE"), 20)
    assert run and run.status == "CANCELLED"
    assert stack.cmd(r["data"]["command_id"]).state == "APPLIED"
    # and the server never published anything that could be mistaken for relay control
    assert not [1 for _t, tp, _p, _r in stack.probe.msgs() if tp.endswith("/weight/ctl")
                and tp != "cas/snd1/weight/ctl"]


def test_zero_tare_never_applied_even_if_the_sender_lies(make_stack):
    s = make_stack().up().ready()
    assert wait_for(lambda: s.sender.link.is_ready, 5)
    ids = []
    for action in ("ZERO", "TARE"):
        r = s.op.call("control.cmd", {"action": action, "channel_id": "CH1"})
        assert r["ok"], r
        ids.append(r["data"]["command_id"])
    assert wait_for(lambda: all(s.cmd(i).state == "FAILED" for i in ids), 10), \
        [s.cmd(i).state for i in ids]
    for i in ids:
        got = s.op.call("command.get", {"command_id": i})["data"]
        assert got["state"] == "FAILED" and got["result"] == "failed" and "VERIFICATION" in got["reason"]
    # a firmware bug / lying sender that ACKs APPLIED is converted to FAILED by the backend
    s.reboot_sender(zero_applied=True)
    assert wait_for(lambda: s.sender.link.is_ready, 5)
    time.sleep(1.0)                                         # fresh sender status
    r = s.op.call("control.cmd", {"action": "ZERO", "channel_id": "CH1"})
    assert r["ok"], r
    lie = r["data"]["command_id"]
    assert wait_for(lambda: s.cmd(lie).state != "PENDING", 10)
    assert wait_for(lambda: s.cmd(lie).state == "FAILED", 5), s.cmd(lie).state
    assert s.cmd(lie).error_text == "ACK_INVALID_ZERO_TARE_APPLIED"
    assert [c for c in s.db.all(DeviceCommand) if c.command_type in ("ZERO", "TARE")
            and c.state == "APPLIED"] == []


def test_arbitrary_target_is_in_history_but_never_in_graph_categories(stack):
    stack.relay.begin_adhoc("CH1", 7000)                    # not a canonical category
    run7 = stack.wait_run(("COMPLETE",), 40)
    assert run7 and run7.target_g == 7000
    assert wait_for(lambda: not stack.relay.busy() and not stack.relay.ring, 10)
    stack.plant.set_weight("CH1", 0)
    time.sleep(0.5)
    cid = stack.create_job(5000)
    assert wait_for(lambda: len(stack.runs()) == 2 and stack.runs()[-1].status == "COMPLETE", 40)
    op = stack.op
    hist = op.call("runs.search", {"limit": 50})
    assert hist["ok"] and {i["target_g"] for i in hist["data"]["items"]} == {7000, 5000}
    only7 = op.call("runs.search", {"target_g": 7000})
    assert only7["ok"] and [i["run_id"] for i in only7["data"]["items"]] == [run7.run_id]
    assert op.call("run.get", {"run_id": run7.run_id})["ok"]
    refused = op.call("graphs.category", {"target_g": 7000})
    assert not refused["ok"] and refused["code"] == "REFUSED"
    g5 = op.call("graphs.category", {"target_g": 5000})
    assert g5["ok"] and [i["target_g"] for i in g5["data"]["items"]] == [5000]
    assert all(i["run_id"] != run7.run_id for i in g5["data"]["items"])


def test_pending_command_past_ttl_is_expired_never_published(make_stack):
    s = make_stack(backend_env={"COMMAND_TTL_MS": 1000, "TRANSIENT_COMMAND_TTL_SECONDS": 1}).up().ready()
    old = datetime.utcnow() - timedelta(seconds=120)
    with s.db.engine.begin() as c:
        c.execute(DeviceCommand.__table__.insert().values(
            device_id="rly1", command_type="READY", channel_id="CH1", state="PENDING",
            held=False, promoted=False, created_at=old, updated_at=old))
    cid = s.db.all(DeviceCommand)[0].id
    assert wait_for(lambda: s.cmd(cid).state == "EXPIRED", 15), s.cmd(cid).state
    time.sleep(0.5)
    assert not s.probe.msgs(topic="cas/rly1/commands")      # never sent to the relay
    assert s.relay.cmd_log == []                            # and never executed


def test_acked_state_is_only_what_the_relay_said(stack):
    """DB command states can only be reached through relay ACKs: a JOB the relay refuses
    is FAILED with the relay's reason, not APPLIED."""
    r = stack.op.call("queue.job.create", {"material_id": "M1", "target_g": 5000,
                                           "profile_version_id": stack.ids["profiles"][0]})
    cid = r["data"]["command_id"]
    assert wait_for(lambda: cid in stack.relay.executed, 15)
    assert (cid, "QUEUED") in stack.relay.cmd_log and (cid, "APPLIED") in stack.relay.cmd_log
    assert stack.wait_run(("COMPLETE",), 40)
    assert wait_for(lambda: stack.cmd(cid).state == "APPLIED", 10)
    got = stack.op.call("command.get", {"command_id": cid})
    assert got["ok"] and got["data"]["state"] == "APPLIED"
