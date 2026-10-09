"""A whole system under test: broker + backend subprocess + SQLite file + sims + operator."""
import json
import threading
import time
from pathlib import Path

from argon2 import PasswordHasher
from sqlalchemy.orm import sessionmaker

from sim.mqttx import Link, wait_for, BrokerInfo
from sim.opclient import OperatorSim
from sim.plant import Plant
from sim.relay import RelaySim
from sim.sender import SenderSim

from .harness import PORT, PW, Backend, Broker, DB

RELAY, SENDER = "rly1", "snd1"


class Probe:
    """Read-only observer (sim_probe): every cas/# and bits/# message with its retain flag."""

    def __init__(self, broker: Broker):
        self.log: list[tuple[float, str, bytes, bool]] = []
        self.lock = threading.Lock()
        self.link = Link(BrokerInfo(PORT, broker.passwords), "sim_probe",
                         subs=[("cas/#", 1), ("bits/#", 1)], on_message=self._m).start()
        assert self.link.wait_ready(10), "probe could not connect"

    def _m(self, topic, payload, retain):
        with self.lock:
            self.log.append((time.monotonic(), topic, payload, retain))

    def msgs(self, topic_suffix: str | None = None, *, topic: str | None = None, retained=None):
        with self.lock:
            out = list(self.log)
        return [(t, tp, p, r) for t, tp, p, r in out
                if (topic is None or tp == topic) and (topic_suffix is None or tp.endswith(topic_suffix))
                and (retained is None or r == retained)]

    def bodies(self, topic_suffix=None, **kw):
        return [json.loads(p) for _t, _tp, p, _r in self.msgs(topic_suffix, **kw) if p]

    def backend_online(self):
        b = [x for x in self.bodies(topic="bits/v1/ops/state/backend", retained=False) if x.get("online")]
        return b[-1] if b else None

    def close(self):
        self.link.close()


def seed(db_url: str, *, profiles=None) -> dict:
    """Schema + materials + operators + MQTT-transport device rows + pinned profiles."""
    from app import database, models, models_ops  # noqa: F401
    from app.models import DeviceStatus, Material
    from app.models_ops import Operator
    from app.schemas import TuningProfileIn
    from app.services import auth as auth_service
    from app.services import profiles as profile_service

    engine = database._make_engine(db_url)
    models.Base.metadata.create_all(engine)
    Session = sessionmaker(bind=engine, expire_on_commit=False, autoflush=False, future=True)
    cheap = PasswordHasher(time_cost=1, memory_cost=8, parallelism=1)
    auth_service.set_hasher(cheap)
    out = {}
    try:
        with Session() as db:
            for mid, ch in (("M1", "CH1"), ("M2", "CH2")):
                db.add(Material(material_id=mid, name=mid, channel_id=ch, pump_id=f"Pump {mid[-1]}",
                                relay_id=f"Relay {mid[-1]}", scale_id=f"Scale {mid[-1]}", enabled=True))
            for dev, role in ((RELAY, "relay_controller"), (SENDER, "weight_sender")):
                db.add(DeviceStatus(device_id=dev, status_json={}, command_transport="MQTT",
                                    caps_json=["cmd_mqtt", "weight_mqtt"],
                                    updated_at=__import__("datetime").datetime(1970, 1, 1)))
            db.commit()
            for name, role in (("vue1", "viewer"), ("opr1", "operator"), ("adm1", "admin")):
                auth_service.create_operator(db, name, PW, role)
            db.add(Operator(username="norole", password_hash=cheap.hash(PW), role="none"))
            db.commit()
            base = dict(profile_id="default", target_g=5000, kp=0.5, ki=0.001, kd=0.1,
                        tolerance_g=60, max_overshoot_g=150, max_duration_ms=60000,
                        window_ms=500, min_on_ms=40, min_off_ms=40)
            for spec in (profiles or [{}]):
                row = dict(base, material_id="M1", channel_id="CH1", **spec)
                p = profile_service.create_profile(db, TuningProfileIn(**row))
                out.setdefault("profiles", []).append(p.profile_version_id)
            m2 = profile_service.create_profile(db, TuningProfileIn(
                **dict(base, material_id="M2", channel_id="CH2")))
            out["m2"] = m2.profile_version_id
            # profiles are created as drafts now: the harness activates them directly (the
            # validate/activate lifecycle is not under test here and would queue profile commands)
            from app.models import TuningProfile
            seen = set()                  # one active profile per pump/target (the migration refuses twins)
            for pid in [*out["profiles"], out["m2"]]:
                row = db.get(TuningProfile, pid)
                key = (row.material_id, getattr(row, "target_g", None))
                if getattr(row, "status", "active") != "active" and key not in seen:
                    row.status, row.active = "active", True
                seen.add(key)
            db.commit()
    finally:
        auth_service.set_hasher(PasswordHasher())
        engine.dispose()
    return out


class Stack:
    def __init__(self, broker: Broker, tmp: Path, *, profiles=None, backend_env=None,
                 relay_kw=None, sender_kw=None, plant_flow=2500.0):
        self.broker, self.tmp = broker, tmp
        self.db_url = "sqlite:///" + (tmp / "it.db").as_posix()
        self.ids = seed(self.db_url, profiles=profiles)
        self.db = DB(self.db_url)
        self.binfo = BrokerInfo(PORT, broker.passwords)
        self.backend = Backend(self.db_url, broker, tmp / "backend.log", **(backend_env or {}))
        self.plant = Plant(flow_gps=plant_flow)
        self.relay_kw, self.sender_kw = relay_kw or {}, sender_kw or {}
        self.probe: Probe | None = None
        self.sender: SenderSim | None = None
        self.relay: RelaySim | None = None
        self.op: OperatorSim | None = None
        self.relays: list[RelaySim] = []      # every relay incarnation (reboots)
        self.senders: list[SenderSim] = []
        self.ops: list[OperatorSim] = []

    # -- bring-up ---------------------------------------------------------------
    def up(self, *, backend=True, sender=True, relay=True, operator=True, login="opr1") -> "Stack":
        self.probe = Probe(self.broker)
        if backend:
            self.start_backend()
        if sender:
            self.start_sender()
        if relay:
            self.start_relay()
        if operator:
            self.start_operator(login)
        return self

    def start_backend(self, wait=True) -> None:
        before = self.probe.backend_online() if self.probe else None
        self.backend.start()
        if wait:
            assert self.wait_backend_online(prev=before), "backend did not come online\n" + self.backend.log_tail()

    def wait_backend_online(self, prev=None, timeout=30.0) -> bool:
        def ok():
            cur = self.probe.backend_online()
            return (cur is not None and cur.get("mqtt") == "CONNECTED"
                    and (prev is None or cur.get("epoch") != prev.get("epoch")))
        return bool(wait_for(ok, timeout, 0.1))

    def start_sender(self, wait=True, **kw) -> SenderSim:
        self.sender = SenderSim(SENDER, self.plant, self.binfo, **{**self.sender_kw, **kw}).start()
        self.senders.append(self.sender)
        assert not wait or self.sender.link.wait_ready(10), "sender sim did not connect"
        return self.sender

    def start_relay(self, wait=True, **kw) -> RelaySim:
        self.relay = RelaySim(RELAY, SENDER, self.plant, self.binfo, **{**self.relay_kw, **kw}).start()
        self.relays.append(self.relay)
        assert not wait or self.relay.link.wait_ready(10), "relay sim did not connect"
        return self.relay

    def start_operator(self, login="opr1", station="st1") -> OperatorSim:
        self.op = OperatorSim(station, self.binfo).start()
        self.ops.append(self.op)
        if login:
            r = self.op.login(login, PW)
            assert r["ok"], r
        return self

    def wait_weight(self, ch="CH1", timeout=10.0) -> bool:
        return bool(wait_for(lambda: self.relay.cache.get(ch) is not None, timeout))

    def wait_relay_known(self, timeout=15.0) -> bool:
        """backend has fresh relay_controller status from the sim"""
        from app.models import DeviceStatus

        def ok():
            row = self.db.get(DeviceStatus, RELAY)
            return bool(row and (row.status_json or {}).get("role") == "relay_controller"
                        and row.boot_id == self.relay.boot_id)
        return bool(wait_for(ok, timeout, 0.1))

    def ready(self) -> "Stack":
        assert self.wait_weight(), "relay never accepted weight/ctl frames"
        assert self.wait_relay_known(), "backend never saw relay status\n" + self.backend.log_tail()
        return self

    # -- actions ------------------------------------------------------------------
    def create_job(self, target=5000, material="M1", pv=None) -> int:
        pv = pv or (self.ids["profiles"][0] if material == "M1" else self.ids["m2"])
        r = self.op.call("queue.job.create", {"material_id": material, "target_g": target,
                                              "profile_version_id": pv})
        assert r["ok"], r
        return r["data"]["command_id"]

    def activate_only(self, pv: int) -> None:
        """Make ``pv`` the single active profile of its pump/target (direct DB flip, no lifecycle/commands)."""
        from app.models import TuningProfile
        with self.db.Session() as db:
            row = db.get(TuningProfile, pv)
            for other in db.query(TuningProfile).filter(TuningProfile.material_id == row.material_id,
                                                        TuningProfile.status == "active").all():
                if other.profile_version_id != pv and getattr(other, "target_g", None) == getattr(row, "target_g", None):
                    other.status, other.active = "validated", False
            row.status, row.active = "active", True
            db.commit()

    def command_wire(self, cid, device="rly1", timeout=5.0) -> bytes:
        """The exact bytes the backend published for a command (as seen by the probe)."""
        def find():
            for _t, _tp, p, _r in self.probe.msgs(topic=f"cas/{device}/commands"):
                if json.loads(p).get("command_id") == cid:
                    return p
        return wait_for(find, timeout, 0.05)

    def cmd(self, cid):
        from app.models import DeviceCommand
        return self.db.get(DeviceCommand, cid)

    def wait_cmd(self, cid, states, timeout=15.0):
        return wait_for(lambda: (c := self.cmd(cid)) is not None and c.state in states, timeout, 0.1)

    def runs(self):
        from app.models import DispenseRun
        return self.db.all(DispenseRun, order=DispenseRun.started_at)

    def wait_run(self, status_in=("COMPLETE", "COMPLETE_PARTIAL", "FAILED", "CANCELLED", "INTERRUPTED"),
                 timeout=30.0, index=-1):
        def ok():
            rs = self.runs()
            return rs and rs[index].status in status_in and rs[index]
        return wait_for(ok, timeout, 0.2)

    # -- faults ----------------------------------------------------------------------
    def reboot_relay(self, **kw) -> RelaySim:
        """Power-cycle the relay: crash (no DISCONNECT), then a brand-new instance."""
        self.relay.crash()
        time.sleep(0.3)
        return self.start_relay(**kw)

    def reboot_sender(self, **kw) -> SenderSim:
        self.sender.crash()
        time.sleep(0.3)
        return self.start_sender(**kw)

    def down(self) -> None:
        for s in self.ops + self.relays + self.senders:
            try:
                s.stop()
            except Exception:  # noqa: BLE001
                pass
        self.plant.stop()
        if self.probe:
            self.probe.close()
        self.backend.stop()
        self.backend.kill9()
