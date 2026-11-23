"""Which true paragraph each OCR line belongs to.

An OCR line takes the DOM block it overlaps most. It is left unlabelled (never scored, still grouped) when it
lies over no DOM text (text inside a picture), over an ignored block, or over two blocks at once, and when its
text is blank: the application drops such lines before grouping, so they never reach the translator."""


def box_of(line):
    xs = [p[0] for p in line["quad"]]
    ys = [p[1] for p in line["quad"]]
    return min(xs), min(ys), max(xs), max(ys)


def label_lines(truth, lines):
    out = []
    for line in lines:
        x0, y0, x1, y1 = box_of(line)
        area = max(1e-6, (x1 - x0) * (y1 - y0))
        overlap = {}
        for t in truth["lines"]:
            a, b, c, d = t["box"]
            w, h = min(x1, c) - max(x0, a), min(y1, d) - max(y0, b)
            if w > 0 and h > 0:
                overlap[t["block"]] = overlap.get(t["block"], 0) + w * h
        ranked = sorted(overlap.items(), key=lambda kv: -kv[1])
        if not line["text"].strip() or not ranked or ranked[0][1] / area < 0.3:
            out.append(None)
        elif truth["blocks"][str(ranked[0][0])]["ignore"]:
            out.append(None)
        elif len(ranked) > 1 and ranked[1][1] / area >= 0.25:
            out.append(None)
        else:
            out.append(ranked[0][0])
    return out
