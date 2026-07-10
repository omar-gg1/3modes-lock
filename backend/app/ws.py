"""WebSocket fan-out.

Each connection owns an asyncio.Queue. The MQTT thread (a foreign thread)
enqueues messages via broadcast_threadsafe, which hops onto the app event loop
with run_coroutine_threadsafe. The per-connection sender task drains its queue.

Queue-per-connection means a slow or stuck client can never block the
broadcaster or other clients — its queue just backs up (and is bounded).
"""
import asyncio
import logging

log = logging.getLogger("ws")

_loop: asyncio.AbstractEventLoop | None = None
_QUEUE_MAX = 100


class Manager:
    def __init__(self):
        self.queues: set[asyncio.Queue] = set()

    def add(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=_QUEUE_MAX)
        self.queues.add(q)
        return q

    def remove(self, q: asyncio.Queue) -> None:
        self.queues.discard(q)

    async def _deliver(self, message: dict) -> None:
        for q in list(self.queues):
            try:
                q.put_nowait(message)
            except asyncio.QueueFull:
                # Drop for a client that isn't draining; never block others.
                log.warning("ws client queue full, dropping message")


manager = Manager()


def set_loop(loop: asyncio.AbstractEventLoop) -> None:
    global _loop
    _loop = loop


def broadcast_threadsafe(message: dict) -> None:
    """Safe to call from the paho MQTT thread (or any thread)."""
    if _loop is None:
        return
    asyncio.run_coroutine_threadsafe(manager._deliver(message), _loop)
