from pydantic import BaseModel

from jabba.core.node import Node


class PydanticParse(Node):
    """Parses and validates the incoming request into a pydantic model."""

    def __init__(self, model: type[BaseModel], *, name: str = "parse") -> None:
        self.name = name
        self._model = model
        super().__init__()

    async def run(self, request):
        return self._model.model_validate(request)
