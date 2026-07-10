import json
from app import mqtt_client, reachability, commands


class FakeMsg:
    def __init__(self, topic, payload):
        self.topic = topic
        self.payload = json.dumps(payload).encode()


def test_ack_message_marks_heard_and_handles(monkeypatch):
    seen = {}
    monkeypatch.setattr(commands, "handle_ack", lambda ack: seen.update(ack))
    mqtt_client._on_message(None, None,
        FakeMsg("smartlock/lock-01/acks",
                {"nonce": "n1", "result": "ok", "detail": "pong", "ts": 5}))
    assert seen["nonce"] == "n1"
    assert reachability.status("lock-01")["state"] == "connected"


def test_event_message_still_marks_heard():
    mqtt_client._on_message(None, None,
        FakeMsg("smartlock/lock-02/events",
                {"event": "access", "method": "button",
                 "result": "granted", "ts": 9}))
    assert reachability.status("lock-02")["state"] == "connected"
