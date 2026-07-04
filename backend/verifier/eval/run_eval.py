"""Run a face-recognition evaluation and produce a full report.

Pipeline: load dataset pairs -> embed each image with ArcFace (in-process, the
same model the production /verify uses) -> cosine-score every pair -> split into
genuine/impostor score arrays -> compute FAR/FRR/EER/ROC/AUC -> persist to the
eval tables -> write a text report + ROC/distribution plots.

Usage (inside the verifier container or any env with the deps):
  python -m eval.run_eval --dataset lfw --subset test
  python -m eval.run_eval --dataset folder --root /data/my_people
"""
import argparse
import logging
import os

import cv2
import numpy as np

# Import the SAME engine production uses, so eval numbers reflect production.
import sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from app import face_engine  # noqa: E402
from eval import datasets, metrics, eval_storage, report  # noqa: E402

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
log = logging.getLogger("run_eval")


def _embed_path(path):
    img = cv2.imread(path, cv2.IMREAD_COLOR)
    if img is None:
        return None
    emb, _ = face_engine.embed_largest_face(img)
    return emb


def _embed_img(img_bgr):
    emb, _ = face_engine.embed_largest_face(img_bgr)
    return emb


def run_lfw(subset: str):
    pairs, labels = datasets.build_lfw_pairs(subset=subset)
    genuine, impostor = [], []
    skipped = 0
    # LFW images are already tightly cropped/aligned to the face — too tight for
    # the scene detector, so embed them directly (detection-free).
    for i, ((a, b), lab) in enumerate(zip(pairs, labels)):
        ea = face_engine.embed_aligned_face(a)
        eb = face_engine.embed_aligned_face(b)
        if ea is None or eb is None:
            skipped += 1
            continue
        sim = face_engine.cosine_similarity(ea, eb)
        (genuine if lab == 1 else impostor).append(sim)
        if (i + 1) % 200 == 0:
            log.info("  scored %d/%d pairs (skipped %d no-face)",
                     i + 1, len(labels), skipped)
    log.info("done: %d genuine, %d impostor, %d skipped",
             len(genuine), len(impostor), skipped)
    return genuine, impostor, None


def run_folder(root: str):
    pairs, n_ident = datasets.build_folder_pairs(root)
    # Cache embeddings per path so each image is embedded once.
    cache = {}

    def emb(p):
        if p not in cache:
            cache[p] = _embed_path(p)
        return cache[p]

    genuine, impostor = [], []
    skipped = 0
    for a, b, is_gen in pairs:
        ea, eb = emb(a), emb(b)
        if ea is None or eb is None:
            skipped += 1
            continue
        sim = face_engine.cosine_similarity(ea, eb)
        (genuine if is_gen else impostor).append(sim)
    log.info("done: %d genuine, %d impostor, %d skipped",
             len(genuine), len(impostor), skipped)
    return genuine, impostor, n_ident


def persist(dataset, model, genuine, impostor, m, n_ident):
    eval_storage.wait_for_db()
    eval_storage.init_db()
    s = eval_storage.SessionLocal()
    try:
        run = eval_storage.EvalRun(
            dataset=dataset, model=model,
            num_identities=n_ident or 0,
            num_genuine=len(genuine), num_impostor=len(impostor),
            eer=m.eer, eer_threshold=m.eer_threshold, auc=m.auc,
        )
        s.add(run)
        s.flush()  # get run.id
        # Store each score (bulk). Keeps ROC reproducible + enables insights.
        rows = [eval_storage.EvalScore(run_id=run.id, similarity=float(x),
                                       is_genuine=True) for x in genuine]
        rows += [eval_storage.EvalScore(run_id=run.id, similarity=float(x),
                                        is_genuine=False) for x in impostor]
        s.bulk_save_objects(rows)
        s.commit()
        log.info("persisted eval run id=%d (%d scores)", run.id, len(rows))
        return run.id
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dataset", choices=["lfw", "folder"], default="lfw")
    ap.add_argument("--subset", default="test", help="LFW subset: train/test/10_folds")
    ap.add_argument("--root", default="/data/my_people", help="folder dataset root")
    ap.add_argument("--out", default="/data/eval_report", help="report output dir")
    ap.add_argument("--no-persist", action="store_true", help="skip DB write")
    args = ap.parse_args()

    model = os.environ.get("INSIGHTFACE_MODEL", "buffalo_l")

    if args.dataset == "lfw":
        genuine, impostor, n_ident = run_lfw(args.subset)
        dataset_name = f"lfw_{args.subset}"
    else:
        genuine, impostor, n_ident = run_folder(args.root)
        dataset_name = f"folder:{os.path.basename(args.root.rstrip('/'))}"

    m = metrics.compute(genuine, impostor)
    log.info("EER=%.4f @thr=%.3f  AUC=%.4f", m.eer, m.eer_threshold, m.auc)

    run_id = None
    if not args.no_persist:
        run_id = persist(dataset_name, model, genuine, impostor, m, n_ident)

    os.makedirs(args.out, exist_ok=True)
    report.write_report(args.out, dataset_name, model, genuine, impostor, m,
                        run_id=run_id)
    log.info("report written to %s", args.out)


if __name__ == "__main__":
    main()
