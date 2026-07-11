from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, Response
from prometheus_client import CONTENT_TYPE_LATEST, generate_latest

from jabba.core.node import Node
from jabba.obs.request_record import RecordSink
from jabba.service.registry import GraphRegistry
from jabba.service.runner import GraphRunner


def _unique_nodes(registry: GraphRegistry) -> list[Node]:
    seen: set[int] = set()
    ordered: list[Node] = []
    for graph in registry.all_graphs():
        for node in graph.nodes.values():
            if id(node) not in seen:
                seen.add(id(node))
                ordered.append(node)
    return ordered


def create_app(
    *,
    registry: GraphRegistry,
    resources: Any = None,
    record_sink: RecordSink | None = None,
    title: str = "jabba",
) -> FastAPI:
    """Service wiring: /ping, /metrics, node lifecycle, a ready-to-use runner.

    FastAPI is not hidden: add your own routes to the returned app and call
    ``app.state.runner.run(key, request)`` from them.
    """

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        nodes = _unique_nodes(registry)
        for node in nodes:
            await node.setup(resources)
        yield
        for node in reversed(nodes):
            await node.shutdown()

    app = FastAPI(title=title, lifespan=lifespan)
    app.state.registry = registry
    app.state.resources = resources
    app.state.runner = GraphRunner(registry, record_sink=record_sink)

    @app.get("/ping")
    async def ping() -> dict[str, str]:
        return {"status": "ok"}

    @app.get("/metrics")
    async def metrics() -> Response:
        return Response(content=generate_latest(), media_type=CONTENT_TYPE_LATEST)

    return app
