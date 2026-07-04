"""Storage for face-recognition EVALUATION runs — kept fully separate from the
production face_encodings table so test data never pollutes real enrolled users.

Two tables:
  eval_runs    : one row per evaluation run (dataset, params, headline metrics)
  eval_scores  : every genuine/impostor comparison score of a run (for ROC etc.)

This lets us (a) reproduce/plot any past run and (b) serve real-time insights
(current FAR/FRR/EER, score distributions) from the DB.
"""
import os
import time
from datetime import datetime, timezone

from sqlalchemy import (create_engine, Column, Integer, String, Float,
                        DateTime, ForeignKey, Boolean)
from sqlalchemy.exc import OperationalError
from sqlalchemy.orm import declarative_base, sessionmaker, relationship

# Reuse the same DATABASE_URL as the verifier; separate tables.
DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:///./eval.db")
_is_sqlite = DATABASE_URL.startswith("sqlite")
connect_args = {"check_same_thread": False} if _is_sqlite else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args,
                       pool_pre_ping=not _is_sqlite)
SessionLocal = sessionmaker(bind=engine, autocommit=False, autoflush=False)
Base = declarative_base()


class EvalRun(Base):
    __tablename__ = "eval_runs"
    id = Column(Integer, primary_key=True, index=True)
    dataset = Column(String(64))            # e.g. "lfw", "my_people"
    model = Column(String(32))              # e.g. "buffalo_l"
    num_identities = Column(Integer)
    num_genuine = Column(Integer)           # genuine comparison count
    num_impostor = Column(Integer)          # impostor comparison count
    # Headline metrics (computed once, cached here for fast insights):
    eer = Column(Float)                     # Equal Error Rate
    eer_threshold = Column(Float)           # threshold at EER
    auc = Column(Float)                     # ROC area under curve
    created_at = Column(DateTime, default=lambda: datetime.now(timezone.utc))

    scores = relationship("EvalScore", back_populates="run",
                          cascade="all, delete-orphan")


class EvalScore(Base):
    """One comparison: is this pair the SAME person (genuine) or DIFFERENT
    (impostor), and what similarity did the model give it? These are what the
    threshold sweep / ROC / FAR / FRR are computed from."""
    __tablename__ = "eval_scores"
    id = Column(Integer, primary_key=True, index=True)
    run_id = Column(Integer, ForeignKey("eval_runs.id"), index=True)
    similarity = Column(Float, index=True)
    is_genuine = Column(Boolean, index=True)   # True = same person pair
    run = relationship("EvalRun", back_populates="scores")


def wait_for_db(max_wait_s: int = 60):
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
            time.sleep(2)


def init_db():
    Base.metadata.create_all(bind=engine)
