from app import reachability as r


def test_unknown_device_is_offline():
    s = r.status("ghost", now=1000.0)
    assert s["state"] == "offline"
    assert s["last_heard_at"] is None


def test_recent_is_connected():
    r.mark_heard("lock-01", ts=1000.0)
    s = r.status("lock-01", now=1005.0)
    assert s["state"] == "connected"
    assert s["seconds_since"] == 5.0


def test_grace_window_is_reconnecting():
    r.mark_heard("lock-01", ts=1000.0)
    s = r.status("lock-01", now=1020.0)
    assert s["state"] == "reconnecting"


def test_stale_is_offline():
    r.mark_heard("lock-01", ts=1000.0)
    s = r.status("lock-01", now=1100.0)
    assert s["state"] == "offline"


def test_rtt_is_reported():
    r.mark_heard("lock-01", ts=1000.0)
    r.mark_ping_rtt("lock-01", 240.0)
    s = r.status("lock-01", now=1001.0)
    assert s["rtt_ms"] == 240.0
