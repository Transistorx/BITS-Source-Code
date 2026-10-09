"""Per-client rate limit, in-flight history cap and (client_id, corr_id) dedupe."""

import threading
from collections import deque
from datetime import datetime, timedelta

RATE_PER_S = 20
MAX_INFLIGHT_HISTORY = 4
DEDUPE_TTL = timedelta(minutes=5)
DEDUPE_MAX = 5000
MAX_CLIENTS = 1000


class RateLimiter:
    """Sliding 1 s window, 20 requests per client."""

    def __init__(self, per_s: int = RATE_PER_S):
        self._per_s = per_s
        self._lock = threading.Lock()
        self._hits: dict[str, deque] = {}

    def allow(self, client_id: str, now: datetime) -> bool:
        with self._lock:
            if len(self._hits) > MAX_CLIENTS:
                self._hits = {k: v for k, v in self._hits.items()
                              if v and (now - v[-1]).total_seconds() < 1}
            q = self._hits.setdefault(client_id, deque())
            while q and (now - q[0]).total_seconds() >= 1:
                q.popleft()
            if len(q) >= self._per_s:
                return False
            q.append(now)
            return True


class InflightHistory:
    def __init__(self, cap: int = MAX_INFLIGHT_HISTORY):
        self._cap = cap
        self._lock = threading.Lock()
        self._n: dict[str, int] = {}

    def acquire(self, client_id: str) -> bool:
        with self._lock:
            if self._n.get(client_id, 0) >= self._cap:
                return False
            self._n[client_id] = self._n.get(client_id, 0) + 1
            return True

    def release(self, client_id: str) -> None:
        with self._lock:
            left = self._n.get(client_id, 0) - 1
            if left > 0:
                self._n[client_id] = left
            else:
                self._n.pop(client_id, None)


class Dedupe:
    """In-memory (client_id, corr_id) -> stored response for 5 minutes.

    claim() marks a key in progress so a concurrent duplicate never executes the
    method twice: it gets IN_PROGRESS and the caller answers BUSY."""

    NEW, DONE, IN_PROGRESS = "new", "done", "in_progress"

    def __init__(self):
        self._lock = threading.Lock()
        self._items: dict[tuple[str, str], tuple[datetime, dict | None]] = {}

    def _evict(self, now: datetime) -> None:
        if len(self._items) < DEDUPE_MAX:
            return
        cutoff = now - DEDUPE_TTL
        self._items = {k: v for k, v in self._items.items() if v[0] > cutoff or v[1] is None}
        while len(self._items) >= DEDUPE_MAX:
            self._items.pop(next(iter(self._items)))

    def lookup(self, key: tuple[str, str], now: datetime):
        with self._lock:
            item = self._items.get(key)
            if item is None:
                return self.NEW, None
            stamp, resp = item
            if resp is None:
                return self.IN_PROGRESS, None
            if now - stamp > DEDUPE_TTL:
                del self._items[key]
                return self.NEW, None
            return self.DONE, resp

    def claim(self, key: tuple[str, str], now: datetime):
        """Atomically: DONE+response, IN_PROGRESS, or NEW (now claimed)."""
        with self._lock:
            item = self._items.get(key)
            if item is not None:
                stamp, resp = item
                if resp is None:
                    return self.IN_PROGRESS, None
                if now - stamp <= DEDUPE_TTL:
                    return self.DONE, resp
            self._evict(now)
            self._items[key] = (now, None)
            return self.NEW, None

    def finish(self, key: tuple[str, str], now: datetime, response: dict | None) -> None:
        """Store the response; None releases the claim without storing."""
        with self._lock:
            if response is None:
                self._items.pop(key, None)
            else:
                self._items[key] = (now, response)
