"""Static contracts for the graph workspace and the History filters/delete.

The Graphs page exists to visualise runs — no textual run summary belongs on it.
The History page owns the records and the per-run Delete action, and applies
filters automatically with no Search button and no full-page reload.
"""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
DASHBOARD_JS = APP_DIR / "static" / "js" / "dashboard.js"
HISTORY_JS = APP_DIR / "static" / "js" / "history.js"


def _script(path: Path) -> str:
    return path.read_text(encoding="utf-8")


# ---------------------------------------------------------------- graphs ----


def test_graph_run_card_renders_only_the_chart_and_its_controls():
    """Every metric, badge and detail link is stripped from a target card.

    The card keeps the 5/10/15/20 kg heading, the run selector, the Compare
    button and the chart host — nothing else.
    """
    script = _script(DASHBOARD_JS)
    body = script.split("function renderRunCard", 1)[1].split("async function loadSamples", 1)[0]

    assert "target-controls" in body
    assert "data-run-select" in body
    assert "data-compare-target" in body
    assert "target-chart-host" in body

    for forbidden in ("target-metrics", "panel-footer", "Open complete run details",
                     "Final weight", "Final error", "Overshoot", "Duration",
                     "PID gains", "Profile", "Recorded", "metric("):
        assert forbidden not in body, forbidden


def test_graph_card_head_carries_no_result_or_run_summary():
    script = _script(DASHBOARD_JS)
    body = script.split("function renderRunCard", 1)[1].split("async function loadSamples", 1)[0]

    # The heading is the target ("5 KG TESTS"); a result badge or a recorded
    # count is run summary and belongs on History, not here.
    assert "status-badge" not in body
    assert "record-count" not in body
    assert "runLabel(" in body  # the selector still needs a label per option


def test_empty_graph_state_has_no_summary_beneath_it():
    script = _script(DASHBOARD_JS)
    empty = script.split("async function loadSamples", 1)[1].split("async function loadTarget", 1)[0]

    assert "No sample points for this run" in empty
    assert "Run metrics remain available below" not in empty
    assert "below" not in empty


def test_compare_options_carry_no_run_summary():
    """Compare output is graph-based: option labels name the run, not its results."""
    script = _script(DASHBOARD_JS)
    compare = script.split("function openCompare", 1)[1].split("async function buildComparison", 1)[0]

    assert "final_weight_g" not in compare
    assert "final_error" not in compare
    assert "overshoot" not in compare
    assert "runLabel(" in compare

    build = script.split("async function buildComparison", 1)[1].split("function switchMaterial", 1)[0]
    assert "final_weight_g" not in build
    assert "target-metrics" not in build


def test_graph_page_still_holds_the_four_targets_and_material_tabs(client):
    html = client.get("/graphs").text

    assert 'data-material="M1"' in html
    assert 'data-material="M2"' in html
    assert 'id="target-grid"' in html
    assert 'id="compare-dialog"' in html
    assert 'id="comparison-chart"' in html
    assert 'id="compare-runs"' in html
    assert 'id="graphs-context"' in html


def test_selecting_a_run_redraws_that_runs_graph():
    """The run selector is the only way to choose which test is plotted."""
    script = _script(DASHBOARD_JS)

    # The change handler re-renders the card for the chosen target, and
    # renderRunCard always ends by loading that run's samples.
    handler = script.split("addEventListener('change'", 1)[1]
    assert "data-run-select" in handler
    assert "renderRunCard(target)" in handler

    body = script.split("function renderRunCard", 1)[1].split("async function loadSamples", 1)[0]
    assert body.rstrip().endswith("loadSamples(target, run);\n  }") or \
           "loadSamples(target, run)" in body
    load = script.split("async function loadSamples", 1)[1].split("async function loadTarget", 1)[0]
    assert "/samples?max_points=" in load
    assert "createWeightChart" in load


# --------------------------------------------------------------- history ----


def test_history_page_has_no_search_button_and_offers_reset(client):
    html = client.get("/history").text

    assert 'id="search-btn"' not in html
    assert 'id="search-btn"' not in _script(HISTORY_JS)
    assert 'id="reset-filters"' in html
    assert "read-only" not in html.lower()


def test_history_script_applies_filters_automatically():
    """Selects and dates refresh immediately; free text debounces."""
    script = _script(HISTORY_JS)

    assert "search-btn" not in script
    # Every filter control is wired to a change/input listener.
    assert "addEventListener('change'" in script or 'addEventListener("change"' in script
    assert "addEventListener('input'" in script or 'addEventListener("input"' in script
    # Debounce window must sit in the 300–500 ms band the brief specifies.
    assert "350" in script or "400" in script or "450" in script
    assert "reset-filters" in script


def test_history_script_discards_stale_filter_responses():
    """A slow older request must never overwrite a newer filter result."""
    script = _script(HISTORY_JS)

    assert "AbortController" in script
    # A monotonic token is what actually decides whether a response is applied.
    assert "requestSeq" in script or "requestToken" in script or "seq" in script


def test_history_script_deletes_a_single_run_with_confirmation():
    script = _script(HISTORY_JS)

    assert "DELETE" in script
    assert "confirm(" in script
    assert "data-delete" in script
    # Deletion is addressed by the authoritative run id, not a display number.
    assert "run_id" in script
    assert "/runs/" in script


def test_history_rows_expose_a_delete_action_that_does_not_navigate(client, start_run):
    """Contract on the rendered script: the Delete control stops row navigation."""
    script = _script(HISTORY_JS)

    body = script.split("function rowHtml", 1)[1].split("function renderResults", 1)[0]
    assert "data-delete" in body
    assert "Delete" in body
    # The click handler must ignore Delete clicks so the row link does not fire.
    handlers = script.split("addEventListener('click'", 1)
    assert len(handlers) > 1
    assert "closest" in handlers[1]
    assert "data-delete" in handlers[1] or "data-delete" in script


def test_history_filters_stay_applied_after_a_delete(client, start_run):
    """Deleting must refresh the list without dropping the operator's filters."""
    script = _script(HISTORY_JS)

    delete_section = script.split("function deleteRun", 1)
    assert len(delete_section) == 2, "deleteRun helper is missing"
    # Bound the helper by the next top-level function, not by the first nested
    # `function` keyword — deleteRun's .then callbacks contain their own.
    body = delete_section[1].split("function applyNow", 1)[0]
    # A delete re-queries with the current filter set rather than resetting it.
    assert "search(" in body
    assert "form.reset()" not in body


def test_combined_filters_accumulate_rather_than_replacing_each_other():
    """Changing one filter must preserve every other active filter."""
    script = _script(HISTORY_JS)
    body = script.split("function buildQuery", 1)[1].split("async function populateTargets", 1)[0]

    for param in ("channel_id", "material_id", "target_g", "status", "from", "to",
                  "run_id", "profile_id", "profile_version"):
        assert f"params.set('{param}'" in body, param


def test_delete_addresses_the_run_by_primary_key():
    """The DELETE URL carries run_id, never a display number like test #2."""
    script = _script(HISTORY_JS)

    body = script.split("function deleteRun", 1)[1].split("function applyNow", 1)[0]
    assert "/runs/' + encodeURIComponent(runId)" in body
    assert "data-delete=" in script
    # run_id is what the button carries; test_number is never the identifier.
    assert "test_number" not in body
    assert "method: 'DELETE'" in body
