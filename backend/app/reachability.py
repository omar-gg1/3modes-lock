"""In-memory per-device reachability: last-heard timestamp + last ping RTT."""
import time
import threading

CONNECTED_MAX_S = 10.0
RECONNECTING_MAX_S = 30.0

_lock = threading.Lock()
_last_heard: dict[str, float] = {}
_rtt_ms: dict[str, float] = {}


def mark_heard(device_id: str, ts: float | None = None) -> None:
    with _lock:
        _last_heard[device_id] = ts if ts is not None else time.time()


def mark_ping_rtt(device_id: str, ms: float) -> None:
    with _lock:
        _rtt_ms[device_id] = ms


def status(device_id: str, now: float | None = None) -> dict:
    now = now if now is not None else time.time()
    with _lock:
        last = _last_heard.get(device_id)
        rtt = _rtt_ms.get(device_id)
    if last is None:
        return {"state": "offline", "last_heard_at": None,
                "rtt_ms": rtt, "seconds_since": None}
    since = now - last
    if since <= CONNECTED_MAX_S:
        state = "connected"
    elif since <= RECONNECTING_MAX_S:
        state = "reconnecting"
    else:
        state = "offline"
    return {"state": state, "last_heard_at": last,
            "rtt_ms": rtt, "seconds_since": since}
