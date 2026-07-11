import pytest

from jabba.core.graph import GraphSpec
from jabba.core.node import node
from jabba.service.registry import GraphRegistry


def compiled(name):
    @node(name="only")
    async def only() -> int:
        return 1

    return GraphSpec(name).add(only).compile()


def test_resolve_falls_back_to_default():
    registry = GraphRegistry(default=compiled("base"))
    assert registry.resolve("unknown_service").name == "base"
    assert registry.resolve(None).name == "base"


def test_resolve_personal_graph_by_key():
    registry = GraphRegistry(default=compiled("base"))
    registry.register("service_x", compiled("custom"))
    assert registry.resolve("service_x").name == "custom"
    assert registry.resolve("service_y").name == "base"


def test_register_duplicate_key_fails():
    registry = GraphRegistry(default=compiled("base"))
    registry.register("service_x", compiled("custom"))
    with pytest.raises(ValueError, match="service_x"):
        registry.register("service_x", compiled("another"))


def test_all_graphs_iterates_default_and_personal():
    registry = GraphRegistry(default=compiled("base"))
    registry.register("service_x", compiled("custom"))
    names = sorted(g.name for g in registry.all_graphs())
    assert names == ["base", "custom"]
