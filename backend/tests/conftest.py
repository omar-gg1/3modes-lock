import os
import tempfile

# A temp FILE (not :memory:) so every connection — the request's session and a
# test's own SessionLocal — sees the same database. In-memory SQLite gives each
# connection its own empty DB, which breaks any test that writes rows in one
# place and reads them via the API in another.
_db_path = os.path.join(tempfile.gettempdir(), "nixis_test.db")
if os.path.exists(_db_path):
    os.remove(_db_path)
os.environ.setdefault("DATABASE_URL", f"sqlite:///{_db_path}")
os.environ.setdefault("CMD_HMAC_SECRET", "00" * 32)
os.environ.setdefault("JWT_SECRET", "test-jwt-secret")
os.environ.setdefault("NIXIS_USER", "admin")
os.environ.setdefault("NIXIS_PASSWORD", "adminpass")

import pytest


@pytest.fixture()
def published(monkeypatch):
    """Capture everything the backend would publish to MQTT."""
    sent = []
    import app.mqtt_client as mc
    monkeypatch.setattr(mc, "publish", lambda topic, payload: sent.append((topic, payload)))
    return sent
