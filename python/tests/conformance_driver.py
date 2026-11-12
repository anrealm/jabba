"""Runs the language-neutral conformance cases from spec/cases against the Python core."""

import asyncio
import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from jabba.core.errors import GraphCompileError, GraphTimeoutError, NodeFailedError
from jabba.core.graph import GraphSpec
from jabba.core.node import UNSET, Node

#: spec/ next to python/ in a checkout; JABBA_SPEC_DIR overrides it when vendored elsewhere.
SPEC_DIR = Path(os.environ.get("JABBA_SPEC_DIR") or Path(__file__).resolve().parents[2] / "spec")
LANGUAGE = "python"
CAPABILITIES = {"ignore_cancel"}


def load_case_files() -> dict[str, dict[str, Any]]:
    return {
        path.name: json.loads(path.read_text(encoding="utf-8"))
        for path in sorted((SPEC_DIR / "cases").glob("*.json"))
    }


def load_cases() -> list[dict[str, Any]]:
    cases = []
    for file_name, content in load_case_files().items():
        for case in content["cases"]:
            cases.append({**case, "_file": file_name})
    return cases


@dataclass
class Trace:
    """What scripted nodes did: start/end events and named signals."""

    events: list[str] = field(default_factory=list)
    signals: dict[str, asyncio.Event] = field(default_factory=dict)

    def signal(self, name: str) -> asyncio.Event:
        return self.signals.setdefault(name, asyncio.Event())


async def _sleep(seconds: float, *, ignore_cancel: bool) -> None:
    loop = asyncio.get_running_loop()
    until = loop.time() + seconds
    while (left := until - loop.time()) > 0:
        try:
            await asyncio.sleep(left)
        except asyncio.CancelledError:
            if not ignore_cancel:
                raise


class ScriptedNode(Node):
    """A node whose behavior is described by a conformance case."""

    def __init__(self, spec: dict[str, Any], trace: Trace) -> None:
        self.name = spec["name"]
        self.deps = tuple(spec.get("deps", ()))
        self.after = tuple(spec.get("after", ()))
        self.optional = spec.get("optional", False)
        self.default = spec.get("default", UNSET)  # a JSON null is a real default
        timeout_ms = spec.get("timeout_ms")
        self.timeout = None if timeout_ms is None else timeout_ms / 1000
        self._behavior = spec.get("behavior", {})
        self._trace = trace
        super().__init__()

    async def run(self, request, ctx, **inputs):
        b = self._behavior
        self._trace.events.append(f"start:{self.name}")
        if "signal" in b:
            self._trace.signal(b["signal"]).set()
        if "wait_for" in b:
            await self._trace.signal(b["wait_for"]).wait()
        if "sleep_ms" in b:
            await _sleep(b["sleep_ms"] / 1000, ignore_cancel=b.get("ignore_cancel", False))
        for key, value in b.get("set_field", {}).items():
            ctx.stat.set(key, value)
        self._trace.events.append(f"end:{self.name}")
        if "fail" in b:
            raise RuntimeError(b["fail"])
        if b.get("echo_inputs"):
            return dict(inputs)
        if b.get("echo_request"):
            return request
        return b.get("value")


@dataclass
class Outcome:
    status: str
    compile_error: str | None = None
    error_node: str | None = None
    output: Any = None
    record: dict[str, Any] | None = None
    events: list[str] = field(default_factory=list)
    duration_ms: float = 0.0


async def run_case(case: dict[str, Any]) -> Outcome:
    trace = Trace()
    graph_spec = case["graph"]
    try:
        spec = GraphSpec(case["name"], output=graph_spec.get("output"))
        spec.add(*(ScriptedNode(n, trace) for n in graph_spec["nodes"]))
        graph = spec.compile()
    except GraphCompileError as error:
        return Outcome(status="compile_error", compile_error=error.kind)

    deadline_ms = case.get("deadline_ms")
    loop = asyncio.get_running_loop()
    started = loop.time()
    run = asyncio.create_task(
        graph.run(case.get("request"), timeout=None if deadline_ms is None else deadline_ms / 1000)
    )
    if "cancel_after_ms" in case:
        await asyncio.sleep(case["cancel_after_ms"] / 1000)
        run.cancel()

    outcome = Outcome(status="ok", events=trace.events)
    stat = None
    try:
        result = await run
        outcome.output = result.output
        stat = result.stat
    except NodeFailedError as error:
        outcome.status, outcome.error_node, stat = "failed", error.node, error.stat
    except GraphTimeoutError as error:
        outcome.status, stat = "timeout", error.stat
    except asyncio.CancelledError:
        outcome.status = "cancelled"
    outcome.duration_ms = (loop.time() - started) * 1000

    await asyncio.sleep(case.get("settle_ms", 0) / 1000)
    if stat is not None:
        outcome.record = stat.to_record()
    return outcome


def check(case: dict[str, Any], outcome: Outcome) -> list[str]:
    """Compare an outcome with the case's expectations; returns human-readable mismatches."""
    expect = case["expect"]
    errors: list[str] = []

    def eq(what: str, actual: Any, expected: Any) -> None:
        if actual != expected:
            errors.append(f"{what}: expected {expected!r}, got {actual!r}")

    eq("status", outcome.status, expect["status"])
    for key in ("compile_error", "error_node", "output"):
        if key in expect:
            eq(key, getattr(outcome, key), expect[key])

    if expect["status"] in ("cancelled", "compile_error"):
        if outcome.record is not None:
            errors.append("record: expected none")
    elif outcome.record is None:
        errors.append("record: expected one, got none")
    else:
        for name, node_expect in expect.get("nodes", {}).items():
            actual = outcome.record["nodes"].get(name)
            if actual is None:
                errors.append(f"node '{name}': missing from the record")
                continue
            for key, value in node_expect.items():
                eq(f"node '{name}' {key}", actual[key], value)
        if "fields" in expect:
            eq("fields", outcome.record["fields"], expect["fields"])

    events = outcome.events
    for before, after in expect.get("order", []):
        end, start = f"end:{before}", f"start:{after}"
        if end not in events or start not in events or events.index(end) > events.index(start):
            errors.append(f"order: '{before}' must finish before '{after}' starts; events {events}")
    for name in expect.get("stopped", []):
        if f"end:{name}" in events:
            errors.append(f"stopped: node '{name}' ran to the end of its body")
    if "max_duration_ms" in expect and outcome.duration_ms > expect["max_duration_ms"]:
        errors.append(f"duration: {outcome.duration_ms:.0f}ms > max {expect['max_duration_ms']}ms")
    return errors
