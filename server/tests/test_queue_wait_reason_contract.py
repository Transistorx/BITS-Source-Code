from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
JS = (APP_DIR / "static" / "js" / "queue.js").read_text(encoding="utf-8")


def _body():
    start = JS.index("function waitReason(")
    return JS[start:JS.index("function renderQueue(", start)]


def test_reasons_present_in_priority_order():
    body = _body()
    needles = [
        "Controller offline",
        "Waiting for controller to accept the job",
        "faulted:",
        "Operator must clear it at the machine",
        "No valid weight from scale",
        "is paused",
        "Scale in use by",
        "Awaiting operator READY",
        "Job on hold",
        "(position ",
        "Ready - waiting for the controller to start it",
    ]
    last = -1
    for needle in needles:
        pos = body.index(needle)
        assert pos > last, needle
        last = pos


def test_unknown_fields_do_not_invent_reasons():
    body = _body()
    assert "channel.weight_valid === false" in body  # undefined weight_valid -> no reason
    assert "if (!channel) return null;" in body


def test_wait_reason_is_rendered_escaped_and_read_only():
    assert "esc(why.text)" in JS
    assert "waitReason(job, {" in JS
    assert "renderQueue(jobs, channels, online)" in JS
    body = _body()
    assert "post(" not in body and "data-control" not in body and "apiWrite" not in body
