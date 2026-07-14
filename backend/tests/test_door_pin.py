"""Door code endpoint — validation + publish contract."""
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db

client = TestClient(app)


def _auth():
    return {"Authorization": f"Bearer {security.make_token('admin')}"}


def test_door_pin_requires_auth():
    r = client.post("/devices/lock-01/door_pin", json={"pin": "1234"})
    assert r.status_code == 401


def test_door_pin_rejects_bad_pin():
    init_db()
    for bad in ["123", "123456789", "12ab", ""]:
        r = client.post("/devices/lock-01/door_pin",
                        json={"pin": bad}, headers=_auth())
        assert r.status_code == 422, bad


def test_door_pin_publishes_set_door_pin(published, monkeypatch):
    init_db()
    import app.main as m

    async def _fake_wait_for(fut, timeout):
        return {"result": "ok", "detail": "updated"}
    monkeypatch.setattr(m.asyncio, "wait_for", _fake_wait_for)

    r = client.post("/devices/lock-01/door_pin",
                    json={"pin": "86420"}, headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "set_door_pin"
    assert payload["args"] == {"pin": "86420"}
