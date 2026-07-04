"""Dataset loaders for evaluation.

LFW (Labeled Faces in the Wild) is the standard face-verification benchmark:
~13k images of ~5.7k people. We build genuine (same-person) and impostor
(different-person) IMAGE PAIRS, embed each image once with ArcFace, and score
each pair by cosine similarity — the raw material for FAR/FRR/EER/ROC.

Two sources:
  - build_lfw_pairs(): uses scikit-learn's fetch_lfw_pairs (auto-downloads,
    gives the canonical 6000 pairs = 3000 genuine + 3000 impostor). Best for a
    reproducible, publishable benchmark number.
  - build_folder_pairs(): for YOUR OWN people later — a folder per person, all
    same-person combos are genuine, cross-person are impostor.
"""
import itertools
import logging
import os

import numpy as np

log = logging.getLogger("datasets")


def build_lfw_pairs(subset: str = "test", color: bool = True):
    """Return (pairs, labels) where pairs is a list of (imgA, imgB) BGR uint8
    arrays and labels is 1 (genuine/same) or 0 (impostor/different).

    Uses sklearn's canonical LFW pairs (auto-downloads ~200MB on first call).
    """
    from sklearn.datasets import fetch_lfw_pairs

    # funneled + resize keeps faces aligned/consistent; color for ArcFace.
    log.info("fetching LFW pairs (subset=%s) — first run downloads ~200MB...", subset)
    data = fetch_lfw_pairs(subset=subset, color=color, resize=1.0,
                           funneled=True)
    # data.pairs shape: (n_pairs, 2, H, W, 3) float in [0,1] (RGB).
    pairs_raw = data.pairs
    labels = data.target  # 1 = same, 0 = different

    pairs = []
    for a, b in pairs_raw:
        pairs.append((_to_bgr_uint8(a), _to_bgr_uint8(b)))
    log.info("LFW: %d pairs (%d genuine, %d impostor)",
             len(labels), int((labels == 1).sum()), int((labels == 0).sum()))
    return pairs, labels.astype(int)


def _to_bgr_uint8(img_float_rgb):
    """sklearn gives float [0,1] RGB; convert to uint8 BGR for OpenCV/InsightFace.
    LFW images are pre-cropped/aligned faces, embedded detection-free by the
    runner (embed_aligned_face), so no upscaling/detection margin is needed."""
    import cv2
    img = (np.clip(img_float_rgb, 0, 1) * 255).astype(np.uint8)
    return cv2.cvtColor(img, cv2.COLOR_RGB2BGR)


def build_folder_pairs(root: str):
    """For your own people: root/<person>/*.jpg. Returns (image_paths grouped)
    then all genuine (same-person) and impostor (cross-person) pairs as file
    paths (loaded lazily by the runner to save RAM on big sets).

    Returns list of (pathA, pathB, is_genuine)."""
    people = {}
    for person in sorted(os.listdir(root)):
        pdir = os.path.join(root, person)
        if not os.path.isdir(pdir):
            continue
        imgs = [os.path.join(pdir, f) for f in sorted(os.listdir(pdir))
                if f.lower().endswith((".jpg", ".jpeg", ".png"))]
        if imgs:
            people[person] = imgs

    pairs = []
    names = list(people)
    # Genuine: every unordered pair of photos WITHIN a person.
    for person in names:
        for a, b in itertools.combinations(people[person], 2):
            pairs.append((a, b, True))
    # Impostor: one photo from each of two different people (cap to keep it
    # balanced-ish; all-vs-all across people explodes combinatorially).
    for pa, pb in itertools.combinations(names, 2):
        for a in people[pa][:3]:
            for b in people[pb][:3]:
                pairs.append((a, b, False))

    n_gen = sum(1 for _, _, g in pairs if g)
    log.info("folder pairs: %d people, %d genuine, %d impostor",
             len(names), n_gen, len(pairs) - n_gen)
    return pairs, len(names)
