import uuid
from collections.abc import Hashable
from typing import Any

from loguru import logger

from jabba.core.context import GraphResult
from jabba.core.errors import GraphTimeoutError, NodeFailedError
from jabba.obs.metrics import observe_graph
from jabba.obs.request_record import RecordSink, RequestRecordEmitter


class GraphRunner:
    """Runs graphs with observability: metrics, request record, final log line.

    HTTP-agnostic on purpose — the same runner serves FastAPI routes, CLIs
    and tests alike.
    """

    def __init__(self, registry: Any, *, record_sink: RecordSink | None = None) -> None:
        self._registry = registry
        self._emitter = RequestRecordEmitter(sink=record_sink)

    async def run(
        self,
        key: Hashable | None,
        request: Any,
        *,
        timeout: float | None = None,
        request_id: str | None = None,
    ) -> GraphResult:
        graph = self._registry.resolve(key)
        request_id = request_id or uuid.uuid4().hex
        with logger.contextualize(request_id=request_id):
            try:
                result = await graph.run(request, timeout=timeout, request_id=request_id)
            except (NodeFailedError, GraphTimeoutError) as error:
                self._finish(error.stat)
                logger.error("graph '{}' aborted: {}", graph.name, error)
                raise
            self._finish(result.stat)
            logger.info("graph '{}' ok in {:.1f}ms", graph.name, result.stat.duration_ms or 0.0)
            return result

    def _finish(self, stat: Any) -> None:
        observe_graph(stat)
        self._emitter.emit(stat)
