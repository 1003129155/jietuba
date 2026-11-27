"""Training the table models and turning them into src/text_recognition/table_{row,extent,caption}_model.inc.

The measurements come from the application itself (jietuba_table_tool build), so training and the shipped code
never disagree about a feature. Labels come from the answer's cell boxes: each segment belongs to the cell its centre
falls in.
- rows (--kind rows): a pair of consecutive visual rows joins when both rows hold text of one table row and parts when
  the lower row's table rows all come after the upper row's. Pairs with unplaced text are left out.
- extent (--kind extent): a visual row belongs to the table when any of its text falls in a cell; titles, captions
  and notes around the table do not.
- caption (--kind caption): a row just above the table is its title when its text lies in the answer's title box, a
  row just below is its note when it lies in the note box (answers from --context collection); others are not.

  python train.py fit --traces T.jsonl... --model M.pkl [--val-frac 0.2]
  python train.py given --traces T.jsonl --model M.pkl --out G.json     decisions for jietuba_table_tool --given
  python train.py oracle --traces T.jsonl --out G.json                   the answer's own decisions, as an upper bound
  python train.py export --model M.pkl --names N.txt --out table_row_model.inc
  python train.py check --kind K --model M.pkl --traces T.jsonl...       the built tool decides as the model does
A trace file is the --out of jietuba_table_tool build; its keys are <folder name>/<image name>, and the answers
are found next to the images, under --root (default data/). pipeline.sh runs the whole training.
"""
import argparse
import hashlib
import json
import pickle
import sys
from pathlib import Path

import numpy as np

KIND = "rows"
# Feature positions (ROW_FEATURE_NAMES in table.cpp): the pair's rows the rules centred over a row span.
CENTRED_UPPER, CENTRED_LOWER = 50, 51
SPAN_LABELS = "rules"
# The trees overrule the rules only with this much confidence (ROW_OVERRIDE_MARGIN in table.cpp).
MARGIN = 0.5


# Extent feature positions (EXTENT_FEATURE_NAMES): the most column boundaries one segment runs through, and their number.
STRADDLE_SEG, BOUNDS = 14, 16


def decide(scores, rules, features=None):
    out = np.where(np.asarray(rules) == 1, np.asarray(scores) >= -MARGIN, np.asarray(scores) >= MARGIN).astype(int)
    if KIND == "extent" and features is not None:
        # As in table_extent: a row the rules left out whose segment crosses several boundaries stays out.
        f = np.asarray(features)
        need = np.maximum(2, np.floor(f[:, BOUNDS] / 3))
        out = np.where((np.asarray(rules) == 0) & (f[:, STRADDLE_SEG] >= need), 0, out)
    return out
PARAMETERS = dict(max_depth=5, max_iter=250, learning_rate=0.08, l2_regularization=1.0, random_state=0, early_stopping=False)


def load_traces(paths):
    for path in paths:
        for line in open(path, encoding="utf-8"):
            yield json.loads(line)


def cell_of(cells, x, y):
    best = None
    for c in cells:
        b = c.get("box")
        if b and b[0] <= x <= b[2] and b[1] <= y <= b[3]:
            area = (b[2] - b[0]) * (b[3] - b[1])
            if best is None or area < best[0]:
                best = (area, c)
    return best[1] if best else None


def row_labels(trace, spec, spanning=None):
    """Per visual row: the set of answer rows its single-row cells belong to (empty when none).
    spanning, when given, maps each row whose placed text all belongs to row-spanning cells to the
    first answer row those cells cover."""
    out = []
    for k, segs in enumerate(trace["rows"]):
        rows, starts = set(), []
        for x0, y0, x1, y1, _ in segs:
            c = cell_of(spec["cells"], (x0 + x1) / 2, (y0 + y1) / 2)
            if c is not None and c["text"].strip():
                if c["rs"] == 1:
                    rows.add(c["r"])
                else:
                    starts.append(c["r"])
        if spanning is not None and starts and not rows:
            spanning[k] = min(starts)
        out.append(rows)
    return out


def pair_labels(trace, spec):
    spanning = {}
    rows = row_labels(trace, spec, spanning)
    out = []
    for i, p in enumerate(trace["pairs"]):
        a, b = rows[i], rows[i + 1]
        # A row holding only text of row-spanning cells. "start": it belongs with the row its cells
        # start in, joining the row above when they start there, else the row below when they start
        # there. "rules": next to a row the rules centred over a span, the rules' placement is right.
        if SPAN_LABELS == "start" and i + 1 in spanning:
            out.append(None if not a else int(spanning[i + 1] in a))
        elif SPAN_LABELS == "start" and i in spanning:
            above = rows[i - 1] if i > 0 else set()
            out.append(None if not b else int(spanning[i] in b and spanning[i] not in above))
        elif SPAN_LABELS == "rules" and ((p["f"][CENTRED_UPPER] == 1 and i in spanning) or (p["f"][CENTRED_LOWER] == 1 and i + 1 in spanning)):
            out.append(int(p["rules"]))
        elif not a or not b:
            out.append(None)
        elif a & b:
            out.append(1)
        elif max(a) < min(b):
            out.append(0)
        else:
            out.append(None)
    return out


def extent_labels(trace, spec):
    if not any("box" in c for c in spec["cells"]):
        return [None] * len(trace["all_rows"])
    return [int(any(cell_of(spec["cells"], (x0 + x1) / 2, (y0 + y1) / 2) is not None for x0, y0, x1, y1, _ in segs))
            for segs in trace["all_rows"]]


def caption_labels(trace, spec):
    if "outside" not in spec:
        return [None] * len(trace["captions"])
    out = []
    for c in trace["captions"]:
        target = spec.get("title" if c["above"] else "note")
        if not isinstance(target, dict):
            out.append(0)
            continue
        x0, y0, x1, y1 = target["box"]
        segs = trace["all_rows"][c["row"]]
        inside = sum(1 for a, b, cc, d, _ in segs if x0 - 3 <= (a + cc) / 2 <= x1 + 3 and y0 - 3 <= (b + d) / 2 <= y1 + 3)
        out.append(int(2 * inside >= len(segs)))
    return out


def by_row(trace, values):
    """Caption decisions go to jietuba_table_tool per visual row; the other kinds are already in order."""
    if KIND != "caption":
        return values
    out = [-1] * len(trace["all_rows"])
    for c, v in zip(trace["captions"], values):
        out[c["row"]] = v
    return out


def items(trace):
    return {"rows": trace["pairs"], "extent": trace["extent"], "caption": trace.get("captions", [])}[KIND]


def labels_of(trace, spec):
    return {"rows": pair_labels, "extent": extent_labels, "caption": caption_labels}[KIND](trace, spec)


def spec_for(key, root):
    folder, name = key.split("/")
    return json.loads((root / folder / f"{name}.json").read_text(encoding="utf-8"))


def group_of(key, spec):
    """Validation split unit: a web page (all its tables), or a synthetic image."""
    return spec.get("url") or key


SALT = ""


def in_validation(group, frac):
    h = int(hashlib.md5((SALT + group).encode("utf-8")).hexdigest()[:8], 16)
    return h / 0xFFFFFFFF < frac


def examples(traces, root, frac, part):
    """Feature rows, labels, the rules' decisions, and each row's source (trace file, real or synthetic)."""
    X, y, rules, sources = [], [], [], []
    for source, t in traces:
        spec = spec_for(t["key"], root)
        if (part == "val") != in_validation(group_of(t["key"], spec), frac):
            continue
        kind = "web" if spec.get("url") else "synthetic"
        for p, label in zip(items(t), labels_of(t, spec)):
            if label is None:
                continue
            X.append(p["f"]); y.append(label); rules.append(int(p.get("rules", 0))); sources.append(f"{source}:{kind}")
    return np.asarray(X, dtype=np.float32), np.asarray(y), np.asarray(rules), np.asarray(sources)


def tagged(paths):
    for path in paths:
        for t in load_traces([path]):
            yield path.stem, t


def fit(args):
    from sklearn.ensemble import HistGradientBoostingClassifier
    global SALT, SPAN_LABELS, KIND
    SALT = args.salt
    SPAN_LABELS = args.span_labels
    KIND = args.kind
    traces = list(tagged(args.traces))
    root = Path(args.root)
    Xt, yt, rt, _ = examples(traces, root, args.val_frac, "train")
    Xv, yv, rv, sv = examples(traces, root, args.val_frac, "val")
    for k in args.drop:
        Xt[:, k] = 0
        if len(yv): Xv[:, k] = 0
    params = dict(PARAMETERS)
    if args.trees: params["max_iter"] = args.trees
    if args.depth: params["max_depth"] = args.depth
    _, _, _, st = examples(traces, root, args.val_frac, "train")
    weight = lambda sources: np.where(np.char.find(sources.astype(str), ":web") >= 0, args.web_weight, 1.0)
    model = HistGradientBoostingClassifier(**params).fit(Xt, yt, sample_weight=weight(st))
    print(f"train pairs {len(yt)} (join {yt.mean():.2f}), validation pairs {len(yv)}")
    if len(yv):
        pv = decide(model.decision_function(Xv), rv, Xv)
        print(f"validation accuracy: rules {np.mean(rv == yv):.4f}  model {np.mean(pv == yv):.4f}")
        for name, mask in (("join", yv == 1), ("part", yv == 0)):
            print(f"  true {name}: rules {np.mean(rv[mask] == yv[mask]):.4f}  model {np.mean(pv[mask] == yv[mask]):.4f}  (n={mask.sum()})")
        for source in sorted(set(sv)):
            m = sv == source
            j, p = m & (yv == 1), m & (yv == 0)
            print(f"  {source:28s} n={m.sum():5d}  errors rules {np.sum(rv[m] != yv[m]):4d} model {np.sum(pv[m] != yv[m]):4d}"
                  f"   missed joins {np.sum(rv[j] != 1)}/{np.sum(pv[j] != 1)}  wrong joins {np.sum(rv[p] != 0)}/{np.sum(pv[p] != 0)}")
    if args.final:
        X = np.concatenate([Xt, Xv]) if len(yv) else Xt
        Y = np.concatenate([yt, yv]) if len(yv) else yt
        W = np.concatenate([weight(st), weight(sv)]) if len(yv) else weight(st)
        model = HistGradientBoostingClassifier(**params).fit(X, Y, sample_weight=W)
        print(f"refit on all {len(Y)} pairs")
    with open(args.model, "wb") as f:
        pickle.dump({"model": model, "pairs": int(len(yt) + (len(yv) if args.final else 0)), "traces": [str(p) for p in args.traces]}, f)


def given(args):
    global KIND
    KIND = args.kind
    model = pickle.load(open(args.model, "rb"))["model"]
    out = {}
    for t in load_traces(args.traces):
        if not items(t):
            continue
        features = np.asarray([p["f"] for p in items(t)], dtype=np.float32)
        decisions = [int(d) for d in decide(model.decision_function(features), [int(p.get("rules", 0)) for p in items(t)], features)]
        out[t["key"]] = by_row(t, decisions)
    Path(args.out).write_text(json.dumps(out), encoding="utf-8")


def oracle(args):
    global KIND
    KIND = args.kind
    root = Path(args.root)
    out = {}
    for t in load_traces(args.traces):
        labels = labels_of(t, spec_for(t["key"], root))
        out[t["key"]] = by_row(t, [-1 if v is None else v for v in labels])
    Path(args.out).write_text(json.dumps(out), encoding="utf-8")


def check(args):
    """Counts the decisions in traces (made by the built tool) that differ from the pickled model's."""
    global KIND
    KIND = args.kind
    model = pickle.load(open(args.model, "rb"))["model"]
    key = {"rows": "joined", "extent": "in", "caption": "kept"}[KIND]
    same = differ = 0
    for t in load_traces(args.traces):
        its = items(t)
        if not its:
            continue
        features = np.asarray([p["f"] for p in its], dtype=np.float32)
        for p, d in zip(its, decide(model.decision_function(features), [int(p.get("rules", 0)) for p in its], features)):
            same += int(p[key]) == d
            differ += int(p[key]) != d
    print(f"{KIND}: same {same} differ {differ}")
    return 1 if differ else 0


def _float_threshold(threshold):
    """The largest float at or below threshold, so float features decide the same way in 4 bytes."""
    t = np.float32(threshold)
    if float(t) > threshold:
        t = np.nextafter(t, np.float32(-np.inf))
    return float(t)


def _float_literal(value):
    text = repr(float(np.float32(value)))
    return (text if ("." in text or "e" in text or "inf" in text) else text + ".0") + "f"


def export(args):
    saved = pickle.load(open(args.model, "rb"))
    model = saved["model"]
    names = [n.strip() for n in open(args.names, encoding="utf-8") if n.strip()]
    assert model.n_trees_per_iteration_ == 1 and model.n_features_in_ == len(names)
    splits, leaves, roots = [], [], []
    for predictor in model._predictors:
        nodes = predictor[0].nodes
        index = {}
        for k, node in enumerate(nodes):
            if node["is_leaf"]:
                index[k] = ~len(leaves)
                leaves.append(float(node["value"]))
            else:
                index[k] = len(splits)
                splits.append(None)
        for k, node in enumerate(nodes):
            if not node["is_leaf"]:
                assert not node["is_categorical"]
                splits[index[k]] = (int(node["feature_idx"]), _float_threshold(float(node["num_threshold"])), index[int(node["left"])], index[int(node["right"])])
        roots.append(index[0])
    assert len(splits) < 32767 and len(leaves) < 32767, "children are stored as 16-bit indices"
    lines = ["// Generated by scripts/table_model/train.py; do not edit by hand.",
             f"// Trained on {saved['pairs']} examples from {', '.join(Path(p).stem for p in saved['traces'])}.",
             f"// {len(roots)} trees, {len(splits)} splits, {len(leaves)} leaves; features: {', '.join(names)}.",
             "constexpr bool MODEL_TRAINED = true;",
             f"constexpr int MODEL_FEATURE_COUNT = {len(names)};",
             f"constexpr double MODEL_BASELINE = {float(model._baseline_prediction.ravel()[0])!r};",
             "// Per split: threshold (go left when the feature is at or below it), left child, right child, feature.",
             "constexpr ModelSplit MODEL_SPLITS[] = {"]
    lines += [f"    {{{_float_literal(t)}, {left}, {right}, {feature}}}," for feature, t, left, right in splits]
    lines.append("};")
    lines.append("constexpr double MODEL_LEAVES[] = {")
    lines += [f"    {value!r}," for value in leaves]
    lines.append("};")
    lines.append("constexpr short MODEL_TREES[] = {" + ", ".join(map(str, roots)) + "};")
    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(roots)} trees, {len(splits) + len(leaves)} nodes")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fit")
    f.add_argument("--traces", nargs="+", type=Path, required=True)
    f.add_argument("--kind", default="rows", choices=["rows", "extent", "caption"])
    f.add_argument("--model", required=True)
    f.add_argument("--root", default="data")
    f.add_argument("--val-frac", type=float, default=0.2)
    f.add_argument("--trees", type=int)
    f.add_argument("--depth", type=int)
    f.add_argument("--span-labels", default="rules", choices=["none", "rules", "start"], help="how rows of row-spanning text are labelled")
    f.add_argument("--salt", default="", help="another validation split")
    f.add_argument("--drop", type=int, nargs="*", default=[], help="features to blank, to measure what they add")
    f.add_argument("--web-weight", type=float, default=1.0, help="weight of pairs from real web pages")
    f.add_argument("--final", action="store_true", help="refit on training and validation pairs together")
    g = sub.add_parser("given")
    g.add_argument("--traces", nargs="+", type=Path, required=True)
    g.add_argument("--kind", default="rows", choices=["rows", "extent", "caption"])
    g.add_argument("--model", required=True)
    g.add_argument("--out", required=True)
    o = sub.add_parser("oracle")
    o.add_argument("--traces", nargs="+", type=Path, required=True)
    o.add_argument("--kind", default="rows", choices=["rows", "extent", "caption"])
    o.add_argument("--root", default="data")
    o.add_argument("--out", required=True)
    e = sub.add_parser("export")
    e.add_argument("--model", required=True)
    e.add_argument("--names", required=True, help="feature names, one per line (jietuba_table_tool names)")
    e.add_argument("--out", required=True)
    c = sub.add_parser("check")
    c.add_argument("--traces", nargs="+", type=Path, required=True)
    c.add_argument("--kind", default="rows", choices=["rows", "extent", "caption"])
    c.add_argument("--model", required=True)
    args = ap.parse_args()
    return {"fit": fit, "given": given, "oracle": oracle, "export": export, "check": check}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
