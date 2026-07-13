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
                        LargeBinary, func, select, text)
from sqlalchemy.dialects.mysql import MEDIUMBLOB
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
    # The original enrollment JPEG. Kept so the ESP can PULL a cloud-enrolled
    # face (e.g. enrolled from the phone app) and re-embed it with its own local
    # recognizer — esp-dl and ArcFace embeddings are not interchangeable, so we
    # must ship the image, not the embedding, to seed the local matcher.
    # MEDIUMBLOB (16 MB) on MySQL; plain BLOB on SQLite (tests) via the variant.
    image = Column(LargeBinary().with_variant(MEDIUMBLOB(), "mysql"))


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
    # create_all only creates missing TABLES, never adds a column to a table that
    # already exists. Add `image` to a pre-existing face_encodings idempotently.
    if _is_sqlite:
        col_type, exists_sql = "BLOB", (
            "SELECT 1 FROM pragma_table_info('face_encodings') WHERE name='image'")
    else:
        col_type, exists_sql = "MEDIUMBLOB", (
            "SELECT 1 FROM information_schema.columns WHERE table_schema=DATABASE()"
            " AND table_name='face_encodings' AND column_name='image'")
    with engine.begin() as conn:
        if conn.execute(text(exists_sql)).first() is None:
            conn.execute(text(f"ALTER TABLE face_encodings ADD COLUMN image {col_type}"))
            log.info("added face_encodings.image column")


def encoding_to_blob(emb: np.ndarray) -> bytes:
    return np.asarray(emb, dtype=np.float32).tobytes()


def blob_to_encoding(blob: bytes) -> np.ndarray:
    return np.frombuffer(blob, dtype=np.float32)


def save_encoding(user_id: int, name: str, emb: np.ndarray,
                  source: str, det_score: float,
                  image: bytes | None = None) -> None:
    s = SessionLocal()
    try:
        row = FaceEncoding(
            user_id=user_id, name=name,
            encoding=encoding_to_blob(emb),
            source=source, det_score=f"{det_score:.3f}",
            image=image,
        )
        s.add(row)
        s.commit()
    finally:
        s.close()


def faces_revision() -> dict:
    """Cheap monotonic revision token for the whole gallery. count changes on any
    insert/delete; max_id changes on any insert (ids never reused). The ESP GETs
    this on boot and only reconciles when it differs from its last-synced value —
    replacing the old blind 35s re-push-everything boot sync."""
    s = SessionLocal()
    try:
        count = s.query(func.count(FaceEncoding.id)).scalar() or 0
        max_id = s.query(func.max(FaceEncoding.id)).scalar() or 0
        return {"count": int(count), "max_id": int(max_id)}
    finally:
        s.close()


def list_encodings_meta():
    """[(id, user_id, name, source), ...] — no blobs. Lets the ESP diff which
    faces it already holds locally before pulling images."""
    s = SessionLocal()
    try:
        rows = s.execute(
            select(FaceEncoding.id, FaceEncoding.user_id,
                   FaceEncoding.name, FaceEncoding.source)).all()
        return [(r.id, r.user_id, r.name, r.source) for r in rows]
    finally:
        s.close()


def get_encoding_image(enc_id: int) -> bytes | None:
    s = SessionLocal()
    try:
        row = s.get(FaceEncoding, enc_id)
        return bytes(row.image) if row and row.image else None
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


def delete_user_encodings(user_id: int) -> int:
    """Remove all references for one user. Returns rows deleted.

    Enrollment (save_encoding) always INSERTS, so re-syncing the same person
    would stack duplicate references and skew the genuine-score distribution in
    the FAR/FRR eval. The ESP calls this once before pushing a fresh batch so a
    re-sync REPLACES that user's references instead of accumulating them —
    keeping the reference set (and therefore the metrics) honest.
    """
    s = SessionLocal()
    try:
        n = s.query(FaceEncoding).filter(FaceEncoding.user_id == user_id).delete()
        s.commit()
        return n
    finally:
        s.close()


def delete_all_encodings() -> int:
    """Wipe every enrolled reference. Returns rows deleted. Full cloud reset."""
    s = SessionLocal()
    try:
        n = s.query(FaceEncoding).delete()
        s.commit()
        return n
    finally:
        s.close()
