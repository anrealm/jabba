import time
from dataclasses import dataclass, field
from typing import Any, Literal

#: Bump when the finalized record layout changes; consumers rely on this.
RECORD_VERSION = 1

NodeStatus = Literal["ok", "failed", "timeout", "skipped"]
GraphStatus = Literal["ok", "failed", "timeout"]


@dataclass
class NodeStat:
    status: NodeStatus
    started_at: float | None = None
    duration_ms: float | None = None
    error: str | None = None


@dataclass
class RequestStat:
    """Per-request statistics, enriched by the executor and by nodes via ctx.stat.set()."""

    request_id: str
    graph: str
    status: GraphStatus = "ok"
    duration_ms: float | None = None
    nodes: dict[str, NodeStat] = field(default_factory=dict)
    fields: dict[str, Any] = field(default_factory=dict)
    record_version: int = RECORD_VERSION

    def set(self, key: str, value: Any) -> None:
        self.fields[key] = value

    def to_record(self) -> dict[str, Any]:
        """Finalized structured record: one per request, schema-versioned."""
        return {
            "record_version": self.record_version,
            "request_id": self.request_id,
            "graph": self.graph,
            "status": self.status,
            "duration_ms": self.duration_ms,
            "nodes": {
                name: {
                    "status": ns.status,
                    "started_at": ns.started_at,
                    "duration_ms": ns.duration_ms,
                    "error": ns.error,
                }
                for name, ns in self.nodes.items()
            },
            "fields": self.fields,
        }


@dataclass
class Ctx:
    """Request-scoped context passed to nodes that declare a ``ctx`` parameter."""

    request_id: str
    stat: RequestStat
    started_at: float = field(default_factory=time.time)


@dataclass
class GraphResult:
    outputs: dict[str, Any]
    stat: RequestStat
    output_name: str | None = None

    @property
    def output(self) -> Any:
        if self.output_name is None:
            return None
        return self.outputs.get(self.output_name)
