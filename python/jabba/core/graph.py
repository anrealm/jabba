import inspect
from typing import TYPE_CHECKING, Any

from .errors import GraphCompileError
from .node import RESERVED_PARAMS, Node

if TYPE_CHECKING:
    from .context import GraphResult


class GraphSpec:
    """Mutable, build-time graph assembly. Compile into an immutable CompiledGraph."""

    def __init__(self, name: str, *, output: str | None = None) -> None:
        self.name = name
        self.output = output
        self._nodes: dict[str, Node] = {}

    def add(self, *nodes: Node) -> "GraphSpec":
        for n in nodes:
            if n.name in RESERVED_PARAMS:
                raise GraphCompileError(f"graph '{self.name}': node name '{n.name}' is reserved")
            if n.name in self._nodes:
                raise GraphCompileError(f"graph '{self.name}': duplicate node name '{n.name}'")
            self._nodes[n.name] = n
        return self

    def compile(self) -> "CompiledGraph":
        nodes = dict(self._nodes)
        self._check_edges(nodes)
        self._check_optional_defaults(nodes)
        self._check_annotations(nodes)
        order = self._topo_sort(nodes)
        return CompiledGraph(name=self.name, nodes=nodes, order=order, output=self.output)

    def _check_edges(self, nodes: dict[str, Node]) -> None:
        for n in nodes.values():
            for dep in (*n.deps, *n.after):
                if dep not in nodes:
                    kind = "after" if dep in n.after else "dependency"
                    raise GraphCompileError(
                        f"graph '{self.name}': node '{n.name}' has {kind} on unknown node '{dep}'"
                    )
        if self.output is not None and self.output not in nodes:
            raise GraphCompileError(
                f"graph '{self.name}': output points to unknown node '{self.output}'"
            )

    def _check_optional_defaults(self, nodes: dict[str, Node]) -> None:
        for n in nodes.values():
            for dep in n.deps:
                provider = nodes[dep]
                if provider.optional and not provider.has_default:
                    raise GraphCompileError(
                        f"graph '{self.name}': optional node '{provider.name}' has data "
                        f"dependents but declares no default"
                    )

    def _check_annotations(self, nodes: dict[str, Node]) -> None:
        for n in nodes.values():
            for dep in n.deps:
                provided = nodes[dep].output_annotation
                expected = n.input_annotations.get(dep)
                if not (inspect.isclass(provided) and inspect.isclass(expected)):
                    continue  # best-effort: only plain classes are checked
                if not issubclass(provided, expected):
                    raise GraphCompileError(
                        f"graph '{self.name}': node '{n.name}' expects '{dep}' as "
                        f"{expected.__name__}, but node '{dep}' returns {provided.__name__} "
                        f"— incompatible"
                    )

    def _topo_sort(self, nodes: dict[str, Node]) -> tuple[str, ...]:
        indegree = {name: len(n.deps) + len(n.after) for name, n in nodes.items()}
        children: dict[str, list[str]] = {name: [] for name in nodes}
        for n in nodes.values():
            for dep in (*n.deps, *n.after):
                children[dep].append(n.name)
        ready = sorted(name for name, d in indegree.items() if d == 0)
        order: list[str] = []
        while ready:
            name = ready.pop()
            order.append(name)
            for child in children[name]:
                indegree[child] -= 1
                if indegree[child] == 0:
                    ready.append(child)
        if len(order) != len(nodes):
            stuck = sorted(name for name, d in indegree.items() if d > 0)
            raise GraphCompileError(f"graph '{self.name}': cycle involving nodes {stuck}")
        return tuple(order)


class CompiledGraph:
    """Immutable, validated graph ready for per-request execution."""

    def __init__(
        self,
        *,
        name: str,
        nodes: dict[str, Node],
        order: tuple[str, ...],
        output: str | None,
    ) -> None:
        self.name = name
        self.nodes = nodes
        self.order = order
        self.output = output
        # Precomputed for the executor: who to notify on completion, initial dep counts.
        self.children: dict[str, tuple[str, ...]] = {n: () for n in nodes}
        acc: dict[str, list[str]] = {n: [] for n in nodes}
        for n in nodes.values():
            for dep in (*n.deps, *n.after):
                acc[dep].append(n.name)
        self.children = {k: tuple(v) for k, v in acc.items()}
        self.indegree: dict[str, int] = {
            name: len(n.deps) + len(n.after) for name, n in nodes.items()
        }

    @property
    def node_names(self) -> tuple[str, ...]:
        return tuple(self.nodes)

    async def run(
        self,
        request: Any = None,
        *,
        timeout: float | None = None,
        request_id: str | None = None,
    ) -> "GraphResult":
        from .executor import run_graph

        return await run_graph(self, request, timeout=timeout, request_id=request_id)
