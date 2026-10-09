"""Weight-sender simulator (CONTRACT 8.2, 9.7).

Publishes cas/{S}/weight/ctl at ~22 Hz (QoS 0, never retained; one shared seq counter for
both channels, boot_id from the instance), the 1-2 Hz cas/{S}/telemetry/weight copy, the
5 s telemetry/status, retained birth + LWT on cas/{S}/status. ZERO/TARE commands are
ALWAYS answered FAILED (the CAS CI-150A remote frame is unverified): ``zero_applied``
makes it lie with APPLIED to prove the backend refuses to store that.
"""
import json
import os
import threading
import time

from .mqttx import BrokerInfo, Link


class SenderSim:
    def __init__(self, sender_id: str, plant, broker: BrokerInfo, *, channel: str = "CH1",
                 rate_hz: float = 22.0, zero_applied: bool = False, boot_id: str | None = None):
        self.id, self.plant, self.channel = sender_id, plant, channel
        self.rate_hz, self.zero_applied = rate_hz, zero_applied
        self.also: list[str] = []              # extra channels streamed alongside ``channel`` (two-pump runs)
        self.boot_id = boot_id or os.urandom(4).hex()
        self.t0 = time.monotonic()
        self.seq = 0
        self.paused = threading.Event()        # silence the weight stream (cable pulled)
        self.frames = 0
        self.commands: list[dict] = []
        self.acks: list[dict] = []
        self._seen: dict[int, dict] = {}
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.link = Link(broker, f"dev_{sender_id}", subs=[(f"cas/{sender_id}/commands", 1)],
                         on_message=self._on_msg, on_ready=self._birth,
                         will=(f"cas/{sender_id}/status", json.dumps({"online": False}), 1, True))

    # -- lifecycle -----------------------------------------------------------
    def start(self) -> "SenderSim":
        self.link.start()
        self._thread = threading.Thread(target=self._loop, name=f"sender-{self.id}", daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        self.link.close()

    def crash(self) -> None:
        self._stop.set()
        self.link.crash()

    def uptime_ms(self) -> int:
        return int((time.monotonic() - self.t0) * 1000) & 0xFFFFFFFF

    def _birth(self) -> None:
        self.link.publish(f"cas/{self.id}/status", json.dumps(
            {"online": True, "boot_id": self.boot_id, "role": "weight_sender",
             "caps": ["weight_mqtt"]}), 1, True)

    # -- publishing ------------------------------------------------------------
    def frame(self, channel: str | None = None, **over) -> dict:
        ch = channel or self.channel
        self.seq = (self.seq + 1) & 0xFFFFFFFF
        body = {"schema_version": 1, "boot_id": self.boot_id, "seq": self.seq,
                "uptime_ms": self.uptime_ms(), "channel": ch,
                "weight_g": self.plant.weight(ch), "stable": self.plant.stable(ch),
                "age_ms": 20, "cas_seq": self.seq, "source": "CAS"}
        body.update(over)
        return body

    def inject(self, payload, topic: str | None = None, retain: bool = False) -> bool:
        """Publish an arbitrary raw weight/ctl payload (out-of-order / foreign boot tests)."""
        data = payload if isinstance(payload, (bytes, str)) else json.dumps(payload, separators=(",", ":"))
        return self.link.publish(topic or f"cas/{self.id}/weight/ctl", data, 0, retain)

    def _loop(self) -> None:
        period = 1.0 / self.rate_hz
        nxt = time.monotonic()
        last_copy = last_status = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now < nxt:
                time.sleep(min(0.005, nxt - now))
                continue
            nxt += period
            if nxt < now - 0.5:
                nxt = now
            if self.paused.is_set() or not self.link.is_ready:
                continue
            body = self.frame()
            if self.link.publish(f"cas/{self.id}/weight/ctl", json.dumps(body, separators=(",", ":")), 0):
                self.frames += 1
            for extra in self.also:
                self.link.publish(f"cas/{self.id}/weight/ctl", json.dumps(self.frame(extra), separators=(",", ":")), 0)
            if now - last_copy >= 0.5:
                last_copy = now
                self.link.publish(f"cas/{self.id}/telemetry/weight", json.dumps(
                    {"schema_version": 1, "boot_id": self.boot_id, "uptime_ms": body["uptime_ms"],
                     "channel": body["channel"], "weight_g": body["weight_g"],
                     "stable": body["stable"], "age_ms": body["age_ms"]}, separators=(",", ":")), 0)
            if now - last_status >= 5.0 or last_status == 0.0:
                last_status = now
                self.link.publish(f"cas/{self.id}/telemetry/status", json.dumps(
                    {"role": "weight_sender", "firmware": "sender-sim", "cas_link": "ONLINE",
                     "cas_seq": self.seq, "cas_age_ms": 20, "ws_clients": 0,
                     "uptime_ms": body["uptime_ms"], "boot_id": self.boot_id,
                     "online": True}, separators=(",", ":")), 0)

    # -- commands (ZERO / TARE only) ------------------------------------------
    def _on_msg(self, topic: str, payload: bytes, retained: bool) -> None:
        if retained or not topic.endswith("/commands"):
            return                                    # retained commands are never executed
        try:
            body = json.loads(payload)
            cid = body["command_id"]
        except (ValueError, KeyError, TypeError):
            return
        if type(cid) is not int or cid < 1:
            return
        self.commands.append(body)
        if cid in self._seen:                          # duplicate: re-ACK stored outcome
            self._send_ack(self._seen[cid])
            return
        ctype = body.get("type") or body.get("command_type")
        if ctype in ("ZERO", "TARE"):
            reason = "VERIFICATION REQUIRED: CAS CI-150A remote ZERO/TARE frame unverified"
            ack = {"command_id": cid, "state": "APPLIED" if self.zero_applied else "FAILED",
                   "device_id": self.id, "channel_id": body.get("channel_id"),
                   "result": "success" if self.zero_applied else "failed", "reason": reason,
                   "error": reason, "boot_id": self.boot_id}
        else:
            ack = {"command_id": cid, "state": "FAILED", "device_id": self.id,
                   "error": "rejected: unknown type", "reason": "rejected", "result": "rejected",
                   "boot_id": self.boot_id}
        self._seen[cid] = ack
        self._send_ack(ack)

    def _send_ack(self, ack: dict) -> None:
        self.acks.append(ack)
        self.link.publish(f"cas/{self.id}/commands/ack", json.dumps(ack, separators=(",", ":")), 1)
