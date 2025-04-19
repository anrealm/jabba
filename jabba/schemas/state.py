from typing import Any, Dict
from pydantic import BaseModel

class State(BaseModel):
    data: Dict[str, Any] = {}
    metadata: Dict[str, Any] = {}
