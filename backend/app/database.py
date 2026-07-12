"""Database setup — SQLAlchemy engine, session, and the AccessEvent model.

The DATABASE_URL env var selects the backend with NO code changes:
  - sqlite:///./smartlock.db          (local quick dev)
  - mysql+pymysql://user:pw@host/db   (the MySQL stack, local + AWS EC2)
"""
import logging
import os
import time
from datetime import datetime, timezone

from sqlalchemy import create_engine, Column, Integer, String, Float, DateTime
from sqlalchemy.exc import OperationalError
from sqlalchemy.orm import declarative_base, sessionmaker

log = logging.getLogger("database")

DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:///./smartlock.db")
_is_sqlite = DATABASE_URL.startswith("sqlite")

# check_same_thread is a SQLite-only quirk: the MQTT subscriber runs on a
# different thread than the API, and SQLite otherwise refuses cross-thread use.
connect_args = {"check_same_thread": False} if _is_sqlite else {}

# pool_pre_ping recycles dead connections — important for MySQL, whose server
# drops idle connections after a timeout; without this the API throws
# "MySQL server has gone away" after a quiet period.
engine = create_engine(
    DATABASE_URL,
    connect_args=connect_args,
    pool_pre_ping=not _is_sqlite,
)
SessionLocal = sessionmaker(bind=engine, autocommit=False, autoflush=False)
Base = declarative_base()


def wait_for_db(max_wait_s: int = 60) -> None:
    """Block until the DB accepts a connection. MySQL takes 15-30s to init on
    first container boot, so the API must not give up connecting too early. A
    no-op for SQLite (a local file is always ready)."""
    if _is_sqlite:
        return
    deadline = time.time() + max_wait_s
    while True:
        try:
            with engine.connect():
                log.info("database is ready")
                return
        except OperationalError as e:
            if time.time() >= deadline:
                log.error("database not ready after %ds: %s", max_wait_s, e)
                raise
            log.info("waiting for database to come up...")
            time.sleep(2)


class AccessEvent(Base):
    """One access attempt at the lock — granted or denied, and how.

    This is exactly what the firmware publishes over MQTT, plus a server-side
    received timestamp. The mobile app's log viewer reads rows of this table.
    """
    __tablename__ = "access_events"

    id = Column(Integer, primary_key=True, index=True)

    # MySQL requires an explicit VARCHAR length (SQLite does not). These are
    # short enum-ish strings, so modest lengths are plenty.
    device_id = Column(String(64), index=True)       # which lock (from the MQTT topic)
    event = Column(String(32))                       # e.g. "access"
    method = Column(String(16), index=True)          # "face" | "pin" | "button"
    result = Column(String(16), index=True)          # "granted" | "denied"
    user_id = Column(Integer, nullable=True)         # matched face id, if any
    score = Column(Float, nullable=True)             # similarity score, if face

    # Time the lock says the event happened (epoch seconds it sent), and the
    # time the server actually received it. Both kept — they differ if the lock
    # was offline and the event was buffered/replayed later.
    device_ts = Column(Integer, nullable=True)
    received_at = Column(DateTime, default=lambda: datetime.now(timezone.utc))


class User(Base):
    """A named person the lock recognizes, keyed to the firmware's user_id.

    user_id is allocated by the backend (monotonic, MAX+1), NOT auto-increment,
    because phase-2 firmware enroll must be told which id to enroll a face into.
    It is also the join key into access_events.user_id, so the /events history
    can show "SUDO OJ unlocked the door" instead of "user 0".
    """
    __tablename__ = "users"

    # NOT autoincrement — the backend allocates ids explicitly (see main.py).
    user_id = Column(Integer, primary_key=True, autoincrement=False)
    name = Column(String(64))                        # display name (MySQL needs a length)
    image_url = Column(String(512), nullable=True)   # phase-3 profile picture URL; unused in phase 1
    created_at = Column(DateTime, default=lambda: datetime.now(timezone.utc))


def init_db():
    """Create tables if they don't exist, and seed user_id=0 as SUDO OJ.

    Safe to call on every startup: the seed inserts only if the row is missing,
    so a later rename of user 0 survives restarts.
    """
    Base.metadata.create_all(bind=engine)
    db = SessionLocal()
    try:
        if db.get(User, 0) is None:
            db.add(User(user_id=0, name="SUDO OJ"))
            db.commit()
    finally:
        db.close()
