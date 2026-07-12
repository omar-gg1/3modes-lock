"""User management — the users table, seed, CRUD, and /events name join."""
import pytest
from fastapi.testclient import TestClient

from app.main import app
from app import security
from app.database import init_db, SessionLocal, User, AccessEvent

client = TestClient(app)


@pytest.fixture(autouse=True)
def clean_tables():
    """Every test starts from an empty users/events table, then re-seeds.

    conftest gives the whole file ONE temp-file SQLite db, so without this a row
    left by one test (e.g. the id-32767 ceiling row) poisons the next test's
    id allocation. Truncate + re-seed keeps each test independent.
    """
    init_db()
    db = SessionLocal()
    try:
        db.query(AccessEvent).delete()
        db.query(User).delete()
        db.commit()
    finally:
        db.close()
    init_db()   # re-seed SUDO OJ (user_id=0)


def _token() -> str:
    return security.make_token("admin")


def _auth():
    return {"Authorization": f"Bearer {_token()}"}


# --- Task 1: model + seed ---

def test_seed_creates_sudo_oj():
    init_db()
    db = SessionLocal()
    try:
        row = db.get(User, 0)
        assert row is not None
        assert row.name == "SUDO OJ"
    finally:
        db.close()


def test_seed_is_idempotent_and_preserves_rename():
    init_db()
    db = SessionLocal()
    try:
        row = db.get(User, 0)
        row.name = "Renamed Admin"
        db.commit()
    finally:
        db.close()
    # a second init_db must NOT clobber the rename back to "SUDO OJ"
    init_db()
    db = SessionLocal()
    try:
        assert db.get(User, 0).name == "Renamed Admin"
    finally:
        db.close()


# --- Task 2: schemas ---

def test_user_out_schema_reads_from_orm_row():
    from app.schemas import UserOut, UserIn, UserUpdate
    from datetime import datetime, timezone

    # UserIn: image_url is optional
    assert UserIn(name="Ada").image_url is None
    # UserUpdate: every field optional
    assert UserUpdate().name is None and UserUpdate().image_url is None

    row = User(user_id=5, name="Ada", image_url=None,
               created_at=datetime.now(timezone.utc))
    out = UserOut.model_validate(row)     # from_attributes path
    assert out.user_id == 5 and out.name == "Ada"


def test_access_event_out_has_user_name_field():
    from app.schemas import AccessEventOut
    assert "user_name" in AccessEventOut.model_fields
    # default is None so existing serialization stays valid
    assert AccessEventOut.model_fields["user_name"].default is None


# --- Task 3: list + create ---

def test_users_list_requires_auth():
    r = client.get("/users")
    assert r.status_code == 401


def test_list_includes_seed_and_is_ordered():
    init_db()
    r = client.get("/users", headers=_auth())
    assert r.status_code == 200
    rows = r.json()
    ids = [u["user_id"] for u in rows]
    assert 0 in ids
    assert ids == sorted(ids)                 # ascending order
    assert next(u for u in rows if u["user_id"] == 0)["name"] == "SUDO OJ"


def test_create_allocates_next_monotonic_id():
    init_db()
    r = client.post("/users", json={"name": "Ada"}, headers=_auth())
    assert r.status_code == 200
    body = r.json()
    assert body["name"] == "Ada"
    assert body["user_id"] >= 1               # seed holds 0, so first created ≥ 1


def test_two_creates_strictly_increasing_no_collision():
    init_db()
    a = client.post("/users", json={"name": "Grace"}, headers=_auth()).json()
    b = client.post("/users", json={"name": "Alan"}, headers=_auth()).json()
    assert b["user_id"] > a["user_id"]


def test_create_rejects_id_over_int16_ceiling():
    init_db()
    db = SessionLocal()
    try:
        db.add(User(user_id=32767, name="Ceiling"))
        db.commit()
    finally:
        db.close()
    r = client.post("/users", json={"name": "Overflow"}, headers=_auth())
    assert r.status_code == 400


# --- Task 4: get-one / patch / delete ---

def test_get_one_and_404():
    init_db()
    created = client.post("/users", json={"name": "Edith"}, headers=_auth()).json()
    uid = created["user_id"]

    got = client.get(f"/users/{uid}", headers=_auth())
    assert got.status_code == 200
    assert got.json()["name"] == "Edith"

    missing = client.get("/users/99999", headers=_auth())
    assert missing.status_code == 404


def test_patch_rename_reflected_on_read():
    init_db()
    created = client.post("/users", json={"name": "Old"}, headers=_auth()).json()
    uid = created["user_id"]

    r = client.patch(f"/users/{uid}", json={"name": "New"}, headers=_auth())
    assert r.status_code == 200
    assert client.get(f"/users/{uid}", headers=_auth()).json()["name"] == "New"


def test_patch_only_applies_provided_fields():
    init_db()
    created = client.post("/users", json={"name": "Keep", "image_url": "u"},
                          headers=_auth()).json()
    uid = created["user_id"]
    # patch only name — image_url must be untouched
    client.patch(f"/users/{uid}", json={"name": "Renamed"}, headers=_auth())
    got = client.get(f"/users/{uid}", headers=_auth()).json()
    assert got["name"] == "Renamed"
    assert got["image_url"] == "u"


def test_delete_normal_user_then_gone():
    init_db()
    uid = client.post("/users", json={"name": "Temp"}, headers=_auth()).json()["user_id"]
    r = client.delete(f"/users/{uid}", headers=_auth())
    assert r.status_code == 204
    assert client.get(f"/users/{uid}", headers=_auth()).status_code == 404


def test_delete_seed_user_zero_is_refused():
    init_db()
    r = client.delete("/users/0", headers=_auth())
    assert r.status_code == 400
    # seed still present
    assert client.get("/users/0", headers=_auth()).status_code == 200


# --- Task 5: /events name join ---

def test_events_carry_current_user_name_and_rename_relabels_history():
    init_db()
    # a user, then an event attributed to them
    uid = client.post("/users", json={"name": "Alice"}, headers=_auth()).json()["user_id"]
    db = SessionLocal()
    try:
        db.add(AccessEvent(device_id="lock-01", event="access", method="face",
                           result="granted", user_id=uid, score=0.9, device_ts=10))
        db.commit()
    finally:
        db.close()

    rows = client.get("/events?device_id=lock-01", headers=_auth()).json()
    mine = next(e for e in rows if e["user_id"] == uid)
    assert mine["user_name"] == "Alice"

    # rename -> the SAME historical event now reads the new name (join at read time)
    client.patch(f"/users/{uid}", json={"name": "Alice Cooper"}, headers=_auth())
    rows = client.get("/events?device_id=lock-01", headers=_auth()).json()
    mine = next(e for e in rows if e["user_id"] == uid)
    assert mine["user_name"] == "Alice Cooper"


def test_event_without_matching_user_has_null_name():
    init_db()
    db = SessionLocal()
    try:
        # user_id present but no user row, and a NULL-user button event
        db.add(AccessEvent(device_id="lock-99", event="access", method="face",
                           result="granted", user_id=31000, score=0.5, device_ts=11))
        db.add(AccessEvent(device_id="lock-99", event="access", method="button",
                           result="granted", user_id=None, score=None, device_ts=12))
        db.commit()
    finally:
        db.close()
    rows = client.get("/events?device_id=lock-99", headers=_auth()).json()
    assert all(e["user_name"] is None for e in rows)
