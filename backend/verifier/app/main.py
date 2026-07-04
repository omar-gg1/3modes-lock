"""Mode 3 cloud verifier — ArcFace second-opinion face verification.

The ESP32 recognizes locally and only asks here when it is UNSURE (murky
confidence). This service embeds the face with ArcFace and compares it to the
enrolled reference embeddings, returning a confident verdict.

Endpoints (all behind X-API-Key):
  GET  /            health
  POST /enroll      register a reference face (multipart image + user_id + name)
  POST /verify      second-opinion match (multipart image) -> {match,user_id,...}
  GET  /encodings   how many references are enrolled (diagnostics)

Runs as its OWN container so a heavy/slow model can never take down the Mode 2
event pipeline (broker/API/MySQL).
"""
import logging
import os
from contextlib import asynccontextmanager

import cv2
import numpy as np
from fastapi import FastAPI, File, Form, UploadFile, Header, HTTPException

from . import face_engine, storage

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
log = logging.getLogger("verifier")

API_KEY = os.environ.get("VERIFIER_API_KEY", "")

# Match threshold on cosine similarity. Calibrate against real FAR/FRR data
# (Phase B). ArcFace: same person ~0.4-0.8, different <~0.3, so 0.35-0.5 is the
# usual operating range. Start conservative-ish; tune from the ROC curve.
MATCH_THRESHOLD = float(os.environ.get("MATCH_THRESHOLD", "0.40"))

# Enrollment quality gate — warn (don't hard-fail) on small/low-confidence faces
# so users know an ESP-captured enrollment may be poor.
ENROLL_MIN_DET_SCORE = 0.60
ENROLL_MIN_BOX = 80  # px; a face smaller than this in the frame is low quality


@asynccontextmanager
async def lifespan(app: FastAPI):
    storage.wait_for_db()
    storage.init_db()
    # NOTE: we do NOT preload the model here — it's loaded lazily on first
    # /enroll or /verify so the service comes up fast and healthchecks pass even
    # while the model file is still downloading on first boot.
    yield


app = FastAPI(title="Smart Lock — Mode 3 Cloud Verifier", lifespan=lifespan)


def _check_key(x_api_key: str | None):
    if not API_KEY:
        # Misconfiguration guard: refuse to run wide-open.
        raise HTTPException(500, "verifier API key not configured")
    if x_api_key != API_KEY:
        raise HTTPException(401, "invalid or missing X-API-Key")


def _decode_image(data: bytes):
    """Decode uploaded JPEG/PNG bytes into a BGR OpenCV image."""
    arr = np.frombuffer(data, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        raise HTTPException(400, "could not decode image")
    return img


@app.get("/")
def health():
    return {"status": "ok", "service": "cloud-verifier",
            "enrolled": storage.count_encodings(),
            "threshold": MATCH_THRESHOLD}


@app.post("/enroll")
async def enroll(
    user_id: int = Form(...),
    name: str = Form(...),
    source: str = Form("app"),
    image: UploadFile = File(...),
    x_api_key: str | None = Header(default=None),
):
    _check_key(x_api_key)
    img = _decode_image(await image.read())
    emb, quality = face_engine.embed_largest_face(img)
    if emb is None:
        raise HTTPException(422, {"error": "no usable face", "detail": quality})

    # Quality warnings (non-fatal): a poor reference degrades all future matches.
    warnings = []
    if quality["det_score"] < ENROLL_MIN_DET_SCORE:
        warnings.append("low_detector_confidence")
    if min(quality["box_w"], quality["box_h"]) < ENROLL_MIN_BOX:
        warnings.append("face_too_small")
    if quality["num_faces"] > 1:
        warnings.append("multiple_faces_used_largest")

    storage.save_encoding(user_id, name, emb, source, quality["det_score"])
    log.info("enrolled user_id=%s name=%s source=%s det=%.3f warnings=%s",
             user_id, name, source, quality["det_score"], warnings)
    return {"enrolled": True, "user_id": user_id, "name": name,
            "quality": quality, "warnings": warnings,
            "total_encodings": storage.count_encodings()}


@app.post("/verify")
async def verify(
    image: UploadFile = File(...),
    x_api_key: str | None = Header(default=None),
):
    _check_key(x_api_key)
    img = _decode_image(await image.read())
    emb, quality = face_engine.embed_largest_face(img)
    if emb is None:
        return {"match": False, "reason": "no_face", "detail": quality}

    enrolled = storage.load_all_encodings()
    if not enrolled:
        return {"match": False, "reason": "no_enrolled_faces"}

    user_id, sim = face_engine.best_match(emb, enrolled)
    match = sim >= MATCH_THRESHOLD
    log.info("verify -> match=%s user_id=%s similarity=%.3f (thr=%.2f)",
             match, user_id if match else None, sim, MATCH_THRESHOLD)
    return {
        "match": match,
        "user_id": user_id if match else None,
        "confidence": round(sim, 4),      # cosine similarity
        "threshold": MATCH_THRESHOLD,
        "quality": quality,
    }


@app.get("/encodings")
def encodings(x_api_key: str | None = Header(default=None)):
    _check_key(x_api_key)
    return {"total_encodings": storage.count_encodings()}
