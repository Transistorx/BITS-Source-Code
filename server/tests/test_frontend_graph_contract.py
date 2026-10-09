"""Static and rendered contracts for the four-target graph workspace."""

from pathlib import Path


APP_DIR = Path(__file__).resolve().parents[1] / "app"


def test_graph_page_renders_material_controls_and_comparison_dialog(client):
    response = client.get("/graphs")

    assert response.status_code == 200
    html = response.text
    assert 'data-material="M1"' in html
    assert 'data-material="M2"' in html
    assert 'id="target-grid"' in html
    assert 'id="compare-dialog"' in html
    assert 'href="/history"' in html


def test_graph_script_uses_four_integer_targets_and_scoped_latest_ten_queries():
    script = (APP_DIR / "static" / "js" / "dashboard.js").read_text(encoding="utf-8")

    assert "const TARGETS = [5000, 10000, 15000, 20000]" in script
    assert "material_id: currentMaterial" in script
    assert "channel_id: MATERIALS[currentMaterial].channel" in script
    assert "target_g: String(target)" in script
    assert "limit: '10'" in script
    assert "`/runs/latest?${query}`" in script


def test_empty_target_card_does_not_create_a_canvas():
    script = (APP_DIR / "static" / "js" / "dashboard.js").read_text(encoding="utf-8")
    empty_renderer = script.split("function renderEmpty", 1)[1].split("function renderError", 1)[0]

    assert "No ${kg(target)} tests recorded yet" in empty_renderer
    assert "<canvas" not in empty_renderer


def test_target_layout_is_two_columns_and_collapses_to_one():
    stylesheet = (APP_DIR / "static" / "css" / "style.css").read_text(encoding="utf-8")

    assert ".target-grid { display: grid; grid-template-columns: repeat(2" in stylesheet
    assert ".channel-grid, .target-grid { grid-template-columns: 1fr; }" in stylesheet
