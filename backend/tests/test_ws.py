"""WebSocket tests.

Note on approach: broadcast_threadsafe hops from a foreign thread onto the app
event loop via run_coroutine_threadsafe. Starlette's synchronous TestClient runs
the app in a separate portal thread whose loop cannot be reliably kicked from the
test thread, so we test the two halves directly:

  1. token rejection at the endpoint (sync TestClient — no cross-thread push),
  2. the delivery mechanism (manager queue + _deliver) on a live loop,

which together cover the real code path used in production (uvicorn's serving
loop, where run_coroutine_threadsafe works normally).
"""
import asyncio

import pytest
from fastapi.testclient import TestClient

from app.main import app
from app import security, ws

client = TestClient(app)


def test_ws_rejects_without_valid_token():
    with pytest.raises(Exception):
        with client.websocket_connect("/ws?token=bad"):
            pass


async def test_broadcast_reaches_connection_queue():
    # Simulate the endpoint registering a connection queue on the running loop.
    ws.set_loop(asyncio.get_running_loop())
    q = ws.manager.add()
    try:
        # This is exactly what mqtt_client calls (from its thread in prod).
        ws.broadcast_threadsafe({"kind": "event", "result": "granted"})
        msg = await asyncio.wait_for(q.get(), timeout=1.0)
        assert msg["kind"] == "event"
        assert msg["result"] == "granted"
    finally:
        ws.manager.remove(q)


async def test_broadcast_to_no_clients_is_safe():
    ws.set_loop(asyncio.get_running_loop())
    # No queues registered — must not raise.
    ws.broadcast_threadsafe({"kind": "event"})
    await asyncio.sleep(0.05)
