"""The MQTT-only backend service must not load a web/HTTP stack, and the local
envelope encoder must keep the wire output of fastapi's jsonable_encoder."""

import subprocess
import sys
from datetime import date, datetime, time, timedelta, timezone
from decimal import Decimal
from enum import Enum
from pathlib import Path
from uuid import UUID

import pytest
from fastapi.encoders import jsonable_encoder  # TEST ONLY: golden comparison
from pydantic import BaseModel

from app.rpc import envelope

SERVER_DIR = Path(__file__).resolve().parents[1]


class Color(Enum):
    RED = "red"
    ONE = 1


class Model(BaseModel):
    a: int
    when: datetime
    d: Decimal
    c: Color


GOLDEN = [
    {"t": datetime(2026, 1, 2, 3, 4, 5, 678000, tzinfo=timezone.utc)},
    {"t": datetime(2026, 1, 2, 3, 4, 5)},
    {"d": date(2026, 1, 2), "tm": time(1, 2, 3), "td": timedelta(seconds=90)},
    {"dec": [Decimal("12"), Decimal("1.50"), Decimal("1E+2"), Decimal("-0.001")]},
    {"e": Color.RED, "e2": Color.ONE},
    {"s": {3, 3}, "tu": (1, "x", None), "fs": frozenset([7])},
    {"b": b"hello", "u": UUID("12345678-1234-5678-1234-567812345678"), "p": Path("a")},
    {"m": Model(a=1, when=datetime(2026, 1, 2, tzinfo=timezone.utc), d=Decimal("2.5"), c=Color.RED)},
    {"nested": {"k": [{"x": Decimal("3")}]}, 1: "intkey", "f": 1.5, "z": None, "bool": True},
    [Model(a=2, when=datetime(2026, 5, 6, tzinfo=timezone.utc), d=Decimal("1"), c=Color.ONE)],
]


@pytest.mark.parametrize("value", GOLDEN)
def test_json_safe_matches_jsonable_encoder(value):
    got, want = envelope.json_safe(value), jsonable_encoder(value)
    assert got == want
    assert envelope.encode({"x": got}) == envelope.encode({"x": want})
    if isinstance(value, dict) and "s" in value:
        assert got["s"] == want["s"]


def test_response_wire_output_unchanged():
    data = {"t": datetime(2026, 1, 2, tzinfo=timezone.utc), "d": Decimal("1.5")}
    r = envelope.response("c1", ok=True, code="OK", data=data, server_time="x", seq=1)
    assert r["data"] == jsonable_encoder(data)
    assert envelope.response("c1", ok=True, code="OK", server_time="x", seq=1)["data"] is None


def test_non_finite_floats_rejected_on_encode():
    for bad in (float("nan"), float("inf"), float("-inf")):
        r = envelope.response("c", ok=True, code="OK", data={"v": bad}, server_time="x", seq=1)
        with pytest.raises(ValueError):
            envelope.encode(r)


def test_unknown_type_rejected():
    with pytest.raises(TypeError):
        envelope.json_safe(object())


_PROBE = r"""
import sys, json
sys.path.insert(0, sys.argv[1])
import app.service, app.rpc.ops_plane, app.rpc.dispatch, app.rpc.methods, app.rpc.admin
import app.rpc.envelope, app.rpc.limits, app.rpc.state
import app.run_ingest, app.recovery, app.mqtt_bridge
import app.services.analytics, app.services.auth, app.services.command_claim
import app.services.control, app.services.errors, app.services.history
import app.services.live_state, app.services.materials, app.services.profiles
import app.services.profile_pin, app.services.queue, app.services.runs
import app.services.telemetry_ingest
bad = ("fastapi", "starlette", "uvicorn", "httpx", "requests", "websockets", "aiohttp", "http.client")
print(json.dumps(sorted(m for m in bad if m in sys.modules)))
"""


def test_service_import_graph_loads_no_third_party_http_stack():
    proc = subprocess.run([sys.executable, "-I", "-c", _PROBE, str(SERVER_DIR)],
                          capture_output=True, text=True, timeout=120, cwd=str(SERVER_DIR))
    assert proc.returncode == 0, proc.stderr
    loaded = proc.stdout.strip().splitlines()[-1]
    # http.client is stdlib and may be pulled by paho/drivers; only third-party stacks are asserted.
    third_party = [m for m in __import__("json").loads(loaded) if m != "http.client"]
    assert third_party == [], f"service import graph loads {third_party}"
