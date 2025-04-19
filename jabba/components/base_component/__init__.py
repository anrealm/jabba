import time
from typing import Dict, Any
from functools import wraps

class ComponentMeta(type):
    def __new__(cls, name, bases, namespace):
        if "call" in namespace:
            original_call = namespace["call"]
            
            @wraps(original_call)
            async def wrapped_call(self, state):
                start_time = time.time()
                try:
                    await original_call(self, state)
                finally:
                    execution_time = time.time() - start_time
                    if "component_times" not in state.data:
                        state.data["component_times"] = {}
                    state.data["component_times"][self.name] = execution_time
            
            namespace["call"] = wrapped_call
        
        return super().__new__(cls, name, bases, namespace)

class BaseComponent(metaclass=ComponentMeta):
    def __init__(self, name: str, dependencies: list["BaseComponent"] = []):
        self.name = name
        self.dependencies = dependencies

    def add_dependency(self, component: "BaseComponent"):
        self.dependencies.append(component)

    async def call(self, state):
        pass
