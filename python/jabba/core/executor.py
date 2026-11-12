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

#: How long a deadline/failure waits for cancelled nodes before raising anyway.
#: A node that ignores cancellation (or cleans up slowly) must not hold the error.
CANCEL_GRACE_S = 0.05


async def run_graph(
    graph: "CompiledGraph",
    request: Any = None,
    *,
    timeout: float | None = None,
    request_id: str | None = None,
) -> GraphResult:
    """Execute a compiled graph for one request.

    Event-driven: a node starts as soon as its dependency counter hits zero.
    Each loop turn is one tick (spec/semantics.md §3): completions are recorded
    first, then expired per-node timeouts, then the graph deadline. Only the
    executor writes node statuses, so a cancelled node cannot rewrite its record.
    """
    request_id = request_id or uuid.uuid4().hex
    stat = RequestStat(request_id=request_id, graph=graph.name)
    ctx = Ctx(request_id=request_id, stat=stat)
    loop = asyncio.get_running_loop()
    started = loop.time()
    graph_deadline = None if timeout is None else started + timeout

    results: dict[str, Any] = {}
    pending_deps = dict(graph.indegree)
    tasks: dict[asyncio.Task[Any], str] = {}
    node_deadlines: dict[asyncio.Task[Any], float] = {}
    node_started: dict[str, float] = {}
    abandoned: list[tuple[asyncio.Task[Any], str]] = []

    def start_ready() -> None:
        ready = [name for name, count in pending_deps.items() if count == 0]
        for name in ready:
            del pending_deps[name]
            node = graph.nodes[name]
            stat.nodes[name] = NodeStat(status="ok", started_at=time.time())
            node_started[name] = loop.time()
            task = asyncio.create_task(_call(node, results, request, ctx))
            tasks[task] = name
            if node.timeout is not None:
                node_deadlines[task] = node_started[name] + node.timeout

    def record(name: str, status: str, error: str | None = None, kind: str | None = None) -> None:
        node_stat = stat.nodes[name]
        node_stat.status = status  # type: ignore[assignment]
        node_stat.error = error
        node_stat.error_kind = kind  # type: ignore[assignment]
        node_stat.duration_ms = (loop.time() - node_started[name]) * 1000

    def provide(name: str, value: Any) -> None:
        results[name] = value
        for child in graph.children[name]:
            pending_deps[child] -= 1

    def abandon(task: asyncio.Task[Any]) -> None:
        """Stop tracking a running node: cancel it and let it finish in the background."""
        name = tasks.pop(task)
        node_deadlines.pop(task, None)
        task.cancel()
        task.add_done_callback(_consume)
        abandoned.append((task, name))

    async def stop_running() -> None:
        """Cancel every tracked node and wait for them at most CANCEL_GRACE_S."""
        running = list(tasks)
        for task in running:
            abandon(task)
        if running:
            await asyncio.wait(running, timeout=CANCEL_GRACE_S)

    def warn_still_running() -> None:
        names = sorted(name for task, name in abandoned if not task.done())
        if names:
            logger.warning(
                "nodes {} of graph '{}' did not stop on cancellation; left to finish in background",
                names,
                graph.name,
            )

    def finish(status: str) -> None:
        stat.status = status  # type: ignore[assignment]
        stat.duration_ms = (loop.time() - started) * 1000
        for name in pending_deps:
            stat.nodes[name] = NodeStat(status="skipped")
        warn_still_running()

    failure: tuple[str, BaseException] | None = None
    try:
        start_ready()
        while tasks:
            wake_at = min(node_deadlines.values(), default=None)
            if graph_deadline is not None:
                wake_at = graph_deadline if wake_at is None else min(wake_at, graph_deadline)
            wait_s = None if wake_at is None else max(0.0, wake_at - loop.time())
            done, _ = await asyncio.wait(tasks, timeout=wait_s, return_when=asyncio.FIRST_COMPLETED)

            # 1. Completions: a node recorded before the deadline keeps its status.
            for task in done:
                name = tasks.pop(task)
                node_deadlines.pop(task, None)
                error = _error_of(task)
                if error is None:
                    record(name, "ok")
                    provide(name, task.result())
                    continue
                record(name, "failed", f"{type(error).__name__}: {error}", "failed")
                if graph.nodes[name].optional:
                    provide(name, graph.nodes[name].default)
                elif failure is None:
                    failure = (name, error)
            if failure is not None:
                break

            # 2. Per-node timeouts: cancel and move on without waiting for the node.
            now = loop.time()
            for task, at in list(node_deadlines.items()):
                if at > now:
                    continue
                name = tasks[task]
                node = graph.nodes[name]
                abandon(task)
                record(name, "timeout", f"node timeout after {node.timeout}s", "timeout")
                if node.optional:
                    provide(name, node.default)
                elif failure is None:
                    failure = (name, TimeoutError(f"node '{name}' timed out after {node.timeout}s"))
            if failure is not None:
                break

            # 3. Graph deadline.
            if graph_deadline is not None and now >= graph_deadline:
                for name in tasks.values():
                    record(name, "timeout", f"graph deadline after {timeout}s", "deadline")
                await stop_running()
                finish("timeout")
                raise GraphTimeoutError(graph.name, stat)

            start_ready()
    except asyncio.CancelledError:
        # Cancelled from outside (client gone): nodes must not outlive it. No record.
        await stop_running()
        warn_still_running()
        raise

    if failure is not None:
        # Outside the loop: waiting for cancelled nodes cannot turn a failure into a timeout.
        for name in tasks.values():
            record(name, "skipped", "cancelled", "cancelled")
        await stop_running()
        finish("failed")
        raise NodeFailedError(failure[0], stat) from failure[1]

    finish("ok")
    return GraphResult(outputs=results, stat=stat, output_name=graph.output)


async def _call(node: Node, results: dict[str, Any], request: Any, ctx: Ctx) -> Any:
    kwargs: dict[str, Any] = {dep: results[dep] for dep in node.deps}
    if node.wants_request:
        kwargs["request"] = request
    if node.wants_ctx:
        kwargs["ctx"] = ctx
    return await node.run(**kwargs)


def _error_of(task: asyncio.Task[Any]) -> BaseException | None:
    if task.cancelled():  # the node raised CancelledError on its own
        return asyncio.CancelledError("node raised CancelledError")
    return task.exception()


def _consume(task: asyncio.Task[Any]) -> None:
    """Retrieve a background node's outcome so asyncio does not report it as unhandled."""
    if not task.cancelled():
        task.exception()
