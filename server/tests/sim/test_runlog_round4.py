"""Unit tests of the relay sim's run-history ring against run_log.c after its fourth fix round
(esp32-relay-controller/components/telemetry_client/run_log.c). No broker, no backend.

  1 staggered slot reserve (samples/events 6, start 4, terminal event 2, complete 0)
  2 a resend try counts only if SOME valid run/ack arrived since the item was sent; silence = backoff only
  3 start / complete / tombstone are NEVER released by a timeout; samples/events -> tombstone after 6 tries
  4 fair send selection across runs, global window 2
  5 UNKNOWN_RUN: the replies a cycle still owes are absorbed; a resend timeout / run drop clears them
  6 a new job closes the pending old run as COMPLETE FAILED / RUN_SUPERSEDED
  7 sample/event bodies are allocated >= 256 B (a tombstone always fits), accounted apart from their length
"""
import json

from sim.relay import Item, RUNLOG_MAX_RESENDS, TOMB_BODY_MAX
from sim.test_sim_units import alive, ack, feed_weight, frame, make_relay, unknown_cycle, Item_for, run_with_items


def _sent(r):
    return [i for i in r.ring if i.sent_at is not None]


def _samples_only(r):
    r.ring = [i for i in r.ring if i.kind == "samples"]
    return r.ring[0], r.ring[1]


# ---- 2 + 3 : counting ---------------------------------------------------------------------

def test_total_silence_never_counts_never_abandons_and_backoff_caps_at_80s():
    r, plant = make_relay(max_inflight=1)
    run_with_items(r, 5)
    head = r.ring[0]
    t, sends = 0.0, []
    r._pump(t)
    for _ in range(300):                                  # ~ 25 minutes of silence
        before = head.sends
        t += 5.0
        r._pump(t)
        if head.sends != before:
            sends.append(t)
    gaps = [b - a for a, b in zip(sends, sends[1:])]
    assert gaps[:3] == [20, 40, 80] and len(gaps) > 10 and set(gaps[3:]) <= {80}      # 10/20/40/80, then capped at 80
    assert head.tries == 0 and head.silent > 3
    assert r.stats["resend_abandoned"] == 0 and r.stats["tombstones"] == 0 and r.stats["abandoned_items"] == 0
    assert len(r.ring) == 5
    plant.stop()


def test_a_try_counts_only_after_some_valid_ack_of_any_run_since_the_send():
    r, plant = make_relay(max_inflight=1)
    run_with_items(r, 5)
    head, _ = _samples_only(r)
    r._pump(0.0)
    r._pump(10.0)                                         # timeout without any ack: silent
    assert head.silent == 1 and head.tries == 0
    alive(r)                                              # BATCH_OK of an unrelated run is a valid ack
    r._pump(30.0)                                         # sent at 10, ack came after: counted
    assert head.tries == 1 and head.silent == 1
    r._on_run_ack(b'{"run_id":"x","acked_batch_seq":0,"state":"START_OK","acked_batch_seq":1')   # malformed
    r._pump(60.0)                                         # (invalid ack does not bump the generation)
    assert head.tries == 1 and head.silent == 2
    plant.stop()


def test_six_counted_tries_make_a_tombstone_and_it_is_never_released_by_timeout():
    r, plant = make_relay(max_inflight=1)
    d = run_with_items(r, 5)
    head, _ = _samples_only(r)
    seq, t = head.seq, 0.0
    r._pump(t)
    for _ in range(RUNLOG_MAX_RESENDS):
        alive(r)
        t += 80
        r._pump(t)
    assert head.tomb and head.seq == seq and r.stats["resend_abandoned"] == 1
    assert json.loads(head.payload)["dropped"] is True and d.dropped == r.dropped_samples > 0
    for _ in range(30):
        alive(r)
        t += 80
        r._pump(t)
    assert head in r.ring and r.stats["resend_abandoned"] == 1 and r.stats["abandoned_items"] == 0
    plant.stop()


def test_events_head_also_becomes_a_tombstone_but_costs_no_samples():
    r, plant = make_relay(max_inflight=1)
    d = run_with_items(r, 4)
    r.ring = [i for i in r.ring if i.kind == "events"]
    head, t = r.ring[0], 0.0
    r._pump(t)
    for _ in range(RUNLOG_MAX_RESENDS):
        alive(r)
        t += 80
        r._pump(t)
    assert head.tomb and head.kind == "events" and r.dropped_samples == 0 and d.dropped == 0
    assert json.loads(head.payload)["samples"] == 0
    plant.stop()


# ---- 1 : reserves ---------------------------------------------------------------------------

def test_terminal_event_goes_out_on_run_events_with_a_deeper_reserve():
    r, plant = make_relay()
    feed_weight(r)
    r._adhoc("CH1", 5000)
    d = r.disp["CH1"]
    r._finish("CH1", "COMPLETE", "JOB_COMPLETE", None)
    term = [i for i in r.ring if i.terminal]
    assert len(term) == 1 and term[0].kind == "events" and term[0].topic.endswith("/run/events")
    assert json.loads(term[0].payload)["events"][0]["event"] == "JOB_COMPLETE"
    assert [i.kind for i in r.ring][-1] == "complete" and not r.backlog
    plant.stop()


# ---- 4 : fairness ----------------------------------------------------------------------------

def _two_runs(r):
    feed_weight(r)
    for i in range(2):
        r.cache.feed(frame(3 + i, 1090 + 45 * i, ch="CH2"))
    r._adhoc("CH1", 5000)
    r._adhoc("CH2", 5000)
    return r.disp["CH1"], r.disp["CH2"]


def test_fair_selection_the_run_with_fewest_in_flight_goes_next_global_window_2():
    r, plant = make_relay()
    a, b = _two_runs(r)
    for _ in range(6):                                    # run A is long, run B is short
        a.pending.append({"idx": a.samples + 1, "elapsed_ms": 0})
        a.samples += 1
        r._flush_samples(a)
    r._pump(0.0)
    sent = _sent(r)
    assert len(sent) == 2 and {i.run_id for i in sent} == {a.run_id, b.run_id}      # one each, not A, A
    assert all(i.kind == "start" for i in sent)
    # an ack frees A's slot: A has 0 in flight, B has 1 -> A goes next, oldest first
    ack(r, a, "START_OK", 0)
    r._pump(1.0)
    nxt = [i for i in _sent(r) if i.run_id == a.run_id]
    assert len(_sent(r)) == 2 and nxt and nxt[0].kind == "events" and nxt[0].seq == 1
    plant.stop()


def test_a_stuck_run_cannot_starve_another_run():
    r, plant = make_relay()
    a, b = _two_runs(r)
    for _ in range(8):                                    # A grows and is never acked
        a.pending.append({"idx": a.samples + 1, "elapsed_ms": 0})
        a.samples += 1
        r._flush_samples(a)
    t = 0.0
    r._pump(t)
    for _ in range(6):                                    # B is acked every round
        ack(r, b, "BATCH_OK", 99)                         # releases only what B already published
        t += 1
        r._pump(t)
    assert not [i for i in r.ring if i.run_id == b.run_id]        # B fully delivered despite A hogging
    assert [i for i in r.ring if i.run_id == a.run_id] and len(_sent(r)) <= 2
    plant.stop()


# ---- 5 : UNKNOWN_RUN owed replies -------------------------------------------------------------

def test_unknown_run_storm_counts_once_per_cycle_and_the_owed_replies_are_absorbed():
    r, plant = make_relay()
    d = run_with_items(r, 6)
    r._pump(0.0)                                          # 2 in flight -> 1 reply owed after the first
    for _ in range(200):
        ack(r, d, "UNKNOWN_RUN")
    assert sorted({i.unknown for i in r.ring}) == [0, 1] and r.outcome(d.run_id) is None
    plant.stop()


def test_resend_timeout_clears_the_owed_replies_so_the_next_cycle_counts():
    r, plant = make_relay()
    d = run_with_items(r, 6)
    r._pump(0.0)
    ack(r, d, "UNKNOWN_RUN")                              # cycle 1 counted, 1 reply owed
    assert r.unk_owed[d.run_id] == 1
    r._pump(1.0)                                          # re-sent after the reply
    r._pump(1000.0)                                       # everything timed out: the owed reply is never coming
    assert d.run_id not in r.unk_owed
    r._pump(1001.0)
    ack(r, d, "UNKNOWN_RUN")
    assert max(i.unknown for i in r.ring) == 2
    plant.stop()


def test_run_drop_clears_the_owed_replies():
    r, plant = make_relay()
    d = run_with_items(r, 6)
    r._pump(0.0)
    ack(r, d, "UNKNOWN_RUN")
    assert r.unk_owed.get(d.run_id) == 1
    ack(r, d, "COMPLETE", 0)
    assert r.ring == [] and d.run_id not in r.unk_owed
    plant.stop()


def test_unknown_run_abandons_after_three_cycles_with_two_replies_each():
    r, plant = make_relay()
    d = run_with_items(r, 4)
    for n in range(3):
        r._pump(float(n))
        unknown_cycle(r, d)
        assert r.ring, n
    r._pump(9.0)
    unknown_cycle(r, d)
    assert r.ring == [] and r.outcome(d.run_id) == "ABANDONED"
    plant.stop()


# ---- 6 : superseded run -----------------------------------------------------------------------

def test_new_job_closes_the_unclosed_old_run_as_failed_run_superseded():
    r, plant = make_relay(ring_slots=8)                   # tiny ring: the old run's complete stays in the backlog
    feed_weight(r)
    r._adhoc("CH1", 5000)
    old = r.disp["CH1"]
    r._finish("CH1", "COMPLETE", "JOB_COMPLETE", None)
    assert any(i.kind == "complete" for _d, i in r.backlog)
    r._adhoc("CH1", 5000)                                 # next job takes the slot
    comp = [i for i in r.ring if i.kind == "complete" and i.run_id == old.run_id]
    assert len(comp) == 1 and r.stats["superseded_runs"] == 1 and r.stats["superseded_unclosed"] == 0
    body = json.loads(comp[0].payload)
    assert body["status"] == "FAILED" and body["error"] == "RUN_SUPERSEDED"
    added = {i.seq for i in r.ring if i.run_id == old.run_id and i.kind != "complete"}
    assert body["end_seq"] == max(added) and comp[0].seq == body["end_seq"] + 1      # contiguous: no phantom gap
    assert not [1 for d, i in r.backlog if d is old]
    plant.stop()


def test_superseded_complete_refused_when_the_ring_is_full_is_counted_unclosed():
    r, plant = make_relay(ring_slots=3)
    feed_weight(r)
    r._adhoc("CH1", 5000)
    old = r.disp["CH1"]
    r._finish("CH1", "COMPLETE", "JOB_COMPLETE", None)
    for k in range(3 - len(r.ring)):                      # fill the ring completely
        assert r._add(Item("filler", 100 + k, "complete", "t", "{}"))
    r._adhoc("CH1", 5000)
    assert r.stats["superseded_runs"] == 1 and r.stats["superseded_unclosed"] == 1
    assert r.runs[old.run_id]["local_status"] == "UNCLOSED"
    plant.stop()


# ---- 7 : body accounting -----------------------------------------------------------------------

def test_samples_and_events_bodies_are_allocated_at_least_256_bytes_and_a_tombstone_always_fits():
    r, plant = make_relay(sample_bytes=500)
    d = run_with_items(r, 10)
    small = Item_for(r, d, "events", 90)
    assert small.size < TOMB_BODY_MAX <= small.alloc          # allocation floor, length accounted apart
    big = [i for i in r.ring if i.kind == "samples" and not i.tomb]
    r._drop_oldest_sample()
    tomb = next(i for i in r.ring if i.tomb)
    assert tomb.size < TOMB_BODY_MAX and tomb.alloc == tomb.size + 1      # shrunk for real
    assert r._total_bytes() == sum(i.size for i in r.ring)                 # byte caps use the real length
    assert big
    plant.stop()
