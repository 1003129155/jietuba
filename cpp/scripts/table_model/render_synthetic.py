"""Renders synthetic.py tables through Playwright (Edge) so each cell's box comes with the answer.

Usage: python render_synthetic.py <out_dir> <first_seed> <count> [--context]
Writes <name>.png and <name>.json (the spec, each cell with "box" [x0, y0, x1, y1] in screenshot pixels).
--context: more varied titles (some centred, as merged cells in a sheet) and notes under the table; the answer gains
"title" and "note" ({"text", "box"}) and "outside" (paragraphs around the table), as collect_web.py --context.
"""
import html
import json
import random
import sys
from pathlib import Path

from PIL import Image, ImageChops
from playwright.sync_api import sync_playwright

import synthetic

BOXES_JS = r"""
() => Array.from(document.querySelectorAll('td,th')).map(td => {
  const b = td.getBoundingClientRect();
  return [b.left + scrollX, b.top + scrollY, b.right + scrollX, b.bottom + scrollY];
})
"""

# Boxes of the text around the table, by role, over the extent of the text itself.
AROUND_JS = r"""
() => {
  const out = {};
  for (const el of document.querySelectorAll('[data-role]')) {
    const r = document.createRange();
    r.selectNodeContents(el);
    const b = r.getBoundingClientRect();
    (out[el.dataset.role] = out[el.dataset.role] || []).push({text: el.innerText.trim(), box: [b.left + scrollX, b.top + scrollY, b.right + scrollX, b.bottom + scrollY]});
  }
  return out;
}
"""

TOPICS = {"zh": ["销售", "库存", "员工", "成绩", "预算", "社保基数", "采购", "考勤", "报销"],
          "en": ["Sales", "Inventory", "Staff", "Grades", "Budget", "Orders", "Expenses", "Shipments"],
          "ja": ["売上", "在庫", "社員", "予算", "受注"], "ko": ["매출", "재고", "직원", "예산", "주문"]}
NOTES = {"zh": ["注：{}数据截至{}月底。", "备注：金额单位为元。", "资料来源：公司财务部。", "*{}为预估值。", "ps：工伤个人不缴费。", "说明：带*的为临时人员。"],
         "en": ["Note: {} figures as of month {}.", "Source: internal records.", "* Estimated values.", "Notes: amounts in USD."],
         "ja": ["注：{}は{}月末時点。", "出典：社内資料。", "※金額は税込です。"], "ko": ["주: {} 수치는 {}월 말 기준.", "출처: 내부 자료."]}


def context_title(rng, lang):
    topic = rng.choice(TOPICS[lang])
    year, month = rng.randint(2019, 2027), rng.randint(1, 12)
    return rng.choice({
        "zh": [f"{year}年{month}月{topic}明细", f"{topic}统计表", f"{year}年度{topic}汇总", f"表{rng.randint(1, 9)} {topic}一览"],
        "en": [f"Table {rng.randint(1, 9)}. {topic} by Region", f"{topic} {year}", f"Monthly {topic} Summary"],
        "ja": [f"{year}年{month}月 {topic}一覧", f"表{rng.randint(1, 9)} {topic}"],
        "ko": [f"{year}년 {month}월 {topic} 현황", f"표 {rng.randint(1, 9)}. {topic}"]}[lang])


def context_html(spec, rng):
    """synthetic.to_html with roles on the text around the table, a varied title and maybe a note."""
    lang = spec.get("lang", "en") if spec.get("lang") in TOPICS else "en"
    if rng.random() < 0.55:
        spec["title"] = context_title(rng, lang)
    page = synthetic.to_html(spec)
    if spec["title"]:
        style = "text-align:center;" if rng.random() < 0.4 else ""
        style += f"font-size:{rng.choice([1.0, 1.15, 1.3])}em;"
        # Only text right above the table titles it; with a paragraph between, it is outside.
        role = "outside" if spec["para_above"] else "title"
        page = page.replace("<h3 style='margin:0 0 10px'>", f"<h3 data-role='{role}' style='margin:0 0 {rng.randint(4, 14)}px;{style}'>", 1)
    if spec["para_above"]:
        page = page.replace("<p style='max-width:600px'>", "<p data-role='outside' style='max-width:600px'>", 1)
    if spec["para_below"]:
        head, tail = page.rsplit("<p style='max-width:600px'>", 1)
        role = "note" if spec["para_below"].lower().startswith(("注", "note")) else "outside"
        page = head + f"<p data-role='{role}' style='max-width:600px'>" + tail
    note = None
    if rng.random() < 0.35 and not (spec["para_below"] or "").lower().startswith(("注", "note")):
        note = rng.choice(NOTES[lang])
        note = note.format(rng.choice(TOPICS[lang]), rng.randint(1, 12)) if "{}" in note else note
        page = page.replace("</tbody></table>", f"</tbody></table><p data-role='note' style='margin:{rng.randint(3, 10)}px 0;font-size:0.9em'>{html.escape(note)}</p>", 1)
    spec["note_text"] = note
    return page


def trim(png, margin):
    """synthetic.trim, returning the crop's top-left corner."""
    img = Image.open(png).convert("RGB")
    bg = Image.new("RGB", img.size, img.getpixel((2, 2)))
    box = ImageChops.difference(img, bg).convert("L").point(lambda v: 255 if v > 12 else 0).getbbox()
    if not box:
        return 0, 0
    l, t, r, b = box
    x0, y0 = max(0, l - margin), max(0, t - margin)
    img.crop((x0, y0, min(img.width, r + margin), min(img.height, b + margin))).save(png)
    return x0, y0


def main():
    args = [a for a in sys.argv[1:] if a != "--context"]
    context = "--context" in sys.argv
    out = Path(args[0])
    first, count = int(args[1]), int(args[2])
    out.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="msedge", headless=True)
        for seed in range(first, first + count):
            spec = synthetic.make_spec(seed)
            name = f"s{seed:05d}"
            rng = random.Random(seed * 7919 + 17)
            page_html = context_html(spec, rng) if context else synthetic.to_html(spec)
            ctx = browser.new_context(viewport={"width": 1500, "height": 1400}, device_scale_factor=spec["dsf"])
            page = ctx.new_page()
            page.set_content(page_html, wait_until="load")
            rects = page.evaluate(BOXES_JS)
            around = page.evaluate(AROUND_JS) if context else {}
            png = out / f"{name}.png"
            page.screenshot(path=str(png), full_page=True)
            ctx.close()
            ox, oy = trim(png, int(spec["margin"] * spec["dsf"]))
            # td/th come in the order to_html writes them: rows in order, cells by column.
            order = sorted(spec["cells"], key=lambda c: (c["r"], c["c"]))
            if len(order) != len(rects):
                print(name, "cell count differs", len(order), len(rects), flush=True)
                continue
            d = spec["dsf"]
            to_image = lambda r: [round(r[0] * d - ox, 1), round(r[1] * d - oy, 1), round(r[2] * d - ox, 1), round(r[3] * d - oy, 1)]
            for c, r in zip(order, rects):
                c["box"] = to_image(r)
            if context:
                for role in ("title", "note"):
                    if around.get(role):
                        b = around[role][0]
                        spec[role] = {"text": b["text"], "box": to_image(b["box"])}
                spec["outside"] = [{"text": b["text"], "box": to_image(b["box"])} for b in around.get("outside", [])]
            (out / f"{name}.json").write_text(json.dumps(spec, ensure_ascii=False, indent=1), encoding="utf-8")
        browser.close()


if __name__ == "__main__":
    main()
