import pytest
from fastapi.testclient import TestClient

from jabba.core.errors import NodeFailedError
from jabba.core.graph import GraphSpec
from jabba.core.node import Node, node
from jabba.service.app import create_app
from jabba.service.registry import GraphRegistry
from jabba.service.runner import GraphRunner


def make_registry():
    @node()
    async def parse(request) -> int:
        return request["x"]

    @node()
    async def double(parse: int) -> int:
        return parse * 2

    return GraphRegistry(default=GraphSpec("base", output="double").add(parse, double).compile())


async def test_runner_runs_and_emits_record():
    records = []
    runner = GraphRunner(make_registry(), record_sink=records.append)
    result = await runner.run(None, {"x": 21})
    assert result.output == 42
    assert len(records) == 1
    assert records[0]["graph"] == "base"
    assert records[0]["status"] == "ok"


async def test_runner_emits_record_even_on_failure():
    @node()
    async def boom() -> int:
        raise ValueError("nope")

    registry = GraphRegistry(default=GraphSpec("base").add(boom).compile())
    records = []
    runner = GraphRunner(registry, record_sink=records.append)
    with pytest.raises(NodeFailedError):
        await runner.run(None, None)
    assert records[0]["status"] == "failed"


async def test_runner_resolves_personal_graph():
    registry = make_registry()

    @node()
    async def fixed() -> int:
        return 7

    registry.register("svc", GraphSpec("personal", output="fixed").add(fixed).compile())
    records = []
    runner = GraphRunner(registry, record_sink=records.append)
    result = await runner.run("svc", None)
    assert result.output == 7
    assert records[0]["graph"] == "personal"


def test_ping_and_metrics_endpoints():
    app = create_app(registry=make_registry())
    with TestClient(app) as client:
        assert client.get("/ping").json() == {"status": "ok"}
        assert "jabba_requests_total" in client.get("/metrics").text


def test_lifespan_setup_shutdown_deduplicated():
    calls = []

    class Tracked(Node):
        def __init__(self, name):
            self.name = name
            super().__init__()

        async def run(self) -> int:
            return 1

        async def setup(self, resources) -> None:
            calls.append(("setup", self.name, resources["k"]))

        async def shutdown(self) -> None:
            calls.append(("shutdown", self.name))

    shared = Tracked("shared")
    solo = Tracked("solo")
    registry = GraphRegistry(default=GraphSpec("base").add(shared).compile())
    registry.register("svc", GraphSpec("personal").add(shared, solo).compile())

    app = create_app(registry=registry, resources={"k": "v"})
    with TestClient(app):
        pass

    setups = [c for c in calls if c[0] == "setup"]
    shutdowns = [c for c in calls if c[0] == "shutdown"]
    assert len(setups) == 2  # shared node set up once despite being in two graphs
    assert {c[1] for c in setups} == {"shared", "solo"}
    assert len(shutdowns) == 2
    assert [c[1] for c in shutdowns] == [c[1] for c in reversed(setups)]
