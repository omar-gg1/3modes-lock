"""BLE-pass endpoint: POST /devices/{id}/ble_pass mints a signed unlock command
the app replays over Bluetooth. It must never leak the secret and must verify
byte-for-byte against the same HMAC the lock uses."""
from fastapi.testclient import TestClient

from app.main import app, BLE_PASS_TTL_S
from app import security, commands

client = TestClient(app)


def _auth():
    return {"Authorization": f"Bearer {security.make_token('admin')}"}


def test_ble_pass_requires_auth():
    assert client.post("/devices/lock-01/ble_pass").status_code == 401


def test_ble_pass_returns_well_formed_unlock_blob():
    r = client.post("/devices/lock-01/ble_pass", headers=_auth())
    assert r.status_code == 200
    p = r.json()
    assert p["type"] == "unlock"
    assert p["args"] == {}
    assert len(p["nonce"]) == 16
    assert len(p["sig"]) == 64


def test_ble_pass_signature_verifies():
    p = client.post("/devices/lock-01/ble_pass", headers=_auth()).json()
    expect = security.sign_command(
        commands.CMD_HMAC_SECRET, "lock-01", "unlock",
        p["nonce"], p["iat"], p["exp"], p["args"])
    assert p["sig"] == expect


def test_ble_pass_has_long_ttl():
    p = client.post("/devices/lock-01/ble_pass", headers=_auth()).json()
    assert p["exp"] - p["iat"] == BLE_PASS_TTL_S


def test_ble_pass_nonce_is_unique_per_call():
    a = client.post("/devices/lock-01/ble_pass", headers=_auth()).json()
    b = client.post("/devices/lock-01/ble_pass", headers=_auth()).json()
    assert a["nonce"] != b["nonce"]
