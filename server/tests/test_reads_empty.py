"""Read endpoints on an empty database: empty states, never a 500.

A freshly-installed server has no runs at all, and the dashboard/history/live
pages must render their "nothing yet" states from these responses.
"""


def test_targets_is_empty_list(client):
    response = client.get("/api/v1/targets")

    assert response.status_code == 200
    assert response.json() == []


def test_dashboard_has_no_targets(client):
    response = client.get("/api/v1/dashboard")

    assert response.status_code == 200
    body = response.json()
    assert body["targets"] == []
    assert body["generated_at"] is not None


def test_latest_runs_is_empty_list(client):
    response = client.get("/api/v1/runs/latest")

    assert response.status_code == 200
    assert response.json() == []


def test_run_search_echoes_paging_with_zero_total(client):
    response = client.get("/api/v1/runs")

    assert response.status_code == 200
    body = response.json()
    assert body == {"items": [], "total": 0, "limit": 50, "offset": 0}


def test_live_reports_no_active_run(client):
    response = client.get("/api/v1/live")

    assert response.status_code == 200
    body = response.json()
    assert body["active_run"] is None
    assert body["is_live"] is False
    assert body["samples"] == []
    assert body["last_sample"] is None
    assert body["database"] == "up"
    assert body["server_time"] is not None


def test_missing_run_is_404(client):
    assert client.get("/api/v1/runs/nope").status_code == 404
    assert client.get("/api/v1/runs/nope/samples").status_code == 404
    assert client.get("/api/v1/runs/nope/events").status_code == 404
    assert client.get("/api/v1/runs/nope/full").status_code == 404
    assert client.get("/api/v1/runs/nope/csv").status_code == 404


def test_garbage_from_timestamp_is_422(client):
    assert client.get("/api/v1/runs?from=not-a-date").status_code == 422
    assert client.get("/api/v1/runs?to=13/07/2026").status_code == 422