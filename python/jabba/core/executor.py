import asyncio
import time
import uuid
from typing import TYPE_CHECKING, Any

from loguru import logger

from .context import Ctx, GraphResult, NodeStat, RequestStat
from .errors import GraphTimeoutError, NodeFailedError
from .node import Node

if TYPE_CHECKING:
    from .graph import CompiledGraph


async def run_graph(
    graph: "CompiledGraph",
    request: Any = None,
    *,
    timeout: float | None = None,
    request_id: str | None = None,
) -> GraphResult:
    """Execute a compiled graph for one request.

    Event-driven: a node starts as soon as its dependency counter hits zero;
    completion decrements the counters of its children. Parallelism is bounded
    only by the graph structure.
    """
    request_id = request_id or uuid.uuid4().hex
    stat = RequestStat(request_id=request_id, graph=graph.name)
    ctx = Ctx(request_id=request_id, stat=stat)
    started = time.monotonic()

    results: dict[str, Any] = {}
    pending_deps = dict(graph.indegree)
    tasks: dict[asyncio.Task[Any], str] = {}

    def start_ready() -> None:
        ready = [name for name, count in pending_deps.items() if count == 0]
        for name in ready:
            del pending_deps[name]
            task = asyncio.create_task(_run_node(graph.nodes[name], results, request, ctx))
            tasks[task] = name

    def finish(status: str) -> None:
        stat.status = status  # type: ignore[assignment]
        stat.duration_ms = (time.monotonic() - started) * 1000
        for name in pending_deps:
            stat.nodes[name] = NodeStat(status="skipped")

    failure: tuple[str, BaseException] | None = None
    try:
        async with asyncio.timeout(timeout):
            start_ready()
            while tasks and failure is None:
                done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    name = tasks.pop(task)
                    node = graph.nodes[name]
                    error = task.exception()
                    if error is None:
                        results[name] = task.result()
                    elif node.optional:
                        results[name] = node.default
                    else:
                        failure = (name, error)
                        break
                    for child in graph.children[name]:
                        pending_deps[child] -= 1
                if failure is None:
                    start_ready()
    except TimeoutError:
        # Only what was still running at the deadline: a node that finished in the
        # same tick keeps its own status.
        running = [name for task, name in tasks.items() if not task.done()]
        await _cancel(tasks)
        for name in running:
            node_stat = stat.nodes.setdefault(name, NodeStat(status="timeout"))
            node_stat.status = "timeout"
            node_stat.error = f"graph deadline after {timeout}s"
        finish("timeout")
        raise GraphTimeoutError(graph.name, stat) from None
    except BaseException:
        await _cancel(tasks)  # cancelled from outside (client gone): nodes must not outlive it
        raise

    if failure is not None:
        # Outside the deadline block: a slow cancel cannot turn a failure into a timeout.
        await _cancel(tasks)
        finish("failed")
        raise NodeFailedError(failure[0], stat) from failure[1]

    finish("ok")
    return GraphResult(outputs=results, stat=stat, output_name=graph.output)


#: How long a deadline/failure waits for cancelled nodes before raising anyway.
#: A node that ignores cancellation (or cleans up slowly) must not hold the error.
CANCEL_GRACE_S = 0.05


async def _cancel(tasks: dict[asyncio.Task[Any], str]) -> None:
    for task in tasks:
        task.cancel()
    if not tasks:
        return
    _, stuck = await asyncio.wait(tasks, timeout=CANCEL_GRACE_S)
    if stuck:
        logger.warning(
            "nodes {} did not stop within {}s of cancellation; left to finish in background",
            sorted(tasks[t] for t in stuck),
            CANCEL_GRACE_S,
        )


async def _run_node(node: Node, results: dict[str, Any], request: Any, ctx: Ctx) -> Any:
    """Run one node with timing, per-node timeout and stat bookkeeping."""
    kwargs: dict[str, Any] = {dep: results[dep] for dep in node.deps}
    if node.wants_request:
        kwargs["request"] = request
    if node.wants_ctx:
        kwargs["ctx"] = ctx

    node_stat = NodeStat(status="ok", started_at=time.time())
    ctx.stat.nodes[node.name] = node_stat
    started = time.monotonic()
    try:
        return await asyncio.wait_for(node.run(**kwargs), node.timeout)
    except TimeoutError:
        node_stat.status = "timeout"
        node_stat.error = f"node timeout after {node.timeout}s"
        raise
    except asyncio.CancelledError:
        node_stat.status = "skipped"
        node_stat.error = "cancelled"
        raise
    except Exception as error:
        node_stat.status = "failed"
        node_stat.error = f"{type(error).__name__}: {error}"
        raise
    finally:
        node_stat.duration_ms = (time.monotonic() - started) * 1000
