"""Flags real-page shots that may be layout tables (score boxes, cards, legends) and draws contact sheets of them.

Usage: python bench_screen.py <set_dir> <sheet_dir> [kind,kind...]
Flagged: Wikipedia tables without the wikitable class, tables where most cells are empty, and every shot of the
listed page kinds (wiki, data...), where layout tables are common.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H, PER_ROW, PER_SHEET = 420, 260, 4, 16


def flagged(spec, kinds=()):
    cells = spec["cells"]
    empty = sum(1 for c in cells if not c["text"]) / max(1, len(cells))
    wiki_layout = "wikipedia.org" in spec["url"] and "wikitable" not in spec.get("cls", "")
    return wiki_layout or empty > 0.4 or spec.get("style") in kinds


def main():
    d, out = Path(sys.argv[1]), Path(sys.argv[2])
    kinds = sys.argv[3].split(",") if len(sys.argv) > 3 else ()
    out.mkdir(parents=True, exist_ok=True)
    names = [p.stem for p in sorted(d.glob("*.json")) if flagged(json.loads(p.read_text(encoding="utf-8")), kinds)]
    print(len(names), "flagged")
    for s in range(0, len(names), PER_SHEET):
        chunk = names[s:s + PER_SHEET]
        sheet = Image.new("RGB", (W * PER_ROW, (H + 20) * ((len(chunk) + PER_ROW - 1) // PER_ROW)), "white")
        draw = ImageDraw.Draw(sheet)
        for i, name in enumerate(chunk):
            img = Image.open(d / f"{name}.png").convert("RGB")
            img.thumbnail((W - 10, H))
            x, y = (i % PER_ROW) * W, (i // PER_ROW) * (H + 20)
            sheet.paste(img, (x + 5, y + 18))
            draw.text((x + 5, y + 2), name, fill="red")
        sheet.save(out / f"sheet_{s // PER_SHEET:02d}.png")


if __name__ == "__main__":
    main()
