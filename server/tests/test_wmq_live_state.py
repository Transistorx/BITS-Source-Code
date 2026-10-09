"""REQ-WMQ-10 / 13: boot_id regression guard and reject diagnostics in live_state."""
import pytest

from app.services.live_state import LiveState


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


@pytest.fixture()
def ls():
    c = Clock()
    state = LiveState(clock=c)
    state.c = c
    return state


def _put(ls, boot, uptime, device="s1", channel="CH1"):
    return ls.update_sender_weight(device, channel, uptime, 100, 10, boot_id=boot)


def test_REQ_WMQ_10_boot_id_regression_within_10s_is_rejected_and_counted(ls):
    assert _put(ls, "aaaaaaaa", 1000)
    ls.c.t += 1
    assert _put(ls, "bbbbbbbb", 2000)
    ls.c.t += 1
    before = ls.snapshot()["diagnostics"]["rejected"]
    assert _put(ls, "aaaaaaaa", 3000) is False
    diag = ls.snapshot()["diagnostics"]
    assert diag["rejected"] == before + 1
    assert diag["weight_rejected"] >= 1
    assert "s1" in diag["last_reject"]


def test_REQ_WMQ_10_rejected_regression_does_not_overwrite_current_value(ls):
    _put(ls, "aaaaaaaa", 1000)
    ls.c.t += 1
    ls.update_sender_weight("s1", "CH1", 2000, 777, 10, boot_id="bbbbbbbb")
    ls.c.t += 1
    ls.update_sender_weight("s1", "CH1", 3000, 111, 10, boot_id="aaaaaaaa")
    assert ls.snapshot()["senders"]["s1"]["channels"][0]["weight_g"] == 777


def test_REQ_WMQ_10_regression_accepted_after_the_10s_window(ls):
    _put(ls, "aaaaaaaa", 1000)
    ls.c.t += 1
    _put(ls, "bbbbbbbb", 2000)
    ls.c.t += 10.5
    assert _put(ls, "aaaaaaaa", 20000) is True


def test_REQ_WMQ_10_same_boot_id_keeps_flowing(ls):
    for i in range(5):
        assert _put(ls, "aaaaaaaa", 1000 + i * 500)
        ls.c.t += 0.5


def test_REQ_WMQ_10_missing_boot_id_is_accepted_for_legacy_firmware(ls):
    assert ls.update_sender_weight("s1", "CH1", 1000, 5, 10) is True
    ls.c.t += 1
    assert ls.update_sender_weight("s1", "CH1", 2000, 6, 10, boot_id=None) is True


def test_REQ_WMQ_10_guard_is_per_device(ls):
    _put(ls, "aaaaaaaa", 1000, device="s1")
    ls.c.t += 1
    _put(ls, "bbbbbbbb", 2000, device="s1")
    assert _put(ls, "aaaaaaaa", 1000, device="s2") is True


def test_REQ_WMQ_13_note_reject_records_reason_per_device(ls):
    ls.note_reject("s9", "bad channel")
    diag = ls.snapshot()["diagnostics"]
    assert diag["weight_rejected"] == 1
    assert "bad channel" in str(diag["last_reject"]["s9"])


def test_REQ_WMQ_13_last_reject_keeps_latest_reason(ls):
    ls.note_reject("s9", "first")
    ls.note_reject("s9", "second")
    diag = ls.snapshot()["diagnostics"]
    assert diag["weight_rejected"] == 2
    assert "second" in str(diag["last_reject"]["s9"])


def test_REQ_WMQ_13_clear_resets_reject_diagnostics(ls):
    ls.note_reject("s9", "x")
    ls.clear()
    diag = ls.snapshot()["diagnostics"]
    assert diag["weight_rejected"] == 0 and not diag["last_reject"]
