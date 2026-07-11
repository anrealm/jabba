import json

from prometheus_client import generate_latest

from jabba.core.context import NodeStat, RequestStat
from jabba.obs.metrics import observe_graph
from jabba.obs.request_record import RequestRecordEmitter, jsonl_sink


def make_stat():
    stat = RequestStat(request_id="r1", graph="g", status="ok", duration_ms=12.5)
    stat.nodes["a"] = NodeStat(status="ok", started_at=1.0, duration_ms=5.0)
    stat.nodes["b"] = NodeStat(status="failed", started_at=1.0, duration_ms=3.0, error="X: boom")
    stat.set("custom", 42)
    return stat


def test_emitter_sends_finalized_record_to_sink():
    records = []
    emitter = RequestRecordEmitter(sink=records.append)
    emitter.emit(make_stat())
    assert len(records) == 1
    record = records[0]
    assert record["record_version"] == 1
    assert record["request_id"] == "r1"
    assert record["graph"] == "g"
    assert record["nodes"]["b"]["error"] == "X: boom"
    assert record["fields"]["custom"] == 42


def test_jsonl_sink_writes_parseable_lines(tmp_path):
    path = tmp_path / "records.jsonl"
    emitter = RequestRecordEmitter(sink=jsonl_sink(path))
    emitter.emit(make_stat())
    emitter.emit(make_stat())
    lines = path.read_text().strip().splitlines()
    assert len(lines) == 2
    assert json.loads(lines[0])["graph"] == "g"


def test_observe_graph_records_prometheus_metrics():
    observe_graph(make_stat())
    text = generate_latest().decode()
    assert 'jabba_requests_total{graph="g",status="ok"}' in text
    assert 'jabba_node_duration_seconds_count{graph="g",node="a"}' in text
    assert 'jabba_node_failures_total{graph="g",kind="error",node="b"}' in text
    assert 'jabba_graph_duration_seconds_count{graph="g"}' in text
