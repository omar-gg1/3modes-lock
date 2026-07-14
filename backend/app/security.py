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


# --- Door-code encryption (reveal-able, but useless in the DB alone) ---
# The door code is stored encrypted, not plaintext: a DB leak must not hand
# over every door PIN. Fernet (AES-128-CBC + HMAC) is reversible so an authed
# admin can reveal the real digits — the protection is that the key lives in an
# env var, never in the DB. Rotating DOOR_PIN_KEY invalidates stored ciphertext.
import base64 as _b64

from cryptography.fernet import Fernet

# Derive a valid 32-byte urlsafe-base64 Fernet key from a plain env secret so
# ops set a normal string, not a pre-encoded key. ponytail: SHA-256 KDF, swap
# for a real KDF (scrypt) only if the secret is low-entropy.
_DOOR_KEY_SECRET = os.environ.get("DOOR_PIN_KEY", "dev-door-pin-key")
_fernet = Fernet(_b64.urlsafe_b64encode(hashlib.sha256(_DOOR_KEY_SECRET.encode()).digest()))


# Generic pin crypto — the door code and the liveness-confirmation code share the
# same key. encrypt_door_pin/decrypt_door_pin stay as aliases for existing callers.
def encrypt_pin(pin: str) -> str:
    return _fernet.encrypt(pin.encode()).decode()


def decrypt_pin(token: str) -> str:
    return _fernet.decrypt(token.encode()).decode()


encrypt_door_pin = encrypt_pin
decrypt_door_pin = decrypt_pin
