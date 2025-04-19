import pytest
from fastapi.testclient import TestClient
from jabba.jabba import Jabba

jabba = Jabba()
client = TestClient(jabba.app)

def test_ping():
    response = client.get("/ping")
    assert response.status_code == 200
    assert response.json() == {"status": "pong"} 
