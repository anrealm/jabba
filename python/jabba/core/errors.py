class JabbaError(Exception):
    """Base class for all jabba errors."""


class GraphCompileError(JabbaError):
    """Graph specification is invalid: cycles, missing deps, contract violations."""


class NodeFailedError(JabbaError):
    """A non-optional node failed; the graph run was aborted (fail-fast)."""

    def __init__(self, node: str, stat: "RequestStat") -> None:  # noqa: F821
        super().__init__(f"node '{node}' failed")
        self.node = node
        self.stat = stat


class GraphTimeoutError(JabbaError):
    """The whole-graph deadline expired before execution finished."""

    def __init__(self, graph: str, stat: "RequestStat") -> None:  # noqa: F821
        super().__init__(f"graph '{graph}' timed out")
        self.graph = graph
        self.stat = stat
