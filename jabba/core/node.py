import inspect
import re
from collections.abc import Callable, Coroutine
from typing import Any

#: Parameter names with special meaning: never treated as node dependencies.
RESERVED_PARAMS = ("request", "ctx")

#: Sentinel distinguishing "default not declared" from "default is None".
UNSET = object()


def _snake_case(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


class Node:
    """A computable unit of a graph.

    Subclass and implement ``run()``. Dependencies are inferred from the
    ``run()`` signature: each parameter names the node whose result it receives.
    ``request`` and ``ctx`` are reserved and injected by the executor.
    """

    name: str | None = None
    optional: bool = False
    default: Any = UNSET
    timeout: float | None = None
    after: tuple[str, ...] = ()

    def __init__(self) -> None:
        if self.name is None:
            self.name = _snake_case(type(self).__name__)
        self.after = tuple(self.after)
        self._introspect(self._target())

    def _target(self) -> Callable[..., Coroutine[Any, Any, Any]]:
        return self.run

    def _introspect(self, fn: Callable[..., Any]) -> None:
        sig = inspect.signature(fn)
        deps: list[str] = []
        self.wants_request = False
        self.wants_ctx = False
        self.input_annotations: dict[str, Any] = {}
        for param in sig.parameters.values():
            if param.kind in (inspect.Parameter.VAR_KEYWORD, inspect.Parameter.VAR_POSITIONAL):
                continue  # generic nodes take **inputs and declare deps explicitly
            if param.name == "request":
                self.wants_request = True
            elif param.name == "ctx":
                self.wants_ctx = True
            else:
                deps.append(param.name)
                if param.annotation is not inspect.Parameter.empty:
                    self.input_annotations[param.name] = param.annotation
        explicit = getattr(self, "deps", None)
        self.deps: tuple[str, ...] = tuple(explicit) if explicit is not None else tuple(deps)
        self.output_annotation: Any = (
            None if sig.return_annotation is inspect.Signature.empty else sig.return_annotation
        )

    @property
    def has_default(self) -> bool:
        return self.default is not UNSET

    async def run(self, **kwargs: Any) -> Any:
        raise NotImplementedError

    async def setup(self, resources: Any) -> None:
        """Called once at application startup. Override if the node needs resources."""

    async def shutdown(self) -> None:
        """Called once at application shutdown, in reverse setup order."""


class FunctionNode(Node):
    """A node built from a plain async function via the ``@node()`` decorator."""

    def __init__(
        self,
        fn: Callable[..., Coroutine[Any, Any, Any]],
        *,
        name: str | None = None,
        optional: bool = False,
        default: Any = UNSET,
        timeout: float | None = None,
        after: tuple[str, ...] = (),
    ) -> None:
        self._fn = fn
        self.name = name or fn.__name__
        self.optional = optional
        self.default = default
        self.timeout = timeout
        self.after = tuple(after)
        self._introspect(fn)

    async def run(self, **kwargs: Any) -> Any:
        return await self._fn(**kwargs)


def node(
    name: str | None = None,
    *,
    optional: bool = False,
    default: Any = UNSET,
    timeout: float | None = None,
    after: tuple[str, ...] | list[str] = (),
) -> Callable[[Callable[..., Coroutine[Any, Any, Any]]], FunctionNode]:
    """Turn an async function into a graph node. Deps come from the signature."""

    def decorator(fn: Callable[..., Coroutine[Any, Any, Any]]) -> FunctionNode:
        return FunctionNode(
            fn, name=name, optional=optional, default=default, timeout=timeout, after=tuple(after)
        )

    return decorator
