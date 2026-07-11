from fastapi.testclient import TestClient

from examples.demo_scoring.app import build_app


def test_demo_scores_with_default_graph():
    records = []
    app = build_app(record_sink=records.append)
    with TestClient(app) as client:
        response = client.post("/score", json={"user_id": "alice", "amount": 2.0})
    assert response.status_code == 200
    body = response.json()
    assert body["user_id"] == "alice"
    assert body["score"] > 0
    assert records[0]["graph"] == "scoring_default"
    assert records[0]["status"] == "ok"


def test_demo_personal_graph_for_vip_service():
    records = []
    app = build_app(record_sink=records.append)
    with TestClient(app) as client:
        base = client.post("/score", json={"user_id": "alice", "amount": 2.0}).json()
        vip = client.post(
            "/score", json={"user_id": "alice", "amount": 2.0, "service": "vip"}
        ).json()
    assert vip["score"] == base["score"] * 2
    assert records[1]["graph"] == "scoring_vip"


def test_demo_debug_section_exposes_stat():
    app = build_app()
    with TestClient(app) as client:
        body = client.post(
            "/score", json={"user_id": "alice", "amount": 2.0}, params={"debug": "1"}
        ).json()
    assert body["debug"]["graph"] == "scoring_default"
    assert set(body["debug"]["nodes"]) == {
        "parsed",
        "user_features",
        "external_report",
        "features",
        "score",
        "response",
    }
