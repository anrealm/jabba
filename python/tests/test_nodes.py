import pytest
from pydantic import BaseModel

from jabba.core.graph import GraphSpec
from jabba.core.node import Node, node
from jabba.nodes import ModelApply, PydanticParse


class ScoringRequest(BaseModel):
    user_id: str
    amount: float


def test_explicit_deps_override_signature_introspection():
    class Generic(Node):
        name = "generic"

        def __init__(self, source: str):
            self.deps = (source,)
            super().__init__()

        async def run(self, **inputs):
            return inputs

    n = Generic("user_features")
    assert n.deps == ("user_features",)  # not introspected from **inputs


async def test_explicit_deps_receive_data_in_graph():
    class Doubler(Node):
        def __init__(self, source: str, name: str):
            self.name = name
            self.deps = (source,)
            self._source = source
            super().__init__()

        async def run(self, **inputs):
            return inputs[self._source] * 2

    @node()
    async def base() -> int:
        return 21

    graph = GraphSpec("g", output="doubled").add(base, Doubler("base", "doubled")).compile()
    result = await graph.run(None)
    assert result.output == 42


async def test_pydantic_parse_validates_request():
    parse = PydanticParse(ScoringRequest, name="parsed")

    @node()
    async def consumer(parsed: ScoringRequest) -> str:
        return parsed.user_id

    graph = GraphSpec("g", output="consumer").add(parse, consumer).compile()
    result = await graph.run({"user_id": "u1", "amount": 10.5})
    assert result.outputs["parsed"].amount == 10.5
    assert result.output == "u1"


async def test_pydantic_parse_rejects_bad_request():
    from jabba.core.errors import NodeFailedError

    parse = PydanticParse(ScoringRequest, name="parsed")
    graph = GraphSpec("g").add(parse).compile()
    with pytest.raises(NodeFailedError):
        await graph.run({"user_id": "u1"})  # amount missing


async def test_model_apply_loads_on_setup_and_applies():
    class WeightedSum(ModelApply):
        async def load(self, resources):
            return resources["weights"]

        def apply(self, model, features):
            return sum(model[k] * v for k, v in features.items())

    apply_node = WeightedSum(name="score", features_from="feats")

    @node(name="feats")
    async def feats() -> dict:
        return {"a": 2.0, "b": 3.0}

    await apply_node.setup({"weights": {"a": 10.0, "b": 1.0}})
    graph = GraphSpec("g", output="score").add(feats, apply_node).compile()
    result = await graph.run(None)
    assert result.output == 23.0


class FakeModel:
    """Duck-typed SupportsPredict: what a consumer would inject."""

    def __init__(self, factor: float):
        self.factor = factor

    def predict(self, features):
        return {k: v * self.factor for k, v in features.items()}


async def test_model_apply_with_injected_sync_loader():
    apply_node = ModelApply(
        name="score", features_from="feats", loader=lambda resources: FakeModel(resources["factor"])
    )

    @node(name="feats")
    async def feats() -> dict:
        return {"a": 2.0}

    await apply_node.setup({"factor": 10.0})
    graph = GraphSpec("g", output="score").add(feats, apply_node).compile()
    result = await graph.run(None)
    assert result.output == {"a": 20.0}


async def test_model_apply_with_injected_async_loader():
    async def loader(resources):
        return FakeModel(resources["factor"])

    apply_node = ModelApply(name="score", features_from="feats", loader=loader)

    @node(name="feats")
    async def feats() -> dict:
        return {"a": 3.0}

    await apply_node.setup({"factor": 2.0})
    graph = GraphSpec("g", output="score").add(feats, apply_node).compile()
    result = await graph.run(None)
    assert result.output == {"a": 6.0}


async def test_model_apply_without_loader_or_override_fails_clearly():
    apply_node = ModelApply(name="score", features_from="feats")
    with pytest.raises(NotImplementedError, match="loader"):
        await apply_node.setup({})
