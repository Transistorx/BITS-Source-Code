"""Unit tests of the simulators' own contract logic (no broker, no backend).

These pin the rules the integration tests rely on: profile canonicalisation, the relay
weight cache, the run ring and ack trimming, command dedupe, operator freshness.
"""
import hashlib
import json
import threading
import time

import pytest

from app.services import profile_pin
from sim import canon
from sim.mqttx import BrokerInfo
from sim.opclient import LiveView, PREFIX
from sim.plant import Plant
from sim.relay import RelaySim
from sim.wcache import WeightCache


# --------------------------------------------------------------------------- canon

PROFILE = {"profile_id": "default", "version": 3, "kp": 0.5, "ki": 0.001, "kd": 0.1,
           "tolerance_g": 20, "max_overshoot_g": 100, "max_duration_ms": 60000,
           "window_ms": 500, "min_on_ms": 40, "min_off_ms": 40}


def wire(profile, **top):
    body = {"command_id": 1, "type": "JOB", "material_id": "M1", "target_g": 5000,
            "profile_id": profile.get("profile_id"), "profile_version": profile.get("version"),
            "profile": profile, **top}
    return json.dumps(body, separators=(",", ":"))


@pytest.mark.parametrize("value", [0.5, 0.001, 1e-3, 12.0, 0.10, 1e-05, 0.0, 1.0, 100000.0,
                                   0.30000000000000004, 5e-324, 1e22, 123456789.125])
def test_server_hash_equals_relay_hash_for_python_float_tokens(value):
    """What the server emits (json.dumps) hashes identically as server text and relay text."""
    profile = dict(PROFILE, kp=value, extra_staged=value)
    text = wire(profile)
    assert canon.hash_command_profile(text)[1] == profile_pin.profile_hash(profile)


def test_relay_hashes_received_text_not_a_reserialised_float():
    """If a sender used another float spelling the relay would hash THAT text: the server
    must hash what it sends (CONTRACT 9.3 'MUST hash the same text')."""
    profile = dict(PROFILE, kp=0.5, ki=0.001, kd=0.1)
    server_hash = profile_pin.profile_hash(profile)
    for spelled, token in (("1e-3", "0.001"), ("0.10", "0.1"), ("5E-1", "0.5")):
        text = wire(profile).replace(f":{token},", f":{spelled},").replace(f":{token}}}", f":{spelled}}}")
        if spelled not in text:
            continue
        assert canon.hash_command_profile(text)[1] != server_hash, spelled


def test_canonical_form_sorts_members_strips_whitespace_keeps_tokens():
    text = '{"b" : 1.50 , "a" : { "z" : 1e-3 , "y" : "x y" } , "c" : [ 1 , 2 ] }'
    assert canon.canonical(text) == '{"a":{"y":"x y","z":1e-3},"b":1.50,"c":[1,2]}'
    assert canon.profile_hash_of(canon.canonical(text)) == hashlib.sha256(
        canon.canonical(text).encode()).hexdigest()[:16]


@pytest.mark.parametrize("text,prefix", [
    ('{"a":1,"a":2}', "PIN_DUPLICATE_KEY"), ('{"a":01}', "PIN_MALFORMED"),
    ('{"a":1', "PIN_MALFORMED"), ('{"a":NaN}', "PIN_MALFORMED"), ('[1]', "PIN_MALFORMED")])
def test_canon_refuses_bad_text(text, prefix):
    with pytest.raises(canon.CanonError) as exc:
        canon.canonical(text)
    assert exc.value.args[0].startswith(prefix)


def test_duplicate_top_level_profile_member_is_refused():
    text = '{"command_id":1,"profile":{"kp":0.5},"profile":{"kp":0.1}}'
    with pytest.raises(canon.CanonError):
        canon.hash_command_profile(text)


# ------------------------------------------------------------------------- wcache

def frame(seq, up, ch="CH1", boot="aabbccdd", w=1000, age=20, **over):
    body = {"schema_version": 1, "boot_id": boot, "seq": seq, "uptime_ms": up, "channel": ch,
            "weight_g": w, "stable": True, "age_ms": age}
    body.update(over)
    return json.dumps(body).encode()


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def primed(clock):
    c = WeightCache(clock)
    assert c.feed(frame(1, 1000)) != "accepted"         # new boot: held
    clock.t += 0.045
    assert c.feed(frame(2, 1045)) == "accepted"         # consecutive: accepted
    return c


def test_new_boot_needs_two_consecutive_messages_and_is_unstable_first():
    ck = Clock()
    c = WeightCache(ck)
    assert c.get("CH1") is None
    assert c.feed(frame(10, 1000)) == "held_new_boot" and c.get("CH1") is None
    assert c.feed(frame(12, 1090)) == "held_new_boot" and c.get("CH1") is None   # gap
    assert c.feed(frame(13, 1135)) == "accepted"
    assert c.get("CH1")[1] is False                      # forced unstable on the first accepted
    ck.t += 0.045
    assert c.feed(frame(14, 1180)) == "accepted" and c.get("CH1")[1] is True


def test_shared_counter_accepts_new_boot_when_other_channel_is_the_predecessor():
    ck = Clock()
    c = WeightCache(ck)
    c.feed(frame(5, 1000, ch="CH1"))
    assert c.feed(frame(6, 1045, ch="CH2")) == "accepted"


def test_duplicate_older_and_uptime_regression_dropped_without_moving_freshness():
    ck = Clock()
    c = primed(ck)
    stamp = c.last_valid_age_s("CH1")
    ck.t += 1.0
    for raw in (frame(2, 1045, w=99999), frame(1, 1000, w=99999), frame(3, 900, w=99999)):
        assert c.feed(raw) != "accepted"
    assert c.get("CH1")[0] == 1000                       # old value kept
    assert c.last_valid_age_s("CH1") == pytest.approx(stamp + 1.0)   # freshness did not move
    assert c.feed(frame(3, 2090, w=1100)) == "accepted"   # 1 s of clock, 1 s of uptime


def test_other_boot_id_never_replaces_the_trusted_boot_with_one_message():
    ck = Clock()
    c = primed(ck)
    assert c.feed(frame(900, 5, boot="deadbeef", w=99999)) == "held_new_boot"
    assert c.get("CH1")[0] == 1000
    assert c.feed(frame(3, 1090, w=1100)) == "accepted"  # trusted boot continues


def test_staleness_over_5s_and_frame_age_bound():
    ck = Clock()
    c = primed(ck)
    ck.t += 4.9
    assert c.get("CH1") is not None
    ck.t += 0.2
    assert c.get("CH1") is None
    assert c.feed(frame(3, 6000, age=5001)) == "age"


@pytest.mark.parametrize("raw,why", [
    (frame(3, 1090, w=-1), "weight"), (frame(3, 1090, w=100001), "weight"),
    (b'{"schema_version":1,"boot_id":"aabbccdd","seq":3,"uptime_ms":1090,"channel":"CH1",'
     b'"weight_g":10.0,"age_ms":1}', "malformed"),
    (frame(3, 1090, channel="CH3"), "channel"), (frame(3, 1090, schema_version=2), "schema"),
    (frame(3, 1090, boot="zz"), "boot_id"), (frame(True, 1090), "seq_uptime"),
    (b"x" * 300, "oversize"), (b"not json", "malformed")])
def test_invalid_frames_rejected(raw, why):
    ck = Clock()
    c = primed(ck)
    assert c.feed(raw) == why
    assert c.get("CH1")[0] == 1000


def test_retained_and_late_transit_frames_rejected():
    ck = Clock()
    c = primed(ck)
    assert c.feed(frame(3, 1090), retained=True) == "retained"
    ck.t += 0.045 + 0.4                                  # arrives 400 ms later than its uptime says
    assert c.feed(frame(3, 1090)) == "transit"
    assert c.feed(frame(4, 1135 + 400)) == "accepted"    # back to the minimum offset...
    assert c.get("CH2") is None                          # a CH1 frame never feeds CH2


# ------------------------------------------------------------------------ relay sim

class FakeLink:
    def __init__(self):
        self.sent = []
        self.is_ready = True
        self.ready = threading.Event()

    def publish(self, topic, payload, qos=0, retain=False):
        if not self.is_ready:
            return False
        self.sent.append((topic, payload, qos, retain))
        return True

    def runs(self, kind):
        return [json.loads(p) for t, p, *_ in self.sent if t.endswith(f"/run/{kind}")]


def make_relay(**kw):
    plant = Plant()
    plant.set_relay("CH1", True)                         # leftover state from before "boot"
    r = RelaySim("rly1", "snd1", plant, BrokerInfo(1), **kw)
    r.link = FakeLink()
    return r, plant


def feed_weight(r, n=2, w=1000, boot="aabbccdd"):
    for i in range(n):
        r.cache.feed(frame(1 + i, 1000 + 45 * i, boot=boot, w=w))


def job_payload(cid=7, profile=PROFILE, **over):
    return wire(profile, command_id=cid, profile_hash=profile_pin.profile_hash(profile), **over).encode()


def test_relay_boots_with_relays_off():
    r, plant = make_relay()
    assert plant.relay("CH1") is False and plant.relay("CH2") is False
    plant.stop()


def test_relay_job_is_acked_with_applied_profile_and_executed_once():
    r, plant = make_relay()
    feed_weight(r)
    payload = job_payload(7)
    r._on_command(payload, time.monotonic())
    ack = r.acks_sent[-1]
    assert ack["state"] == "QUEUED" and ack["applied_profile"] == {
        "profile_id": "default", "version": 3, "hash": profile_pin.profile_hash(PROFILE)}
    r._control(time.monotonic())
    assert r.executed == [7]
    for _ in range(3):                                   # duplicate delivery: re-ACK, never re-run
        r._on_command(payload, time.monotonic())
    r._control(time.monotonic())
    assert r.executed == [7] and r.duplicates == 3 and not r.queue
    plant.stop()


def test_relay_refuses_hash_mismatch_missing_profile_and_bad_pin():
    r, plant = make_relay()
    feed_weight(r)
    bad = json.loads(job_payload(8))
    bad["profile_hash"] = "0" * 16
    r._on_command(json.dumps(bad).encode(), time.monotonic())
    assert r.refused[-1] == (8, "PROFILE_HASH_MISMATCH")
    nopin = json.loads(job_payload(9))
    nopin.pop("profile")
    r._on_command(json.dumps(nopin).encode(), time.monotonic())
    assert r.refused[-1] == (9, "PROFILE_REQUIRED")
    oor = json.dumps(dict(PROFILE, kp=1.5))
    r._on_command(job_payload(10, dict(PROFILE, kp=1.5)), time.monotonic())
    assert r.refused[-1][1].startswith("PIN_OUT_OF_RANGE")
    r._on_command(job_payload(11, dict(PROFILE, tolerance_g=1.5)), time.monotonic())
    assert r.refused[-1][1].startswith("PIN_NOT_INTEGER")
    assert not r.queue and r.executed == []
    plant.stop()


def test_relay_ignores_retained_commands_and_wrong_channel():
    r, plant = make_relay()
    r._on_msg("cas/rly1/commands", job_payload(5), True)
    assert not r._inbox
    r._on_command(job_payload(6, material_id="M1", channel_id="CH2"), time.monotonic())
    assert r.refused[-1][0] == 6 and not r.queue
    plant.stop()


def run_with_items(r, n_items):
    """Hand-build a run with n ring items (start=0, then samples 1..n-1)."""
    feed_weight(r)
    r.begin_adhoc("CH1", 5000)
    r._inbox_pop = None
    item = r._inbox.popleft()
    r._adhoc(item[1], item[2])
    d = r.disp["CH1"]
    for _ in range(n_items - 1 - 2):                     # JOB_ASSIGNED + JOB_STARTED already queued
        d.pending.append({"idx": d.samples + 1, "elapsed_ms": 0, "weight_g": 1, "target_g": 5000})
        d.samples += 1
        r._flush_samples(d)
    return d


def test_ring_two_in_flight_cumulative_trim_and_minus_one_trims_nothing():
    r, plant = make_relay(resend_s=10.0)
    d = run_with_items(r, 6)
    assert [i.seq for i in r.ring] == [0, 1, 2, 3, 4, 5]
    r._pump(time.monotonic())
    assert [i.seq for i in r.ring if i.sent_at is not None] == [0, 1]       # <= 2 in flight
    r._pump(time.monotonic())
    assert len(r.link.runs("start")) + len(r.link.runs("events")) + len(r.link.runs("samples")) == 2
    rid = d.run_id
    r._on_run_ack(json.dumps({"run_id": rid, "acked_batch_seq": -1, "state": "BATCH_OK"}).encode())
    assert len(r.ring) == 6                              # -1 means nothing stored
    r._on_run_ack(json.dumps({"run_id": rid, "acked_batch_seq": 0, "state": "START_OK"}).encode())
    assert [i.seq for i in r.ring] == [1, 2, 3, 4, 5]
    r._pump(time.monotonic())                            # window of 2: only seq 2 joins seq 1
    r._on_run_ack(json.dumps({"run_id": rid, "acked_batch_seq": 3, "state": "BATCH_OK"}).encode())
    assert [i.seq for i in r.ring] == [3, 4, 5]          # 3 was never published: not released
    r._on_run_ack(json.dumps({"run_id": "someone-else", "acked_batch_seq": 99,
                              "state": "COMPLETE"}).encode())
    assert len(r.ring) == 3                              # another run's ack touches nothing
    r._on_run_ack(json.dumps({"run_id": rid, "acked_batch_seq": 5, "state": "COMPLETE_PARTIAL",
                              "missing_ranges": {"batch_seq": [[2, 2]]}}).encode())
    assert r.ring == [] and r.outcome(rid) == "COMPLETE_PARTIAL"
    plant.stop()


@pytest.mark.parametrize("state", ["COMPLETE", "INTERRUPTED", "REJECTED"])
def test_release_states_drop_the_whole_run(state):
    r, plant = make_relay()
    d = run_with_items(r, 5)
    r._on_run_ack(json.dumps({"run_id": d.run_id, "acked_batch_seq": -1, "state": state}).encode())
    assert r.ring == [] and r.outcome(d.run_id) == state
    plant.stop()


def test_resend_after_timeout_and_after_reconnect_oldest_first_and_unknown_run_abandon():
    r, plant = make_relay(resend_s=0.2)
    d = run_with_items(r, 4)
    r._pump(time.monotonic())
    first = len(r.link.sent)
    r._pump(time.monotonic())
    assert len(r.link.sent) == first                     # nothing new until the timeout
    time.sleep(0.25)
    r._pump(time.monotonic())
    assert len(r.link.sent) > first                      # unacked item sent again
    r._on_ready(time.monotonic())                        # reconnect: everything unsent again
    assert all(i.sent_at is None for i in r.ring)
    r.link.sent.clear()
    r._pump(time.monotonic())
    assert [json.loads(p).get("batch_seq", 0) for _t, p, *_ in r.link.sent] == [0, 1]
    for n in range(3):
        r._pump(time.monotonic())                        # a cycle = items sent, then UNKNOWN_RUN
        unknown_cycle(r, d)
        assert r.ring and all(i.sent_at is None for i in r.ring), n
    r._pump(time.monotonic())
    unknown_cycle(r, d)
    assert r.ring == [] and r.outcome(d.run_id) == "ABANDONED"
    plant.stop()


def unknown_cycle(r, d):
    """One resend cycle: UNKNOWN_RUN for every item in flight (the 2nd reply is absorbed, not a new cycle)."""
    for _ in range(max(1, sum(1 for i in r.ring if i.sent_at is not None))):
        r._on_run_ack(json.dumps({"run_id": d.run_id, "acked_batch_seq": -1, "state": "UNKNOWN_RUN"}).encode())


def alive(r):
    """Any valid run/ack of any run: the backend demonstrably answered (response generation bumps)."""
    r._on_run_ack(json.dumps({"run_id": "other-run", "acked_batch_seq": 0, "state": "BATCH_OK"}).encode())


def _strict_ack(r, d, token, state):
    r._on_run_ack(('{"run_id":"%s","acked_batch_seq":%s,"state":"%s"}' % (d.run_id, token, state)).encode())


@pytest.mark.parametrize("token", ["-01", "-1.5", "-2", "4294967296", "99999999999"])
def test_strict_ack_malformed_acked_batch_seq_ignores_whole_ack(token):
    r, plant = make_relay(strict_acks=True)
    d = run_with_items(r, 5)
    n = len(r.ring)
    for state in ("BATCH_OK", "REJECTED"):
        _strict_ack(r, d, token, state)
    assert len(r.ring) == n and r.ack_ignored == 2
    plant.stop()


def test_strict_ack_minus_one_is_valid_but_never_trims():
    r, plant = make_relay(strict_acks=True)
    d = run_with_items(r, 5)
    n = len(r.ring)
    _strict_ack(r, d, "-1", "BATCH_OK")
    assert len(r.ring) == n and r.ack_ignored == 1
    plant.stop()


def test_strict_ack_minus_one_rejected_releases_run():
    r, plant = make_relay(strict_acks=True)
    d = run_with_items(r, 5)
    _strict_ack(r, d, "-1", "REJECTED")
    assert r.ring == [] and r.outcome(d.run_id) == "REJECTED" and r.ack_ignored == 0
    plant.stop()


def test_strict_ack_unknown_run_resends_then_abandons_after_three():
    r, plant = make_relay(strict_acks=True)
    d = run_with_items(r, 4)
    for n in range(3):
        r._pump(time.monotonic())
        _strict_ack(r, d, "-1", "UNKNOWN_RUN")
        _strict_ack(r, d, "-1", "UNKNOWN_RUN")           # 2nd reply of the same cycle: absorbed
        assert r.ring and all(i.sent_at is None for i in r.ring), n
    r._pump(time.monotonic())
    _strict_ack(r, d, "-1", "UNKNOWN_RUN")
    assert r.ring == [] and r.outcome(d.run_id) == "ABANDONED"
    plant.stop()


def test_ring_overflow_by_bytes_drops_oldest_samples_only_and_counts():
    r, plant = make_relay(sample_bytes=800)
    d = run_with_items(r, 20)
    kinds = [i.kind for i in r.ring]
    assert kinds.count("start") == 1 and kinds.count("events") == 2   # never dropped
    assert r.ring_overflow > 0 and r.dropped_samples == r.ring_overflow * 1
    assert d.dropped == r.dropped_samples and r._sample_bytes() <= 800
    seqs = [i.seq for i in r.ring if i.kind == "samples" and not i.tomb]
    assert seqs == sorted(seqs) and seqs[0] > 3          # the OLDEST sample batches went
    tombs = [json.loads(i.payload) for i in r.ring if i.tomb]  # replaced by tombstones, same seq
    assert len(tombs) == r.ring_overflow == r.stats["tombstones"] and all(t["dropped"] is True for t in tombs)
    assert all(t["boot_id"] == r.boot_id for t in tombs)
    assert sum(t["samples"] for t in tombs) == r.dropped_samples
    assert not r.backlog
    plant.stop()


# ---- third-round run_log.c semantics (see esp32-relay-controller/.../run_log.c) -------------

def ack(r, d, state, seq=...):
    body = {"run_id": d.run_id, "state": state}
    if seq is not ...:
        body["acked_batch_seq"] = seq
    r._on_run_ack(json.dumps(body).encode())


def test_minus_one_trims_nothing_in_any_state_even_start_ok_and_counts_ignored():
    r, plant = make_relay()
    d = run_with_items(r, 5)
    r._pump(time.monotonic())
    for state in ("START_OK", "BATCH_OK"):
        ack(r, d, state, -1)
    assert len(r.ring) == 5 and r.ack_ignored == 2
    ack(r, d, "START_OK")                                # ABSENT field on START_OK trims the start
    assert [i.seq for i in r.ring] == [1, 2, 3, 4] and r.ack_ignored == 2
    ack(r, d, "BATCH_OK")                                # absent on BATCH_OK is ignored
    assert len(r.ring) == 4 and r.ack_ignored == 3
    plant.stop()


def test_ack_releases_only_items_published_at_least_once():
    r, plant = make_relay()
    d = run_with_items(r, 6)
    r._pump(time.monotonic())                            # only seq 0 and 1 are published
    ack(r, d, "BATCH_OK", 4294967295)
    assert [i.seq for i in r.ring] == [2, 3, 4, 5]
    assert all(not i.ever_sent for i in r.ring)
    plant.stop()


def test_resend_backoff_10_20_40_80_counted_tries_only_head_blamed_start_never_counted():
    r, plant = make_relay(max_inflight=2)
    d = run_with_items(r, 6)
    r.ring = [i for i in r.ring if i.kind == "samples"]      # head of the run is a samples batch
    head, behind = r.ring[0], r.ring[1]
    t = 1000.0
    r._pump(t)
    assert [i.seq for i in r.ring if i.sent_at is not None] == [head.seq, behind.seq]
    heads = []
    for gap in (10, 20, 40, 80, 80):
        r.link.sent.clear()
        r._pump(t + gap - 1)
        assert head.sent_at == t, gap                        # head backs off: not resent 1 s early
        t += gap
        alive(r)                                             # the backend answered something since the send
        r._pump(t)                                           # timeout: head tries++, resent at once
        heads.append(head.tries)
        assert behind.tries == 0                             # the item behind is never blamed
    assert heads == [1, 2, 3, 4, 5]
    alive(r)
    r._pump(t + 80)                                          # 6th counted timeout of the oldest item
    assert r.stats["resend_abandoned"] == 1 and head.tomb and head.tries == 0
    plant.stop()


def test_start_and_complete_are_never_released_nor_counted_by_timeouts():
    r, plant = make_relay(max_inflight=2)
    d = run_with_items(r, 3)
    r._finish("CH1", "COMPLETE", "JOB_COMPLETE", None)
    r._feed_ring()
    t = 0.0
    for _ in range(30):                                      # backend answers other things, never this run
        alive(r)
        t += 90
        r._pump(t)
    kinds = [i.kind for i in r.ring]
    assert "start" in kinds and "complete" in kinds
    assert r.stats["abandoned_items"] == 0
    assert all(i.tries == 0 for i in r.ring if i.kind in ("start", "complete"))
    plant.stop()


def test_samples_head_abandon_becomes_tombstone_keeping_batch_seq_then_tombstone_is_released():
    r, plant = make_relay(max_inflight=1)
    d = run_with_items(r, 4)
    r.ring = [i for i in r.ring if i.kind == "samples"]  # head of the run is a samples batch
    head = r.ring[0]
    seq, n = head.seq, head.n_samples
    big = head.size
    t = 0.0
    r._pump(t)
    for gap in (10, 20, 40, 80, 80, 80):
        t += gap
        alive(r)
        r._pump(t)
    assert head.tomb and head.seq == seq and head.tries == 0
    assert json.loads(head.payload) == {"run_id": d.run_id, "boot_id": r.boot_id, "batch_seq": seq,
                                        "dropped": True, "samples": n}
    assert head.size < big                               # real size shrank (byte budget by real size)
    assert r.stats["tombstones"] == 1 and r.stats["resend_abandoned"] == 1
    assert r.dropped_samples == n and d.dropped == n
    for gap in (10, 20, 40, 80, 80, 80, 80, 80):         # the tombstone itself times out: NEVER released
        t += gap
        alive(r)
        r._pump(t)
    assert head in r.ring and head.tomb and r.stats["abandoned_items"] == 0 and r.stats["resend_abandoned"] == 1
    plant.stop()


def test_slot_reserve_is_staggered_samples_events_6_start_4_terminal_2_complete_0():
    r2, plant2 = make_relay(ring_slots=12)
    d2 = run_with_items(r2, 3)                                  # start + 2 events: 9 free
    assert r2._add(Item_for(r2, d2, "events", 10)) is True      # 8 free
    assert r2._add(Item_for(r2, d2, "samples", 11)) is True     # 7 free
    assert r2._add(Item_for(r2, d2, "samples", 12)) is True     # 6 free: samples/events stop here
    assert r2._add(Item_for(r2, d2, "samples", 13)) is False
    assert r2._add(Item_for(r2, d2, "events", 14)) is False
    for i, kind in enumerate(("start", "start")):               # start may use down to 4 free
        assert r2._add(Item_for(r2, d2, kind, 20 + i)) is True
    assert r2._add(Item_for(r2, d2, "start", 22)) is False      # 4 free: start stops
    term = Item_for(r2, d2, "events", 30)
    term.terminal = True
    assert r2._add(term) is True                                # 3 free
    term2 = Item_for(r2, d2, "events", 31)
    term2.terminal = True
    assert r2._add(term2) is True                               # 2 free
    term3 = Item_for(r2, d2, "events", 32)
    term3.terminal = True
    assert r2._add(term3) is False                              # terminal stops at 2 free
    assert r2._add(Item_for(r2, d2, "complete", 40)) is True    # 1 free
    assert r2._add(Item_for(r2, d2, "complete", 41)) is True    # 0 free
    assert r2._add(Item_for(r2, d2, "complete", 42)) is False   # truly full
    plant2.stop()


def Item_for(r, d, kind, seq):
    from sim.relay import Item
    return Item(d.run_id, seq, kind, f"cas/{r.id}/run/{kind}",
                json.dumps({"run_id": d.run_id, "batch_seq": seq}), 1 if kind == "samples" else 0)


def test_unknown_run_counted_once_per_resend_cycle_and_abandoned_after_three():
    r, plant = make_relay()
    d = run_with_items(r, 4)
    t = 0.0
    r._pump(t)                                           # window of 2: two items sent
    for _ in range(5):                                   # many replies in one cycle count once
        ack(r, d, "UNKNOWN_RUN")
    assert sorted({i.unknown for i in r.ring}) == [0, 1] and r.outcome(d.run_id) is None
    for cycle in (2, 3):
        t += 1
        r._pump(t)
        ack(r, d, "UNKNOWN_RUN")
        ack(r, d, "UNKNOWN_RUN")                         # owed reply of the same cycle: absorbed
        assert r.outcome(d.run_id) is None and max(i.unknown for i in r.ring) == cycle
    t += 1
    r._pump(t)
    ack(r, d, "UNKNOWN_RUN")                             # 4th counted cycle abandons
    assert r.ring == [] and r.outcome(d.run_id) == "ABANDONED"
    plant.stop()


def test_byte_budget_total_cap_refuses_noncritical_but_start_complete_use_reserve():
    r, plant = make_relay(total_bytes=600)
    d = run_with_items(r, 3)
    used = r._total_bytes()
    big = Item_for(r, d, "events", 20)
    big.payload = json.dumps({"x": "y" * 700})
    assert r._add(big) is False and r.stats["refused_total_cap"] == 1
    comp = Item_for(r, d, "complete", 21)
    comp.payload = json.dumps({"x": "y" * 700})
    assert r._add(comp) is True and r._total_bytes() == used + comp.size
    plant.stop()

def test_weight_silence_turns_relay_off_and_fails_job(monkeypatch):
    r, plant = make_relay(weight_timeout_s=5.0)
    ck = Clock()
    r.cache = WeightCache(ck)
    r.cache.feed(frame(1, 1000))
    ck.t += 0.045
    r.cache.feed(frame(2, 1045))
    r._on_command(job_payload(7), time.monotonic())
    r._control(time.monotonic())
    plant.set_relay("CH1", True)
    ck.t += 4.9
    r._control(time.monotonic())
    assert r.disp["CH1"] is not None and plant.relay("CH1")   # still within the 5 s bound
    ck.t += 0.2
    r._control(time.monotonic())
    assert plant.relay("CH1") is False and r.disp["CH1"] is None
    done = [json.loads(i.payload) for i in r.ring if i.kind == "complete"]
    assert done and done[0]["status"] == "FAILED"
    plant.stop()


# ------------------------------------------------------------------ operator freshness

def test_liveview_retained_is_unverified_and_never_live():
    ck = Clock()
    v = LiveView(ck)
    v.on_connection(True)
    v.on_message(PREFIX + "state/backend", {"online": True, "db": {"ok": True}}, True)
    v.on_message(PREFIX + "state/live", {"epoch": "e1", "seq": 1, "devices": [
        {"device_id": "rly1", "stale": False, "channels": [
            {"channel_id": "CH1", "weight_g": 500, "weight_valid": True}]}]}, True)
    assert v.status() == "OFFLINE" and v.last_known_unverified() and v.weight("rly1", "CH1") is None
    v.on_message(PREFIX + "state/backend", {"online": True, "db": {"ok": True}}, False)
    assert v.status() == "STALE"                         # backend ok but no fresh live data
    v.on_message(PREFIX + "evt/live", {"epoch": "e1", "seq": 5, "devices": [
        {"device_id": "rly1", "stale": False, "channels": [
            {"channel_id": "CH1", "weight_g": 600, "weight_valid": True}]}]}, False)
    assert v.status() == "LIVE" and v.weight("rly1", "CH1") == 600
    ck.t += 3.0
    assert v.status() == "STALE" and v.weight("rly1", "CH1") is None
    ck.t += 4.0
    assert v.status() == "OFFLINE"                       # heartbeat absent for 6 s
    v.on_connection(False)
    assert v.status() == "RECONNECTING"


def test_liveview_gap_and_epoch_change_trigger_resync():
    v = LiveView(Clock())
    v.on_connection(True)
    assert v.on_message(PREFIX + "evt/live", {"epoch": "e1", "seq": 1, "devices": []}, False) is False
    assert v.on_message(PREFIX + "evt/live", {"epoch": "e1", "seq": 2, "devices": []}, False) is False
    assert v.on_message(PREFIX + "evt/live", {"epoch": "e1", "seq": 4, "devices": []}, False) is True
    assert v.on_message(PREFIX + "evt/live", {"epoch": "e2", "seq": 1, "devices": []}, False) is True
    assert v.resyncs == 2 and v.gaps == 1


def test_liveview_backend_lwt_is_honoured_at_once():
    ck = Clock()
    v = LiveView(ck)
    v.on_connection(True)
    v.on_message(PREFIX + "state/backend", {"online": True, "db": {"ok": True}}, False)
    v.on_message(PREFIX + "evt/live", {"epoch": "e", "seq": 1, "devices": []}, False)
    assert v.status() == "LIVE"
    v.on_message(PREFIX + "state/backend", {"online": False}, False)
    assert v.status() == "OFFLINE"
