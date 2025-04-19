from ..base_component import BaseComponent
from ...schemas.state import State

class HelloWorldComponent(BaseComponent):
    def __init__(self):
        super().__init__("hello_world")

    async def call(self, state: State) -> None:
        state.data["message"] = "Hello, World!" 
