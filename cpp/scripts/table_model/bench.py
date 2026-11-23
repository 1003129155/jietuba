"""Scores the fixed benchmark by scenario, with 95% bootstrap intervals.

Usage: python bench.py --bench DIR [--tag v5] [--base base] [--list KEY=VALUE]
The benchmark folder holds one set per subfolder, each recognised and built by bench_run.sh:
  web        collect_web.py --context --pages pages/bench.json --per-page 4
  dark       the same, --dark --per-page 2, on the docs, component, GitHub and tutorial pages
  scale_100, scale_150, scale_200   the same, --dsf 1 / 1.5 / 2 --per-page 2, on a sample of pages
  syn_a, syn_b                      render_synthetic.py --context (seeds 9001-9300, 19001-19300)
  neg                               negatives.py (seeds 201-400)
  excluded.txt                      layout tables (score boxes, legends, diagrams) left out, one name per line
Reads pred_<tag>_one / pred_<tag>_pp in every set. Real pages are resampled by page, synthetic images one by one.
Korean is left out of everything but its own row: the default PP-OCR model does not read it. --base adds the gain
over another tag (bench_run.sh writes the rules alone as base), paired on the same images.
"""
import collections
import json
import sys
import urllib.parse
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))
import score  # noqa: E402

BENCH = Path(".")
SETS = [("web", "web"), ("dark", "dark"), ("scale_100", "scale"), ("scale_150", "scale"), ("scale_200", "scale"),
        ("syn_a", "syn"), ("syn_b", "syn"), ("neg", "neg")]
ENGINES = ("one", "pp")
DARK_LUM = 110
B = 2000


def training_hosts():
    return {urllib.parse.urlparse(p[2]).netloc for f in sorted((HERE / "pages").glob("train*.json"))
            for p in json.loads(f.read_text(encoding="utf-8"))}


SEEN = training_hosts()


def size_bucket(n, cuts, names):
    for c, name in zip(cuts, names):
        if n <= c:
            return name
    return names[-1]


def attributes(spec, group):
    a = {}
    lang = spec.get("lang", "?")
    a["语言"] = {"zh": "中文", "en": "英文", "ja": "日文", "ko": "韩文", "mix": "中英混排"}.get(lang, lang)
    a["行数"] = size_bucket(spec["rows"], (5, 12), ("≤5 行", "6–12 行", "13 行以上"))
    a["列数"] = size_bucket(spec["cols"], (3, 6), ("2–3 列", "4–6 列", "7 列以上"))
    a["合并格"] = "有合并格" if any(c["rs"] > 1 or c["cs"] > 1 for c in spec["cells"]) else "无合并格"
    title, note = score.context_texts(spec)
    a["截法"] = ("带标题或注释" if title or note else "只截表格") if not spec.get("outside") else "截进正文"
    a["缩放"] = f"{int(round(spec.get('dsf', 1) * 100))}%"
    if group in ("web", "dark", "scale"):
        host = urllib.parse.urlparse(spec["url"]).netloc
        a["网页类型"] = {"wiki": "维基", "docs": "技术文档", "tutorial": "教程站", "data": "数据/政务/金融",
                      "component": "组件库示例", "github": "GitHub README"}.get(spec["style"], spec["style"])
        a["站点"] = "训练见过的站点" if host in SEEN else "没见过的站点"
        a["长表截断"] = "截断的长表" if spec.get("cut") else "完整的表"
        if "zh-tw" in spec["url"] or "zh-hk" in spec["url"]:
            a["语言"] = "繁体中文"
    else:
        a["合成样式"] = spec.get("style", "?")
    return a


def excluded():
    path = BENCH / "excluded.txt"
    if not path.exists():
        return set()
    return {line.split()[0] for line in path.read_text(encoding="utf-8").splitlines() if line.strip() and not line.startswith("#")}


def load(tag):
    """{set: {name: record}} with per-engine scores under tag."""
    out = {}
    skip = excluded()
    for set_name, group in SETS:
        d = BENCH / set_name
        if not d.exists():
            continue
        recs = {}
        for spec_path in sorted(d.glob("*.json")):
            if spec_path.name.endswith("_scores.json"):
                continue
            # Shots of one table share its name across the light, dark and scale sets.
            if spec_path.stem in skip:
                continue
            spec = json.loads(spec_path.read_text(encoding="utf-8"))
            name = spec_path.stem
            rec = {"name": name, "spec": spec, "group": group, "set": set_name}
            for e in ENGINES:
                p = d / f"pred_{tag}_{e}" / f"{name}.json"
                if not p.exists():
                    break
                pred = json.loads(p.read_text(encoding="utf-8"))
                if group.startswith("neg"):
                    rec[e] = {"offered": float(pred["offered"])}
                    continue
                cells = pred["cells"] if pred["offered"] else []
                s = score.score(score.with_context(spec), cells)
                rec[e] = {"pos": s["pos"], "perfect": float(s["perfect"]), "adj": s["adj"]}
            else:
                if group.startswith("neg"):
                    rec["cluster"] = f"{set_name}/{name}"
                else:
                    rec["cluster"] = spec.get("url") or f"{set_name}/{name}"
                    rec["attrs"] = attributes(spec, group)
                recs[name] = rec
        out[set_name] = recs
    return out


def interval(values, clusters, rng):
    """Mean and 95% percentile interval, resampling whole clusters."""
    if not values:
        return None
    keys = sorted(set(clusters))
    index = {k: i for i, k in enumerate(keys)}
    sums, counts = np.zeros(len(keys)), np.zeros(len(keys))
    for v, c in zip(values, clusters):
        sums[index[c]] += v
        counts[index[c]] += 1
    draws = rng.integers(0, len(keys), size=(B, len(keys)))
    means = sums[draws].sum(1) / counts[draws].sum(1)
    lo, hi = np.percentile(means, [2.5, 97.5])
    return float(sums.sum() / counts.sum()), float(lo), float(hi), len(keys)


def fmt(iv, pct=False):
    if iv is None:
        return ""
    m, lo, hi, _ = iv
    if pct:
        return f"{m * 100:.0f}% ({lo * 100:.0f}–{hi * 100:.0f})"
    return f"{m:.3f} ({lo:.2f}–{hi:.2f})"


def fmt_gain(iv):
    if iv is None:
        return ""
    m, lo, hi, _ = iv
    return f"{m:+.3f} ({lo:+.2f}–{hi:+.2f})"


def row(label, recs, rng, base=None):
    recs = [r for r in recs if all(e in r for e in ENGINES)]
    if not recs:
        return
    cl = [r["cluster"] for r in recs]
    cells = [label, f"{len(recs)} ({len(set(cl))})"]
    for e in ENGINES:
        cells.append(fmt(interval([r[e]["pos"] for r in recs], cl, rng)))
        cells.append(fmt(interval([r[e]["perfect"] for r in recs], cl, rng), pct=True))
    if base is not None:
        paired = [(r, base[r["set"]].get(r["name"])) for r in recs]
        paired = [(r, b) for r, b in paired if b is not None and all(e in b for e in ENGINES)]
        for e in ENGINES:
            cells.append(fmt_gain(interval([r[e]["pos"] - b[e]["pos"] for r, b in paired], [r["cluster"] for r, _ in paired], rng)))
    print("| " + " | ".join(cells) + " |")


def paired_gain(label, pairs, rng):
    """Second minus first on the same tables, in the pos columns."""
    cl = [a["cluster"] for a, _ in pairs]
    cells = [label, f"{len(pairs)} ({len(set(cl))})"]
    for e in ENGINES:
        cells += [fmt_gain(interval([b[e]["pos"] - a[e]["pos"] for a, b in pairs], cl, rng)),
                  fmt_gain(interval([b[e]["perfect"] - a[e]["perfect"] for a, b in pairs], cl, rng))]
    print("| " + " | ".join(cells) + " |")


def header(title, base):
    print(f"\n### {title}\n")
    cols = ["场景", "图（页）", "oneocr pos", "oneocr 全对", "PP-OCR pos", "PP-OCR 全对"]
    if base:
        cols += ["oneocr 比规则", "PP-OCR 比规则"]
    print("| " + " | ".join(cols) + " |")
    print("|" + "---|" * len(cols))


def by(recs, key):
    groups = collections.defaultdict(list)
    for r in recs:
        groups[r["attrs"].get(key, "?")].append(r)
    return sorted(groups.items(), key=lambda kv: -len(kv[1]))


def main():
    global BENCH
    args = sys.argv[1:]
    opt = lambda k, d=None: args[args.index(k) + 1] if k in args else d
    if not opt("--bench"):
        sys.exit(__doc__)
    BENCH = Path(opt("--bench"))
    tag, base_tag = opt("--tag", "v5"), opt("--base")
    rng = np.random.default_rng(20261123)
    data = load(tag)
    base = load(base_tag) if base_tag else None
    no_ko = lambda rs: [r for r in rs if r["spec"].get("lang") != "ko"]
    web = no_ko(data.get("web", {}).values())
    syn = no_ko(list(data.get("syn_a", {}).values()) + list(data.get("syn_b", {}).values()))

    if opt("--list"):
        key, value = opt("--list").split("=")
        for r in sorted(web + syn, key=lambda r: r["one"]["pos"]):
            if r["attrs"].get(key) == value:
                print(r["set"], r["name"], f"one {r['one']['pos']:.2f} pp {r['pp']['pos']:.2f}", r["spec"].get("url", r["spec"].get("style")))
        return

    header("总体（去掉韩文）", base)
    row("真实网页", web, rng, base)
    row("合成图", syn, rng, base)
    ko = [r for r in data.get("web", {}).values() if r["spec"].get("lang") == "ko"]
    if ko:
        recs = [r for r in ko if "one" in r]
        cl = [r["cluster"] for r in recs]
        print(f"| 韩文网页（只看 oneocr） | {len(recs)} ({len(set(cl))}) | {fmt(interval([r['one']['pos'] for r in recs], cl, rng))} | "
              f"{fmt(interval([r['one']['perfect'] for r in recs], cl, rng), True)} | | |")

    for key in ("网页类型", "语言", "截法", "站点", "长表截断", "行数", "列数", "合并格", "缩放"):
        header(f"真实网页按{key}", base)
        for value, rs in by(web, key):
            row(value, rs, rng, base)
    for key in ("合成样式", "语言", "截法", "合并格", "缩放"):
        header(f"合成图按{key}", base)
        for value, rs in by(syn, key):
            row(value, rs, rng, base)

    # The same tables shot light and dark, and at three scales.
    light = data.get("web", {})
    dark = [r for r in no_ko(data.get("dark", {}).values()) if r["spec"].get("lum", 255) < DARK_LUM]
    pairs = [(light[r["name"]], r) for r in dark if r["name"] in light and light[r["name"]]["spec"].get("lum", 0) >= DARK_LUM]
    if pairs:
        header("同一批表格：浅色和深色", None)
        row("浅色", [p[0] for p in pairs], rng)
        row("深色", [p[1] for p in pairs], rng)
        paired_gain("深色减浅色", pairs, rng)
    scales = {s: data.get(f"scale_{s}", {}) for s in (100, 150, 200)}
    common = set.intersection(*(set(v) for v in scales.values())) if all(scales.values()) else set()
    common = {n for n in common if scales[100][n]["spec"].get("lang") != "ko"}
    if common:
        header("同一批表格：三种缩放", None)
        for s, recs in scales.items():
            row(f"{s}%", [recs[n] for n in sorted(common)], rng)
        for s in (150, 200):
            paired_gain(f"{s}% 减 100%", [(scales[100][n], scales[s][n]) for n in sorted(common)], rng)

    print("\n### 反例（不是表格的截图被当成表格的比例）\n")
    print("| 集合 | 图 | oneocr | PP-OCR |\n|---|---|---|---|")
    for set_name in ("neg",):
        recs = [r for r in data.get(set_name, {}).values() if all(e in r for e in ENGINES)]
        if not recs:
            continue
        # Menus and settings pages are two aligned columns; offering them as a table is by design.
        for label, pick in (("文字、列表、代码、聊天", lambda r: r["spec"].get("kind") not in ("menu", "settings")),
                            ("菜单、设置页（两列对齐）", lambda r: r["spec"].get("kind") in ("menu", "settings"))):
            rs = [r for r in recs if pick(r)]
            cl = [r["cluster"] for r in rs]
            print(f"| {set_name}：{label} | {len(rs)} | " + " | ".join(fmt(interval([r[e]["offered"] for r in rs], cl, rng), True) for e in ENGINES) + " |")
        kinds = collections.defaultdict(list)
        for r in recs:
            kinds[r["spec"].get("kind", "?")].append(r)
        for k, rs in sorted(kinds.items()):
            print(f"| {set_name}：{k} | {len(rs)} | " + " | ".join(f"{sum(r[e]['offered'] for r in rs):.0f}" for e in ENGINES) + " |")


if __name__ == "__main__":
    main()
