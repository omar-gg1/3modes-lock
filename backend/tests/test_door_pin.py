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


def test_encrypt_round_trips_but_hides_plaintext():
    enc = security.encrypt_door_pin("4242")
    assert enc != "4242"                       # not stored in the clear
    assert "4242" not in enc
    assert security.decrypt_door_pin(enc) == "4242"


def test_set_then_reveal_returns_the_pin(published, monkeypatch):
    init_db()
    import app.main as m

    async def _ok(fut, timeout):
        return {"result": "ok", "detail": "updated"}
    monkeypatch.setattr(m.asyncio, "wait_for", _ok)

    client.post("/devices/lock-02/door_pin",
                json={"pin": "13579"}, headers=_auth())
    r = client.get("/devices/lock-02/door_pin", headers=_auth())
    assert r.status_code == 200
    assert r.json()["pin"] == "13579"


def test_reveal_requires_auth():
    r = client.get("/devices/lock-01/door_pin")
    assert r.status_code == 401


def test_reveal_unset_device_is_null(published, monkeypatch):
    init_db()
    r = client.get("/devices/never-set/door_pin", headers=_auth())
    assert r.status_code == 200
    assert r.json()["pin"] is None


def test_denied_command_is_not_stored(published, monkeypatch):
    init_db()
    import app.main as m

    async def _denied(fut, timeout):
        return {"result": "denied", "detail": "bad_pin"}
    monkeypatch.setattr(m.asyncio, "wait_for", _denied)

    client.post("/devices/lock-03/door_pin",
                json={"pin": "99999"}, headers=_auth())
    r = client.get("/devices/lock-03/door_pin", headers=_auth())
    assert r.json()["pin"] is None          # rejected code is never recorded
