"""Responsiveness changes: command_wire without N+1, queue/visibility JS guards."""

from pathlib import Path

from sqlalchemy import event, inspect

from app import database
from test_profile_pinning import _create_profile, _queue_job, _seed_device

APP_DIR = Path(__file__).resolve().parents[1] / "app"
QUEUE_JS = (APP_DIR / "static" / "js" / "queue.js").read_text(encoding="utf-8")
COMMON_JS = (APP_DIR / "static" / "js" / "common.js").read_text(encoding="utf-8")


def test_device_commands_prefetch_profiles_in_one_query(client):
    device_id = _seed_device(client)
    _create_profile(client)                 # v1 (active)
    first = _queue_job(client, profile_version=1).json()
    _create_profile(client, kp=0.003)       # v2 (immutable insert; supersedes v1)
    second = _queue_job(client, profile_version=2).json()

    statements = []
    engine = database.get_engine()

    def capture(conn, cursor, statement, *args):
        statements.append(statement)

    event.listen(engine, "before_cursor_execute", capture)
    try:
        delivered = client.get("/api/v1/device/commands", params={"device_id": device_id}).json()
    finally:
        event.remove(engine, "before_cursor_execute", capture)

    by_id = {item["command_id"]: item for item in delivered}
    assert by_id[first["command_id"]]["profile"]["kp"] == 0.0025
    assert by_id[first["command_id"]]["profile"]["version"] == 1
    assert by_id[second["command_id"]]["profile"]["kp"] == 0.003
    assert by_id[second["command_id"]]["profile"]["version"] == 2
    profile_selects = [s for s in statements if "FROM tuning_profiles" in s]
    assert len(profile_selects) <= 1, profile_selects


def test_deleted_profile_still_yields_null_gains(client):
    from app import models
    from app.routes.operations import command_wire, _with_gains
    row = models.DeviceCommand(device_id="d", command_type="JOB", profile_id="gone",
                               profile_version=3, target_g=5000)
    with database.get_engine().connect() as conn:
        from sqlalchemy.orm import Session
        with Session(conn) as db:
            (pair,) = _with_gains([row], db)
            assert command_wire(*pair[:1], db, pair[1])["profile"] is None


def test_device_command_indexes_exist(client):
    names = {i["name"] for i in inspect(database.get_engine()).get_indexes("device_commands")}
    assert {"ix_device_commands_local_job", "ix_device_commands_channel_state",
            "ix_device_commands_device_state_held"} <= names


def test_queue_js_has_pending_feedback_and_queue_only_refresh():
    for needle in ("Cancelling…", "Holding…", "Promoting…", "PENDING_JOB_TIMEOUT_MS",
                   "refreshGeneration", "refresh(true)", "lastQueueHtml", "liveDevice()"):
        assert needle in QUEUE_JS, needle
    # Failure restores the row; success never fakes a device ACK.
    assert "clearRowPending(row.kind, row.id)" in QUEUE_JS
    assert "setInterval(refresh" not in QUEUE_JS


def test_polling_pauses_when_tab_hidden():
    assert "function visibleInterval" in COMMON_JS
    assert "visibilitychange" in COMMON_JS
    assert "visibleInterval: visibleInterval" in COMMON_JS
    for name in ("queue.js", "live.js", "dashboard.js"):
        js = (APP_DIR / "static" / "js" / name).read_text(encoding="utf-8")
        assert "setInterval(" not in js, name
        assert "visibleInterval" in js, name
    assert "setInterval(pollLive" not in COMMON_JS
