from fastapi.testclient import TestClient
from app.main import app

client = TestClient(app)


def test_login_success_returns_token():
    r = client.post("/auth/login",
                    json={"username": "admin", "password": "adminpass"})
    assert r.status_code == 200
    assert r.json()["token_type"] == "bearer"
    assert r.json()["access_token"]


def test_login_bad_password_is_401():
    r = client.post("/auth/login",
                    json={"username": "admin", "password": "wrong"})
    assert r.status_code == 401


def test_protected_route_requires_token():
    r = client.get("/devices/lock-01/status")
    assert r.status_code == 401


def test_protected_route_with_token_ok():
    tok = client.post("/auth/login",
                      json={"username": "admin", "password": "adminpass"}
                      ).json()["access_token"]
    r = client.get("/devices/lock-01/status",
                   headers={"Authorization": f"Bearer {tok}"})
    assert r.status_code == 200
    assert r.json()["state"] in ("connected", "reconnecting", "offline")
