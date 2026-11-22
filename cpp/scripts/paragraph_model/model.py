"""Training the pair-scoring trees and turning them into src/translation/line_grouping_model.inc.

The measurements come from the application itself (jietuba_paragraph_tool pairs), so training and the shipped
code can never disagree about a feature. A pair is a positive example when its two lines are consecutive lines
of one true paragraph, a negative one when they belong to two different paragraphs; pairs with an unlabelled
line, or two lines of one paragraph with another between them, are left out."""
import json

import numpy as np

MAX_PARAGRAPH_LINES = 30
# Chosen on a validation fifth of the training split: depth 6 and 400 trees beat 4 x 120 by about one point.
PARAMETERS = dict(max_depth=6, max_iter=400, learning_rate=0.08, l2_regularization=1.0, random_state=0, early_stopping=False)


def examples(pairs_case, labels, truth_tops):
    """(rows, targets) of one page. truth_tops: input index -> vertical centre, to find consecutive lines."""
    by_block = {}
    for index, label in enumerate(labels):
        if label is not None:
            by_block.setdefault(label, []).append(index)
    consecutive = set()
    for members in by_block.values():
        members.sort(key=lambda i: truth_tops[i])
        consecutive |= set(zip(members, members[1:]))
    rows, targets = [], []
    for (a, b), row in zip(pairs_case["pairs"], pairs_case["features"]):
        if labels[a] is None or labels[b] is None:
            continue
        if labels[a] != labels[b]:
            rows.append(row); targets.append(0)
        elif (a, b) in consecutive:
            rows.append(row); targets.append(1)
    return rows, targets


def fit(rows, targets):
    from sklearn.ensemble import HistGradientBoostingClassifier
    model = HistGradientBoostingClassifier(**PARAMETERS)
    model.fit(np.asarray(rows, dtype=np.float32), np.asarray(targets))
    return model


def group(pairs_case, scores):
    """The same grouping as group_lines_into_paragraphs, for scores of a model not compiled in yet."""
    lines = {entry[0]: entry for entry in pairs_case["lines"]}
    best_below, best_above = {}, {}
    for (a, b), s in zip(pairs_case["pairs"], scores):
        if s >= 0:
            if a not in best_below or s > best_below[a][1]:
                best_below[a] = (b, s)
            if b not in best_above or s > best_above[b][1]:
                best_above[b] = (a, s)
    markers = {}  # item line -> its detached markers, left to right
    for entry in sorted((e for e in lines.values() if e[5] >= 0), key=lambda e: e[2]):
        markers.setdefault(entry[5], []).append(entry[0])
    parent = {i: i for i in lines if lines[i][5] < 0}

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a, (b, _) in best_below.items():
        if best_above[b][0] == a:
            parent[find(a)] = find(b)
    groups = {}
    for i in parent:
        groups.setdefault(find(i), []).append(i)
    paragraphs = []

    def add(piece):
        for k in range(0, len(piece), MAX_PARAGRAPH_LINES):
            paragraphs.append([m for i in piece[k:k + MAX_PARAGRAPH_LINES] for m in markers.get(i, []) + [i]])
    for members in groups.values():
        members.sort(key=lambda i: (lines[i][1], lines[i][2]))
        piece = [members[0]]
        for i in members[1:]:
            if lines[i][4] or (len(piece) == 1 and lines[piece[0]][3] and lines[i][3]):
                add(piece)
                piece = [i]
            else:
                piece.append(i)
        add(piece)
    return paragraphs


def scores(model, pairs_case):
    if not pairs_case["pairs"]:
        return []
    return model.decision_function(np.asarray(pairs_case["features"], dtype=np.float32))


def _float_threshold(threshold):
    """The largest float at or below threshold. Every feature is a float, so value <= threshold and
    value <= this float decide the same way, and the table stores 4 bytes instead of 8."""
    t = np.float32(threshold)
    if float(t) > threshold:
        t = np.nextafter(t, np.float32(-np.inf))
    return float(t)


def _float_literal(value):
    text = repr(float(np.float32(value)))
    return (text if ("." in text or "e" in text or "inf" in text) else text + ".0") + "f"


def export(model, feature_names, path, note):
    """Splits go to MODEL_SPLITS, leaf values to MODEL_LEAVES; a child index below zero is ~leaf."""
    assert model.n_trees_per_iteration_ == 1
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
    lines = ["// Generated by scripts/paragraph_model/pipeline.py train; do not edit by hand.",
             f"// {note}",
             f"// {len(roots)} trees, {len(splits)} splits, {len(leaves)} leaves; features: {', '.join(feature_names)}.",
             f"constexpr int MODEL_FEATURE_COUNT = {len(feature_names)};",
             f"constexpr double MODEL_BASELINE = {float(model._baseline_prediction.ravel()[0])!r};",
             "// Per split: threshold (go left when the feature is at or below it), left child, right child, feature.",
             "constexpr ModelSplit MODEL_SPLITS[] = {"]
    lines += [f"    {{{_float_literal(t)}, {left}, {right}, {feature}}}," for feature, t, left, right in splits]
    lines.append("};")
    lines.append("constexpr double MODEL_LEAVES[] = {")
    lines += [f"    {value!r}," for value in leaves]
    lines.append("};")
    lines.append("constexpr short MODEL_TREES[] = {" + ", ".join(map(str, roots)) + "};")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return len(roots), len(splits) + len(leaves)


def save_card(path, card):
    path.write_text(json.dumps(card, ensure_ascii=False, indent=1), encoding="utf-8")
