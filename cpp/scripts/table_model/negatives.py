"""Renders screenshots that are not tables (Playwright, Edge), to measure how often one is offered as a table.

Usage: python negatives.py <out_dir> <first_seed> <count>
Kinds: paragraph, list, code, chat, article (never tables); menu and settings (two aligned columns, reported apart).
Writes <name>.png and <name>.json ({"seed", "kind"}).
"""
import html
import json
import random
import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

import synthetic

KINDS = ["paragraph", "list", "code", "chat", "article", "paragraph", "list", "menu", "settings", "article"]
ZH = ["本季度整体销量同比增长 18%，其中华东地区贡献最大，线上渠道的占比也在持续提升。",
      "为了保证数据安全，系统会在每天凌晨自动备份，备份文件保留三十天，过期后自动清理。",
      "如果你在使用过程中遇到问题，可以先查看帮助文档，或者在设置页面里提交反馈。",
      "新版本修复了长截图偶尔接不上的问题，并改进了文字识别在深色背景下的效果。",
      "会议定于下周三上午十点在三楼会议室举行，请各部门负责人提前准备好季度总结。"]
EN = ["Overall sales grew 18% year over year, led by the East region, and the share of online channels keeps rising.",
      "To keep your data safe, the system backs up automatically every night and keeps each backup for thirty days.",
      "If you run into a problem, check the help pages first, or send feedback from the settings page.",
      "This release fixes scrolling captures that sometimes failed to join and improves text recognition on dark backgrounds.",
      "The meeting is set for next Wednesday at 10 a.m. in the third-floor room; please bring your quarterly summaries."]
CODE = ["def build(lines):", "    rows = []", "    for line in lines:", "        if not line.text:", "            continue",
        "        rows.append(line)", "    return rows", "", "class Table:", "    def __init__(self, cells):", "        self.cells = cells",
        "for (int i = 0; i < n; ++i) {", "    total += values[i];", "}", "const auto result = recognize(image);"]
NAMES = ["张伟", "王芳", "Alice", "Bob", "李娜", "George"]
MENU = [("复制", "Ctrl+C"), ("粘贴", "Ctrl+V"), ("剪切", "Ctrl+X"), ("全选", "Ctrl+A"), ("撤销", "Ctrl+Z"), ("查找", "Ctrl+F"), ("保存", "Ctrl+S")]
SETTINGS = [("开机自动启动", "开"), ("截图后自动复制", "关"), ("界面语言", "简体中文"), ("主题", "跟随系统"), ("保存格式", "PNG"), ("快捷键", "F1")]


def make(seed):
    rng = random.Random(seed)
    kind = KINDS[seed % len(KINDS)]
    zh = rng.random() < 0.6
    font = rng.choice(["Microsoft YaHei", "SimSun", "DengXian"] if zh else ["Segoe UI", "Arial", "Times New Roman", "Calibri"])
    size = rng.randint(13, 20)
    width = rng.randint(280, 720)
    style = f"body{{margin:30px;font-family:'{font}',sans-serif;font-size:{size}px;color:#222;background:#fff}} p{{max-width:{width}px;line-height:1.6}}"
    texts = ZH if zh else EN
    body = ""
    if kind == "paragraph":
        body = "".join(f"<p>{html.escape(' '.join(rng.sample(texts, rng.randint(1, 3))))}</p>" for _ in range(rng.randint(1, 3)))
    elif kind == "list":
        items = rng.sample(texts, rng.randint(3, 5))
        body = "<ul style='max-width:%dpx'>%s</ul>" % (width, "".join(f"<li>{html.escape(t)}</li>" for t in items))
    elif kind == "code":
        start = rng.randint(0, 5)
        lines = CODE[start:start + rng.randint(5, 10)]
        style += " pre{font-family:Consolas,monospace;background:#f6f8fa;padding:12px}"
        body = "<pre>" + html.escape("\n".join(lines)) + "</pre>"
    elif kind == "chat":
        style += " .b{max-width:%dpx;padding:8px 12px;border-radius:10px;margin:6px 0} .l{background:#f0f0f0} .r{background:#95ec69;margin-left:auto}" % width
        parts = []
        for _ in range(rng.randint(3, 6)):
            side = rng.choice(["l", "r"])
            parts.append(f"<div style='color:#999;font-size:12px'>{rng.choice(NAMES)} {rng.randint(8, 22)}:{rng.randint(10, 59)}</div>"
                         f"<div class='b {side}'>{html.escape(rng.choice(texts))}</div>")
        body = "<div style='width:%dpx'>%s</div>" % (width + 80, "".join(parts))
    elif kind == "article":
        body = (f"<h2>{html.escape(rng.choice(['版本更新说明', '使用指南', 'Release notes', 'Getting started']))}</h2>"
                f"<div style='color:#888'>{rng.choice(NAMES)} · 2025-{rng.randint(1, 12):02d}-{rng.randint(1, 28):02d}</div>"
                + "".join(f"<p>{html.escape(rng.choice(texts))}</p>" for _ in range(rng.randint(1, 3))))
    elif kind == "menu":
        items = rng.sample(MENU, rng.randint(4, 7))
        style += " .m{width:240px;border:1px solid #ddd;border-radius:6px;padding:4px 0} .i{display:flex;justify-content:space-between;padding:6px 14px} .k{color:#888}"
        body = "<div class='m'>" + "".join(f"<div class='i'><span>{a}</span><span class='k'>{b}</span></div>" for a, b in items) + "</div>"
    elif kind == "settings":
        items = rng.sample(SETTINGS, rng.randint(3, 6))
        style += " .s{width:%dpx} .i{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid #eee}" % max(width, 360)
        body = "<div class='s'>" + "".join(f"<div class='i'><span>{a}</span><span>{b}</span></div>" for a, b in items) + "</div>"
    page = f"<!doctype html><html><head><meta charset='utf-8'><style>{style}</style></head><body>{body}</body></html>"
    return kind, page, rng.choice([1, 1, 1.25, 1.5]), rng.randint(4, 24)


def main():
    out = Path(sys.argv[1])
    first, count = int(sys.argv[2]), int(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="msedge", headless=True)
        for seed in range(first, first + count):
            kind, page_html, dsf, margin = make(seed)
            name = f"n{seed:05d}"
            ctx = browser.new_context(viewport={"width": 1500, "height": 1400}, device_scale_factor=dsf)
            page = ctx.new_page()
            page.set_content(page_html, wait_until="load")
            png = out / f"{name}.png"
            page.screenshot(path=str(png), full_page=True)
            ctx.close()
            synthetic.trim(png, int(margin * dsf))
            (out / f"{name}.json").write_text(json.dumps({"seed": seed, "kind": kind}), encoding="utf-8")
        browser.close()


if __name__ == "__main__":
    main()
