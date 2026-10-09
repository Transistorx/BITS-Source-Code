"""GET /health — contract §4.

The endpoint must always answer HTTP 200 with a readable body so an external
monitor (or the dashboard's status dot) can distinguish "server up, DB down"
from "server unreachable".
"""


def test_health_on_empty_database(client):
    response = client.get("/health")

    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "ok"
    assert body["database"] == "up"
    assert body["last_telemetry_at"] is None
    assert body["run_count"] == 0


def test_health_counts_runs_and_reports_last_telemetry(client, start_run):
    start_run("bits-a4cf12ab34cd-j1-100-aaaa")
    start_run("bits-a4cf12ab34cd-j2-200-bbbb", target_g=10000)

    body = client.get("/health").json()

    assert body["run_count"] == 2
    assert body["last_telemetry_at"] is not None


def test_health_reports_degraded_not_error_when_database_is_down(
        client, monkeypatch):
    from app import database

    monkeypatch.setenv(
        "DATABASE_URL", "mysql+pymysql://u:p@127.0.0.1:1/none?charset=utf8mb4"
    )
    database.reset_engine()
    try:
        response = client.get("/health")

        assert response.status_code == 200
        body = response.json()
        assert body["status"] == "degraded"
        assert body["database"] == "down"
        assert body["run_count"] == 0
        assert body["last_telemetry_at"] is None
    finally:
        database.reset_engine()