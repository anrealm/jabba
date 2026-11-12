import pytest

from jabba.core.errors import GraphCompileError
from jabba.core.graph import GraphSpec
from jabba.core.node import node


def make(name, deps=(), **kwargs):
    """Build a simple node named `name` depending on `deps` (data deps)."""
    args = ", ".join(deps)
    namespace = {}
    exec(f"async def {name}({args}): return None", namespace)  # noqa: S102
    return node(**kwargs)(namespace[name])


def test_compile_ok_and_immutable_spec_reuse():
    a = make("a")
    b = make("b", deps=("a",))
    graph = GraphSpec("g").add(a, b).compile()
    assert graph.name == "g"
    assert set(graph.node_names) == {"a", "b"}


def test_missing_dependency_fails():
    b = make("b", deps=("a",))
    with pytest.raises(GraphCompileError, match="unknown node 'a'"):
        GraphSpec("g").add(b).compile()


def test_cycle_fails():
    a = make("a", deps=("b",))
    b = make("b", deps=("a",))
    with pytest.raises(GraphCompileError, match="cycle"):
        GraphSpec("g").add(a, b).compile()


def test_duplicate_node_name_fails():
    a1 = make("a")
    a2 = make("a")
    with pytest.raises(GraphCompileError, match="duplicate"):
        GraphSpec("g").add(a1, a2)


def test_reserved_node_name_fails():
    bad = make("ctx")
    with pytest.raises(GraphCompileError, match="reserved"):
        GraphSpec("g").add(bad)


def test_optional_provider_with_dependent_requires_default():
    a = make("a", optional=True)  # no default declared
    b = make("b", deps=("a",))
    with pytest.raises(GraphCompileError, match="default"):
        GraphSpec("g").add(a, b).compile()


def test_optional_provider_with_default_is_ok():
    a = make("a", optional=True, default=None)
    b = make("b", deps=("a",))
    GraphSpec("g").add(a, b).compile()


def test_optional_leaf_without_default_is_ok():
    a = make("a", optional=True)  # nobody depends on it
    GraphSpec("g").add(a).compile()


def test_incompatible_annotations_fail():
    @node()
    async def a() -> int:
        return 1

    @node()
    async def b(a: str):
        return a

    with pytest.raises(GraphCompileError, match="incompatible"):
        GraphSpec("g").add(a, b).compile()


def test_compatible_and_missing_annotations_ok():
    @node()
    async def a() -> bool:  # bool is subclass of int
        return True

    @node()
    async def b(a: int):
        return a

    @node()
    async def c(b):  # no annotation — skipped
        return b

    GraphSpec("g").add(a, b, c).compile()


def test_after_unknown_node_fails():
    a = make("a", after=["nope"])
    with pytest.raises(GraphCompileError, match="unknown node 'nope'"):
        GraphSpec("g").add(a).compile()


def test_after_participates_in_cycle_detection():
    a = make("a", after=["b"])
    b = make("b", deps=("a",))
    with pytest.raises(GraphCompileError, match="cycle"):
        GraphSpec("g").add(a, b).compile()


@pytest.mark.parametrize(
    ("build", "kind"),
    [
        (lambda: GraphSpec("g").add(make("request")), "reserved_name"),
        (lambda: GraphSpec("g").add(make("a"), make("a")), "duplicate_name"),
        (lambda: GraphSpec("g").add(make("b", deps=("a",))).compile(), "unknown_dep"),
        (lambda: GraphSpec("g").add(make("b", after=("a",))).compile(), "unknown_after"),
        (lambda: GraphSpec("g", output="x").add(make("a")).compile(), "unknown_output"),
        (
            lambda: GraphSpec("g").add(make("a", optional=True), make("b", deps=("a",))).compile(),
            "optional_without_default",
        ),
        (
            lambda: GraphSpec("g").add(make("a", deps=("b",)), make("b", deps=("a",))).compile(),
            "cycle",
        ),
    ],
)
def test_compile_error_kind(build, kind):
    with pytest.raises(GraphCompileError) as info:
        build()
    assert info.value.kind == kind


def test_optional_output_without_default_fails():
    a = make("a", optional=True)
    with pytest.raises(GraphCompileError, match="optional output node 'a'") as info:
        GraphSpec("g", output="a").add(a).compile()
    assert info.value.kind == "optional_output_without_default"


def test_optional_output_with_default_compiles():
    a = make("a", optional=True, default=None)
    GraphSpec("g", output="a").add(a).compile()
