"""How well paragraphs were found, measured on the lines that have a true paragraph.

The headline number is the share of text characters that end up in exactly their true paragraph: what reaches
the translator in the right unit. Beside it: pair F1 (two lines of one paragraph together, two of different
ones apart), how many produced paragraphs mix several true ones (over-merge), and how many true multi-line
paragraphs got cut (split)."""
from collections import defaultdict

import numpy as np

KEYS = ("lines", "pairs_pred", "pairs_gt", "pairs_tp", "n_pred", "n_gt", "matched", "chars_total", "chars_ok", "multi_gt", "multi_ok",
        "single_gt", "single_ok", "over_paras", "split_paras")


def _c2(n):
    return n * (n - 1) // 2


def case_counts(texts, labels, paragraphs):
    n = len(labels)
    cluster = [-1] * n
    for k, p in enumerate(paragraphs):
        for i in p:
            cluster[i] = k
    for i in range(n):
        if cluster[i] < 0:
            cluster[i] = len(paragraphs) + i
    scored = [i for i in range(n) if labels[i] is not None]
    pred, true, both = defaultdict(list), defaultdict(list), defaultdict(int)
    for i in scored:
        pred[cluster[i]].append(i)
        true[labels[i]].append(i)
        both[(cluster[i], labels[i])] += 1
    true_sets = {frozenset(v) for v in true.values()}
    pred_sets = {frozenset(v) for v in pred.values()}
    chars = lambda idx: sum(len(texts[i]) for i in idx)
    matched = [v for v in pred.values() if frozenset(v) in true_sets]
    multi = [v for v in true.values() if len(v) > 1]
    single = [v for v in true.values() if len(v) == 1]
    return {
        "lines": len(scored), "pairs_pred": sum(_c2(len(v)) for v in pred.values()), "pairs_gt": sum(_c2(len(v)) for v in true.values()),
        "pairs_tp": sum(_c2(v) for v in both.values()), "n_pred": len(pred), "n_gt": len(true), "matched": len(matched),
        "chars_total": chars(scored), "chars_ok": sum(chars(v) for v in matched),
        "multi_gt": len(multi), "multi_ok": sum(frozenset(v) in pred_sets for v in multi), "single_gt": len(single), "single_ok": sum(frozenset(v) in pred_sets for v in single),
        "over_paras": sum(1 for v in pred.values() if len({labels[i] for i in v}) > 1), "split_paras": sum(1 for v in multi if len({cluster[i] for i in v}) > 1)}


def summary(rows):
    total = {k: sum(r[k] for r in rows) for k in KEYS}
    ratio = lambda a, b: total[a] / total[b] if total[b] else float("nan")
    p, r = ratio("pairs_tp", "pairs_pred"), ratio("pairs_tp", "pairs_gt")
    return {"chars": ratio("chars_ok", "chars_total"), "pair_f1": 2 * p * r / (p + r) if p + r else 0.0, "over_merge": ratio("over_paras", "n_pred"),
            "split": ratio("split_paras", "multi_gt"), "multi": ratio("multi_ok", "multi_gt"), "single": ratio("single_ok", "single_gt"),
            "lines": total["lines"], "paragraphs": total["n_gt"]}


def paired_difference(rows_a, rows_b, draws=2000, seed=5):
    """Mean and 95% interval of (chars share of a) - (chars share of b), resampling pages."""
    ok_a = np.array([r["chars_ok"] for r in rows_a], float)
    ok_b = np.array([r["chars_ok"] for r in rows_b], float)
    total = np.array([r["chars_total"] for r in rows_a], float)
    rnd = np.random.default_rng(seed)
    diffs = []
    for _ in range(draws):
        pick = rnd.integers(0, len(total), len(total))
        if total[pick].sum():
            diffs.append(ok_a[pick].sum() / total[pick].sum() - ok_b[pick].sum() / total[pick].sum())
    low, high = np.percentile(diffs, [2.5, 97.5])
    return float(np.mean(diffs)), float(low), float(high)
