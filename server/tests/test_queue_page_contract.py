"""Queue page: KPI strip, New Job layout, profile preview, unique-id cancel, pump control."""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
QUEUE_HTML = APP_DIR / "templates" / "queue.html"
QUEUE_JS = APP_DIR / "static" / "js" / "queue.js"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def test_kpi_strip_present_and_tied_to_real_ids():
    """The strip must exist in the page AND be filled from real ids by
    renderKpiStrip. A comment mentioning queue-kpis must not pass."""
    html = _read(QUEUE_HTML)
    assert 'id="queue-kpis"' in html
    for kid in ("kpi-controller", "kpi-waiting", "kpi-active", "kpi-telemetry"):
        assert f'id="{kid}"' in html, kid
    js = _read(QUEUE_JS)
    assert "function renderKpiStrip" in js
    body = js.split("function renderKpiStrip", 1)[1].split("function ", 1)[0]
    # renderKpiStrip has no nested `function `, so the naive capture is safe.
    for kid in ("queue-kpis", "kpi-controller", "kpi-waiting", "kpi-active", "kpi-telemetry"):
        assert f"T.el('{kid}')" in body, kid


def test_new_job_row_holds_four_fields_and_the_add_button():
    """R3 pins the proportional column sizing in CSS; this test pins that the
    row carries the four fields and the submit control the handler binds to."""
    html = _read(QUEUE_HTML)
    for field in ("job-material", "job-target", "job-profile", "job-priority", "add-job"):
        assert f'id="{field}"' in html, field
    assert "new-job-grid" in html


def test_selected_profile_preview_strip_exists():
    html = _read(QUEUE_HTML)
    assert 'id="job-profile-preview"' in html
    js = _read(QUEUE_JS)
    body = js.split("function renderProfilePreview", 1)[1].split("function ", 1)[0]
    # renderProfilePreview has no nested `function `, so the naive capture is safe.
    # Pin the READS, not the labels: a static template of the words must not pass.
    for field in ("row.profile_id", "row.version", "row.kp", "row.ki", "row.kd",
                  "row.window_ms", "row.min_on_ms", "row.min_off_ms",
                  "row.tolerance_g", "row.max_overshoot_g", "row.max_duration_ms"):
        assert field in body, field
    assert "row.active" in body  # ACTIVE vs HISTORICAL badge branch


def test_cancel_targets_the_unique_id_never_the_target_weight():
    js = _read(QUEUE_JS)
    # NB: cancelJob's runAction callback contains a nested `function `, so
    # split("function ", 1)[0] truncates at `return runAction(` and drops the
    # dropRow call. Bound by the next top-level function instead.
    body = js.split("function cancelJob", 1)[1].split("document.addEventListener", 1)[0]
    assert "local_job_id" in body or "command_id" in body
    assert "target_g" not in body, "cancel must not be addressed by target weight"


def test_emergency_controls_panel_removed_but_backend_preserved():
    """The visible Emergency controls panel is removed from the Queue page,
    but the underlying ESTOP/CLEAR endpoints and firmware safety latch must
    remain intact so hardware/system safety mechanisms still work."""
    html = _read(QUEUE_HTML)
    assert "safety-panel" not in html, "Emergency controls panel must be removed"
    assert 'id="estop"' not in html, "Global Emergency Stop button must be removed"
    assert 'id="clear-estop"' not in html, "Clear safety latch button must be removed"
    # Backend capability preserved: ESTOP/CLEAR still accepted by the server.
    ops = (APP_DIR / "routes" / "operations.py").read_text(encoding="utf-8")
    assert "ESTOP" in ops, "ESTOP endpoint must remain"
    assert "CLEAR" in ops, "CLEAR endpoint must remain"


def _render_queue_body() -> str:
    # NB: renderQueue's map callback contains nested `function ` tokens, so
    # split("function ", 1)[0] truncates at `jobs.map(function (job, index)`
    # and loses every cell. Bound the capture by the next top-level function.
    js = _read(QUEUE_JS)
    return js.split("function renderQueue", 1)[1].split("function renderKpiStrip", 1)[0]


def test_queue_table_lists_profile_version_priority_created():
    """Regression pin — renderQueue was not changed in the Task 4 diff, so this
    was already green. Pins the real thead structure and the path the profile
    version takes to the cell (renderQueue -> pinLabel -> job.profile_version)."""
    body = _render_queue_body()
    assert "<thead>" in body
    expected = ("<th>Position</th><th>Job ID</th><th>Material</th><th>Target</th>"
                "<th>PID profile</th><th>Priority</th><th>Created</th>"
                "<th>Status</th><th>Action</th>")
    assert expected in body, "queue table headers changed order or wording"
    # The profile version reaches the cell through pinLabel, not as a bare token.
    assert "pinLabel(job)" in body
    pin = _read(QUEUE_JS).split("function pinLabel", 1)[1].split("function ", 1)[0]
    assert "job.profile_version" in pin


def test_new_job_is_not_a_full_width_button():
    css = _read(APP_DIR / "static" / "css" / "style.css")
    assert "new-job-grid" in css
    # Every rule mentioning .new-job-grid, not just the first. Splitting on the
    # selector yields one chunk per occurrence; the LAST chunk is the 640px
    # mobile rule, which is the only one allowed to force full width.
    blocks = css.split(".new-job-grid")[1:]
    assert len(blocks) >= 3, "expected base + at least one media override"
    for chunk in blocks[:-1]:
        rule = chunk.split("}", 1)[0]
        assert "width: 100%" not in rule, f"desktop rule forces full width: .new-job-grid{rule[:60]}"
    mobile_rule = blocks[-1].split("}", 1)[0]
    assert "width: 100%" in mobile_rule, "mobile 640px block must still fill the row"
