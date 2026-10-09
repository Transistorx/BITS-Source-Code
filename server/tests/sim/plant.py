"""Physical stand-in shared by the sender and relay simulators (the scale and the pump).

The relay sim switches ``relay[ch]``; the sender sim reads ``weight(ch)``. Nothing in the
backend or the broker touches it: a dead broker or backend never changes the plant, so
the relay sim's own safety logic is what the fault-injection tests observe.
"""
import threading
import time


class Plant:
    def __init__(self, flow_gps: float = 2500.0, tick_s: float = 0.02):
        self.flow_gps = flow_gps
        self._w = {"CH1": 0.0, "CH2": 0.0}
        self._relay = {"CH1": False, "CH2": False}
        self._moved = {"CH1": time.monotonic(), "CH2": time.monotonic()}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._tick = tick_s
        self.relay_on_time = {"CH1": 0.0, "CH2": 0.0}   # seconds a relay was energised
        self._t = threading.Thread(target=self._run, name="plant", daemon=True)
        self._t.start()

    def _run(self) -> None:
        last = time.monotonic()
        while not self._stop.wait(self._tick):
            now = time.monotonic()
            dt, last = now - last, now
            with self._lock:
                for ch, on in self._relay.items():
                    if on:
                        self._w[ch] += self.flow_gps * dt
                        self.relay_on_time[ch] += dt
                        self._moved[ch] = now

    def set_relay(self, ch: str, on: bool) -> None:
        with self._lock:
            self._relay[ch] = bool(on)

    def relay(self, ch: str) -> bool:
        with self._lock:
            return self._relay[ch]

    def any_relay_on(self) -> bool:
        with self._lock:
            return any(self._relay.values())

    def weight(self, ch: str) -> int:
        with self._lock:
            return int(self._w[ch])

    def stable(self, ch: str) -> bool:
        with self._lock:
            return not self._relay[ch] and time.monotonic() - self._moved[ch] > 0.3

    def set_weight(self, ch: str, grams: float) -> None:
        with self._lock:
            self._w[ch] = float(grams)

    def stop(self) -> None:
        self._stop.set()
