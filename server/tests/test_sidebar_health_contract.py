"""Sidebar navigation, icon sprite, and System Health state wording."""

import re
from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
NAV = APP_DIR / "templates" / "nav.html"
COMMON_JS = APP_DIR / "static" / "js" / "common.js"
CSS = APP_DIR / "static" / "css" / "style.css"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def test_sprite_defines_the_icons_the_nav_and_health_use():
    nav = _read(NAV)
    assert "<svg hidden" in nav or 'aria-hidden="true"' in nav
    for name in ("i-queue", "i-tuning", "i-graphs", "i-history",
                 "i-server", "i-database", "i-relay", "i-sender", "i-scale"):
        assert f'id="{name}"' in nav, name


def test_nav_keeps_brand_and_all_four_pages():
    nav = _read(NAV)
    assert "BITS" in nav
    assert "Dispensing" in nav
    for href in ('href="/queue"', 'href="/tuning"', 'href="/graphs"', 'href="/history"'):
        assert href in nav, href
    assert 'aria-current="page"' in nav


def test_system_health_rows_carry_names_and_state_and_age_slots():
    nav = _read(NAV)
    for name in ("server", "database", "relay", "sender", "scale1", "scale2"):
        assert f'data-health="{name}"' in nav, name
    assert "health-state" in nav
    assert "health-age" in nav


def test_health_renderer_emits_state_word_not_only_a_dot():
    js = _read(COMMON_JS)
    setter = js.split("function setHealth", 1)[1].split("function ", 1)[0]
    assert ".health-state" in setter
    assert ".health-age" in setter
    assert "textContent" in setter
    body = js.split("function startHealthIndicator", 1)[1].split("function ", 1)[0]
    assert "freshnessState(" in body
    assert "setHealth(" in body


def test_scale_rows_never_claim_online_when_the_relay_is_stale():
    """Stale relay data must not surface as healthy scale readings."""
    js = _read(COMMON_JS)
    body = js.split("function startHealthIndicator", 1)[1].split("function ", 1)[0]
    assert "relayFresh" in body
    assert "if (!relayFresh || !ch)" in body


def test_sidebar_collapses_into_a_drawer_on_small_screens():
    css = _read(CSS)
    assert "nav-drawer" in css or "site-header.open" in css
    assert "hamburger" in css or "menu-toggle" in css


def test_menu_toggle_base_rule_precedes_its_media_override():
    """display:none must sit above the 760px override, or it wins at every width."""
    css = _read(CSS)
    base_match = re.search(r"\.menu-toggle\s*\{\s*display:\s*none", css)
    assert base_match, "base .menu-toggle{display:none} rule missing"
    media_start = css.find("@media (max-width: 760px)")
    assert media_start != -1, "760px media block missing"
    assert base_match.start() < media_start, (
        "base .menu-toggle{display:none} must precede the 760px override"
    )
