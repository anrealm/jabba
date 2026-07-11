from jabba.core.node import Node, node


def test_function_node_deps_from_signature():
    @node()
    async def score(parsed: dict, user_features: dict, ctx) -> float:
        return 1.0

    assert score.name == "score"
    assert score.deps == ("parsed", "user_features")


def test_ctx_and_request_params_are_not_deps():
    @node()
    async def parse(request, ctx) -> dict:
        return {}

    assert parse.deps == ()
    assert parse.wants_request
    assert parse.wants_ctx


def test_function_node_name_override_and_flags():
    @node(name="fetch", optional=True, default=None, timeout=0.5)
    async def fetch_something() -> dict:
        return {}

    assert fetch_something.name == "fetch"
    assert fetch_something.optional
    assert fetch_something.has_default
    assert fetch_something.default is None
    assert fetch_something.timeout == 0.5


def test_node_without_declared_default_has_no_default():
    @node(optional=True)
    async def fetch() -> dict:
        return {}

    assert not fetch.has_default


def test_class_node_deps_from_run_signature():
    class Score(Node):
        name = "score"

        async def run(self, parsed: dict, ctx) -> float:
            return 1.0

    n = Score()
    assert n.name == "score"
    assert n.deps == ("parsed",)
    assert n.wants_ctx
    assert not n.wants_request


def test_class_node_default_name_is_snake_case_class_name():
    class FetchUserFeatures(Node):
        async def run(self) -> dict:
            return {}

    assert FetchUserFeatures().name == "fetch_user_features"


def test_ordering_only_deps_via_after():
    @node(after=["warmup"])
    async def compute() -> int:
        return 1

    assert compute.after == ("warmup",)
    assert compute.deps == ()
