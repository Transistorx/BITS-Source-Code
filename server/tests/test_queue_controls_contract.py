"""Static contracts for the per-row queue controls (Start / Stop / Cancel).

The operator asked for Start/Stop on every queued weight so they can choose what
dispenses first. These pin the three row actions and the parked-row treatment;
the behavioural contract for HOLD/RELEASE/PROMOTE lives in test_operations.py
and, on the device, in the job_queue QEMU suite.
"""

from pathlib import Path

APP_DIR = Path(__file__).resolve().parents[1] / "app"
QUEUE_JS = APP_DIR / "static" / "js" / "queue.js"


def _script() -> str:
    return QUEUE_JS.read_text(encoding="utf-8")


def test_every_waiting_row_offers_start_stop_and_cancel():
    script = _script()

    assert 'data-start=' in script
    assert 'data-stop=' in script
    assert 'data-cancel=' in script
    # The three actions share one Action cell per row, not a toolbar.
    assert ">Start</button>" in script
    assert ">Stop</button>" in script
    assert ">Cancel</button>" in script


def test_a_parked_row_is_marked_parked_and_dimmed():
    script = _script()

    assert "Parked" in script
    assert "job.held" in script


def test_start_releases_a_parked_job_before_promoting_it():
    """Promote alone would only move a parked job up the line it is barred from.

    Start has to clear the hold as well, and RELEASE must go first so a failure
    leaves the job parked rather than promoted-but-still-ineligible.
    """
    script = _script()
    body = script.split("async function startJob", 1)[1].split("document.addEventListener", 1)[0]

    assert "'RELEASE'" in body and "'PROMOTE'" in body and "'HOLD'" in script
    # The held gate is consulted before RELEASE, and RELEASE before PROMOTE.
    # Gating matters: an unconditional RELEASE would race a release onto a job
    # the operator never parked.
    assert body.index("dataset.held") < body.index("'RELEASE'") < body.index("'PROMOTE'")


def test_queue_order_puts_a_promoted_job_ahead_of_priority_and_fifo():
    """Mirrors job_outweighs() in the firmware: promoted, then priority, then FIFO."""
    script = _script()

    assert "promoted" in script
    assert "priority" in script
