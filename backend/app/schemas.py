"""Pydantic schemas — the shapes of data crossing the API boundary.

`AccessEventIn`  : what the firmware publishes over MQTT (validated on ingest).
`AccessEventOut` : what the REST API returns to the mobile app.

Keeping these separate from the SQLAlchemy model means a malformed MQTT message
is rejected cleanly instead of corrupting the database.
"""
from datetime import datetime
from typing import Optional

from pydantic import BaseModel, Field


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

    # Let Pydantic read attributes off the SQLAlchemy row object directly.
    model_config = {"from_attributes": True}
