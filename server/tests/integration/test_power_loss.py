"""Whole-system power loss: kill -9 of backend / broker, hard crash of relay / sender /
operator app at random moments, restart in different orders, then assert the invariants.
Seeded (reproducible fault schedule); timing between processes is real, so outcomes vary but
the invariants must always hold."""
import random
import threading
import time

import pytest

from app.models import DeviceCommand, DispenseRun
from . import invariants
from sim.mqttx import wait_for
from sim.opclient import Unknown

pytestmark = pytest.mark.slow

FAULTS = ("backend", "broker", "relay", "sender", "operator")


class Chaos:
    def __init__(self, s, rng):
        self.s, self.rng, self.log = s, rng, []
        self.down: list[str] = []
        self._stop = threading.Event()
        self._emptier = threading.Thread(target=self._empty_bin, daemon=True)
        self._emptier.start()

    def _empty_bin(self) -> None:
        """The operator empties the container between dispenses (otherwise every later job
        would start above its target and be refused as OVERWEIGHT)."""
        idle_since = None
        while not self._stop.wait(0.25):
            r, p = self.s.relay, self.s.plant
            busy = r is None or r.busy() or p.any_relay_on()
            if busy:
                idle_since = None
            elif idle_since is None:
                idle_since = time.monotonic()
            elif time.monotonic() - idle_since > 1.5 and p.weight("CH1") > 50:
                p.set_weight("CH1", 0)

    # -- faults --------------------------------------------------------------
    def kill(self, what: str) -> None:
        s = self.s
        self.log.append(f"kill {what}")
        if what == "backend":
            s.backend.kill9()
        elif what == "broker":
            s.broker.kill9()
        elif what == "relay":
            s.relay.crash()
        elif what == "sender":
            s.sender.crash()
        elif what == "operator":
            s.op.crash()
        self.down.append(what)

    def restart(self, what: str) -> None:
        s = self.s
        self.log.append(f"restart {what}")
        if what == "backend":
            s.backend.start()
        elif what == "broker":
            s.broker.start()
        elif what == "relay":
            s.start_relay(wait=False)
        elif what == "sender":
            s.start_sender(wait=False)
        elif what == "operator":
            s.op = None
        self.down.remove(what)

    def restart_all_in_random_order(self) -> None:
        order = list(self.down)
        self.rng.shuffle(order)
        # relay/sender/operator need the broker; bring a dead broker back first or they only
        # reconnect later (their own reconnect logic) - both orders are exercised by the shuffle
        for what in order:
            self.restart(what)
            time.sleep(self.rng.uniform(0.0, 1.2))

    def try_job(self, target=5000) -> None:
        s = self.s
        try:
            r = s.op.call("queue.job.create", {"material_id": "M1", "target_g": target,
                                               "profile_version_id": s.ids["profiles"][0]}, timeout=3)
            self.log.append(f"job -> {r.get('code')}")
        except (Unknown, TimeoutError, AssertionError, AttributeError):
            self.log.append("job -> no answer")


def _scenario(make_stack, seed: int, n_faults: int):
    rng = random.Random(seed)
    s = make_stack(profiles=[{}, {"target_g": 15000}], relay_kw={"resend_s": 1.0},
                   backend_env={"RUN_STALE_SECONDS": 4, "COMMAND_TTL_MS": 2000}).up().ready()
    mon = invariants.SafetyMonitor(s)
    mon.start()
    chaos = Chaos(s, rng)
    chaos.try_job(5000)
    time.sleep(rng.uniform(0.5, 2.0))
    for _ in range(n_faults):
        targets = rng.sample(FAULTS, rng.randint(1, 3))
        for t in targets:
            chaos.kill(t)
            time.sleep(rng.uniform(0.0, 0.8))
        time.sleep(rng.uniform(0.3, 2.5))
        chaos.restart_all_in_random_order()
        time.sleep(rng.uniform(0.5, 2.0))
        if s.op is None or not getattr(s.op.link, "is_ready", False):
            s.start_operator(login=None)
            if s.backend.alive():
                try:
                    s.op.login("opr1", "correct-horse-1")
                except Exception:  # noqa: BLE001
                    pass
        chaos.try_job(5000)
    # everything up, then settle: relay finishes, rings drain, runs reach a terminal state
    for t in list(chaos.down):
        chaos.restart(t)
    assert wait_for(lambda: s.relay.link.is_ready and s.sender.link.is_ready, 30), chaos.log
    assert s.wait_backend_online(prev=None, timeout=40), chaos.log
    # ZERO/TARE during chaos must never end up APPLIED either
    try:
        s.start_operator(login="opr1") if s.op is None or not s.op.link.is_ready else None
        s.op.call("control.cmd", {"action": "ZERO", "channel_id": "CH1"}, timeout=3)
    except Exception:  # noqa: BLE001
        pass
    wait_for(lambda: not s.relay.busy() and not s.relay.ring, 60)
    time.sleep(4.5)                                    # > RUN_STALE_SECONDS: leftovers go stale
    s.backend.kill9()
    s.start_backend()                                  # final restart: startup recovery closes leftovers
    time.sleep(3.0)
    mon.stop()
    chaos._stop.set()
    return s, mon, chaos


@pytest.mark.parametrize("seed", [11, 23, 47])
def test_random_power_loss_keeps_every_invariant(make_stack, seed):
    s, mon, chaos = _scenario(make_stack, seed, n_faults=3)
    problems = invariants.check(s)
    print("CHAOS", seed, chaos.log, [(r.status, r.error_text) for r in s.runs()],
          [(c.id, c.command_type, c.state, c.error_text) for c in s.db.all(DeviceCommand)],
          "executed", [r.executed for r in s.relays], "max_blind", round(mon.max_blind_s, 2))
    assert mon.violations == [], (mon.violations, chaos.log)
    assert problems == [], (problems, chaos.log)
    assert not s.plant.any_relay_on() or s.relay.busy()
    # nothing is left RUNNING after recovery
    running = [r.run_id for r in s.db.all(DispenseRun) if r.status == "RUNNING"]
    assert running == [], (running, chaos.log)
    # the log of what happened is part of the failure message above; here: some work happened
    assert s.db.all(DeviceCommand) or "job -> no answer" in chaos.log
