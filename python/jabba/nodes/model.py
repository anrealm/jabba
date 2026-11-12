import inspect
from collections.abc import Callable
from typing import Any, Protocol, runtime_checkable

from jabba.core.node import UNSET, Node


@runtime_checkable
class SupportsPredict(Protocol):
    """The model interface jabba understands: anything with ``predict(features)``.

    Model providers (registries, formula engines, plain pickles) are injected
    by the consumer — jabba has no dependency on any of them.
    """

    def predict(self, features: Any) -> Any: ...


#: Loads a model once at app startup. May be sync or async.
ModelLoader = Callable[[Any], Any]


class ModelApply(Node):
    """Contract node: load a model at startup, apply it to features per request.

    Two ways to use it:

    - inject a loader — no subclassing; ``apply`` defaults to duck-typed
      ``model.predict(features)`` (see SupportsPredict)::

          ModelApply(name="score", features_from="user_features",
                     loader=lambda resources: my_registry.load_model("demo", version=3))

    ``features_from`` names the graph node whose result is fed to the model
    (it becomes this node's dependency), in whatever format that node returns.

    - subclass and override ``load()`` / ``apply()`` for custom logic.
    """

    def __init__(
        self,
        *,
        name: str = "model_apply",
        features_from: str = "features",
        loader: ModelLoader | None = None,
        optional: bool = False,
        default: Any = UNSET,
        timeout: float | None = None,
    ) -> None:
        self.name = name
        self.deps = (features_from,)
        self.optional = optional
        self.default = default
        self.timeout = timeout
        self._features_key = features_from
        self._loader = loader
        self._model: Any = None
        super().__init__()

    async def load(self, resources: Any) -> Any:
        """Return the ready-to-use model object. Called once at startup."""
        if self._loader is None:
            raise NotImplementedError(f"node '{self.name}': pass loader=... or override load()")
        model = self._loader(resources)
        if inspect.isawaitable(model):
            model = await model
        return model

    def apply(self, model: Any, features: Any) -> Any:
        """Apply the loaded model to one request's features. Default: model.predict()."""
        return model.predict(features)

    async def setup(self, resources: Any) -> None:
        self._model = await self.load(resources)

    async def run(self, **inputs: Any) -> Any:
        return self.apply(self._model, inputs[self._features_key])
