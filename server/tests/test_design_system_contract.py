"""Contracts for the shared design system: tokens, badges, freshness, weight."""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
CSS = APP_DIR / "static" / "css" / "style.css"
COMMON_JS = APP_DIR / "static" / "js" / "common.js"


def _css() -> str:
    return CSS.read_text(encoding="utf-8")


def _js() -> str:
    return COMMON_JS.read_text(encoding="utf-8")


def test_tokens_define_one_semantic_colour_per_meaning():
    css = _css()
    for token in ("--ok-fg:", "--ok-bg:", "--ok-bd:",
                  "--warn-fg:", "--warn-bg:", "--warn-bd:",
                  "--bad-fg:", "--bad-bg:", "--bad-bd:",
                  "--info-fg:", "--info-bg:", "--info-bd:",
                  "--neutral-fg:", "--neutral-bg:", "--neutral-bd:",
                  "--sp-2-5:"):
        assert token in css, token


def test_only_one_button_primitive_remains():
    """`.btn.small` was used but never defined; size variants must be explicit."""
    css = _css()
    assert css.count("\n.btn {") == 1, "primary .btn must be defined once"
    assert ".btn.sm" in css
    assert ".btn.danger" in css
    assert ".btn.secondary" in css


def test_only_one_badge_primitive_remains():
    css = _css()
    assert ".badge" in css
    assert ".badge.ok" in css and ".badge.warn" in css and ".badge.bad" in css


def test_tone_map_covers_every_canonical_state():
    js = _js()
    body = js.split("STATE_TONES = {", 1)[1].split("};", 1)[0]
    for state in ("OFFLINE", "FAULT", "EMERGENCY STOP", "FAILED", "DISPENSING",
                  "RUNNING", "WAITING", "IDLE", "READY", "COMPLETE", "QUEUED",
                  "PENDING DEVICE", "HISTORICAL", "DEACTIVATED", "STALE"):
        assert ("'" + state + "'") in body, state


def test_freshness_uses_the_existing_ten_second_rule():
    js = _js()
    body = js.split("function freshnessState", 1)[1].split("function ", 1)[0]
    assert "10" in body and "30" in body
    assert "'stale'" in body and "'offline'" in body


def test_weight_display_distinguishes_zero_from_unavailable():
    js = _js()
    body = js.split("function fmtWeightDisplay", 1)[1].split("function ", 1)[0]
    assert "—.— kg" in body
    assert "toFixed(3)" in body
    # valid zero must not be treated as missing
    assert "weightValid" in body
    # non-finite weightG is unavailable even when weightValid is set
    assert "!Number.isFinite" in body


def test_icon_helper_emits_an_svg_use_reference():
    js = _js()
    body = js.split("function icon", 1)[1].split("function ", 1)[0]
    assert "<svg" in body
    assert ('href="#' in body) or ("xlink:href" in body)


def test_progress_fill_keeps_height_and_display():
    css = _css()
    body = css.split(".progress span", 1)[1].split("}", 1)[0]
    assert "display: block" in body or "display:block" in body
    assert "height: 100%" in body or "height:100%" in body


def test_svg_hidden_sprite_is_actually_hidden():
    """`<svg hidden>` in nav.html is a 300x150 spacer unless author CSS hides it.

    SVG elements do not implement the HTML `hidden` IDL property, so the UA
    stylesheet's `[hidden] { display: none }` rule does not apply to them.
    Without an explicit author rule the icon sprite renders as blank space at
    the top of every page that includes nav.html.
    """
    css = _css()
    assert "[hidden]" in css
    # the rule must set display:none (with or without spaces)
    import re
    assert re.search(r"\[hidden\]\s*\{[^}]*display:\s*none", css), (
        "author CSS must hide [hidden] elements — SVG sprites ignore the "
        "hidden IDL property"
    )


def test_page_top_padding_is_compact():
    """.page base padding-top must stay in the 24-32px range.

    Larger values push every page header down and recreate the blank-gap bug.
    """
    css = _css()
    body = css.split(".page {", 1)[1].split("}", 1)[0]
    import re
    m = re.search(r"padding:\s*(\d+)px", body)
    assert m, ".page must declare a padding shorthand with a numeric top value"
    top = int(m.group(1))
    assert 24 <= top <= 32, f".page padding-top is {top}px; expected 24-32px"


def test_mobile_page_uses_padding_not_margin_for_hamburger_clearance():
    """Hamburger clearance must replace base padding-top, not stack on top of it.

    The old pattern (`margin-top: 60px` + base `padding-top: 32px`) produced
    92px of blank space at 601-760px viewports. The fix puts the clearance in
    `padding-top` so it overrides the base value.
    """
    css = _css()
    mobile = css.split("@media (max-width: 760px)", 1)[1].split("@media", 1)[0]
    page_rule = mobile.split(".page {", 1)[1].split("}", 1)[0]
    # must use padding-top for clearance
    assert "padding-top" in page_rule, (
        "mobile .page must set padding-top for hamburger clearance"
    )
    # must NOT use margin-top (which stacks with base padding)
    assert "margin-top" not in page_rule, (
        "mobile .page must not use margin-top — it stacks with base padding-top"
    )
    # margin must be reset to 0 so the sidebar offset is dropped
    assert "margin: 0" in page_rule or "margin:0" in page_rule


def test_narrow_page_does_not_override_top_padding():
    """@media (max-width: 600px) must not clobber the mobile padding-top.

    The old `padding: var(--sp-3) ...` shorthand reset padding-top to 16px.
    Side/bottom longhands leave the hamburger clearance intact.
    """
    css = _css()
    narrow = css.split("@media (max-width: 600px)", 1)[1].split("@media", 1)[0]
    page_rule = narrow.split(".page {", 1)[1].split("}", 1)[0]
    # shorthand padding would clobber padding-top from the 760px breakpoint
    assert "padding:" not in page_rule.replace("padding-", ""), (
        "narrow .page must not use the padding shorthand — it overrides "
        "padding-top from the 760px breakpoint"
    )
