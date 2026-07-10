import threading
from fastapi.testclient import TestClient
from app.main import app
from app import security, commands, mqtt_client

client = TestClient(app)


def _tok():
    return security.make_token("admin")


def test_command_requires_auth():
    r = client.post("/devices/lock-01/commands", json={"type": "unlock"})
    assert r.status_code == 401


def test_command_publishes_and_returns_ack(monkeypatch):
    published = {}

    def fake_publish(topic, payload):
        published["topic"] = topic
        published["payload"] = payload
        # Simulate the firmware acking on its own thread shortly after.
        def ack():
            commands.handle_ack({"nonce": payload["nonce"], "result": "ok",
                                 "detail": "unlocked", "ts": 1})
        threading.Timer(0.05, ack).start()

    monkeypatch.setattr(mqtt_client, "publish", fake_publish)

    r = client.post("/devices/lock-01/commands", json={"type": "unlock"},
                    headers={"Authorization": f"Bearer {_tok()}"})
    assert r.status_code == 200
    body = r.json()
    assert body["result"] == "ok"
    assert body["detail"] == "unlocked"
    assert published["topic"] == "smartlock/lock-01/commands"
    assert published["payload"]["type"] == "unlock"


def test_command_times_out_when_no_ack(monkeypatch):
    monkeypatch.setattr(mqtt_client, "publish", lambda t, p: None)
    r = client.post("/devices/lock-01/commands",
                    json={"type": "unlock"},
                    headers={"Authorization": f"Bearer {_tok()}"})
    assert r.status_code == 200
    assert r.json()["result"] == "timeout"
