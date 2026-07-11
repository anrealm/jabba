from prometheus_client import Counter, Histogram

from jabba.core.context import RequestStat

REQUESTS = Counter("jabba_requests_total", "Graph runs by final status", ["graph", "status"])
GRAPH_DURATION = Histogram("jabba_graph_duration_seconds", "Whole-graph wall time", ["graph"])
NODE_DURATION = Histogram("jabba_node_duration_seconds", "Per-node wall time", ["graph", "node"])
NODE_FAILURES = Counter(
    "jabba_node_failures_total", "Node failures by kind", ["graph", "node", "kind"]
)


def observe_graph(stat: RequestStat) -> None:
    """Record a finalized RequestStat into prometheus metrics."""
    REQUESTS.labels(graph=stat.graph, status=stat.status).inc()
    if stat.duration_ms is not None:
        GRAPH_DURATION.labels(graph=stat.graph).observe(stat.duration_ms / 1000)
    for name, node_stat in stat.nodes.items():
        if node_stat.duration_ms is not None:
            NODE_DURATION.labels(graph=stat.graph, node=name).observe(node_stat.duration_ms / 1000)
        if node_stat.status in ("failed", "timeout"):
            kind = "error" if node_stat.status == "failed" else "timeout"
            NODE_FAILURES.labels(graph=stat.graph, node=name, kind=kind).inc()
