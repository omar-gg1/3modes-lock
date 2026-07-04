"""Persistence for face encodings (embeddings) in MySQL.

Embeddings are 512 float32 = 2048 bytes, stored as a raw BLOB. We reuse the same
MySQL that Mode 2 uses (DATABASE_URL), in a separate table `face_encodings`, so
the verifier and the event log share one database.
"""
import logging
import os
import time

import numpy as np
from sqlalchemy import (create_engine, Column, Integer, String, DateTime,
                        LargeBinary, select)
from sqlalchemy.exc import OperationalError
from sqlalchemy.orm import declarative_base, sessionmaker
from datetime import datetime, timezone

log = logging.getLogger("storage")

DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:///./verifier.db")
_is_sqlite = DATABASE_URL.startswith("sqlite")
connect_args = {"check_same_thread": False} if _is_sqlite else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args,
                       pool_pre_ping=not _is_sqlite)
SessionLocal = sessionmaker(bind=engine, autocommit=False, autoflush=False)
Base = declarative_base()


class FaceEncoding(Base):
    """One enrolled face embedding. A user may have several rows (multiple
    photos), which makes matching robust to pose/lighting."""
    __tablename__ = "face_encodings"

    id = Column(Integer, primary_key=True, index=True)
    user_id = Column(Integer, index=True)
    name = Column(String(64))
    # 512 float32 = 2048 bytes. Stored raw; np.frombuffer restores it.
    encoding = Column(LargeBinary(2048))
    # Provenance: 'app' (good phone camera) or 'esp' (low-quality fallback) —
    # lets us warn / weight by quality later.
    source = Column(String(16), default="app")
    det_score = Column(String(16))            # detector confidence at enroll
    created_at = Column(DateTime, default=lambda: datetime.now(timezone.utc))


def wait_for_db(max_wait_s: int = 60) -> None:
    if _is_sqlite:
        return
    deadline = time.time() + max_wait_s
    while True:
        try:
            with engine.connect():
                return
        except OperationalError:
            if time.time() >= deadline:
                raise
            log.info("waiting for database...")
            time.sleep(2)


def init_db():
    Base.metadata.create_all(bind=engine)


def encoding_to_blob(emb: np.ndarray) -> bytes:
    return np.asarray(emb, dtype=np.float32).tobytes()


def blob_to_encoding(blob: bytes) -> np.ndarray:
    return np.frombuffer(blob, dtype=np.float32)


def save_encoding(user_id: int, name: str, emb: np.ndarray,
                  source: str, det_score: float) -> None:
    s = SessionLocal()
    try:
        row = FaceEncoding(
            user_id=user_id, name=name,
            encoding=encoding_to_blob(emb),
            source=source, det_score=f"{det_score:.3f}",
        )
        s.add(row)
        s.commit()
    finally:
        s.close()


def load_all_encodings():
    """Return [(user_id, embedding_ndarray), ...] for every enrolled face."""
    s = SessionLocal()
    try:
        rows = s.execute(select(FaceEncoding)).scalars().all()
        return [(r.user_id, blob_to_encoding(r.encoding)) for r in rows]
    finally:
        s.close()


def count_encodings() -> int:
    s = SessionLocal()
    try:
        return s.query(FaceEncoding).count()
    finally:
        s.close()
