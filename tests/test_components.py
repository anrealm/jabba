import pytest
from jabba.components.hello_world import HelloWorldComponent
from jabba.schemas.state import State

@pytest.mark.asyncio
async def test_hello_world_component():
    component = HelloWorldComponent()
    state = State()
    await component.call(state)
    assert state.data["message"] == "Hello, World!"
    assert "component_times" in state.data
    assert "hello_world" in state.data["component_times"]
    assert isinstance(state.data["component_times"]["hello_world"], float)
    assert state.data["component_times"]["hello_world"] >= 0
