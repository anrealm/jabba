from typing import Any

from jabba.core.node import UNSET, Node


class ModelApply(Node):
    """Contract node: load a model at startup, apply it to features per request.

    Subclass and implement ``load()`` and ``apply()``. The ``features``
    argument names the node whose result is fed into the model — this is the
    integration point for external model libraries.
    """

    def __init__(
        self,
        *,
        name: str = "model_apply",
        features: str = "features",
        optional: bool = False,
        default: Any = UNSET,
        timeout: float | None = None,
    ) -> None:
        self.name = name
        self.deps = (features,)
        self.optional = optional
        self.default = default
        self.timeout = timeout
        self._features_key = features
        self._model: Any = None
        super().__init__()

    async def load(self, resources: Any) -> Any:
        """Return the ready-to-use model object. Called once at startup."""
        raise NotImplementedError

    def apply(self, model: Any, features: Any) -> Any:
        """Apply the loaded model to one request's features."""
        raise NotImplementedError

    async def setup(self, resources: Any) -> None:
        self._model = await self.load(resources)

    async def run(self, **inputs: Any) -> Any:
        return self.apply(self._model, inputs[self._features_key])
