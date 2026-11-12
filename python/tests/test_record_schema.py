import asyncio
import json
import os
from pathlib import Path

import jsonschema
import pytest

from jabba.core.errors import GraphTimeoutError, NodeFailedError
from jabba.core.graph import GraphSpec
from jabba.core.node import node

SPEC_DIR = Path(os.environ.get("JABBA_SPEC_DIR") or Path(__file__).resolve().parents[2] / "spec")
SCHEMA = json.loads((SPEC_DIR / "record.schema.json").read_text())


def validate(record: dict) -> None:
    jsonschema.validate(record, SCHEMA)


async def test_ok_record_matches_schema():
    @node()
    async def a(ctx) -> int:
        ctx.stat.set("k", "v")
        return 1

    @node(optional=True, default=None)
    async def b(a: int) -> int:
        raise ValueError("x")

    result = await GraphSpec("g").add(a, b).compile().run(None)
    validate(result.stat.to_record())


async def test_failed_record_matches_schema():
    @node()
    async def bad() -> int:
        raise RuntimeError("boom")

    @node()
    async def child(bad: int) -> int:
        return bad

    with pytest.raises(NodeFailedError) as info:
        await GraphSpec("g").add(bad, child).compile().run(None)
    validate(info.value.stat.to_record())


async def test_timeout_record_matches_schema():
    @node()
    async def slow() -> int:
        await asyncio.sleep(1)
        return 1

    with pytest.raises(GraphTimeoutError) as info:
        await GraphSpec("g").add(slow).compile().run(None, timeout=0.05)
    validate(info.value.stat.to_record())
