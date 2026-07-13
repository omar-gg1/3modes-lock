"""Phase-2 user-management commands: append_enroll + delete_user."""
from fastapi.testclient import TestClient

from app.main import app
from app import security, commands
from app.database import init_db, SessionLocal, User

client = TestClient(app)


def _token():
    return security.make_token("admin")


def _auth():
    return {"Authorization": f"Bearer {_token()}"}


def _seed_user(name="Face User"):
    init_db()
    return client.post("/users", json={"name": name}, headers=_auth()).json()["user_id"]


# --- Task 1: signing contract ---

def test_build_command_signs_sorted_compact_args():
    cmd = commands.build_command("lock-01", "delete_user", {"user_id": 7})
    expected_args = '{"user_id":7}'
    expected_sig = security.sign_command(
        commands.CMD_HMAC_SECRET, "lock-01", "delete_user",
        cmd["nonce"], cmd["iat"], cmd["exp"], {"user_id": 7})
    assert cmd["type"] == "delete_user"
    assert cmd["args"] == {"user_id": 7}
    assert cmd["sig"] == expected_sig
    assert security._compact_args({"user_id": 7}) == expected_args


def test_append_enroll_multi_arg_ordering_is_sorted():
    args = {"user_id": 3, "samples": 5}
    assert security._compact_args(args) == '{"samples":5,"user_id":3}'


# --- Task 2: endpoints ---

def test_enroll_endpoint_requires_auth():
    r = client.post("/users/1/enroll", json={"device_id": "lock-01"})
    assert r.status_code == 401


def test_enroll_unknown_user_404():
    init_db()
    r = client.post("/users/99999/enroll", json={"device_id": "lock-01"},
                    headers=_auth())
    assert r.status_code == 404


def test_enroll_publishes_append_enroll_command(published, monkeypatch):
    uid = _seed_user()
    import app.main as m

    async def _fake_wait_for(fut, timeout):
        return {"result": "ok", "detail": "arming"}
    monkeypatch.setattr(m.asyncio, "wait_for", _fake_wait_for)

    r = client.post(f"/users/{uid}/enroll",
                    json={"device_id": "lock-01", "samples": 4}, headers=_auth())
    assert r.status_code == 200
    assert r.json()["result"] == "ok"
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "append_enroll"
    assert payload["args"] == {"user_id": uid, "samples": 4}


def test_delete_face_publishes_delete_user_command(published, monkeypatch):
    uid = _seed_user()
    import app.main as m

    async def _fake_wait_for(fut, timeout):
        return {"result": "ok", "detail": "deleted"}
    monkeypatch.setattr(m.asyncio, "wait_for", _fake_wait_for)

    r = client.delete(f"/users/{uid}/face?device_id=lock-01", headers=_auth())
    assert r.status_code == 200
    topic, payload = published[-1]
    assert topic == "smartlock/lock-01/commands"
    assert payload["type"] == "delete_user"
    assert payload["args"] == {"user_id": uid}
