"""Generate the evaluation report: a text summary + two plots.

Plots (matplotlib, saved as PNG — thesis-ready):
  1. ROC curve: TPR (1-FRR) vs FAR, with the EER point marked + AUC in the title.
  2. Score distributions: histograms of genuine vs impostor similarities, with
     the EER threshold line — visually shows the separation.
"""
import json
import os

import numpy as np

from eval import metrics as M


def write_report(out_dir, dataset, model, genuine, impostor, m, run_id=None):
    os.makedirs(out_dir, exist_ok=True)
    g = np.asarray(genuine, dtype=np.float64)
    imp = np.asarray(impostor, dtype=np.float64)

    # FAR/FRR at a few reference thresholds for the table.
    ref_thresholds = [0.2, 0.3, 0.35, 0.4, 0.45, 0.5, m.eer_threshold]
    table = []
    for t in sorted(set(round(x, 3) for x in ref_thresholds)):
        far, frr = M.far_frr_at(g, imp, t)
        table.append((t, far, frr))

    summary = {
        "dataset": dataset,
        "model": model,
        "run_id": run_id,
        "num_genuine": int(len(g)),
        "num_impostor": int(len(imp)),
        "genuine_mean": round(float(g.mean()), 4),
        "genuine_std": round(float(g.std()), 4),
        "impostor_mean": round(float(imp.mean()), 4),
        "impostor_std": round(float(imp.std()), 4),
        "EER": round(m.eer, 4),
        "EER_threshold": round(m.eer_threshold, 4),
        "AUC": round(m.auc, 4),
        "far_frr_table": [
            {"threshold": t, "FAR": round(far, 4), "FRR": round(frr, 4)}
            for t, far, frr in table
        ],
    }

    # JSON (machine-readable) + a human-readable txt.
    with open(os.path.join(out_dir, "report.json"), "w") as f:
        json.dump(summary, f, indent=2)

    lines = [
        f"Face Recognition Evaluation — {dataset}",
        f"Model: {model}    Run id: {run_id}",
        "=" * 60,
        f"Genuine comparisons : {len(g)}  (mean {g.mean():.3f} ± {g.std():.3f})",
        f"Impostor comparisons: {len(imp)}  (mean {imp.mean():.3f} ± {imp.std():.3f})",
        "",
        f"EER  : {m.eer*100:.2f}%   at threshold {m.eer_threshold:.3f}",
        f"AUC  : {m.auc:.4f}   (1.0 = perfect, 0.5 = random)",
        "",
        "FAR / FRR at reference thresholds:",
        f"  {'threshold':>10}  {'FAR':>8}  {'FRR':>8}",
    ]
    for t, far, frr in table:
        tag = "  <- EER" if abs(t - round(m.eer_threshold, 3)) < 1e-6 else ""
        lines.append(f"  {t:>10.3f}  {far*100:>7.2f}%  {frr*100:>7.2f}%{tag}")
    lines += [
        "",
        "FAR = False Acceptance Rate (impostor wrongly accepted; security risk)",
        "FRR = False Rejection Rate (genuine wrongly rejected; usability)",
        "EER = Equal Error Rate (FAR == FRR); lower is a better recogniser.",
    ]
    txt = "\n".join(lines)
    with open(os.path.join(out_dir, "report.txt"), "w") as f:
        f.write(txt + "\n")
    print("\n" + txt + "\n")

    _plots(out_dir, g, imp, m)
    return summary


def _plots(out_dir, g, imp, m):
    import matplotlib
    matplotlib.use("Agg")  # headless (no display on the server)
    import matplotlib.pyplot as plt

    # --- ROC ---
    tpr = 1.0 - m.frr  # true accept rate
    fig, ax = plt.subplots(figsize=(6, 5))
    ax.plot(m.far, tpr, label=f"ROC (AUC={m.auc:.4f})")
    ax.plot([0, 1], [0, 1], "--", color="gray", label="random")
    # EER point:
    idx = int(np.argmin(np.abs(m.far - m.frr)))
    ax.scatter([m.far[idx]], [tpr[idx]], color="red", zorder=5,
               label=f"EER={m.eer*100:.2f}%")
    ax.set_xlabel("False Acceptance Rate (FAR)")
    ax.set_ylabel("True Acceptance Rate (1 - FRR)")
    ax.set_title("ROC curve")
    ax.legend(loc="lower right")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "roc.png"), dpi=150)
    plt.close(fig)

    # --- Score distributions ---
    fig, ax = plt.subplots(figsize=(6, 5))
    ax.hist(g, bins=50, alpha=0.6, label="genuine (same person)", density=True)
    ax.hist(imp, bins=50, alpha=0.6, label="impostor (different)", density=True)
    ax.axvline(m.eer_threshold, color="red", linestyle="--",
               label=f"EER threshold={m.eer_threshold:.3f}")
    ax.set_xlabel("cosine similarity")
    ax.set_ylabel("density")
    ax.set_title("Genuine vs impostor score distributions")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "score_distributions.png"), dpi=150)
    plt.close(fig)
