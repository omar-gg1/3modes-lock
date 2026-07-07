"""Face embedding + matching engine (ArcFace via InsightFace).

WHY pre-trained, not trained-from-scratch: a face recognizer needs the ability
to turn ANY face into a discriminative vector — that ability is learned once by
researchers on millions of faces (ArcFace's whole contribution is a loss
function, a published paper). We DOWNLOAD those weights and only ENROLL specific
people (store their vectors). Training our own on a few faces would overfit and
perform worse. The rigor of this project is the hybrid architecture + measured
FAR/FRR, not re-inventing the model.

Model: InsightFace 'buffalo_l' (ArcFace r100) — SOTA (~99.83% LFW), runs on CPU
via ONNX Runtime. If RAM is tight on the EC2 we can swap to 'buffalo_s' (smaller,
slightly less accurate) by changing MODEL_PACK.
"""
import logging
import os

import numpy as np

log = logging.getLogger("face_engine")

# 'buffalo_l' = full ArcFace r100 (best accuracy, ~1GB RAM).
# 'buffalo_s' = lighter (~lower RAM) if the EC2 struggles. Override via env.
MODEL_PACK = os.environ.get("INSIGHTFACE_MODEL", "buffalo_l")

_app = None  # lazily-initialised FaceAnalysis (loading the model is expensive)


def _get_app():
    """Load the InsightFace model once, on first use. CPU-only provider."""
    global _app
    if _app is None:
        # Imported lazily so the module imports fast and a missing model only
        # errors when actually used (keeps the API up even mid-download).
        from insightface.app import FaceAnalysis
        log.info("loading InsightFace model pack '%s' (CPU)...", MODEL_PACK)
        app = FaceAnalysis(name=MODEL_PACK, providers=["CPUExecutionProvider"])
        # det_size: the face-detector input. 640 is the default; smaller = faster
        # but misses tiny faces. Our images are close-up faces, 640 is plenty.
        app.prepare(ctx_id=-1, det_size=(640, 640))  # ctx_id=-1 => CPU
        _app = app
        log.info("InsightFace model ready")
    return _app


def embed_largest_face(image_bgr):
    """Detect faces in a BGR image and return (embedding, quality) for the
    LARGEST face (the subject), or (None, reason) if no usable face.

    - embedding: L2-normalised 512-d float32 vector (unit length), so a plain
      dot product between two embeddings IS their cosine similarity.
    - quality: dict with det_score + face box size, used to warn on poor
      enrollments (small/blurry faces make bad reference embeddings).
    """
    import cv2
    # Small sources (the ESP32 sends QVGA 320x240 frames) give the detector a
    # face near its minimum size. Upscale 2x so detection is reliable; the
    # recognition crop is normalised to 112x112 afterwards either way, so this
    # helps detection without distorting recognition.
    h, w = image_bgr.shape[:2]
    upscaled = False
    if max(h, w) < 500:
        image_bgr = cv2.resize(image_bgr, None, fx=2, fy=2,
                               interpolation=cv2.INTER_CUBIC)
        upscaled = True

    app = _get_app()
    faces = app.get(image_bgr)
    if not faces:
        return None, {"error": "no_face_detected", "upscaled": upscaled,
                      "src_w": w, "src_h": h}

    # Pick the largest face (biggest bounding-box area) = the intended subject,
    # not a bystander in the background.
    def area(f):
        x1, y1, x2, y2 = f.bbox
        return (x2 - x1) * (y2 - y1)

    face = max(faces, key=area)

    emb = face.normed_embedding  # already L2-normalised by InsightFace
    emb = np.asarray(emb, dtype=np.float32)

    x1, y1, x2, y2 = face.bbox
    quality = {
        "det_score": float(face.det_score),   # detector confidence 0..1
        "box_w": int(x2 - x1),
        "box_h": int(y2 - y1),
        "num_faces": len(faces),
    }
    return emb, quality


def embed_aligned_face(image_bgr):
    """Embed an ALREADY-cropped/aligned face directly, bypassing detection.

    Used for benchmark datasets (e.g. LFW) whose images are pre-cropped tightly
    to the face — too tight for the scene detector to re-detect, but perfect to
    feed straight into the recognition model. Resizes to the recognition input
    (112x112) and returns the L2-normalised 512-d embedding, or None on failure.
    """
    import cv2
    app = _get_app()
    # The recognition sub-model is registered under 'recognition'.
    rec = app.models.get("recognition")
    if rec is None:
        return None
    face = cv2.resize(image_bgr, (112, 112), interpolation=cv2.INTER_CUBIC)
    emb = rec.get_feat(face).flatten()
    emb = np.asarray(emb, dtype=np.float32)
    n = np.linalg.norm(emb)
    if n > 0:
        emb = emb / n     # L2-normalise so dot product == cosine similarity
    return emb


def cosine_similarity(a, b):
    """Cosine similarity of two L2-normalised vectors = their dot product.
    Range [-1, 1]; same person ~0.4-0.8, different person <~0.3 with ArcFace."""
    return float(np.dot(a, b))


def best_match(query_emb, enrolled):
    """Compare a query embedding against enrolled (user_id, embedding) pairs.
    Returns (best_user_id, best_similarity). enrolled may hold several vectors
    per user (multiple photos) — we take the single closest, which is robust to
    pose/lighting because a match only needs to be near ONE stored look."""
    best_uid, best_sim = None, -1.0
    for uid, emb in enrolled:
        sim = cosine_similarity(query_emb, emb)
        if sim > best_sim:
            best_sim, best_uid = sim, uid
    return best_uid, best_sim
