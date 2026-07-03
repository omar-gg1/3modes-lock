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
import logging
from contextlib import asynccontextmanager
from typing import Optional

from fastapi import FastAPI, Depends, HTTPException, Query
from sqlalchemy import func
from sqlalchemy.orm import Session

from . import mqtt_client
from .database import SessionLocal, AccessEvent, init_db, wait_for_db
from .schemas import AccessEventOut

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # --- startup ---
    wait_for_db()   # block until MySQL is up (no-op for SQLite)
    init_db()
    mqtt_client.start()
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
