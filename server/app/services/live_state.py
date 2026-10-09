"""Bounded process-local telemetry; no database or network work under the lock.

Device uptime is only compared with that device's preceding uptime. Freshness
uses server monotonic time plus the reported acquisition age, never wall time.
Deploy with one worker until a shared live-state transport is provided.
"""
from copy import deepcopy
from datetime import datetime, timezone
from threading import Lock
from time import monotonic
from uuid import uuid4


MAX_LIVE_DEVICES = 16  # bound memory against topic-spoofed device ids
IDLE_EVICT_S = 60      # evict per-device order/live records idle this long


class LiveState:
    def __init__(self, clock=monotonic):
        self._clock = clock
        self._lock = Lock()
        self.clear()

    def clear(self):
        with self._lock:
            self._weight = None
            self._controller = None
            self._seq = None
            self._live = {}    # device_id -> {channel_id: (g, valid, age_ms, stable, stamp)}
            self._order = {}   # device_id -> (last accepted weight uptime_ms, stamp)
            self._sender = {}  # sender_id -> {channel_id: (g, age_ms, stable, stamp)}
            self._revision = 0
            self._received = self._rejected = 0
            self._weight_rejected = 0
            self._last_reject = {}
            self._boots = {}   # sender_id -> (current boot_id, previous boot_id, changed stamp)
            self._instance = uuid4().hex

    def reset_weight_session(self):
        with self._lock:
            self._seq = None
            self._weight = None

    def update_weight(self, payload):
        # This helper supports deterministic transport replay in the audit.
        # Production ingress currently uses the controller status endpoint.
        data = deepcopy(payload)
        seq = data.get("seq")
        grams, age = data.get("weight_g"), data.get("age_ms", 0)
        if type(grams) is not int or type(age) is not int or age < 0 or age > 3000:
            return False
        with self._lock:
            if seq is not None and self._seq is not None:
                delta = (seq - self._seq) & 0xFFFFFFFF
                if delta == 0 or delta >= 0x80000000:
                    self._rejected += 1
                    return False
            self._seq = seq
            self._weight = (data, self._clock())
            self._revision += 1
            self._received += 1
        return True

    def update_status(self, device_id, status):
        if status.get("role") != "relay_controller":
            return False
        data = deepcopy(status)
        now = self._clock()
        with self._lock:
            previous = self._controller
            if previous and previous[0] == device_id:
                old_uptime = previous[1].get("uptime_ms")
                uptime = data.get("uptime_ms")
                # A silent controller may have rebooted. Do not accept a
                # regression during an active session (old overlapping POST).
                if (isinstance(old_uptime, int) and isinstance(uptime, int)
                        and now - previous[2] < 10
                        and ((uptime - old_uptime) & 0xFFFFFFFF) >= 0x80000000):
                    self._rejected += 1
                    return False
            self._controller = (device_id, data, now,
                datetime.now(timezone.utc).isoformat())
            uptime = data.get("uptime_ms")
            # The status is a weight source too: if it is newer than the last
            # accepted weight message it supersedes any live slot; otherwise the
            # live slot keeps winning for the channels it carries.
            if not isinstance(uptime, int) or isinstance(uptime, bool) \
                    or self._accept_weight_order(device_id, uptime, now):
                self._live.pop(device_id, None)
            self._revision += 1
            self._received += 1
        return True

    def _accept_weight_order(self, device_id, uptime, now):
        """One total order per device across live topic, status and HTTP POST.
        Strictly greater uptime wins; a regression/duplicate is accepted only as
        a reboot after 10 s without any accepted weight. A forward jump is also
        bounded: delta_ms must not exceed the server-measured time since the last
        accepted update plus 5 s slack, so a forged huge uptime cannot poison the
        record; after >10 s of silence (reboot/outage) any forward jump is allowed.
        A rejected message never touches the record or its stamp.
        Caller holds the lock."""
        self._evict(now)
        prev = self._order.get(device_id)
        if prev is None and len(self._order) >= MAX_LIVE_DEVICES:
            return False
        if prev is not None:
            delta = (uptime - prev[0]) & 0xFFFFFFFF
            elapsed = now - prev[1]
            if not (0 < delta < 0x80000000):
                if delta == 0 or elapsed < 10:
                    return False
            elif elapsed <= 10 and delta > elapsed * 1000 + 5000:
                return False
        self._order[device_id] = (uptime, now)
        return True

    def _evict(self, now):
        """Drop devices idle > IDLE_EVICT_S (server monotonic). Caller holds lock."""
        for dev in [d for d, (_, stamp) in self._order.items() if now - stamp > IDLE_EVICT_S]:
            del self._order[dev]
            self._live.pop(dev, None)
            sender, _, channel = dev.partition("/")
            if channel and sender in self._sender:
                self._sender[sender].pop(channel, None)
                if not self._sender[sender]:
                    del self._sender[sender]

    def update_live(self, device_id, uptime_ms, channels, received_at=None):
        """Compact live-weight topic. Stored apart from the controller status:
        it never refreshes the status stamp, so a 5 Hz weight stream cannot mask
        a dead 1 Hz status. No DB, no network, no relay involvement."""
        now = self._clock() if received_at is None else received_at
        with self._lock:
            if not self._accept_weight_order(device_id, uptime_ms, now):
                self._rejected += 1
                return False
            slot = self._live.get(device_id)
            if slot is None:
                slot = self._live[device_id] = {}
            for c in channels:
                grams = c.get("weight_g")
                age = c.get("weight_age_ms")
                valid = bool(c.get("weight_valid")) and grams is not None and age is not None
                slot[c["channel_id"]] = (grams if valid else None, valid, age,
                                         bool(c.get("stable")) and valid, now)
            self._revision += 1
            self._received += 1
        return True

    def update_sender_weight(self, sender_id, channel_id, uptime_ms, grams, age_ms,
                             stable=False, received_at=None, boot_id=None):
        """cas/{sender}/telemetry/weight (1-2 Hz observability copy, never weight/ctl).
        Kept per sender and channel, apart from the relay's slot; the ordering key is
        '<sender>/<channel>' so the two channels' uptime stamps cannot collide."""
        now = self._clock() if received_at is None else received_at
        with self._lock:
            if boot_id is not None and not self._accept_boot(sender_id, boot_id, now):
                self._reject_weight(sender_id, "boot_id regression", now)
                return False
            if not self._accept_weight_order(f"{sender_id}/{channel_id}", uptime_ms, now):
                self._reject_weight(sender_id, "uptime order", now)
                return False
            if boot_id is not None:
                self._record_boot(sender_id, boot_id, now)
            self._sender.setdefault(sender_id, {})[channel_id] = (grams, age_ms, bool(stable), now)
            self._revision += 1
            self._received += 1
        return True

    def _accept_boot(self, sender_id, boot_id, now):
        cur = self._boots.get(sender_id)
        return not (cur and boot_id != cur[0] and boot_id == cur[1] and now - cur[2] < 10)

    def _record_boot(self, sender_id, boot_id, now):
        cur = self._boots.get(sender_id)
        if cur is None:
            if len(self._boots) >= MAX_LIVE_DEVICES:
                return
            self._boots[sender_id] = (boot_id, None, now)
        elif cur[0] != boot_id:
            self._boots[sender_id] = (boot_id, cur[0], now)

    def _reject_weight(self, device_id, reason, now):
        self._rejected += 1
        self._weight_rejected += 1
        if device_id not in self._last_reject and len(self._last_reject) >= MAX_LIVE_DEVICES:
            del self._last_reject[next(iter(self._last_reject))]
        self._last_reject[device_id] = reason

    def note_reject(self, device_id, reason):
        with self._lock:
            self._reject_weight(device_id, str(reason)[:120], self._clock())

    @staticmethod
    def _sender_view(senders, now):
        out = {}
        for sender, chans in senders.items():
            rows = []
            for channel, (grams, age, stable, stamp) in sorted(chans.items()):
                age_ms = age + int(max(0.0, now - stamp) * 1000)  # reported age + time since receipt
                valid = age_ms <= 3000
                rows.append({"channel_id": channel, "weight_g": grams if valid else None,
                             "weight_valid": valid, "weight_age_ms": age_ms,
                             "stable": stable and valid})
            out[sender] = {"channels": rows, "age_ms": min(
                int(max(0.0, now - c[3]) * 1000) for c in chans.values())}
        return out

    def sender_view(self, sender_id):
        with self._lock:
            chans = self._sender.get(sender_id)
            if not chans:
                return None
            revision, chans = self._revision, dict(chans)
        return revision, self._sender_view({sender_id: chans}, self._clock())[sender_id]

    def snapshot(self):
        with self._lock:
            self._evict(self._clock())
            weight = deepcopy(self._weight)
            controller = deepcopy(self._controller)
            live = dict(self._live.get(controller[0], ())) if controller else {}
            revision, received, rejected = self._revision, self._received, self._rejected
            instance = self._instance
            weight_rejected, last_reject = self._weight_rejected, dict(self._last_reject)
            senders = {s: dict(c) for s, c in self._sender.items()}
        now = self._clock()
        out = {"instance": instance, "revision": revision, "weight": None, "controller": None,
               "diagnostics": {"received": received, "rejected": rejected,
                               "weight_rejected": weight_rejected, "last_reject": last_reject}}
        out["senders"] = self._sender_view(senders, now)
        if weight:
            data, stamp = weight
            data["age_ms"] = data.get("age_ms", 0) + int((now - stamp) * 1000)
            data["weight_valid"] = data["age_ms"] <= 3000
            out["weight"] = data
        if controller:
            device_id, data, stamp, timestamp = controller
            age_ms = max(0, int((now - stamp) * 1000))
            for channel in data.get("channels", []):
                fast = live.get(channel.get("channel_id"))
                if fast:
                    grams, valid, base_age, stable, fast_stamp = fast
                    fast_age = (base_age + int(max(0.0, now - fast_stamp) * 1000)
                                if base_age is not None else None)
                    channel["weight_g"] = grams  # None stays None, never 0
                    channel["stable"] = stable
                    channel["weight_age_ms"] = fast_age
                    channel["weight_valid"] = bool(valid and fast_age is not None and fast_age <= 3000)
                    channel["weight_age_s"] = fast_age / 1000 if fast_age is not None else None
                    if age_ms > 10000:  # dead status: the weight stream must not mask it
                        channel["state"] = "OFFLINE"
                        channel["weight_valid"] = False
                    if not channel["weight_valid"]:  # never expose a stale number
                        channel["weight_g"] = None
                        channel["stable"] = False
                    continue
                acquisition_age = channel.get("weight_age_ms")
                # Legacy status omitted age: receipt age is an upper bound
                # only on transport age, so do not manufacture acquisition age.
                # UINT32_MAX is the firmware "no reading yet" sentinel: unknown age.
                if isinstance(acquisition_age, (int, float)) and 0 <= acquisition_age < 0xFFFFFFFF:
                    channel["weight_age_ms"] = acquisition_age + age_ms
                    channel["weight_valid"] = bool(channel.get("weight_valid")) and channel["weight_age_ms"] <= 3000
                else:
                    channel["weight_valid"] = False
                    channel["weight_age_ms"] = None
                channel["weight_age_s"] = (channel["weight_age_ms"] / 1000
                    if channel.get("weight_age_ms") is not None else None)
                if age_ms > 10000:
                    channel["state"] = "OFFLINE"
                    channel["weight_valid"] = False
                if not channel["weight_valid"] and "weight_g" in channel:
                    channel["weight_g"] = None
                    channel["stable"] = False
            out["controller"] = {"device_id": device_id, "status": data,
                "updated_at": timestamp, "age_seconds": age_ms / 1000,
                "online": age_ms <= 10000}
        return out


live_state = LiveState()
