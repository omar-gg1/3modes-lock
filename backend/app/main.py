"""FastAPI app — the Mode 2 (Hybrid) backend entry point.

On startup it creates the DB tables and starts the MQTT subscriber, so the
moment the API is up it's already ingesting access events from the lock.

REST endpoints (read-only for now; the lock writes via MQTT, not HTTP):
  GET /                      health check
  GET /events                recent access events (newest first), filterable
  GET /events/{event_id}     one event by id
  GET /stats                 quick counts (granted/denied, by method)

Interactive docs are auto-generated at /docs.
"""
import asyncio
import logging
import os
import re
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import (FastAPI, Depends, HTTPException, Query, WebSocket,
                     WebSocketDisconnect, status)
from fastapi.middleware.cors import CORSMiddleware
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from pydantic import BaseModel, field_validator
from sqlalchemy import func
from sqlalchemy.orm import Session

from . import mqtt_client, security, reachability, commands
from .database import SessionLocal, AccessEvent, User, init_db, wait_for_db
from .schemas import (AccessEventOut, LoginIn, TokenOut, DeviceStatusOut,
                      CommandOut, UserIn, UserUpdate, UserOut)

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # --- startup ---
    wait_for_db()   # block until MySQL is up (no-op for SQLite)
    init_db()
    mqtt_client.start()
    # Capture the running loop so the MQTT thread can push to WebSockets.
    from . import ws as _ws
    _ws.set_loop(asyncio.get_running_loop())
    yield
    # --- shutdown ---
    mqtt_client.stop()


app = FastAPI(title="Smart Lock Backend (Mode 2 Hybrid)", lifespan=lifespan)

# Allow the Nixis app (web/mobile) to call the API from a browser origin.
# Open for the demo; tighten allow_origins to the app's host for production.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


def get_db():
    """Per-request DB session, always closed afterwards."""
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


@app.get("/")
def health():
    return {"status": "ok", "service": "smartlock-backend", "mode": "hybrid"}


# --- Nixis control plane (Plan A) ---

_bearer = HTTPBearer(auto_error=False)


def require_auth(creds: HTTPAuthorizationCredentials = Depends(_bearer)) -> str:
    if creds is None:
        raise HTTPException(status_code=401, detail="missing token")
    try:
        return security.verify_token(creds.credentials)["sub"]
    except Exception:
        raise HTTPException(status_code=401, detail="invalid token")


@app.post("/auth/login", response_model=TokenOut)
def login(body: LoginIn):
    if (body.username != os.environ.get("NIXIS_USER", "admin")
            or body.password != os.environ.get("NIXIS_PASSWORD", "adminpass")):
        raise HTTPException(status_code=401, detail="bad credentials")
    return TokenOut(access_token=security.make_token(body.username))


@app.get("/devices/{device_id}/status", response_model=DeviceStatusOut)
def device_status(device_id: str, _sub: str = Depends(require_auth)):
    s = reachability.status(device_id)
    return DeviceStatusOut(device_id=device_id, state=s["state"],
                           rtt_ms=s["rtt_ms"], seconds_since=s["seconds_since"])


COMMAND_TIMEOUT_S = 6.0


class CommandIn(BaseModel):
    type: str
    args: dict = {}


async def _dispatch_command(device_id: str, type_: str, args: dict) -> CommandOut:
    """Shared publish -> await-ack path. Every command endpoint routes through
    here so the nonce/ack correlation lives in exactly one place."""
    cmd = commands.build_command(device_id, type_, args)
    fut = commands.registry.register(cmd["nonce"])
    commands.registry.track(cmd["nonce"], device_id)
    mqtt_client.publish(f"smartlock/{device_id}/commands", cmd)
    try:
        ack = await asyncio.wait_for(fut, timeout=COMMAND_TIMEOUT_S)
    except asyncio.TimeoutError:
        return CommandOut(nonce=cmd["nonce"], result="timeout", detail="no_ack")
    return CommandOut(nonce=cmd["nonce"], result=ack["result"],
                      detail=ack["detail"])


@app.post("/devices/{device_id}/commands", response_model=CommandOut)
async def send_command(device_id: str, body: CommandIn,
                       _sub: str = Depends(require_auth)):
    return await _dispatch_command(device_id, body.type, body.args)


@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket, token: str = ""):
    from . import ws
    try:
        security.verify_token(token)
    except Exception:
        await websocket.close(code=1008)
        return
    await websocket.accept()
    queue = ws.manager.add()
    try:
        while True:
            msg = await queue.get()
            await websocket.send_json(msg)
    except WebSocketDisconnect:
        pass
    finally:
        ws.manager.remove(queue)


def _attach_user_names(db: Session, rows: list) -> list[AccessEventOut]:
    """Stamp each event with the CURRENT display name for its user_id.

    One users query builds a {user_id: name} map, so renaming a user instantly
    relabels all their history. Events with a NULL or unknown user_id get None.
    """
    name_by_id = dict(db.query(User.user_id, User.name).all())
    out = []
    for r in rows:
        e = AccessEventOut.model_validate(r)
        e.user_name = name_by_id.get(r.user_id)
        out.append(e)
    return out


@app.get("/events", response_model=list[AccessEventOut])
def list_events(
    db: Session = Depends(get_db),
    _sub: str = Depends(require_auth),
    device_id: Optional[str] = None,
    method: Optional[str] = Query(default=None, description="face | pin | button"),
    result: Optional[str] = Query(default=None, description="granted | denied"),
    limit: int = Query(default=50, le=500),
):
    """Recent access events, newest first. Optional filters narrow the list."""
    q = db.query(AccessEvent)
    if device_id:
        q = q.filter(AccessEvent.device_id == device_id)
    if method:
        q = q.filter(AccessEvent.method == method)
    if result:
        q = q.filter(AccessEvent.result == result)
    rows = q.order_by(AccessEvent.id.desc()).limit(limit).all()
    return _attach_user_names(db, rows)


@app.get("/events/{event_id}", response_model=AccessEventOut)
def get_event(event_id: int, db: Session = Depends(get_db)):
    row = db.get(AccessEvent, event_id)
    if row is None:
        raise HTTPException(status_code=404, detail="event not found")
    return _attach_user_names(db, [row])[0]


@app.get("/stats")
def stats(db: Session = Depends(get_db)):
    """Quick aggregate counts for the app's dashboard."""
    total = db.query(func.count(AccessEvent.id)).scalar()
    by_result = dict(
        db.query(AccessEvent.result, func.count(AccessEvent.id))
          .group_by(AccessEvent.result).all()
    )
    by_method = dict(
        db.query(AccessEvent.method, func.count(AccessEvent.id))
          .group_by(AccessEvent.method).all()
    )
    return {"total": total, "by_result": by_result, "by_method": by_method}


# --- User management (phase 1) ---

MAX_USER_ID = 32767   # firmware s_feat_user_map is int16_t


@app.get("/users", response_model=list[UserOut])
def list_users(db: Session = Depends(get_db), _sub: str = Depends(require_auth)):
    """All users, ascending by id (seed user_id=0 first)."""
    return db.query(User).order_by(User.user_id.asc()).all()


@app.post("/users", response_model=UserOut)
def create_user(body: UserIn, db: Session = Depends(get_db),
                _sub: str = Depends(require_auth)):
    """Allocate the next monotonic user_id (MAX+1) and create the row.

    Explicit allocation (not DB autoincrement) so phase-2 firmware enroll can be
    told which id to enroll a face into. Rejected if it would exceed int16.
    """
    max_id = db.query(func.max(User.user_id)).scalar()
    next_id = (max_id if max_id is not None else -1) + 1
    if next_id > MAX_USER_ID:
        raise HTTPException(status_code=400,
                            detail=f"user_id would exceed firmware max {MAX_USER_ID}")
    user = User(user_id=next_id, name=body.name, image_url=body.image_url)
    db.add(user)
    db.commit()
    db.refresh(user)
    return user


@app.get("/users/{user_id}", response_model=UserOut)
def get_user(user_id: int, db: Session = Depends(get_db),
             _sub: str = Depends(require_auth)):
    user = db.get(User, user_id)
    if user is None:
        raise HTTPException(status_code=404, detail="user not found")
    return user


@app.patch("/users/{user_id}", response_model=UserOut)
def update_user(user_id: int, body: UserUpdate, db: Session = Depends(get_db),
                _sub: str = Depends(require_auth)):
    """Rename / set image_url. Only fields present in the body are applied."""
    user = db.get(User, user_id)
    if user is None:
        raise HTTPException(status_code=404, detail="user not found")
    for field, value in body.model_dump(exclude_unset=True).items():
        setattr(user, field, value)
    db.commit()
    db.refresh(user)
    return user


@app.delete("/users/{user_id}", status_code=status.HTTP_204_NO_CONTENT)
def delete_user(user_id: int, db: Session = Depends(get_db),
                _sub: str = Depends(require_auth)):
    """Delete a user's record. The SUDO OJ seed (id 0) is protected."""
    if user_id == 0:
        raise HTTPException(status_code=400, detail="cannot delete the SUDO OJ seed user")
    user = db.get(User, user_id)
    if user is None:
        raise HTTPException(status_code=404, detail="user not found")
    db.delete(user)
    db.commit()


# --- User management phase 2: face enroll / delete via the device ---

class EnrollFaceIn(BaseModel):
    device_id: str
    samples: int = 5


@app.post("/users/{user_id}/enroll", response_model=CommandOut)
async def enroll_user_face(user_id: int, body: EnrollFaceIn,
                           db: Session = Depends(get_db),
                           _sub: str = Depends(require_auth)):
    """Ask the device to append-enroll a face under this user_id (non-wiping).
    The ack reports arming; enroll success arrives later as an access event."""
    if db.get(User, user_id) is None:
        raise HTTPException(status_code=404, detail="user not found")
    return await _dispatch_command(body.device_id, "append_enroll",
                                   {"user_id": user_id, "samples": body.samples})


@app.delete("/users/{user_id}/face", response_model=CommandOut)
async def delete_user_face(user_id: int, device_id: str,
                           db: Session = Depends(get_db),
                           _sub: str = Depends(require_auth)):
    """Ask the device to remove this user's face features. Distinct from
    DELETE /users/{id}, which removes the DB row."""
    if db.get(User, user_id) is None:
        raise HTTPException(status_code=404, detail="user not found")
    return await _dispatch_command(device_id, "delete_user", {"user_id": user_id})


# --- Temporary guest PIN (OTP-style; lives on the device, not the DB) ---

class TempPinIn(BaseModel):
    pin: str
    ttl_s: int

    @field_validator("pin")
    @classmethod
    def _pin_digits(cls, v: str) -> str:
        if not re.fullmatch(r"\d{4,6}", v):
            raise ValueError("pin must be 4-6 digits")
        return v

    @field_validator("ttl_s")
    @classmethod
    def _ttl_range(cls, v: int) -> int:
        if not (0 < v <= 86400):
            raise ValueError("ttl_s must be 1..86400 seconds")
        return v


@app.post("/devices/{device_id}/temp_pin", response_model=CommandOut)
async def set_temp_pin(device_id: str, body: TempPinIn,
                       _sub: str = Depends(require_auth)):
    """Arm a one-time guest PIN on the lock. It unlocks once, then dies — or
    expires after ttl_s, whichever comes first. Not stored server-side."""
    return await _dispatch_command(device_id, "set_temp_pin",
                                   {"pin": body.pin, "ttl_s": body.ttl_s})


@app.delete("/devices/{device_id}/temp_pin", response_model=CommandOut)
async def clear_temp_pin(device_id: str, _sub: str = Depends(require_auth)):
    """Revoke any active guest PIN on the lock (empty pin clears the slot)."""
    return await _dispatch_command(device_id, "set_temp_pin",
                                   {"pin": "", "ttl_s": 0})
