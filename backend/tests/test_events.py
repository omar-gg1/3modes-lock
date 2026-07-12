"""The /events history endpoint — protected, and returns stored rows."""
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db, SessionLocal, AccessEvent

client = TestClient(app)


def _token() -> str:
    return security.make_token("admin")


def test_events_requires_auth():
    r = client.get("/events")
    assert r.status_code == 401


def test_events_returns_stored_rows_newest_first():
    init_db()
    db = SessionLocal()
    try:
        db.add(AccessEvent(device_id="lock-01", event="access",
                           method="button", result="granted",
                           user_id=None, score=None, device_ts=1))
        db.add(AccessEvent(device_id="lock-01", event="access",
                           method="face", result="granted",
                           user_id=0, score=0.9, device_ts=2))
        db.commit()
    finally:
        db.close()

    r = client.get("/events?device_id=lock-01",
                   headers={"Authorization": f"Bearer {_token()}"})
    assert r.status_code == 200
    rows = r.json()
    assert len(rows) >= 2
    # newest first — the face event (id 2) comes before the button event (id 1)
    assert rows[0]["method"] == "face"
