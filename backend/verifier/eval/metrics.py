"""Biometric accuracy metrics: FAR, FRR, ROC, EER, AUC.

Definitions (the exact ones examiners expect):
  - GENUINE comparison  : two images of the SAME person. Should score HIGH.
  - IMPOSTOR comparison  : two images of DIFFERENT people. Should score LOW.
  - At a decision threshold T (accept if similarity >= T):
      FAR(T) = # impostor pairs scoring >= T  / # impostor pairs
               ("False Acceptance Rate" — a stranger wrongly accepted; SECURITY)
      FRR(T) = # genuine  pairs scoring <  T  / # genuine  pairs
               ("False Rejection Rate" — a real user wrongly rejected; USABILITY)
  - Sweeping T over all observed scores traces the ROC curve (FAR vs 1-FRR, or
    FAR vs FRR).
  - EER (Equal Error Rate) = the error at the threshold where FAR == FRR. One
    headline number; lower is a better recogniser. The EER threshold is a
    principled default operating point.
  - AUC = area under the ROC (TPR vs FPR); 1.0 = perfect, 0.5 = random.
"""
from dataclasses import dataclass
import numpy as np


@dataclass
class Metrics:
    thresholds: np.ndarray   # swept thresholds
    far: np.ndarray          # FAR at each threshold
    frr: np.ndarray          # FRR at each threshold
    eer: float
    eer_threshold: float
    auc: float


def compute(genuine_scores, impostor_scores, num_points: int = 500) -> Metrics:
    """genuine_scores / impostor_scores: 1-D arrays of similarity values."""
    g = np.asarray(genuine_scores, dtype=np.float64)
    imp = np.asarray(impostor_scores, dtype=np.float64)
    if len(g) == 0 or len(imp) == 0:
        raise ValueError("need at least one genuine and one impostor score")

    lo = min(g.min(), imp.min())
    hi = max(g.max(), imp.max())
    thresholds = np.linspace(lo, hi, num_points)

    # FAR: fraction of impostors accepted (>= T). FRR: fraction of genuine
    # rejected (< T). Vectorised over all thresholds.
    far = np.array([(imp >= t).mean() for t in thresholds])
    frr = np.array([(g < t).mean() for t in thresholds])

    # EER: where |FAR - FRR| is minimised.
    idx = int(np.argmin(np.abs(far - frr)))
    eer = float((far[idx] + frr[idx]) / 2.0)
    eer_threshold = float(thresholds[idx])

    auc = _roc_auc(g, imp)

    return Metrics(thresholds=thresholds, far=far, frr=frr,
                   eer=eer, eer_threshold=eer_threshold, auc=auc)


def _roc_auc(genuine, impostor) -> float:
    """AUC via the probability that a random genuine score exceeds a random
    impostor score (equivalent to the Mann-Whitney U statistic / ROC AUC)."""
    g = np.sort(genuine)
    n_g, n_i = len(genuine), len(impostor)
    # For each impostor score, count genuine scores strictly greater (+0.5 ties).
    wins = 0.0
    for s in impostor:
        gt = n_g - np.searchsorted(g, s, side="right")
        eq = np.searchsorted(g, s, side="right") - np.searchsorted(g, s, side="left")
        wins += gt + 0.5 * eq
    return float(wins / (n_g * n_i))


def far_frr_at(genuine_scores, impostor_scores, threshold: float):
    """FAR and FRR at a single specific operating threshold (e.g. the one the
    lock will actually use)."""
    g = np.asarray(genuine_scores, dtype=np.float64)
    imp = np.asarray(impostor_scores, dtype=np.float64)
    far = float((imp >= threshold).mean())
    frr = float((g < threshold).mean())
    return far, frr
