"""Shared pytest fixtures for the telemetry server suite.

Safety first: ``DATABASE_URL`` is forced to a throwaway SQLite file inside a
temporary directory *before* ``app`` is imported, so the suite can never
reach a real MySQL database — no matter what the developer's .env says.
Every test starts from an empty schema (drop_all + create_all on that
throwaway file); nothing outside the temp directory is ever touched.

Run from the project root:

    server/.venv/Scripts/python.exe -m pytest server/tests -q
"""

import os
import sys
import tempfile
from pathlib import Path

import pytest

SERVER_DIR = Path(__file__).resolve().parents[1]
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

# --- test environment, forced before any app import -----------------------
_TMP_DB_DIR = Path(tempfile.mkdtemp(prefix="dispense-telemetry-tests-"))
os.environ["DATABASE_URL"] = "sqlite:///" + (_TMP_DB_DIR / "test.db").as_posix()
os.environ["API_KEY"] = ""
os.environ["READS_REQUIRE_KEY"] = "false"
os.environ["CORS_ORIGINS"] = ""
# A developer .env with MQTT_ENABLED=true would connect tests to a live broker.
os.environ["MQTT_ENABLED"] = "false"

from fastapi.testclient import TestClient  # noqa: E402

from app import database, models  # noqa: E402
from app.main import app  # noqa: E402

DEVICE_ID = "bits-a4cf12ab34cd"


@pytest.fixture()
def client():
    """TestClient bound to a freshly created empty schema."""
    database.reset_engine()
    engine = database.get_engine()
    models.Base.metadata.drop_all(engine)
    models.Base.metadata.create_all(engine)
    with TestClient(app) as test_client:
        yield test_client
    database.reset_engine()


def make_run_body(run_id: str, target_g: int = 5000, **overrides) -> dict:
    """A valid POST /api/v1/runs/start body (contract §3)."""
    body = {
        "run_id": run_id,
        "material_id": "M1",
        "device_id": DEVICE_ID,
        "job_id": 7,
        "target_g": target_g,
        "priority": 0,
        "firmware": "relay-controller 6.1-telemetry",
        "config": {
            "kp": 0.0025, "ki": 0.0003, "kd": 0.0001, "integral_max": 200.0,
            "tolerance_g": 20, "coarse_transition_g": 1000, "settle_ms": 1500,
            "max_duration_ms": 120000, "max_overshoot_g": 100, "window_ms": 500,
            "min_on_ms": 40, "min_off_ms": 40, "correction_limit": 5,
            "completion_mode": "PROCESS",
        },
    }
    body.update(overrides)
    if body.get("channel_id") == "CH2" and "material_id" not in overrides:
        body["material_id"] = "M2"
    return body


def make_sample(idx: int, weight_g: int, target_g: int = 5000,
                **overrides) -> dict:
    """A valid sample object (contract §3)."""
    sample = {
        "idx": idx,
        "uptime_ms": 1_000_000 + idx * 100,
        "elapsed_ms": idx * 100,
        "seq": idx,
        "weight_g": weight_g,
        "target_g": target_g,
        "error_g": target_g - weight_g,
        "stable": False,
        "weight_age_ms": 20,
        "p_term": 0.4, "i_term": 0.01, "d_term": 0.0, "pid_output": 0.41,
        "relay1": True, "relay2": False, "state": "COARSE_DISPENSE",
    }
    sample.update(overrides)
    if sample.get("channel_id") == "CH2" and "material_id" not in overrides:
        sample["material_id"] = "M2"
    return sample


def make_event(idx: int, event: str = "COARSE_STARTED", **overrides) -> dict:
    """A valid event object (contract §3)."""
    ev = {
        "idx": idx,
        "event": event,
        "elapsed_ms": idx * 100,
        "state": "COARSE_DISPENSE",
        "weight_g": 120,
        "detail": None,
    }
    ev.update(overrides)
    return ev


@pytest.fixture()
def start_run(client):
    """Callable: POST /runs/start and assert 200, returning the response body."""
    def _start(run_id: str = "bits-a4cf12ab34cd-j7-1834-a3f9",
               target_g: int = 5000, **overrides) -> dict:
        response = client.post("/api/v1/runs/start",
                               json=make_run_body(run_id, target_g, **overrides))
        assert response.status_code == 200, response.text
        return response.json()

    return _start


@pytest.fixture()
def push_samples(client):
    """Callable: POST /telemetry/batch, returning the raw response."""
    def _push(run_id: str, samples: list[dict], device_id: str = DEVICE_ID):
        channel = samples[0].get("channel_id", "CH1") if samples else "CH1"
        material = samples[0].get("material_id", "M1" if channel == "CH1" else "M2") if samples else "M1"
        samples = [dict(sample, material_id=sample.get("material_id", material)) for sample in samples]
        return client.post("/api/v1/telemetry/batch",
                           json={"run_id": run_id, "material_id": material, "channel_id": channel, "device_id": device_id,
                                 "samples": samples})

    return _push


@pytest.fixture()
def push_events(client):
    """Callable: POST /events, returning the raw response."""
    def _push(run_id: str, events: list[dict], device_id: str = DEVICE_ID):
        return client.post("/api/v1/events",
                           json={"run_id": run_id, "material_id": "M1", "channel_id": "CH1", "device_id": device_id,
                                 "events": events})

    return _push


def pytest_configure(config):
    config.addinivalue_line("markers", "live_broker: needs a running Mosquitto (BITS_BROKER_* env vars)")

