import asyncio
import time

import pytest

from jabba.core.errors import GraphTimeoutError, NodeFailedError
from jabba.core.graph import GraphSpec
from jabba.core.node import node


def make_graph():
    """parse -> (left, right) -> combine, all pure."""

    @node()
    async def parse(request) -> int:
        return request["x"]

    @node()
    async def left(parse: int) -> int:
        return parse + 1

    @node()
    async def right(parse: int) -> int:
        return parse * 10

    @node()
    async def combine(left: int, right: int) -> int:
        return left + right

    return GraphSpec("g", output="combine").add(parse, left, right, combine)


async def test_data_flows_to_output():
    graph = make_graph().compile()
    result = await graph.run({"x": 3})
    assert result.outputs["parse"] == 3
    assert result.outputs["combine"] == 4 + 30
    assert result.output == 34


async def test_parallel_execution_of_independent_nodes():
    spans = {}

    def track(name):
        @node(name=name)
        async def n():
            start = time.monotonic()
            await asyncio.sleep(0.05)
            spans[name] = (start, time.monotonic())

        return n

    graph = GraphSpec("g").add(track("a"), track("b")).compile()
    await graph.run(None)
    # a and b must overlap in time, not run sequentially
    assert spans["a"][0] < spans["b"][1] and spans["b"][0] < spans["a"][1]


async def test_fail_fast_raises_and_skips_pending():
    @node()
    async def boom() -> int:
        raise ValueError("nope")

    @node()
    async def child(boom: int) -> int:
        return boom

    graph = GraphSpec("g").add(boom, child).compile()
    with pytest.raises(NodeFailedError) as exc_info:
        await graph.run(None)
    assert exc_info.value.node == "boom"
    assert isinstance(exc_info.value.__cause__, ValueError)


async def test_fail_fast_stat_records_failed_and_skipped():
    @node()
    async def boom() -> int:
        raise ValueError("nope")

    @node()
    async def child(boom: int) -> int:
        return boom

    graph = GraphSpec("g").add(boom, child).compile()
    with pytest.raises(NodeFailedError) as exc_info:
        await graph.run(None)
    stat = exc_info.value.stat
    assert stat.status == "failed"
    assert stat.nodes["boom"].status == "failed"
    assert "nope" in stat.nodes["boom"].error
    assert stat.nodes["child"].status == "skipped"


async def test_optional_failure_yields_default():
    @node(optional=True, default={"fallback": True})
    async def flaky() -> dict:
        raise RuntimeError("source down")

    @node()
    async def consumer(flaky: dict) -> dict:
        return flaky

    graph = GraphSpec("g").add(flaky, consumer).compile()
    result = await graph.run(None)
    assert result.outputs["consumer"] == {"fallback": True}
    assert result.stat.status == "ok"
    assert result.stat.nodes["flaky"].status == "failed"


async def test_optional_timeout_yields_default():
    @node(optional=True, default=None, timeout=0.02)
    async def slow() -> dict:
        await asyncio.sleep(1)
        return {}

    @node()
    async def consumer(slow) -> bool:
        return slow is None

    graph = GraphSpec("g").add(slow, consumer).compile()
    result = await graph.run(None)
    assert result.outputs["consumer"] is True
    assert result.stat.nodes["slow"].status == "timeout"


async def test_non_optional_timeout_fails_graph():
    @node(timeout=0.02)
    async def slow() -> dict:
        await asyncio.sleep(1)
        return {}

    graph = GraphSpec("g").add(slow).compile()
    with pytest.raises(NodeFailedError):
        await graph.run(None)


async def test_graph_deadline():
    @node()
    async def slow() -> dict:
        await asyncio.sleep(1)
        return {}

    graph = GraphSpec("g").add(slow).compile()
    with pytest.raises(GraphTimeoutError):
        await graph.run(None, timeout=0.05)


async def test_ctx_stat_enrichment():
    @node()
    async def worker(ctx) -> int:
        ctx.stat.set("reports_count", 3)
        return 1

    graph = GraphSpec("g").add(worker).compile()
    result = await graph.run(None)
    assert result.stat.fields["reports_count"] == 3


async def test_stat_timings_and_metadata():
    graph = make_graph().compile()
    result = await graph.run({"x": 1}, request_id="req-42")
    stat = result.stat
    assert stat.request_id == "req-42"
    assert stat.graph == "g"
    assert stat.status == "ok"
    assert stat.duration_ms >= 0
    assert set(stat.nodes) == {"parse", "left", "right", "combine"}
    for node_stat in stat.nodes.values():
        assert node_stat.status == "ok"
        assert node_stat.duration_ms >= 0


async def test_after_ordering_without_data():
    order = []

    @node()
    async def warmup() -> None:
        await asyncio.sleep(0.02)
        order.append("warmup")

    @node(after=["warmup"])
    async def compute() -> None:
        order.append("compute")

    graph = GraphSpec("g").add(warmup, compute).compile()
    await graph.run(None)
    assert order == ["warmup", "compute"]


async def test_request_id_autogenerated_and_unique():
    graph = make_graph().compile()
    r1 = await graph.run({"x": 1})
    r2 = await graph.run({"x": 1})
    assert r1.stat.request_id and r2.stat.request_id
    assert r1.stat.request_id != r2.stat.request_id
