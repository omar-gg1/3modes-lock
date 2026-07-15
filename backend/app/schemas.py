"""Pydantic schemas — the shapes of data crossing the API boundary.

`AccessEventIn`  : what the firmware publishes over MQTT (validated on ingest).
`AccessEventOut` : what the REST API returns to the mobile app.

Keeping these separate from the SQLAlchemy model means a malformed MQTT message
is rejected cleanly instead of corrupting the database.
"""
from datetime import datetime
from typing import Optional

from pydantic import BaseModel, Field, conint


class AccessEventIn(BaseModel):
    """The JSON the ESP32 sends. Matches the firmware payload exactly.

    Example:
        {"event":"access","method":"face","id":1,"score":0.71,
         "result":"granted","ts":1718800000}
    """
    event: str = "access"
    method: str                                  # "face" | "pin" | "button"
    result: str                                  # "granted" | "denied"
    id: Optional[int] = None                     # matched user/face id
    score: Optional[float] = None                # similarity, for face events
    ts: Optional[int] = Field(default=None)      # device epoch seconds


class AccessEventOut(BaseModel):
    """A stored event as returned by the REST API."""
    id: int
    device_id: Optional[str]
    event: str
    method: str
    result: str
    user_id: Optional[int]
    score: Optional[float]
    device_ts: Optional[int]
    received_at: datetime
    user_name: Optional[str] = None   # current display name for user_id, joined at read time

    # Let Pydantic read attributes off the SQLAlchemy row object directly.
    model_config = {"from_attributes": True}


# --- Nixis control plane (Plan A) ---

class LoginIn(BaseModel):
    username: str
    password: str


class TokenOut(BaseModel):
    access_token: str
    token_type: str = "bearer"


class CommandOut(BaseModel):
    nonce: str
    result: str          # ok | denied | error | timeout
    detail: str
    rtt_ms: Optional[float] = None


class DeviceStatusOut(BaseModel):
    device_id: str
    state: str           # connected | reconnecting | offline
    rtt_ms: Optional[float] = None
    seconds_since: Optional[float] = None


# --- User management (phase 1) ---

class UserIn(BaseModel):
    """Create-user body. Only a name is required; image_url is a phase-3 hook."""
    name: str
    image_url: Optional[str] = None


class UserUpdate(BaseModel):
    """Patch body — every field optional; only provided fields are applied."""
    name: Optional[str] = None
    image_url: Optional[str] = None


class UserOut(BaseModel):
    """A stored user as returned by the REST API."""
    user_id: int
    name: str
    image_url: Optional[str]
    created_at: datetime

    model_config = {"from_attributes": True}


class ModeIn(BaseModel):
    """Set-mode body. 1=Local, 2=Hybrid, 3=Cloud-Assisted. Out-of-range → 422."""
    mode: conint(ge=1, le=3)


class ModeOut(BaseModel):
    """The lock's last-acked operating mode as returned by the REST API."""
    device_id: str
    mode: int
    updated_at: Optional[datetime] = None

    model_config = {"from_attributes": True}


class BlePassOut(BaseModel):
    """A backend-signed `unlock` command the app replays over BLE when near the
    lock. Same wire shape as any command; longer expiry so it survives the walk
    to the door. Never published by the backend — delivered out-of-band over BLE."""
    type: str
    nonce: str
    iat: int
    exp: int
    args: dict
    sig: str
