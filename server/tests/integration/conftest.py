"""Fixtures for the broker-in-the-loop suite. Everything is skipped (with the reason) when
WSL/mosquitto is unavailable; nothing here can reach the real broker, database or server.
"""
import pytest

from . import harness
from .stack import Stack


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: long fault-injection test (kill -9, restarts)")


@pytest.fixture(scope="session")
def broker(tmp_path_factory):
    reason = harness.broker_available()
    if reason:
        pytest.skip(reason)
    b = harness.Broker(tmp_path_factory.mktemp("itbroker"))
    b.start()
    try:
        yield b
    finally:
        b.close()


@pytest.fixture()
def make_stack(broker, tmp_path):
    """Factory: fresh broker state + seeded DB per test; everything torn down afterwards."""
    broker.reset()
    made: list[Stack] = []

    def _make(**kw) -> Stack:
        s = Stack(broker, tmp_path, **kw)
        made.append(s)
        return s

    yield _make
    for s in made:
        s.down()


@pytest.fixture()
def stack(make_stack):
    """Full system, relay and sender up, operator logged in as opr1, weight flowing."""
    return make_stack().up().ready()
