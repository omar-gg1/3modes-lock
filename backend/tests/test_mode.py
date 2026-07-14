"""Runtime operating-mode endpoints: PUT/GET /devices/{id}/mode."""
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db, SessionLocal, DeviceMode

client = TestClient(app)


def _auth():
    return {"Authorization": f"Bearer {security.make_token('admin')}"}


def _clean():
    """Fresh DB state for mode rows (rows leak within the shared temp-file DB)."""
    init_db()
    db = SessionLocal()
    db.query(DeviceMode).delete()
    db.commit()
    db.close()


def _mock_ack(monkeypatch, result="ok", detail="mode_2"):
    import app.main as m

    async def _fake_wait_for(fut, timeout):
        return {"result": result, "detail": detail}
    monkeypatch.setattr(m.asyncio, "wait_for", _fake_wait_for)


def test_mode_requires_auth():
    assert client.get("/devices/lock-01/mode").status_code == 401


def test_get_defaults_to_mode_3():
    _clean()
    r = client.get("/devices/lock-01/mode", headers=_auth())
    assert r.status_code == 200
    assert r.json()["mode"] == 3


def test_put_persists_on_ack_ok(published, monkeypatch):
    _clean()
    _mock_ack(monkeypatch, "ok", "mode_2")
    r = client.put("/devices/lock-01/mode", json={"mode": 2}, headers=_auth())
    assert r.status_code == 200
    assert r.json()["mode"] == 2
    # published command carries the right type + args
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "set_mode"
    assert payload["args"] == {"mode": 2}
    # and it stuck
    assert client.get("/devices/lock-01/mode",
                      headers=_auth()).json()["mode"] == 2


def test_put_denied_ack_not_persisted(published, monkeypatch):
    _clean()
    _mock_ack(monkeypatch, "denied", "busy")
    client.put("/devices/lock-01/mode", json={"mode": 2}, headers=_auth())
    # denial leaves the stored mode at the default
    assert client.get("/devices/lock-01/mode",
                      headers=_auth()).json()["mode"] == 3


def test_put_rejects_out_of_range():
    _clean()
    r = client.put("/devices/lock-01/mode", json={"mode": 4}, headers=_auth())
    assert r.status_code == 422
