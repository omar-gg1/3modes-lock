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
    try:
        emb, quality = face_engine.embed_largest_face(img)
    except Exception as e:
        log.exception("enroll: embedding failed")
        raise HTTPException(422, {"error": "processing_error", "detail": str(e)})
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
    # Never 500 on a bad frame: the lock treats HTTP!=200 as "cloud unreachable"
    # and falls back locally, which hides the real reason. A processing failure
    # is a definitive "couldn't judge this image" — return it as structured JSON.
    try:
        emb, quality = face_engine.embed_largest_face(img)
    except Exception as e:
        log.exception("verify: embedding failed (img %sx%s)",
                      img.shape[1], img.shape[0])
        return {"match": False, "reason": "processing_error", "detail": str(e)}
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


# ---- Evaluation insights (Phase B) --------------------------------------
# Read-only views over the eval_runs / eval_scores tables so you can pull live
# FAR/FRR/EER/score-distributions for any past evaluation run without re-running
# it. The heavy run itself is done offline via `python -m eval.run_eval`.

@app.get("/eval/runs")
def eval_runs(x_api_key: str | None = Header(default=None)):
    """List all evaluation runs with their headline metrics."""
    _check_key(x_api_key)
    from eval import eval_storage
    s = eval_storage.SessionLocal()
    try:
        runs = s.query(eval_storage.EvalRun).order_by(
            eval_storage.EvalRun.id.desc()).all()
        return [{
            "id": r.id, "dataset": r.dataset, "model": r.model,
            "num_identities": r.num_identities,
            "num_genuine": r.num_genuine, "num_impostor": r.num_impostor,
            "eer": r.eer, "eer_threshold": r.eer_threshold, "auc": r.auc,
            "created_at": r.created_at.isoformat() if r.created_at else None,
        } for r in runs]
    finally:
        s.close()


@app.get("/eval/runs/{run_id}")
def eval_run_detail(run_id: int, threshold: float | None = None,
                    x_api_key: str | None = Header(default=None)):
    """Full metrics for one run, recomputed live from its stored scores. Pass
    ?threshold=0.4 to also get FAR/FRR at that specific operating point."""
    _check_key(x_api_key)
    from eval import eval_storage, metrics as M
    s = eval_storage.SessionLocal()
    try:
        run = s.get(eval_storage.EvalRun, run_id)
        if run is None:
            raise HTTPException(404, "run not found")
        scores = s.query(eval_storage.EvalScore).filter_by(run_id=run_id).all()
        genuine = [x.similarity for x in scores if x.is_genuine]
        impostor = [x.similarity for x in scores if not x.is_genuine]
        m = M.compute(genuine, impostor)
        out = {
            "run_id": run_id, "dataset": run.dataset, "model": run.model,
            "num_genuine": len(genuine), "num_impostor": len(impostor),
            "eer": round(m.eer, 4), "eer_threshold": round(m.eer_threshold, 4),
            "auc": round(m.auc, 4),
        }
        if threshold is not None:
            far, frr = M.far_frr_at(genuine, impostor, threshold)
            out["at_threshold"] = {"threshold": threshold,
                                   "FAR": round(far, 4), "FRR": round(frr, 4)}
        return out
    finally:
        s.close()
