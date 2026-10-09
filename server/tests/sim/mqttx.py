"""Thin paho wrapper shared by the simulators.

* never queues while disconnected (a sim that wants a resend does it itself),
* ``ready`` only after CONNACK and every SUBACK,
* ``crash()`` drops the TCP connection without DISCONNECT (power loss: the broker
  publishes the LWT) and silences every callback.
"""
import threading
import time
from dataclasses import dataclass, field

import paho.mqtt.client as mqtt


@dataclass
class BrokerInfo:
    port: int
    passwords: dict = field(default_factory=dict)   # username -> password
    host: str = "127.0.0.1"


class Link:
    def __init__(self, broker: BrokerInfo, username: str, *, subs=(), on_message=None,
                 on_ready=None, on_lost=None, will=None, keepalive: int = 4):
        self.broker, self.username = broker, username
        self.subs = list(subs)              # [(filter, qos)]
        self._on_message, self._on_ready, self._on_lost = on_message, on_ready, on_lost
        self.dead = False
        self.ready = threading.Event()
        self.connects = 0
        self._pending: set = set()
        self._lock = threading.Lock()
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=username,
                        protocol=mqtt.MQTTv311)
        c.username_pw_set(username, broker.passwords.get(username, ""))
        if will:
            topic, payload, qos, retain = will
            c.will_set(topic, payload, qos=qos, retain=retain)
        c.reconnect_delay_set(min_delay=1, max_delay=2)
        c.on_connect, c.on_disconnect = self._connect_cb, self._disconnect_cb
        c.on_subscribe, c.on_message = self._sub_cb, self._msg_cb
        self.c = c
        self._keepalive = keepalive

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> "Link":
        self.c.connect_async(self.broker.host, self.broker.port, keepalive=self._keepalive)
        self.c.loop_start()
        return self

    def wait_ready(self, timeout: float = 10.0) -> bool:
        return self.ready.wait(timeout)

    def close(self) -> None:
        """Graceful: DISCONNECT, so the broker does NOT publish the LWT."""
        self.dead = True
        self.ready.clear()
        try:
            self.c.disconnect()
        except Exception:
            pass
        self.c.loop_stop()

    def crash(self) -> None:
        """Power loss: TCP gone without DISCONNECT, no callback fires afterwards."""
        self.dead = True
        self.ready.clear()
        self.c._thread_terminate = True
        sock = getattr(self.c, "_sock", None)
        try:
            if sock is not None:
                sock.close()
        except Exception:
            pass
        threading.Thread(target=self.c.loop_stop, daemon=True).start()

    # -- io ----------------------------------------------------------------
    def publish(self, topic: str, payload, qos: int = 0, retain: bool = False) -> bool:
        """True if handed to a ready link. Never queued while down."""
        if self.dead or not self.ready.is_set():
            return False
        try:
            info = self.c.publish(topic, payload, qos=qos, retain=retain)
        except Exception:
            return False
        return info.rc == 0

    @property
    def is_ready(self) -> bool:
        return self.ready.is_set() and not self.dead

    # -- callbacks (paho thread) ---------------------------------------------
    def _connect_cb(self, c, _u, _f, rc, _p=None):
        if self.dead or getattr(rc, "is_failure", False):
            return
        self.connects += 1
        with self._lock:
            self._pending = set()
            for flt, qos in self.subs:
                res = c.subscribe(flt, qos)
                self._pending.add(res[1])
            if not self.subs:
                self._set_ready()

    def _sub_cb(self, _c, _u, mid, rcs, _p=None):
        if self.dead:
            return
        with self._lock:
            self._pending.discard(mid)
            if not self._pending:
                self._set_ready()

    def _set_ready(self) -> None:
        self.ready.set()
        if self._on_ready:
            try:
                self._on_ready()
            except Exception:
                pass

    def _disconnect_cb(self, *_a):
        if self.dead:
            return
        self.ready.clear()
        if self._on_lost:
            try:
                self._on_lost()
            except Exception:
                pass

    def _msg_cb(self, _c, _u, msg):
        if self.dead or self._on_message is None:
            return
        try:
            self._on_message(msg.topic, bytes(msg.payload), bool(msg.retain))
        except Exception:
            pass


def wait_for(cond, timeout: float = 5.0, step: float = 0.02) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return cond()
