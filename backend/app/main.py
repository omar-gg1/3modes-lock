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
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, Depends, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.security import HTTPBearer, HTTPAuthorizationCredentials
from pydantic import BaseModel
from sqlalchemy import func
from sqlalchemy.orm import Session

from . import mqtt_client, security, reachability, commands
from .database import SessionLocal, AccessEvent, init_db, wait_for_db
from .schemas import AccessEventOut, LoginIn, TokenOut, DeviceStatusOut, CommandOut

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


@app.post("/devices/{device_id}/commands", response_model=CommandOut)
async def send_command(device_id: str, body: CommandIn,
                       _sub: str = Depends(require_auth)):
    cmd = commands.build_command(device_id, body.type, body.args)
    fut = commands.registry.register(cmd["nonce"])
    commands.registry.track(cmd["nonce"], device_id)
    mqtt_client.publish(f"smartlock/{device_id}/commands", cmd)
    try:
        ack = await asyncio.wait_for(fut, timeout=COMMAND_TIMEOUT_S)
    except asyncio.TimeoutError:
        return CommandOut(nonce=cmd["nonce"], result="timeout", detail="no_ack")
    return CommandOut(nonce=cmd["nonce"], result=ack["result"],
                      detail=ack["detail"])


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


@app.get("/events", response_model=list[AccessEventOut])
def list_events(
    db: Session = Depends(get_db),
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
    return q.order_by(AccessEvent.id.desc()).limit(limit).all()


@app.get("/events/{event_id}", response_model=AccessEventOut)
def get_event(event_id: int, db: Session = Depends(get_db)):
    row = db.get(AccessEvent, event_id)
    if row is None:
        raise HTTPException(status_code=404, detail="event not found")
    return row


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
