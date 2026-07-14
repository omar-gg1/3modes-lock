"""Temporary guest PIN endpoints — validation + publish contract."""
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db

client = TestClient(app)


def _auth():
    return {"Authorization": f"Bearer {security.make_token('admin')}"}


def test_temp_pin_requires_auth():
    r = client.post("/devices/lock-01/temp_pin", json={"pin": "1234", "ttl_s": 60})
    assert r.status_code == 401


def test_temp_pin_rejects_bad_pin():
    init_db()
    for bad in ["123", "1234567", "12ab", ""]:
        r = client.post("/devices/lock-01/temp_pin",
                        json={"pin": bad, "ttl_s": 60}, headers=_auth())
        assert r.status_code == 422, bad


def test_temp_pin_rejects_bad_ttl():
    init_db()
    for bad in [0, -5, 90000]:
        r = client.post("/devices/lock-01/temp_pin",
                        json={"pin": "1234", "ttl_s": bad}, headers=_auth())
        assert r.status_code == 422, bad


def test_temp_pin_publishes_set_temp_pin(published, monkeypatch):
    init_db()
    import app.main as m

    async def _fake_wait_for(fut, timeout):
        return {"result": "ok", "detail": "armed"}
    monkeypatch.setattr(m.asyncio, "wait_for", _fake_wait_for)

    r = client.post("/devices/lock-01/temp_pin",
                    json={"pin": "4729", "ttl_s": 1800}, headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "set_temp_pin"
    assert payload["args"] == {"pin": "4729", "ttl_s": 1800}


def test_clear_temp_pin_publishes_empty(published, monkeypatch):
    init_db()
    import app.main as m

    async def _fake_wait_for(fut, timeout):
        return {"result": "ok", "detail": "cleared"}
    monkeypatch.setattr(m.asyncio, "wait_for", _fake_wait_for)

    r = client.delete("/devices/lock-01/temp_pin", headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert payload["type"] == "set_temp_pin"
    assert payload["args"] == {"pin": "", "ttl_s": 0}
