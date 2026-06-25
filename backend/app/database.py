"""Database setup — SQLAlchemy engine, session, and the AccessEvent model.

SQLite for dev (a single file). The DATABASE_URL env var switches this to
Postgres for the AWS demo without changing any of the code below.
"""
import os
from datetime import datetime, timezone

from sqlalchemy import create_engine, Column, Integer, String, Float, DateTime
from sqlalchemy.orm import declarative_base, sessionmaker

DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:///./smartlock.db")

# check_same_thread is a SQLite-only quirk: the MQTT subscriber runs on a
# different thread than the API, and SQLite otherwise refuses cross-thread use.
connect_args = {"check_same_thread": False} if DATABASE_URL.startswith("sqlite") else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args)
SessionLocal = sessionmaker(bind=engine, autocommit=False, autoflush=False)
Base = declarative_base()


class AccessEvent(Base):
    """One access attempt at the lock — granted or denied, and how.

    This is exactly what the firmware publishes over MQTT, plus a server-side
    received timestamp. The mobile app's log viewer reads rows of this table.
    """
    __tablename__ = "access_events"

    id = Column(Integer, primary_key=True, index=True)

    device_id = Column(String, index=True)          # which lock (from the MQTT topic)
    event = Column(String)                           # e.g. "access"
    method = Column(String, index=True)              # "face" | "pin" | "button"
    result = Column(String, index=True)              # "granted" | "denied"
    user_id = Column(Integer, nullable=True)         # matched face id, if any
    score = Column(Float, nullable=True)             # similarity score, if face

    # Time the lock says the event happened (epoch seconds it sent), and the
    # time the server actually received it. Both kept — they differ if the lock
    # was offline and the event was buffered/replayed later.
    device_ts = Column(Integer, nullable=True)
    received_at = Column(DateTime, default=lambda: datetime.now(timezone.utc))


def init_db():
    """Create tables if they don't exist. Safe to call on every startup."""
    Base.metadata.create_all(bind=engine)
