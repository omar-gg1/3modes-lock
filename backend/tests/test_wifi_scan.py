"""WiFi QR provisioning endpoints — scan command dispatch + status read.

The credentials themselves never touch the backend (the lock reads them off the
phone screen); we only relay the 'start scanning' command and surface whatever
network the lock later reports over MQTT.
"""
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db, SessionLocal, WifiStatus

client = TestClient(app)


def _auth():
    return {"Authorization": f"Bearer {security.make_token('admin')}"}


def test_wifi_scan_requires_auth():
    r = client.post("/devices/lock-01/wifi_scan")
    assert r.status_code == 401


def test_wifi_scan_publishes_start_command(published, monkeypatch):
    init_db()
    import app.main as m

    async def _ok(fut, timeout):
        return {"result": "ok", "detail": "scanning"}
    monkeypatch.setattr(m.asyncio, "wait_for", _ok)

    r = client.post("/devices/lock-01/wifi_scan", headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "start_wifi_scan"
    assert payload["args"] == {}


def test_wifi_status_unset_defaults():
    init_db()
    r = client.get("/devices/never-wifi/wifi", headers=_auth())
    assert r.status_code == 200
    body = r.json()
    assert body["ssid"] is None
    assert body["connected"] is False


def test_wifi_status_reads_stored_row():
    init_db()
    s = SessionLocal()
    try:
        s.merge(WifiStatus(device_id="lock-w2", ssid="SPACEDOME", connected=True))
        s.commit()
    finally:
        s.close()

    r = client.get("/devices/lock-w2/wifi", headers=_auth())
    assert r.status_code == 200
    body = r.json()
    assert body["ssid"] == "SPACEDOME"
    assert body["connected"] is True


def test_wifi_status_requires_auth():
    r = client.get("/devices/lock-01/wifi")
    assert r.status_code == 401
