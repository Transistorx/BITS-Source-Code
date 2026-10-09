"""Active Dispensers: dominant weight, unavailable vs zero, status badges, faults."""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
QUEUE_JS = APP_DIR / "static" / "js" / "queue.js"
QUEUE_HTML = APP_DIR / "templates" / "queue.html"
CSS = APP_DIR / "static" / "css" / "style.css"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _channel_card_body() -> str:
    # Bound the capture by the next top-level function (metric), not by the
    # first nested `function ` token — channelCard now contains a .find()
    # callback that the naive split would truncate at.
    return _read(QUEUE_JS).split("function channelCard", 1)[1].split("function metric", 1)[0]


def test_card_renders_the_full_metric_set():
    body = _channel_card_body()
    for field in ("Target", "Remaining", "PID output", "Relay", "Progress",
                  "Job", "Elapsed"):
        assert field in body, field
    # Profile and Kp/Ki/Kd live in the secondary section (lower priority).
    assert "channel-secondary" in body, "secondary technical details section must exist"
    assert "Profile" in body
    assert "Kp / Ki / Kd" in body


def test_unavailable_weight_is_never_rendered_as_zero():
    body = _channel_card_body()
    assert "fmtWeightDisplay" in body
    assert "T.fmtWeightDisplay(channel.weight_g, weightValid" in body
    assert "—.—" in _read(APP_DIR / "static" / "js" / "common.js")


def test_unavailable_weight_does_not_claim_a_stale_prior_reading():
    """Review finding: a finite age on an unavailable reading fabricates
    'Stale — last reading Xs ago'. Only pass an age when the weight is available."""
    js = _read(QUEUE_JS)
    # NB: split on "function refresh(" not "function refresh" — the latter
    # matches refreshProfileOptions first and silently returns the wrong body.
    body = js.split("function refresh(", 1)[1]
    assert "ch.weight_age_s" in body
    assert "available" in body          # the availability gate
    assert "weight_valid" in body       # gate reads the real validity flag
    assert "weight_g !== null" in body  # and the presence of a reading


def test_null_or_undefined_weight_and_target_coerce_to_nan_not_zero():
    """Ruling A: Number(null) === 0 must not fabricate a 0% bar or Remaining=target."""
    js = _read(QUEUE_JS)
    body = _channel_card_body()
    assert "(channel.target_g === null || channel.target_g === undefined) ? NaN : Number(channel.target_g)" in body
    assert "(channel.weight_g === null || channel.weight_g === undefined) ? NaN : Number(channel.weight_g)" in body
    # error_g is the same class of trap: Number(null) === 0 would fabricate Remaining=0
    assert "(channel.error_g === null || channel.error_g === undefined) ? NaN : Number(channel.error_g)" in body
    # Third site of the same trap in channelCard: Number(null) === 0 would
    # render a missing PID output as a real 0.0% instead of '—'.
    assert "(channel.output === null || channel.output === undefined) ? NaN : Number(channel.output)" in body
    # Fourth site, in localQueuedAt: Number(null) === 0 on queued_ms would
    # fabricate a Created timestamp from uptime alone.
    assert "(job.queued_ms === null || job.queued_ms === undefined) ? NaN : Number(job.queued_ms)" in js


def test_null_weight_age_ms_renders_no_data_not_zero_seconds():
    """Number(null) === 0 at the health row would claim '0s' for a missing age."""
    js = _read(APP_DIR / "static" / "js" / "common.js")
    assert "ch.weight_age_ms !== null && ch.weight_age_ms !== undefined" in js
    assert "display_age_ms" not in js


def test_progress_bar_is_guarded_by_null_progress():
    """Ruling B: targetOk && non-finite weight yields progress === null; toFixed on it would throw."""
    body = _channel_card_body()
    assert "(progress !== null" in body
    assert "progress === null" in body


def test_weight_valid_zero_still_shows_three_decimals():
    js = _read(APP_DIR / "static" / "js" / "common.js")
    body = js.split("function fmtWeightDisplay", 1)[1].split("function ", 1)[0]
    assert "toFixed(3)" in body
    assert "weightValid" in body


def test_status_badge_uses_the_shared_helper_and_state_word():
    js = _read(QUEUE_JS)
    body = _channel_card_body()
    assert "T.badge(" in body
    assert "channelStateWord" in body
    assert "CHANNEL_STATE_WORDS" in js
    for word in ("WAITING FOR SCALE", "WAITING FOR PROFILE", "EMERGENCY STOP"):
        assert word in js, word   # the channel map in queue.js, not just STATE_TONES


def test_faults_render_as_structured_alerts_not_a_red_card():
    body = _channel_card_body()
    assert 'class="alert"' in body
    assert "role=\"alert\"" in body or "role='alert'" in body
    assert "is-fault" in body  # border tone only


def test_progress_is_hidden_when_target_is_invalid():
    body = _channel_card_body()
    assert "progress !== null" in body
    assert "progress === null" in body


def test_missing_stable_is_not_rendered_as_stable_reading():
    js = _read(QUEUE_JS)
    body = _channel_card_body()
    assert "channel.stable !== true" in body
    assert "Reading active" in body


def test_kg_does_not_render_a_missing_weight_as_zero():
    js = _read(QUEUE_JS)
    body = js.split("function kg", 1)[1].split("function ", 1)[0]
    assert "(grams === null || grams === undefined) ? NaN : Number(grams)" in body


def test_channel_card_is_substantially_larger():
    css = _read(CSS)
    assert ".channel-card" in css
    # hero weight is dominant
    assert "channel-weight" in css
    assert "48px" in css or "44px" in css or "2.75rem" in css
    # more internal padding than a compact card
    assert "channel-card {" in css
    assert "padding: 18px 20px" in css or "padding: 20px" in css or "padding: 22px" in css


def test_queue_page_keeps_two_channel_cards_side_by_side():
    css = _read(CSS)
    assert ".channel-grid { display: grid; grid-template-columns: repeat(2" in css
    assert 'id="channel-cards"' in _read(QUEUE_HTML)


def test_no_stale_channel_weight_strong_rule_beside_the_hero_value():
    """Ruling D: the old `> strong { font-size: 30px }` must not shadow `.channel-weight-value`."""
    css = _read(CSS)
    assert ".channel-weight-value" in css
    assert ".channel-weight > strong" not in css


def test_pump_control_section_present_with_confirmed_states():
    """Each Active Dispenser card must show a PUMP CONTROL area with
    controller-confirmed state and START/STOP buttons."""
    body = _channel_card_body()
    assert "pump-control" in body, "PUMP CONTROL section must exist"
    assert "PUMP CONTROL" in body
    assert "pumpState" in body, "state must come from pumpState helper"
    for state in ("RELAY ON (commanded)", "RELAY OFF (commanded)", "COMMAND PENDING",
                  "CONTROLLER OFFLINE", "COMMAND FAILED",
                  "FAULT", "E-STOP ACTIVE"):
        assert state in _read(QUEUE_JS), state


def test_pump_buttons_use_pump_start_pump_stop_commands():
    body = _channel_card_body()
    assert 'data-control="PUMP_START"' in body
    assert 'data-control="PUMP_STOP"' in body


def test_pump_state_is_never_optimistic():
    """The UI must not flip relay state on click — it must wait for the
    controller to confirm via device telemetry."""
    js = _read(QUEUE_JS)
    assert "pendingPump" in js
    assert "COMMAND PENDING" in js
    # The pumpState helper must read channel.relay_on from device telemetry.
    pump_body = js.split("function pumpState", 1)[1].split("function ", 1)[0]
    assert "channel.relay_on" in pump_body
    assert "pendingPump" in pump_body


def test_pump_control_is_in_upper_portion_of_card():
    """START/STOP must appear near the top of the card, after Current Weight
    and BEFORE the metric grid, so operators don't scroll to reach them."""
    body = _channel_card_body()
    weight_idx = body.index("channel-weight")
    pump_idx = body.index("pump-control")
    metrics_idx = body.index("channel-metrics")
    assert weight_idx < pump_idx < metrics_idx, (
        "PUMP CONTROL must sit between Current Weight and the metric grid")


def test_exactly_one_start_and_one_stop_per_pump():
    """No duplicate START/STOP controls anywhere in the card."""
    body = _channel_card_body()
    assert body.count('data-control="PUMP_START"') == 1
    assert body.count('data-control="PUMP_STOP"') == 1


def test_pump1_controls_relay1_only_pump2_controls_relay2_only():
    """Each card's START/STOP buttons carry the correct channel id so
    Pump 1 → Relay 1 and Pump 2 → Relay 2 exclusively."""
    body = _channel_card_body()
    # data-channel is bound to the id variable which is CH1 or CH2.
    assert 'data-channel="' in body
    # The id comes from channel.channel_id — verify the mapping is derived
    # from the channel data, not hardcoded to a single pump.
    assert "channel.channel_id" in body
    assert "var number = id === 'CH1' ? 1 : 2" in body


def test_start_disabled_on_unsafe_states():
    """START must be disabled when controller is offline, job active,
    fault, E-Stop, or other pump running."""
    body = _channel_card_body()
    assert "startDisabled" in body
    assert "!online" in body
    assert "hasJob" in body
    assert "fault" in body.lower()
    assert "otherRunning" in body or "other" in body.lower()


def test_stop_always_available_when_online():
    """STOP must remain immediately accessible whenever the controller
    is reachable, even if the UI believes the pump is already off."""
    body = _channel_card_body()
    assert "stopDisabled" in body
    # STOP is never disabled, including when offline (it queues; see
    # test_stop_button_contract.py for the offline note).
    assert "stopDisabled = isInflight(id, 'PUMP_STOP')" in body
    assert "!online" not in body.split("stopDisabled =")[1].split(";")[0]


def test_weight_renders_unavailable_as_dashes_not_zero():
    """Unavailable weight shows —.— kg, valid zero shows 0.000 kg."""
    body = _channel_card_body()
    assert "fmtWeightDisplay" in body
    assert "is-unavailable" in body
    # The helper is in common.js — verify it's correct there.
    js = _read(APP_DIR / "static" / "js" / "common.js")
    fmt = js.split("function fmtWeightDisplay", 1)[1].split("function ", 1)[0]
    assert "—.— kg" in fmt
    assert "toFixed(3)" in fmt


def test_pump_state_safety_overrides():
    """E-STOP and FAULT states take priority over relay state in pumpState."""
    js = _read(QUEUE_JS)
    pump_body = js.split("function pumpState", 1)[1].split("function ", 1)[0]
    # E-Stop check must come before relay_on check.
    estop_idx = pump_body.index("E-STOP ACTIVE")
    fault_idx = pump_body.index("FAULT")
    relay_idx = pump_body.index("channel.relay_on")
    assert estop_idx < relay_idx, "E-STOP must override relay state"
    assert fault_idx < relay_idx, "FAULT must override relay state"
