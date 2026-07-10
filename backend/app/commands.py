"""Command minting + pending-command (nonce → Future) registry + ack handling."""
import asyncio
import os
import time

from . import security, reachability

CMD_HMAC_SECRET = os.environ.get("CMD_HMAC_SECRET", "00" * 32)


def build_command(device_id: str, type_: str, args: dict | None = None,
                  ttl_s: int = 8) -> dict:
    args = args or {}
    nonce = security.new_nonce()
    iat = int(time.time())
    exp = iat + ttl_s
    sig = security.sign_command(CMD_HMAC_SECRET, device_id, type_,
                                nonce, iat, exp, args)
    return {"type": type_, "nonce": nonce, "iat": iat, "exp": exp,
            "args": args, "sig": sig}


class PendingRegistry:
    def __init__(self):
        self._futures: dict[str, asyncio.Future] = {}
        self._loops: dict[str, asyncio.AbstractEventLoop] = {}
        self._sent_at: dict[str, float] = {}
        self._device: dict[str, str] = {}

    def register(self, nonce: str) -> asyncio.Future:
        loop = asyncio.get_event_loop()
        fut: asyncio.Future = loop.create_future()
        self._futures[nonce] = fut
        self._loops[nonce] = loop
        self._sent_at.setdefault(nonce, time.time())
        return fut

    def track(self, nonce: str, device_id: str) -> None:
        self._device[nonce] = device_id
        self._sent_at[nonce] = time.time()

    def resolve(self, nonce: str, ack: dict) -> bool:
        """Resolve the pending future. Safe to call from a foreign thread
        (the MQTT/paho thread): the result is set on the future's own loop via
        call_soon_threadsafe, which is the only thread-safe way to complete a
        future created on another loop."""
        fut = self._futures.pop(nonce, None)
        loop = self._loops.pop(nonce, None)
        if fut is None or fut.done():
            return False
        if loop is not None and loop.is_running():
            loop.call_soon_threadsafe(
                lambda: None if fut.done() else fut.set_result(ack))
        else:
            fut.set_result(ack)
        return True

    def sent_at(self, nonce: str) -> float | None:
        return self._sent_at.get(nonce)

    def device_of(self, nonce: str) -> str | None:
        return self._device.get(nonce)


registry = PendingRegistry()


def handle_ack(ack: dict, now: float | None = None) -> None:
    """Called from the MQTT thread when an ack arrives."""
    now = now if now is not None else time.time()
    nonce = ack.get("nonce", "")
    sent = registry.sent_at(nonce)
    device = registry.device_of(nonce)
    if sent is not None and device is not None and ack.get("detail") == "pong":
        reachability.mark_ping_rtt(device, (now - sent) * 1000.0)
    registry.resolve(nonce, ack)
