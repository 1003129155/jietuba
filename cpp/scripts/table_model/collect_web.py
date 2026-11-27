"""Screenshots tables on real web pages (Playwright, Edge) and takes the answer from the page's DOM.

Usage: python collect_web.py --pages pages/train.json [--per-page N] [--context] [--dark] [--dsf X] <out_dir> [page_index...]
Per table: <name>.png (the table plus a small random margin, cut at a row boundary when tall) and <name>.json in the
synthetic.py spec format, plus url, site, scale (dsf), colour scheme and median brightness. Columns and rows with no
text in any cell are dropped from the answer, since OCR cannot see them. Each page's random choices are seeded by its
index in the list, so --dark and --dsf shoot the same tables with the same margins.
--context: the caption and some blocks right above and below come into the shot, labelled title, note or outside.
"""
import json
import random
import statistics
import sys
from pathlib import Path

from PIL import Image
from playwright.sync_api import sync_playwright

PAGES = []
PER_PAGE = 3
CONTEXT = False
MAX_ROWS = 22
MAX_HEIGHT = 720
VIEW_W, VIEW_H = 1366, 1000

# Returns candidate tables with their grids, row boxes and clipping problems.
COLLECT_JS = r"""
() => {
  const skipClass = /navbox|metadata|ambox|sidebar|vertical-navbox|mw-collapsible|infobox|toc|plainlinks/;
  const out = [];
  const vis = el => el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const tables = Array.from(document.querySelectorAll('table'));
  tables.forEach((t, index) => {
    if (!vis(t) || t.parentElement.closest('table') || t.querySelector('table')) return;
    if (skipClass.test(t.className || '')) return;
    const rows = Array.from(t.rows).filter(vis);
    // Rank counters drawn on rows or the table are missing from innerText too.
    for (const el of [t, ...rows]) {
      for (const pseudo of ['::before', '::after']) {
        const content = getComputedStyle(el, pseudo).content;
        if (content && content !== 'none' && content !== 'normal' && content !== '""') return;
      }
    }
    if (rows.length < 3) return;
    const taken = new Set();
    const cells = [];
    let ellipsis = false;
    let generated = false;
    rows.forEach((tr, r) => {
      let c = 0;
      for (const td of tr.cells) {
        if (!vis(td)) continue;
        while (taken.has(r + ',' + c)) c++;
        const rs = td.rowSpan === 0 ? rows.length - r : Math.min(td.rowSpan, rows.length - r);
        const cs = Math.max(1, td.colSpan);
        const text = td.innerText.replace(/[\uE000-\uF8FF]/g, '').replace(/\s+/g, ' ').trim();
        for (const el of [td, ...td.querySelectorAll('*')]) {
          // Generated content (rank counters and the like) is drawn but missing from innerText.
          for (const pseudo of ['::before', '::after']) {
            const content = getComputedStyle(el, pseudo).content;
            if (content && content !== 'none' && content !== 'normal' && content !== '""' && /[\p{L}\p{N}]|counter/u.test(content)) generated = true;
          }
          const st = getComputedStyle(el);
          if ((st.overflow !== 'visible' || st.textOverflow === 'ellipsis') && el.scrollWidth > el.clientWidth + 1 && el.innerText.trim()) ellipsis = true;
        }
        cells.push({r, c, rs, cs, text, k: cells.length});
        for (let dr = 0; dr < rs; dr++) for (let dc = 0; dc < cs; dc++) taken.add((r + dr) + ',' + (c + dc));
        c += cs;
      }
    });
    const tb = t.getBoundingClientRect();
    // Clipped by a scrolling or hidden-overflow ancestor?
    let clipped = tb.right > innerWidth || tb.left < 0;
    for (let a = t.parentElement; a && a !== document.body; a = a.parentElement) {
      const st = getComputedStyle(a);
      if (st.overflowX !== 'visible') {
        const ab = a.getBoundingClientRect();
        if (tb.left < ab.left - 1 || tb.right > ab.right + 1) clipped = true;
      }
    }
    const rowBoxes = rows.map(tr => { const b = tr.getBoundingClientRect(); return [b.top + scrollY, b.bottom + scrollY]; });
    out.push({index, cells, rows: rows.length, ellipsis, clipped, generated, cls: t.className || '',
              box: [tb.left + scrollX, tb.top + scrollY, tb.width, tb.height], rowBoxes});
  });
  return out;
}
"""

# The table's box and its visible rows' bottoms in viewport coordinates, after scrolling.
MEASURE_JS = r"""
(index) => {
  const t = document.querySelectorAll('table')[index];
  if (!t) return null;
  const vis = el => el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const rows = Array.from(t.rows).filter(vis);
  const b = t.getBoundingClientRect();
  const rb = rows.map(tr => tr.getBoundingClientRect());
  return {x: b.left, y: b.top, width: b.width, rowTop: rb.map(r => r.top), rowBottom: rb.map(r => r.bottom)};
}
"""

# Viewport boxes of the visible cells, in the order COLLECT_JS numbers them (k).
BOXES_JS = r"""
(index) => {
  const t = document.querySelectorAll('table')[index];
  const vis = el => el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const out = [];
  for (const tr of Array.from(t.rows).filter(vis))
    for (const td of tr.cells) {
      if (!vis(td)) continue;
      const b = td.getBoundingClientRect();
      out.push([b.left, b.top, b.right, b.bottom]);
    }
  return out;
}
"""

# The table's caption and the text blocks just above and below it, in viewport coordinates.
CONTEXT_JS = r"""
(index) => {
  const t = document.querySelectorAll('table')[index];
  const vis = el => el && el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const box = el => { const b = el.getBoundingClientRect(); return [b.left, b.top, b.right, b.bottom]; };
  const text = el => (el.innerText || '').replace(/\s+/g, ' ').trim();
  const out = {caption: null, above: [], below: []};
  if (t.caption && vis(t.caption) && text(t.caption)) out.caption = {text: text(t.caption), box: box(t.caption)};
  // Wrappers that hold only the table (scrolling boxes) belong to it.
  let anchor = t;
  while (anchor.parentElement && anchor.parentElement !== document.body && anchor.parentElement.children.length === 1) anchor = anchor.parentElement;
  // Where a block's text itself lies, not its box: headings span the column, their words do not.
  const inked = el => {
    const r = document.createRange();
    r.selectNodeContents(el);
    const b = r.getBoundingClientRect();
    return [b.left, b.top, b.right, b.bottom];
  };
  const tb = t.getBoundingClientRect();
  const blocks = dir => {
    const res = [];
    let el = dir < 0 ? anchor.previousElementSibling : anchor.nextElementSibling;
    while (el && res.length < 3) {
      if (el.tagName === 'TABLE' || (el.querySelector && el.querySelector('table, img, svg, video, canvas'))) break;
      if (vis(el) && text(el)) {
        const b = inked(el);
        // Text beside the table (a floated table) or reaching past its sides is not shot with it.
        if (b[0] < tb.left - 40 || b[2] > tb.right + 40) break;
        if (dir < 0 ? b[3] > tb.top + 2 : b[1] < tb.bottom - 2) break;
        res.push({text: text(el).slice(0, 400), box: b, tag: el.tagName});
      }
      el = dir < 0 ? el.previousElementSibling : el.nextElementSibling;
    }
    return res;
  };
  out.above = blocks(-1);
  out.below = blocks(1);
  return out;
}
"""

NOTE_MARKS = ("注", "※", "*", "†", "‡", "ps", "p.s", "note", "source", "sources", "出典", "出所", "资料来源", "數據來源", "数据来源", "来源",
              "備考", "备注", "출처", "주", "참고")


def title_like(block):
    """A heading, or a short line that does not lead into the table or read as a sentence."""
    t = block["text"]
    if block["tag"] in ("H1", "H2", "H3", "H4", "H5", "H6"):
        return len(t) <= 120
    return len(t) <= 60 and not t.endswith((":", "：", "。", ".", "!", "?", "！", "？", ","))


def note_like(block):
    t = block["text"].lower()
    return t.startswith(NOTE_MARKS) or (len(t) <= 40 and not t.endswith((":", "：")))


HIDE_FIXED_JS = r"""
() => {
  for (const el of document.querySelectorAll('body *')) {
    const p = getComputedStyle(el).position;
    if ((p === 'fixed' || p === 'sticky') && !el.closest('table')) el.style.setProperty('visibility', 'hidden', 'important');
  }
}
"""


def squeeze(cells):
    """Drops rows and columns that no non-empty cell covers, shrinking spans across them."""
    cols = sorted({k for c in cells if c["text"] for k in range(c["c"], c["c"] + c["cs"])})
    rows = sorted({k for c in cells if c["text"] for k in range(c["r"], c["r"] + c["rs"])})
    cmap, rmap = {k: i for i, k in enumerate(cols)}, {k: i for i, k in enumerate(rows)}
    out = []
    for c in cells:
        cc = [cmap[k] for k in range(c["c"], c["c"] + c["cs"]) if k in cmap]
        rr = [rmap[k] for k in range(c["r"], c["r"] + c["rs"]) if k in rmap]
        if not cc or not rr:
            continue
        out.append({"r": rr[0], "c": cc[0], "rs": len(rr), "cs": len(cc), "text": c["text"], "k": c.get("k")})
    return out, len(rows), len(cols)


def cut_rows(t):
    """Largest row count within the limits that no row span crosses."""
    top = t["rowBoxes"][0][0]
    best = 0
    for k in range(1, t["rows"] + 1):
        if k > MAX_ROWS or t["rowBoxes"][k - 1][1] - top > MAX_HEIGHT:
            break
        if all(not (c["r"] < k < c["r"] + c["rs"]) for c in t["cells"]):
            best = k
    return best


def main():
    global PAGES, PER_PAGE, CONTEXT
    args = sys.argv[1:]
    # --context: shoot titles, captions, notes and other text around the table too, and label them.
    CONTEXT = "--context" in args
    # --dark: pages render with prefers-color-scheme: dark; --dsf X: one scale for every page.
    dark = "--dark" in args
    args = [a for a in args if a not in ("--context", "--dark")]
    fixed_dsf = None
    if "--dsf" in args:
        i = args.index("--dsf")
        fixed_dsf = float(args[i + 1])
        del args[i:i + 2]
    # --pages FILE: a JSON list of [lang, site, url]; --per-page N: tables kept per page.
    if "--pages" not in args:
        sys.exit(__doc__)
    i = args.index("--pages")
    PAGES = [tuple(p) for p in json.loads(Path(args[i + 1]).read_text(encoding="utf-8"))]
    del args[i:i + 2]
    if "--per-page" in args:
        i = args.index("--per-page")
        PER_PAGE = int(args[i + 1])
        del args[i:i + 2]
    out = Path(args[0])
    out.mkdir(parents=True, exist_ok=True)
    pick = [int(x) for x in args[1:]] or range(len(PAGES))
    log = open(out / "collect.log", "a", encoding="utf-8")
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="msedge", headless=True)
        for pi in pick:
            lang, site, url = PAGES[pi]
            rng = random.Random(pi)
            dsf = rng.choice([1, 1.25, 1.5])
            # Drawn either way, so a fixed scale shoots the same tables with the same margins.
            dsf = fixed_dsf or dsf
            ctx = browser.new_context(viewport={"width": VIEW_W, "height": VIEW_H}, device_scale_factor=dsf,
                                      color_scheme="dark" if dark else "light",
                                      locale={"zh": "zh-CN", "en": "en-US", "ja": "ja-JP", "ko": "ko-KR"}[lang])
            page = ctx.new_page()
            try:
                page.goto(url, wait_until="load", timeout=45000)
                page.wait_for_timeout(2500)
                page.evaluate(HIDE_FIXED_JS)
                tables = page.evaluate(COLLECT_JS)
            except Exception as e:
                print(pi, url, "failed:", e, file=log, flush=True)
                print(pi, "failed", type(e).__name__, flush=True)
                ctx.close()
                continue
            ok = []
            for t in tables:
                n = cut_rows(t)
                if n < 3 or t["clipped"] or t["ellipsis"] or t["generated"]:
                    continue
                cells = [c for c in t["cells"] if c["r"] < n]
                cells, nr, nc = squeeze(cells)
                if nr < 3 or nc < 2 or sum(1 for c in cells if c["text"]) < 6:
                    continue
                ok.append((t, n, cells, nr, nc))
            rng.shuffle(ok)
            saved = 0
            for t, n, cells, nr, nc in ok[:PER_PAGE]:
                name = f"r{pi:02d}_{t['index']:02d}"
                x, y, w, _ = t["box"]
                h = t["rowBoxes"][n - 1][1] - y
                margin = rng.randint(6, 24)
                page.evaluate(f"window.scrollTo(0, {max(0, y - (420 if CONTEXT else 150))})")
                page.wait_for_timeout(600)
                # Content loaded on scroll can move the table: measure it again where it now is.
                now = page.evaluate(MEASURE_JS, t["index"])
                if now is None or abs(now["width"] - w) > 2 or len(now["rowBottom"]) < n or                         abs((now["rowBottom"][n - 1] - now["rowTop"][0]) - (t["rowBoxes"][n - 1][1] - t["rowBoxes"][0][0])) > 2 * n:
                    print(name, "moved or changed, skipped", file=log, flush=True)
                    continue
                x, vy = now["x"], now["y"]
                h = now["rowBottom"][n - 1] - vy
                above, below = [], []
                if CONTEXT:
                    # Some of the blocks right above and below come into the shot, as when a title or a
                    # note is selected with the table.
                    near = page.evaluate(CONTEXT_JS, t["index"])
                    edge = vy
                    for k, b in enumerate(near["above"]):
                        if edge - b["box"][3] > 120 or b["box"][3] - b["box"][1] > 200 or b["box"][1] < margin + 2 or rng.random() > (0.75 if k == 0 else 0.45):
                            break
                        above.append(b)
                        edge = b["box"][1]
                    edge = vy + h
                    if n == t["rows"]:
                        for k, b in enumerate(near["below"]):
                            if b["box"][1] - edge > 100 or b["box"][3] - b["box"][1] > 160 or rng.random() > (0.6 if k == 0 else 0.35):
                                break
                            below.append(b)
                            edge = b["box"][3]
                    caption = near["caption"]
                top = min([vy] + [b["box"][1] for b in above])
                bottom = max([vy + h] + [b["box"][3] for b in below])
                left = min([x] + [b["box"][0] for b in above + below])
                right = max([x + w] + [b["box"][2] for b in above + below])
                if bottom + margin > VIEW_H:
                    page.set_viewport_size({"width": VIEW_W, "height": int(bottom + margin + 20)})
                    page.wait_for_timeout(300)
                clip = {"x": max(0, left - margin), "y": max(0, top - margin),
                        "width": min(VIEW_W, right + margin) - max(0, left - margin),
                        # A cut table ends at the row boundary so no half row shows below.
                        "height": bottom - top + margin + (rng.randint(0, 2) if n < t["rows"] else margin)}
                # Cell boxes in screenshot pixels, measured where the table is when it is shot.
                rects = page.evaluate(BOXES_JS, t["index"])
                page.screenshot(path=str(out / f"{name}.png"), clip=clip)
                page.set_viewport_size({"width": VIEW_W, "height": VIEW_H})
                to_image = lambda r: [round((r[0] - clip["x"]) * dsf, 1), round((r[1] - clip["y"]) * dsf, 1),
                                      round((r[2] - clip["x"]) * dsf, 1), round((r[3] - clip["y"]) * dsf, 1)]
                for c in cells:
                    k = c.pop("k", None)
                    if k is not None and k < len(rects):
                        c["box"] = to_image(rects[k])
                extra = {}
                if CONTEXT:
                    # Titles: the caption and title-like blocks right above it; notes: a note-like block
                    # right below. Everything else shot with the table is outside it.
                    titles, outside = ([caption] if caption else []), []
                    for b in above:
                        (titles if title_like(b) and len(outside) == 0 else outside).append(b)
                    notes = below[:1] if below and note_like(below[0]) else []
                    outside += below[len(notes):]
                    join = lambda bs: {"text": " ".join(b["text"] for b in sorted(bs, key=lambda b: b["box"][1])),
                                       "box": to_image([min(b["box"][0] for b in bs), min(b["box"][1] for b in bs),
                                                        max(b["box"][2] for b in bs), max(b["box"][3] for b in bs)])}
                    if titles:
                        extra["title"] = join(titles)
                    if notes:
                        extra["note"] = join(notes)
                    extra["outside"] = [{"text": b["text"], "box": to_image(b["box"])} for b in outside]
                # Median brightness of the shot: whether the page really went dark.
                lum = statistics.median(Image.open(out / f"{name}.png").convert("L").tobytes())
                spec = {"style": site, "lang": lang, "url": url, "dsf": dsf, "table_index": t["index"], "cls": t["cls"],
                        "rows": nr, "cols": nc, "cells": cells, "cut": n < t["rows"], "scheme": "dark" if dark else "light",
                        "lum": lum, **extra}
                (out / f"{name}.json").write_text(json.dumps(spec, ensure_ascii=False, indent=1), encoding="utf-8")
                saved += 1
            print(pi, site, lang, f"{len(tables)} tables, {len(ok)} usable, saved {saved}", flush=True)
            print(pi, url, len(tables), len(ok), saved, file=log, flush=True)
            ctx.close()
        browser.close()


if __name__ == "__main__":
    main()
