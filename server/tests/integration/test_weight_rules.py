"""Weight path end to end: only fresh, ordered frames of the right channel and boot reach the
dispense logic; everything else is dropped and never refreshes the freshness timestamp."""
import time

from sim.mqttx import wait_for


def test_out_of_order_duplicate_and_foreign_boot_frames_are_rejected(stack):
    stack.plant.set_weight("CH1", 500)
    assert wait_for(lambda: (stack.relay.cache.get("CH1") or (0,))[0] == 500, 5)
    stack.sender.paused.set()                      # we are the only voice on weight/ctl now
    time.sleep(0.3)
    good = stack.sender.frame(weight_g=500)
    assert stack.sender.inject(good)
    assert wait_for(lambda: stack.relay.cache._rec.get("CH1", {}).get("seq") == good["seq"], 3)
    base = dict(stack.relay.cache.counts)
    stamp = stack.relay.cache.last_valid_age_s("CH1")
    poison = 99999
    seen = []
    t0 = time.monotonic()
    for i in range(6):                              # 1.2 s of hostile traffic
        stack.sender.inject(good)                                                  # duplicate
        stack.sender.inject(dict(good, seq=good["seq"] - 3 - i, weight_g=poison))  # older seq
        stack.sender.inject(dict(good, seq=good["seq"] + 1000 * (i + 1), boot_id="deadbeef",   # never consecutive
                                 uptime_ms=1, weight_g=poison))                    # other boot
        stack.sender.inject(dict(good, seq=good["seq"] + 50, uptime_ms=good["uptime_ms"] - 400,
                                 weight_g=poison))                                 # uptime regression
        stack.sender.inject(dict(good, seq=good["seq"] + 60, weight_g=-5))         # negative: fail safe
        time.sleep(0.2)
        w = stack.relay.cache.get("CH1")
        seen.append(w[0] if w else None)
    elapsed = time.monotonic() - t0
    counts = stack.relay.cache.counts
    assert poison not in seen and set(seen) <= {500, None}
    assert counts.get("dup_or_old", 0) - base.get("dup_or_old", 0) >= 6
    assert counts.get("held_new_boot", 0) - base.get("held_new_boot", 0) >= 1
    assert counts.get("weight", 0) - base.get("weight", 0) >= 1
    assert counts.get("uptime_regression", 0) - base.get("uptime_regression", 0) >= 1
    # none of the rejected frames refreshed the freshness stamp: it aged by the full elapsed time
    assert stack.relay.cache.last_valid_age_s("CH1") >= stamp + elapsed - 0.05
    stack.sender.paused.clear()


def test_wrong_scale_channel_is_never_used_for_a_job(stack):
    """CH1 frames only; a CH2 job (M2) must never energise Relay 2 nor use Scale 1."""
    stack.plant.set_weight("CH1", 3000)
    stack.plant.set_weight("CH2", 0)
    assert stack.relay.cache.get("CH2") is None
    cid = stack.create_job(5000, material="M2")
    assert stack.wait_cmd(cid, ("QUEUED",), 10)
    t0 = stack.plant.relay_on_time["CH2"]
    time.sleep(4.0)
    assert stack.relay.executed == [] and len(stack.relay.queue) == 1
    assert stack.plant.relay_on_time["CH2"] == t0 and not stack.plant.relay("CH2")
    assert stack.relay.cache.get("CH2") is None
    assert stack.runs() == []
    # the scale now reports the right channel: the SAME job starts (queued job kept, one run)
    stack.sender.channel = "CH2"
    assert wait_for(lambda: stack.relay.executed == [cid], 10)
    run = stack.wait_run(("COMPLETE",), 40)
    assert run and run.channel_id == "CH2" and run.material_id == "M2"
    assert run.start_weight_g == 0                  # CH2 scale, not the 3000 g on CH1
    assert stack.plant.weight("CH1") == 3000        # CH1 untouched
