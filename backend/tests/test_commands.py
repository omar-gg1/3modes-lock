import asyncio
import pytest
from app import commands, security


def test_build_command_shape():
    cmd = commands.build_command("lock-01", "unlock", ttl_s=8)
    assert cmd["type"] == "unlock"
    assert len(cmd["nonce"]) == 16
    assert cmd["exp"] == cmd["iat"] + 8
    assert cmd["args"] == {}
    # sig verifies against the same secret
    expect = security.sign_command(
        "00" * 32, "lock-01", "unlock", cmd["nonce"],
        cmd["iat"], cmd["exp"], {})
    assert cmd["sig"] == expect


async def test_registry_resolves_by_nonce():
    reg = commands.PendingRegistry()
    fut = reg.register("abc")
    assert reg.resolve("abc", {"nonce": "abc", "result": "ok",
                               "detail": "unlocked", "ts": 1}) is True
    ack = await asyncio.wait_for(fut, timeout=1)
    assert ack["detail"] == "unlocked"


async def test_registry_resolve_unknown_nonce_is_false():
    reg = commands.PendingRegistry()
    assert reg.resolve("nope", {"nonce": "nope"}) is False


def test_handle_ack_records_ping_rtt(monkeypatch):
    recorded = {}
    monkeypatch.setattr(commands.reachability, "mark_ping_rtt",
                        lambda d, ms: recorded.update(device=d, ms=ms))
    # simulate a ping command awaiting an ack
    commands.registry._sent_at["png"] = 1000.0
    commands.registry._device["png"] = "lock-01"
    commands.registry.register("png")
    commands.handle_ack({"nonce": "png", "result": "ok",
                         "detail": "pong", "ts": 1}, now=1000.24)
    assert recorded["device"] == "lock-01"
    assert round(recorded["ms"]) == 240
