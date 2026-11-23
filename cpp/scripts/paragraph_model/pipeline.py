"""Builds data for the paragraph model, trains it and measures it.

Every step reads and writes under one data folder (outside the repository), one subfolder per split:
  <data>/corpus.json                         Wikipedia text for the synthetic scenes
  <data>/<split>/manifest.json               each case: scene or web page, language, viewport
  <data>/<split>/images/<id>.png, truth/<id>.json
  <data>/<split>/cases/<engine>.json         lines from the application's OCR engine (oneocr or ppocr)
  <data>/<split>/labels/<engine>.json        the true paragraph of each of those lines
  <data>/<split>/pairs/<engine>.json         line pairs and their measurements, from the application
  <data>/<split>/eval/<engine>/<name>.json   per-page counts of an evaluated system

  python pipeline.py corpus   --data D
  python pipeline.py pages    --data D --split train            (pages to capture: datasets.json)
  python pipeline.py ocr      --data D --split train --engine ppocr --ppocr-models <folder with models and onnxruntime.dll>
  python pipeline.py train    --data D --splits train --engines oneocr,ppocr --save model.pkl --export ../../src/translation/line_grouping_model.inc
  python pipeline.py export   --data D --model model.pkl --export ../../src/translation/line_grouping_model.inc
  python pipeline.py evaluate --data D --split test --engine ppocr --compiled --name shipped
  python pipeline.py evaluate --data D --split test --engine ppocr --model model.pkl --name candidate --against shipped

jietuba_paragraph_tool is built with the tests (BUILD_TESTING); point --tool or JIETUBA_PARAGRAPH_TOOL at it.
Python packages: requirements.txt; Edge must be installed for Playwright's msedge channel."""
import argparse
import ctypes
import hashlib
import io
import json
import os
import pickle
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import capture  # noqa: E402
import labels as labelling  # noqa: E402
import metrics  # noqa: E402
import model as pair_model  # noqa: E402
import scenes  # noqa: E402
import sources  # noqa: E402

try:
    import scenes_extra  # noqa: E402,F401  (registers more scenes)
except ImportError:
    pass

LANG_CYCLE = ["en", "zh", "ja", "en", "zh", "ko", "zh", "en", "ja"]


def run_tool(tool, *args):
    if os.name == "nt":
        ctypes.windll.kernel32.SetErrorMode(0x8007)  # children inherit it: a missing DLL fails instead of opening a dialog
    proc = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True, encoding="utf-8", errors="replace",
                          creationflags=0x08000000 if os.name == "nt" else 0)
    if proc.returncode:
        sys.exit(f"jietuba_paragraph_tool {args[0]} failed: {proc.stderr.strip()}")


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")


def scenario(meta):
    return meta.get("scene") or ("web_" + meta.get("lang", "?"))


# ---------------------------------------------------------------- pages
def capture_pages(data, split, spec):
    split_dir = data / split
    manifest_path = split_dir / "manifest.json"
    manifest = read(manifest_path) if manifest_path.exists() else {}
    scenes.load_corpus(data / "corpus.json")
    shooter = capture.Capturer()
    try:
        scene_spec = spec.get("scenes", {})
        names = list(scenes.SCENES) if scene_spec.get("names", "all") == "all" else scene_spec["names"]
        first, last = scene_spec.get("seeds", [0, 0])
        for k, name in enumerate(names):
            for seed in range(first, last):
                lang = LANG_CYCLE[(seed + k) % len(LANG_CYCLE)]
                case_id = f"{split}_scene_{name}_{seed}_{lang}"
                if case_id in manifest:
                    continue
                html, width, height = scenes.make(name, seed, lang)
                rnd = capture.rng(name, seed, split, "capture")
                if width < 500:
                    dpr = rnd.choice([2.0, 3.0])
                else:
                    roll = rnd.random()
                    dpr = 1.5 if roll < 0.15 else 1.25 if roll < 0.25 else 2.0 if roll < 0.3 else 1.0
                raw, png = shooter.scene(html, width, height, dpr)
                w, h = Image.open(io.BytesIO(png)).size
                image, truth = capture.truth_from(raw, png, dpr, capture.random_crop(rnd, w, h))
                meta = {"kind": "scene", "scene": name, "seed": seed, "lang": lang, "viewport": [width, height], "dpr": dpr}
                if capture.save(split_dir, case_id, image, truth, meta, min_lines=6):
                    manifest[case_id] = meta
            print(f"scene {name}: done", flush=True)
            write(manifest_path, manifest)
        web = spec.get("web", {})
        used = {m.get("url") for split_name in data.iterdir() if (split_name / "manifest.json").exists() for m in read(split_name / "manifest.json").values()}
        urls = [(u, lang) for lang, items in web.get("urls", {}).items() for u in items]
        for lang, count in web.get("random_wiki", {}).items():
            urls += [(u, lang) for u in sources.random_wiki_pages(lang, count, exclude=used)]
        for url, lang in urls:
            rnd = capture.rng(url, split)
            dpr = 1.5 if rnd.random() < 0.12 else 1.0
            dark = rnd.random() < 0.2
            width, height = (1280, 800) if dpr == 1.0 else (960, 640)
            for k, (raw, png) in enumerate(shooter.web(url, width, height, dpr, dark, web.get("scrolls", [0, 1.0, 2.0]))):
                case_id = f"{split}_web_{lang}_{hashlib.md5(url.encode()).hexdigest()[:10]}_{k}"
                if case_id in manifest:
                    continue
                w, h = Image.open(io.BytesIO(png)).size
                image, truth = capture.truth_from(raw, png, dpr, capture.random_crop(rnd, w, h))
                meta = {"kind": "web", "lang": lang, "url": url, "scroll": k, "dark": dark, "dpr": dpr}
                if capture.save(split_dir, case_id, image, truth, meta, min_lines=8):
                    manifest[case_id] = meta
            print(f"web {url}", flush=True)
            write(manifest_path, manifest)
    finally:
        shooter.close()
        write(manifest_path, manifest)


# ---------------------------------------------------------------- OCR and labels
def run_ocr(data, split, engine, tool, ppocr_models):
    split_dir = data / split
    manifest = read(split_dir / "manifest.json")
    done_path = split_dir / "cases" / f"{engine}.json"
    done = {c["id"]: c for c in read(done_path)["cases"]} if done_path.exists() else {}
    cases = []
    for case_id in sorted(manifest):
        if case_id in done:
            continue
        truth = read(split_dir / "truth" / f"{case_id}.json")
        cases.append({"id": case_id, "width": truth["width"], "height": truth["height"], "image": str((split_dir / "images" / f"{case_id}.png").resolve()), "lines": []})
    if cases:  # only pages added since the last run are recognized
        write(split_dir / "cases" / "images.json", {"cases": cases})
        extra = ["--ppocr-models", ppocr_models] if engine == "ppocr" else []
        run_tool(tool, "ocr", "--engine", engine, *extra, "--cases", split_dir / "cases" / "images.json", "--out", split_dir / "cases" / "new.json")
        done.update({c["id"]: c for c in read(split_dir / "cases" / "new.json")["cases"]})
        write(done_path, {"cases": [done[k] for k in sorted(done) if k in manifest]})
    result = {}
    for case in read(done_path)["cases"]:
        result[case["id"]] = labelling.label_lines(read(split_dir / "truth" / f"{case['id']}.json"), case["lines"])
    write(split_dir / "labels" / f"{engine}.json", result)
    print(f"{split}/{engine}: {len(result)} pages, {sum(len(v) for v in result.values())} lines, "
          f"{sum(1 for v in result.values() for x in v if x is not None)} with a true paragraph")


def pairs_for(data, split, engine, tool):
    path = data / split / "pairs" / f"{engine}.json"
    cases_path = data / split / "cases" / f"{engine}.json"
    tool_time = Path(tool).stat().st_mtime
    if not path.exists() or path.stat().st_mtime < max(tool_time, cases_path.stat().st_mtime):
        path.parent.mkdir(parents=True, exist_ok=True)
        run_tool(tool, "pairs", "--cases", cases_path, "--out", path)
    return read(path)


def centres(case):
    return [sum(p[1] for p in line["quad"]) / 4 for line in case["lines"]]


# ---------------------------------------------------------------- training
def train(args):
    rows, targets, names, pages = [], [], None, set()
    for split in args.splits.split(","):
        manifest = read(args.data / split / "manifest.json")
        for engine in args.engines.split(","):
            pairs = pairs_for(args.data, split, engine, args.tool)
            names = pairs["features"]
            labels = read(args.data / split / "labels" / f"{engine}.json")
            for case in read(args.data / split / "cases" / f"{engine}.json")["cases"]:
                meta = manifest[case["id"]]
                if args.exclude_scenes and scenario(meta) in args.exclude_scenes.split(","):
                    continue
                r, t = pair_model.examples(pairs["results"][case["id"]], labels[case["id"]], centres(case))
                rows += r; targets += t; pages.add(case["id"])
    note = f"Trained on {len(pages)} pages ({args.splits}), each read by {args.engines}: {len(targets)} line pairs."
    print(note)
    trained = pair_model.fit(rows, targets)
    if args.save:
        pickle.dump({"model": trained, "features": names, "note": note}, open(args.save, "wb"))
    if args.export:
        trees, nodes = pair_model.export(trained, names, Path(args.export), note)
        print(f"exported {trees} trees, {nodes} nodes to {args.export}")


def export(args):
    loaded = pickle.load(open(args.model, "rb"))
    trees, nodes = pair_model.export(loaded["model"], loaded["features"], Path(args.export), loaded.get("note", ""))
    print(f"exported {trees} trees, {nodes} nodes to {args.export}")


# ---------------------------------------------------------------- evaluation
def evaluate(args):
    split_dir = args.data / args.split
    manifest = read(split_dir / "manifest.json")
    labels = read(split_dir / "labels" / f"{args.engine}.json")
    cases = read(split_dir / "cases" / f"{args.engine}.json")["cases"]
    if args.compiled:
        out = split_dir / "eval" / args.engine / f"{args.name}.groups.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        run_tool(args.tool, "group", "--cases", split_dir / "cases" / f"{args.engine}.json", "--out", out)
        groups = {k: v["paragraphs"] for k, v in read(out)["results"].items()}
    else:
        loaded = pickle.load(open(args.model, "rb"))
        pairs = pairs_for(args.data, args.split, args.engine, args.tool)
        assert pairs["features"] == loaded["features"], "the model was trained on other features"
        groups = {}
        for case in cases:
            entry = pairs["results"][case["id"]]
            grouped = pair_model.group(entry, pair_model.scores(loaded["model"], entry))
            seen = {i for g in grouped for i in g}
            groups[case["id"]] = grouped + [[i] for i in range(len(case["lines"])) if i not in seen]
    rows = {}
    for case in cases:
        texts = [line["text"].strip() for line in case["lines"]]
        rows[case["id"]] = metrics.case_counts(texts, labels[case["id"]], groups[case["id"]])
    write(split_dir / "eval" / args.engine / f"{args.name}.json", rows)
    report(manifest, rows, args.name)
    if args.against:
        other = read(split_dir / "eval" / args.engine / f"{args.against}.json")
        ids = sorted(rows)
        mean, low, high = metrics.paired_difference([rows[i] for i in ids], [other[i] for i in ids])
        print(f"{args.name} - {args.against}: {mean * 100:+.1f} points of text in the right paragraph (95% interval {low * 100:+.1f} to {high * 100:+.1f})")
        by = defaultdict(list)
        for i in ids:
            by[scenario(manifest[i])].append(i)
        for name, members in sorted(by.items()):
            a = metrics.summary([rows[i] for i in members])["chars"]; b = metrics.summary([other[i] for i in members])["chars"]
            print(f"   {name:16} {a * 100:5.1f} vs {b * 100:5.1f}  ({(a - b) * 100:+.1f})")


def report(manifest, rows, name):
    total = metrics.summary(list(rows.values()))
    print(f"{name}: {len(rows)} pages, {total['lines']} lines, {total['paragraphs']} paragraphs | text in the right paragraph {total['chars'] * 100:.1f}%  "
          f"pair F1 {total['pair_f1'] * 100:.1f}  over-merged {total['over_merge'] * 100:.1f}%  split {total['split'] * 100:.1f}%")
    by = defaultdict(list)
    for case_id, row in rows.items():
        by[scenario(manifest[case_id])].append(row)
    for scene_name, members in sorted(by.items()):
        s = metrics.summary(members)
        print(f"   {scene_name:16} {len(members):4} pages  {s['chars'] * 100:5.1f}%  over {s['over_merge'] * 100:4.1f}%  split {s['split'] * 100:4.1f}%")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["corpus", "pages", "ocr", "train", "export", "evaluate"])
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--split")
    parser.add_argument("--splits")
    parser.add_argument("--engine", choices=["oneocr", "ppocr"])
    parser.add_argument("--engines", default="oneocr,ppocr")
    parser.add_argument("--ppocr-models")
    parser.add_argument("--tool", default=os.environ.get("JIETUBA_PARAGRAPH_TOOL", ""))
    parser.add_argument("--spec", type=Path, default=HERE / "datasets.json")
    parser.add_argument("--exclude-scenes", default="")
    parser.add_argument("--save")
    parser.add_argument("--export")
    parser.add_argument("--model")
    parser.add_argument("--compiled", action="store_true")
    parser.add_argument("--name", default="candidate")
    parser.add_argument("--against")
    args = parser.parse_args()
    args.data.mkdir(parents=True, exist_ok=True)
    if args.command == "corpus":
        sources.fetch_corpus(args.data / "corpus.json")
    elif args.command == "pages":
        capture_pages(args.data, args.split, read(args.spec)[args.split])
    elif args.command == "ocr":
        run_ocr(args.data, args.split, args.engine, args.tool, args.ppocr_models)
    elif args.command == "train":
        train(args)
    elif args.command == "export":
        export(args)
    else:
        evaluate(args)


if __name__ == "__main__":
    main()
