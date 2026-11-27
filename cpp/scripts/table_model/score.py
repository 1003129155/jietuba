"""Scores predicted tables against ground truth.

Metrics per image:
  pos     F1 of (row, col, text) over non-empty cells in the unmerged form (text at the top-left of its span)
  adj     F1 of horizontal/vertical neighbour pairs between non-empty cells (ICDAR 2013 style, offset tolerant)
  shape   predicted rows x cols equals ground truth
  perfect pos == 1 and shape matches
  span    merged cells with the same anchor and spans (only counted for ground-truth spans)
with_context(spec) adds the table's title and note to the answer as rows spanning every column, on top and at the
bottom. Answers from collect_web.py --context and render_synthetic.py --context store them as {"text", "box"}; older
synthetic specs store the title as a string, and only text right above or below the table counts.
"""
import copy
import re
import unicodedata

SIM = 0.7


def norm(s):
    s = unicodedata.normalize("NFKC", s)
    return re.sub(r"\s+", "", s)


def lev(a, b):
    if len(a) < len(b):
        a, b = b, a
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def sim(a, b):
    if a == b:
        return 1.0
    if not a or not b:
        return 0.0
    return 1 - lev(a, b) / max(len(a), len(b))


def grid(cells):
    out = {}
    for c in cells:
        t = norm(c["text"])
        if t:
            out[(c["r"], c["c"])] = t
    return out


def neighbours(cells):
    """Pairs (a, b, dir) of non-empty cells adjacent with only empty positions between them."""
    occ = {}
    for c in cells:
        for r in range(c["r"], c["r"] + c["rs"]):
            for k in range(c["c"], c["c"] + c["cs"]):
                occ[(r, k)] = c
    if not occ:
        return set()
    nrows = max(r for r, _ in occ) + 1
    ncols = max(k for _, k in occ) + 1
    pairs = set()
    for c in cells:
        a = norm(c["text"])
        if not a:
            continue
        for r in range(c["r"], c["r"] + c["rs"]):
            k = c["c"] + c["cs"]
            while k < ncols:
                o = occ.get((r, k))
                if o is not None and o is not c and norm(o["text"]):
                    pairs.add((a, norm(o["text"]), "h"))
                    break
                k += 1
        for k in range(c["c"], c["c"] + c["cs"]):
            r = c["r"] + c["rs"]
            while r < nrows:
                o = occ.get((r, k))
                if o is not None and o is not c and norm(o["text"]):
                    pairs.add((a, norm(o["text"]), "v"))
                    break
                r += 1
    return pairs


def f1(tp, npred, ngt):
    p = tp / npred if npred else (1.0 if not ngt else 0.0)
    r = tp / ngt if ngt else 1.0
    return 2 * p * r / (p + r) if p + r else 0.0


def match_pairs(pred, gt):
    used = set()
    tp = 0
    gt = list(gt)
    for a, b, d in pred:
        for i, (ga, gb, gd) in enumerate(gt):
            if i not in used and gd == d and sim(a, ga) >= SIM and sim(b, gb) >= SIM:
                used.add(i)
                tp += 1
                break
    return tp


def score(spec, pred_cells):
    gt_cells = spec["cells"]
    g, p = grid(gt_cells), grid(pred_cells)
    tp = sum(1 for k, t in g.items() if k in p and sim(p[k], t) >= SIM)
    pos = f1(tp, len(p), len(g))
    adj = f1(match_pairs(neighbours(pred_cells), neighbours(gt_cells)), len(neighbours(pred_cells)), len(neighbours(gt_cells)))
    prow = max((c["r"] + c["rs"] for c in pred_cells), default=0)
    pcol = max((c["c"] + c["cs"] for c in pred_cells), default=0)
    shape = prow == spec["rows"] and pcol == spec["cols"]
    spans = [c for c in gt_cells if c["rs"] > 1 or c["cs"] > 1]
    pmap = {(c["r"], c["c"]): c for c in pred_cells}
    span_ok = sum(1 for c in spans if (o := pmap.get((c["r"], c["c"]))) and o["rs"] == c["rs"] and o["cs"] == c["cs"])
    return {"pos": pos, "adj": adj, "shape": shape, "perfect": shape and pos == 1.0, "spans": len(spans), "span_ok": span_ok,
            "prow": prow, "pcol": pcol}


NOTE_STARTS = ("注", "note")


def context_texts(spec):
    title, note = spec.get("title"), spec.get("note")
    t = title["text"] if isinstance(title, dict) else None
    if isinstance(title, str) and title and not spec.get("para_above") and "outside" not in spec:
        t = title
    n = note["text"] if isinstance(note, dict) else None
    below = spec.get("para_below")
    if n is None and "outside" not in spec and isinstance(below, str) and below.lower().startswith(NOTE_STARTS):
        n = below
    return t, n


def with_context(spec):
    t, n = context_texts(spec)
    if t is None and n is None:
        return spec
    out = copy.deepcopy(spec)
    cells = out["cells"]
    if t is not None:
        for c in cells:
            c["r"] += 1
        cells.append({"r": 0, "c": 0, "rs": 1, "cs": spec["cols"], "text": t})
        out["rows"] += 1
    if n is not None:
        cells.append({"r": out["rows"], "c": 0, "rs": 1, "cs": spec["cols"], "text": n})
        out["rows"] += 1
    return out
