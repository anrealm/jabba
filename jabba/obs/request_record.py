import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

from jabba.core.context import RequestStat

RecordSink = Callable[[dict[str, Any]], None]


class RequestRecordEmitter:
    """Structured channel: exactly one finalized JSON record per graph run.

    Deliberately not a logger — the record is a schema-versioned event
    (see RequestStat.to_record) meant for training datasets and data marts.
    """

    def __init__(self, sink: RecordSink | None = None) -> None:
        self._sink = sink

    def emit(self, stat: RequestStat) -> None:
        if self._sink is not None:
            self._sink(stat.to_record())


def jsonl_sink(path: str | Path) -> RecordSink:
    """Append each record as one JSON line to a file."""
    target = Path(path)

    def write(record: dict[str, Any]) -> None:
        with target.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    return write
