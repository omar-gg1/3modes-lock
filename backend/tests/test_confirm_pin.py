"""Liveness-confirmation code endpoints — validation, publish contract, toggle."""
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db

client = TestClient(app)


def _auth():
    return {"Authorization": f"Bearer {security.make_token('admin')}"}


def test_confirm_pin_requires_auth():
    r = client.post("/devices/lock-01/confirm_pin", json={"pin": "1234"})
    assert r.status_code == 401


def test_confirm_pin_rejects_bad_pin():
    init_db()
    for bad in ["123", "123456789", "12ab", ""]:
        r = client.post("/devices/lock-01/confirm_pin",
                        json={"pin": bad}, headers=_auth())
        assert r.status_code == 422, bad


def test_confirm_pin_publishes_set_confirm_pin(published, monkeypatch):
    init_db()
    import app.main as m

    async def _ok(fut, timeout):
        return {"result": "ok", "detail": "updated"}
    monkeypatch.setattr(m.asyncio, "wait_for", _ok)

    r = client.post("/devices/lock-01/confirm_pin",
                    json={"pin": "82461"}, headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "set_confirm_pin"
    assert payload["args"] == {"pin": "82461"}


def test_set_then_reveal_returns_pin_and_enabled(published, monkeypatch):
    init_db()
    import app.main as m

    async def _ok(fut, timeout):
        return {"result": "ok", "detail": "updated"}
    monkeypatch.setattr(m.asyncio, "wait_for", _ok)

    client.post("/devices/lock-c2/confirm_pin",
                json={"pin": "24680"}, headers=_auth())
    r = client.get("/devices/lock-c2/confirm_pin", headers=_auth())
    assert r.status_code == 200
    body = r.json()
    assert body["pin"] == "24680"
    assert body["enabled"] is True          # firmware default


def test_enabled_toggle_publishes_and_persists(published, monkeypatch):
    init_db()
    import app.main as m

    async def _ok(fut, timeout):
        return {"result": "ok", "detail": "updated"}
    monkeypatch.setattr(m.asyncio, "wait_for", _ok)

    r = client.post("/devices/lock-c3/confirm_enabled",
                    json={"enabled": False}, headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert payload["type"] == "set_confirm_enabled"
    assert payload["args"] == {"enabled": False}

    r = client.get("/devices/lock-c3/confirm_pin", headers=_auth())
    assert r.json()["enabled"] is False     # flag round-trips


def test_reveal_requires_auth():
    r = client.get("/devices/lock-01/confirm_pin")
    assert r.status_code == 401


def test_reveal_unset_device_defaults(published, monkeypatch):
    init_db()
    r = client.get("/devices/never-set-c/confirm_pin", headers=_auth())
    assert r.status_code == 200
    body = r.json()
    assert body["pin"] is None
    assert body["enabled"] is True          # defaults to firmware default


def test_denied_command_is_not_stored(published, monkeypatch):
    init_db()
    import app.main as m

    async def _denied(fut, timeout):
        return {"result": "denied", "detail": "bad_pin"}
    monkeypatch.setattr(m.asyncio, "wait_for", _denied)

    client.post("/devices/lock-c4/confirm_pin",
                json={"pin": "55555"}, headers=_auth())
    r = client.get("/devices/lock-c4/confirm_pin", headers=_auth())
    assert r.json()["pin"] is None          # rejected code is never recorded
