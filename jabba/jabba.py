from fastapi import FastAPI
import uvicorn
import asyncio
from .components.base_component import BaseComponent
from .schemas.state import State
from .graphs import Graph

class Jabba:
    def __init__(self):
        self.app = FastAPI()
        self.graph = Graph()

        # Add ping endpoint
        @self.app.get("/ping")
        async def ping():
            return {"status": "pong"}

    def add_component(self, component: BaseComponent):
        """Add a component to the graph."""
        self.graph.add_component(component)

    def add_dependency(self, component_name: str, depends_on: str):
        """Add a dependency between components."""
        self.graph.add_dependency(component_name, depends_on)

    async def execute_components(self, state: State):
        """Execute components immediately when their dependencies are ready."""
        tasks = set()
        
        while not self.graph.is_completed():
            # Get components that are ready to execute
            ready_components = self.graph.get_ready_components()
            
            if not ready_components and not tasks:
                raise ValueError("Deadlock detected - no components ready to execute")
            
            # Start execution of ready components
            for component in ready_components:
                self.graph.mark_running(component.name)
                task = asyncio.create_task(self._execute_component(component, state))
                tasks.add(task)
            
            # Wait for at least one task to complete
            if tasks:
                done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                tasks = pending
                
                # Process completed tasks
                for task in done:
                    try:
                        await task
                    except Exception as e:
                        # Cancel all pending tasks if one fails
                        for t in tasks:
                            t.cancel()
                        raise e

    async def _execute_component(self, component: BaseComponent, state: State):
        """Execute a single component and mark it as completed."""
        await component.call(state)
        self.graph.mark_completed(component.name)

    def run(self):
        uvicorn.run(self.app, host="0.0.0.0", port=8000)
