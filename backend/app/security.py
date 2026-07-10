"""Command signing (HMAC-SHA256) per Plan 0, and JWT helpers (Task 6)."""
import hashlib
import hmac
import json
import os
import secrets


def new_nonce() -> str:
    """16 hex chars = 8 random bytes."""
    return secrets.token_hex(8)


def _compact_args(args: dict) -> str:
    return json.dumps(args or {}, separators=(",", ":"), sort_keys=True)


def signing_string(device_id: str, type_: str, nonce: str,
                   iat: int, exp: int, args: dict) -> str:
    return f"{device_id}|{type_}|{nonce}|{iat}|{exp}|{_compact_args(args)}"


def sign_command(secret_hex: str, device_id: str, type_: str, nonce: str,
                 iat: int, exp: int, args: dict) -> str:
    key = bytes.fromhex(secret_hex)
    msg = signing_string(device_id, type_, nonce, iat, exp, args).encode()
    return hmac.new(key, msg, hashlib.sha256).hexdigest()


# --- JWT (app login tokens) ---
import datetime as _dt

import jwt  # PyJWT

JWT_SECRET = os.environ.get("JWT_SECRET", "dev-jwt-secret")
JWT_ALG = "HS256"
JWT_TTL_MIN = 60 * 24  # 24h


def make_token(sub: str) -> str:
    now = _dt.datetime.now(_dt.timezone.utc)
    payload = {"sub": sub, "iat": now,
               "exp": now + _dt.timedelta(minutes=JWT_TTL_MIN)}
    return jwt.encode(payload, JWT_SECRET, algorithm=JWT_ALG)


def verify_token(token: str) -> dict:
    return jwt.decode(token, JWT_SECRET, algorithms=[JWT_ALG])
