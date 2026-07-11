from collections.abc import Hashable, Iterator

from jabba.core.graph import CompiledGraph


class GraphRegistry:
    """Resolves which compiled graph serves a request.

    Not inheritance: a personal graph is a full graph of its own, assembled
    from reusable nodes. Resolution: personal graph for the key if registered,
    otherwise the default one.
    """

    def __init__(self, default: CompiledGraph) -> None:
        self.default = default
        self._personal: dict[Hashable, CompiledGraph] = {}

    def register(self, key: Hashable, graph: CompiledGraph) -> None:
        if key in self._personal:
            raise ValueError(f"graph for key '{key}' is already registered")
        self._personal[key] = graph

    def resolve(self, key: Hashable | None = None) -> CompiledGraph:
        if key is None:
            return self.default
        return self._personal.get(key, self.default)

    def all_graphs(self) -> Iterator[CompiledGraph]:
        seen: set[int] = set()
        for graph in (self.default, *self._personal.values()):
            if id(graph) not in seen:
                seen.add(id(graph))
                yield graph
