"""Pure PID-profile rules shared by create, activate, job pin and the migration.

No database, no transport. Mirrors the firmware's profile consistency check
(``dual_dispense_profile_consistency_error`` in dual_dispense_controller.c) and
adds ONE server rule the firmware does not enforce: the STALL RULE.
The known livelock (dual_dispense_controller.c) stalls to CHANNEL_TIMEOUT whenever
``inflight_comp_g < |error| <= tolerance_g``. That window is empty exactly when
``inflight_comp_g >= tolerance_g``, so that is what is required here. (The design
text wrote the inequality the other way round; the code says this one.)

Ranged profiles are checked at the WORST CASE target (``target_min_g``) because
tolerance and overshoot are absolute grams and are never scaled by the target.
"""

import struct
from typing import Mapping

# --- firmware constants, mirrored (keep equal; CONTRACT 9.3) -----------------
FW_ABS_MAX_OVERSHOOT_G = 5000        # DCH_ABS_MAX_OVERSHOOT_G
COARSE_CEIL, FINE_CEIL, MICRO_CEIL = 1.0, 0.5, 0.15   # DCH_*_DUTY_CEIL

STAGED_FIELDS = ("coarse_threshold_g", "fine_threshold_g", "micro_threshold_g",
                 "coarse_min_on_ms", "fine_min_on_ms", "micro_min_on_ms",
                 "settle_time_ms", "inflight_comp_g")

# What the firmware itself uses when a pinned profile{} omits a staged field
# (CONFIG_WEIGHT_DEMO_* Kconfig defaults). A schema-2 row stores explicit values;
# a missing field other than inflight_comp_g is filled with these at create time.
FW_STAGED_DEFAULTS = {
    "coarse_threshold_g": 500, "fine_threshold_g": 100, "micro_threshold_g": 30,
    "coarse_min_on_ms": 80, "fine_min_on_ms": 30, "micro_min_on_ms": 15,
    "settle_time_ms": 1500, "inflight_comp_g": 15,
}

STATUSES = ("draft", "validated", "active", "deprecated")
APPLICABILITIES = ("exact", "range", "general")
# draft -> validated (admin + validation_ref) -> active (operator confirm) -> deprecated.
# deactivate returns active -> validated. A schema-1 (legacy) row may also go
# draft -> active directly (it was always activatable without validation).
TRANSITIONS = {
    "draft": ("validated", "deprecated"),
    "validated": ("active", "deprecated"),
    "active": ("validated", "deprecated"),
    "deprecated": (),
}


def _f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", x))[0]


def stage_ceiling_on_ms(ceiling: float, window_ms: int) -> int:
    """Exactly as the firmware rounds it, in float32."""
    on = int(_f32(_f32(_f32(ceiling) * _f32(float(window_ms))) + _f32(0.5)))
    return min(on, window_ms)


def row_range(row) -> tuple[int | None, int | None]:
    """Declared target range of a row; a row without one is [target_g, target_g]."""
    lo = getattr(row, "target_min_g", None)
    hi = getattr(row, "target_max_g", None)
    t = getattr(row, "target_g", None)
    return (lo if lo is not None else t, hi if hi is not None else t)


def contains(row, target_g: int) -> bool:
    lo, hi = row_range(row)
    return lo is not None and hi is not None and lo <= target_g <= hi


def consistency_error(p: Mapping | object, target_g: int) -> str | None:
    """First rule the profile breaks when run at ``target_g``, else None."""
    def g(name):
        return p.get(name) if isinstance(p, Mapping) else getattr(p, name, None)

    for name in STAGED_FIELDS + ("window_ms", "min_on_ms", "min_off_ms",
                                 "tolerance_g", "max_overshoot_g"):
        if g(name) is None:
            return f"PIN_INCOMPLETE {name} is missing"
    window, min_on, min_off = g("window_ms"), g("min_on_ms"), g("min_off_ms")
    if min_on > window or min_off > window:
        return "PIN_INCONSISTENT min_on_ms/min_off_ms > window_ms"
    co, fi, mi = g("coarse_threshold_g"), g("fine_threshold_g"), g("micro_threshold_g")
    if min(co, fi, mi) < 0 or max(co, fi, mi) > 100000:
        return "PIN_OUT_OF_RANGE thresholds (0..100000)"
    if not (mi <= fi <= co):
        return "PIN_INCONSISTENT thresholds need micro<=fine<=coarse"
    con, fon, mon = g("coarse_min_on_ms"), g("fine_min_on_ms"), g("micro_min_on_ms")
    if min(con, fon, mon) < 1 or max(con, fon, mon) > 10000:
        return "PIN_OUT_OF_RANGE stage min_on_ms (1..10000)"
    if not 1 <= g("settle_time_ms") <= 60000:
        return "PIN_OUT_OF_RANGE settle_time_ms (1..60000)"
    comp, tol = g("inflight_comp_g"), g("tolerance_g")
    if not 0 <= comp <= 10000:
        return "PIN_OUT_OF_RANGE inflight_comp_g (0..10000)"
    if stage_ceiling_on_ms(COARSE_CEIL, window) < con:
        return "PIN_STAGE_CANNOT_PULSE coarse (min_on > window)"
    if stage_ceiling_on_ms(FINE_CEIL, window) < fon:
        return "PIN_STAGE_CANNOT_PULSE fine (min_on > 0.5*window)"
    if stage_ceiling_on_ms(MICRO_CEIL, window) < mon:
        return "PIN_STAGE_CANNOT_PULSE micro (min_on > 0.15*window)"
    if stage_ceiling_on_ms(FINE_CEIL, window) + min_off > window:
        return "PIN_MIN_OFF_SATURATES fine (min_off > window - 0.5*window)"
    if stage_ceiling_on_ms(MICRO_CEIL, window) + min_off > window:
        return "PIN_MIN_OFF_SATURATES micro (min_off > window - 0.15*window)"
    if target_g > 0:
        if tol >= target_g:
            return "PIN_INCONSISTENT tolerance_g >= target_g"
        over = g("max_overshoot_g")
        if over > target_g or over > FW_ABS_MAX_OVERSHOOT_G:
            return "PIN_INCONSISTENT max_overshoot_g > min(target_g, cap)"
    if comp < tol:
        return ("PIN_STALL inflight_comp_g < tolerance_g (stall window "
                f"{comp} < |error| <= {tol} g; job would time out)")
    return None


def legacy_effective_staged() -> dict:
    """The staged values the firmware applies to a schema-1 (11-field) profile."""
    return dict(FW_STAGED_DEFAULTS)

