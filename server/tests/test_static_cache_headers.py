"""Static assets must never be heuristically cached.

The History page's Delete button lives in ``history.js``. Before this, static
files were served with only ``ETag``/``Last-Modified`` and no ``Cache-Control``,
so a browser was free to keep serving a previously cached copy indefinitely —
after a deploy the operator still saw the old page with no Delete control and
had no way to tell why.

``Cache-Control: no-cache`` forces revalidation on every use. Combined with the
``ETag`` StaticFiles already sends, an unchanged file is a cheap 304 and a
changed file is always fetched. That is the correct policy for an operational
UI whose scripts are edited in place.
"""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"


def test_static_javascript_is_served_with_a_revalidation_policy(client):
    """history.js must not be heuristically cached by the browser."""
    response = client.get("/static/js/history.js")

    assert response.status_code == 200
    cache_control = response.headers.get("cache-control", "")
    assert cache_control, "static assets must send Cache-Control"
    # "no-cache" means store-but-revalidate; without it the browser may reuse a
    # stale copy forever, which is exactly the bug this pins down.
    assert "no-cache" in cache_control or "no-store" in cache_control


def test_static_stylesheet_is_served_with_a_revalidation_policy(client):
    response = client.get("/static/css/style.css")

    assert response.status_code == 200
    cache_control = response.headers.get("cache-control", "")
    assert "no-cache" in cache_control or "no-store" in cache_control


def test_html_pages_are_not_given_an_aggressive_cache_policy(client):
    """Pages must not outlive their scripts; a stale page would still be wrong."""
    response = client.get("/history")

    assert response.status_code == 200
    cache_control = response.headers.get("cache-control", "")
    assert "no-cache" in cache_control or "no-store" in cache_control or cache_control == ""


def test_every_template_asset_reference_carries_a_version_query():
    """Belt and braces: a versioned URL cannot collide with a cached one at all.

    Even if a proxy or a misbehaving client ignores ``Cache-Control``, a
    ``?v=`` query changes the URL whenever the asset set changes, so the old
    cached body is never requested under the new name.
    """
    templates_dir = APP_DIR / "templates"
    offenders = []
    for path in sorted(templates_dir.glob("*.html")):
        text = path.read_text(encoding="utf-8")
        for marker in ("/static/js/", "/static/css/"):
            for i, line in enumerate(text.splitlines(), start=1):
                if marker in line and "?v=" not in line and "{% static" not in line:
                    offenders.append(f"{path.name}:{i}: {line.strip()}")
    assert offenders == [], "asset references without ?v= cache-buster:\n" + "\n".join(offenders)
