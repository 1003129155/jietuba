"""Renders pages in Edge (Playwright, headless) and reads their paragraphs from the DOM.

A page's ground truth is the list of its visible text lines, each tagged with the DOM block it belongs to
(extract.js). Blocks whose lines do not stack the way a paragraph's do, or that hold <br> or preformatted
text, are marked ignored: their paragraph boundaries are ambiguous, so they are never scored."""
import io
import json
import random
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
EXTRACT_JS = (HERE / "extract.js").read_text(encoding="utf-8")


def mark_ambiguous(lines, blocks):
    by_block = {}
    for line in lines:
        by_block.setdefault(line["b"], []).append(line)
    for block_id, members in by_block.items():
        block = blocks[block_id]
        if block["ignore"] or len(members) < 2:
            continue
        members = sorted(members, key=lambda l: (l["box"][1], l["box"][0]))
        for a, b in zip(members, members[1:]):
            h = min(max(1.0, a["box"][3] - a["box"][1]), max(1.0, b["box"][3] - b["box"][1]))
            gap = b["box"][1] - a["box"][3]
            overlap = min(a["box"][2], b["box"][2]) - max(a["box"][0], b["box"][0])
            narrow = min(a["box"][2] - a["box"][0], b["box"][2] - b["box"][0])
            if gap < -0.5 * h or gap > 1.5 * h or overlap < 0.3 * narrow:
                block["ignore"] = "not a vertical stack"
                break


def truth_from(raw, png, dpr, crop):
    """The capture as an image and its ground truth, boxes in image pixels; crop is (x, y, w, h) or None."""
    blocks, lines = raw["blocks"], raw["lines"]
    mark_ambiguous(lines, blocks)
    image = Image.open(io.BytesIO(png)).convert("RGB")
    out = [{"text": l["text"], "box": [v * dpr for v in l["box"]], "block": l["b"]} for l in lines]
    if crop:
        cx, cy, cw, ch = crop
        image = image.crop((cx, cy, cx + cw, cy + ch))
        kept = []
        for line in out:
            x0, y0, x1, y1 = line["box"]
            inside = max(0, min(x1, cx + cw) - max(x0, cx)) * max(0, min(y1, cy + ch) - max(y0, cy))
            if inside >= 0.9 * (x1 - x0) * (y1 - y0):
                line["box"] = [x0 - cx, y0 - cy, x1 - cx, y1 - cy]
                kept.append(line)
        out = kept
    width, height = image.size
    for line in out:
        b = line["box"]
        line["box"] = [max(0.0, b[0]), max(0.0, b[1]), min(float(width), b[2]), min(float(height), b[3])]
        line["ignore"] = blocks[line["block"]]["ignore"]
    return image, {"width": width, "height": height, "lines": out,
                   "blocks": {str(b["id"]): {"tag": b["tag"], "ignore": b["ignore"]} for b in blocks}}


def random_crop(rnd, width, height):
    """Most screenshots are a selection, not a whole window: four in ten get cut to a random part."""
    if rnd.random() > 0.4:
        return None
    w, h = int(width * rnd.uniform(0.55, 0.9)), int(height * rnd.uniform(0.5, 0.9))
    return [rnd.randint(0, width - w), rnd.randint(0, height - h), w, h]


class Capturer:
    """One headless Edge; a browser context per viewport, scale and colour scheme."""

    def __init__(self):
        from playwright.sync_api import sync_playwright
        self._playwright = sync_playwright().start()
        self._browser = self._playwright.chromium.launch(channel="msedge", headless=True)
        self._contexts = {}

    def close(self):
        self._browser.close()
        self._playwright.stop()

    def page(self, width, height, dpr, dark):
        key = (width, height, dpr, dark)
        if key not in self._contexts:
            self._contexts[key] = self._browser.new_context(viewport={"width": width, "height": height}, device_scale_factor=dpr,
                                                            color_scheme="dark" if dark else "light", locale="en-US")
        return self._contexts[key].new_page()

    def scene(self, html, width, height, dpr):
        page = self.page(width, height, dpr, False)
        try:
            page.set_content(html, wait_until="load")
            page.evaluate("document.fonts.ready")
            page.wait_for_timeout(150)
            return page.evaluate(EXTRACT_JS), page.screenshot(type="png")
        finally:
            page.close()

    def web(self, url, width, height, dpr, dark, scrolls):
        """[(raw, png)] for each scroll position (in viewport heights); [] when the page does not load."""
        page = self.page(width, height, dpr, dark)
        try:
            page.goto(url, wait_until="load", timeout=25000)
            page.wait_for_timeout(1500)
            shots = []
            for offset in scrolls:
                page.evaluate(f"window.scrollTo(0, {int(offset * height)})")
                page.wait_for_timeout(500)
                shots.append((page.evaluate(EXTRACT_JS), page.screenshot(type="png")))
            return shots
        except Exception:
            return []
        finally:
            page.close()


def save(split_dir, case_id, image, truth, meta, min_lines):
    """Writes one case unless too few of its lines can be scored; returns whether it was kept."""
    scored = [l for l in truth["lines"] if not l["ignore"]]
    if len(scored) < min_lines:
        return False
    (split_dir / "images").mkdir(parents=True, exist_ok=True)
    (split_dir / "truth").mkdir(parents=True, exist_ok=True)
    image.save(split_dir / "images" / f"{case_id}.png")
    truth = dict(truth, id=case_id, meta=meta)
    (split_dir / "truth" / f"{case_id}.json").write_text(json.dumps(truth, ensure_ascii=False), encoding="utf-8")
    return True


def rng(*parts):
    return random.Random(":".join(map(str, parts)))
