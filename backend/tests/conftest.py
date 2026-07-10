import os

os.environ.setdefault("DATABASE_URL", "sqlite:///:memory:")
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
