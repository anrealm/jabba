from typing import Dict, List, Set
from ..components.base_component import BaseComponent

class Graph:
    def __init__(self):
        self.components: Dict[str, BaseComponent] = {}
        self.dependencies: Dict[str, Set[str]] = {}
        self.reverse_dependencies: Dict[str, Set[str]] = {}
        self.completed_components: Set[str] = set()
        self.running_components: Set[str] = set()

    def add_component(self, component: BaseComponent):
        """Add a component to the graph."""
        if component.name in self.components:
            raise ValueError(f"Component {component.name} already exists")
        
        self.components[component.name] = component
        self.dependencies[component.name] = set()
        self.reverse_dependencies[component.name] = set()

    def add_dependency(self, component_name: str, depends_on: str):
        """Add a dependency between components."""
        if component_name not in self.components or depends_on not in self.components:
            raise ValueError("One or both components do not exist")
        
        self.dependencies[component_name].add(depends_on)
        self.reverse_dependencies[depends_on].add(component_name)

    def mark_completed(self, component_name: str):
        """Mark a component as completed."""
        self.completed_components.add(component_name)
        self.running_components.remove(component_name)

    def mark_running(self, component_name: str):
        """Mark a component as running."""
        self.running_components.add(component_name)

    def get_ready_components(self) -> List[BaseComponent]:
        """Get components that are ready to execute (all dependencies completed and not running)."""
        ready = []
        for name, deps in self.dependencies.items():
            if (name not in self.completed_components and 
                name not in self.running_components and 
                all(dep in self.completed_components for dep in deps)):
                ready.append(self.components[name])
        return ready

    def is_completed(self) -> bool:
        """Check if all components have been completed."""
        return len(self.completed_components) == len(self.components)

    def get_execution_order(self) -> List[BaseComponent]:
        """Get components in execution order using topological sort."""
        visited: Set[str] = set()
        temp: Set[str] = set()
        order: List[str] = []

        def visit(component_name: str):
            if component_name in temp:
                raise ValueError("Cycle detected in component dependencies")
            if component_name in visited:
                return
            
            temp.add(component_name)
            for dep in self.dependencies[component_name]:
                visit(dep)
            
            temp.remove(component_name)
            visited.add(component_name)
            order.append(component_name)

        for component_name in self.components:
            if component_name not in visited:
                visit(component_name)

        return [self.components[name] for name in reversed(order)] 