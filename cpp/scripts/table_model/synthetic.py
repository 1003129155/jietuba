"""Synthetic table specs with known structure, and the page that draws each one.

make_spec(seed) gives the answer (cells with row and column spans, text, style, language, scale); to_html(spec)
the page. render_synthetic.py renders them and records each cell's box; negatives.py reuses trim.
Seeds from 10000 on also draw spreadsheet, file-list, card and pill styles.
"""
import html
import random

from PIL import Image, ImageChops

STYLES = ["grid", "hlines", "none", "zebra", "booktabs", "grid_light", "header_bg", "outer", "dark_grid", "dark_zebra"]
# Seeds from 10000 on also draw spreadsheet, file-list, card and pill styles and the extra features below.
STYLES2 = ["excel", "list", "cards", "pills", "excel", "list", "grid", "hlines", "none", "zebra"]
LONG_HEADERS = {"zh": ["本月销售额 (万元)", "平均订单金额", "最近一次更新时间", "客户满意度评分"],
                "en": ["Average Order Value", "Last Updated Time", "Customer Rating", "Monthly Revenue (k$)"],
                "ja": ["平均注文金額", "最終更新日時"], "ko": ["평균 주문 금액", "최근 업데이트 시간"]}
INDEX = {"zh": "序号", "en": "#", "ja": "No.", "ko": "번호"}
PILL_COLORS = ["#e6f4ea;color:#137333", "#fce8e6;color:#c5221f", "#fef7e0;color:#b06000", "#e8f0fe;color:#1967d2"]

ITEMS = {
    "zh": ["笔记本电脑", "无线鼠标", "机械键盘", "显示器", "移动硬盘", "路由器", "打印机", "投影仪", "降噪耳机", "摄像头", "扫描仪",
           "平板电脑", "智能手表", "充电宝", "扩展坞", "固态硬盘", "内存条", "显卡", "电源适配器", "网线", "办公椅", "台灯"],
    "en": ["Laptop", "Wireless Mouse", "Keyboard", "Monitor", "Router", "Printer", "Headset", "Webcam", "Tablet", "Smart Watch",
           "Power Bank", "SSD 1TB", "Graphics Card", "Cable", "Desk Lamp", "Office Chair", "Docking Station", "Scanner"],
    "ja": ["ノートPC", "マウス", "キーボード", "モニター", "ルーター", "プリンター", "ヘッドセット", "タブレット", "会議室予約", "資料作成"],
    "ko": ["노트북", "마우스", "키보드", "모니터", "공유기", "프린터", "헤드셋", "태블릿", "회의실 예약", "자료 작성"],
}
PEOPLE = {
    "zh": ["张伟", "王芳", "李娜", "刘洋", "陈静", "杨帆", "赵磊", "黄敏", "周杰", "吴迪", "欧阳明"],
    "en": ["Alice", "Bob", "Charlie", "Diana", "Ethan", "Fiona", "George", "Hannah", "Isaac Newton", "J. Smith"],
    "ja": ["佐藤", "鈴木", "高橋", "田中", "伊藤", "渡辺", "山本"],
    "ko": ["김민준", "이서연", "박지호", "최수아", "정우진"],
}
REGIONS = {
    "zh": ["华东", "华南", "华北", "西南", "东北", "西北", "华中", "上海", "深圳", "北京"],
    "en": ["North", "South", "East", "West", "Central", "New York", "London", "Berlin", "Tokyo"],
    "ja": ["東京", "大阪", "名古屋", "福岡", "札幌", "横浜"],
    "ko": ["서울", "부산", "대구", "인천", "광주"],
}
STATUS = {
    "zh": ["已完成", "进行中", "未开始", "已取消", "待审核", "已发货"],
    "en": ["Done", "In progress", "Pending", "Cancelled", "Shipped", "On hold"],
    "ja": ["完了", "進行中", "未着手", "保留"],
    "ko": ["완료", "진행 중", "대기", "취소"],
}
LONG = {
    "zh": ["支持多种接口，适合办公和家庭使用", "需要在下周三之前提交最终版本的设计稿", "客户反馈加载速度慢，已安排排查",
           "本季度销量同比增长明显，库存偏低", "包装破损，已联系物流重新发货", "价格含税，不含运费和安装费"],
    "en": ["Supports multiple interfaces for office and home use", "Final draft must be submitted before next Wednesday",
           "Customer reported slow loading, investigation scheduled", "Price includes tax but not shipping",
           "Damaged in transit, replacement sent"],
    "ja": ["来週水曜日までに最終版を提出してください", "在庫が少ないため追加発注が必要です"],
    "ko": ["다음 주 수요일까지 최종본을 제출해야 합니다", "재고가 부족하여 추가 주문이 필요합니다"],
}
HEADERS = {
    "item": {"zh": ["商品", "名称", "项目", "产品"], "en": ["Item", "Product", "Name"], "ja": ["品名", "項目"], "ko": ["품목", "항목"]},
    "person": {"zh": ["负责人", "姓名", "销售员"], "en": ["Owner", "Name", "Assignee"], "ja": ["担当者", "氏名"], "ko": ["담당자", "이름"]},
    "region": {"zh": ["地区", "城市", "区域"], "en": ["Region", "City"], "ja": ["地域", "都市"], "ko": ["지역", "도시"]},
    "status": {"zh": ["状态", "进度"], "en": ["Status", "State"], "ja": ["状態"], "ko": ["상태"]},
    "int": {"zh": ["数量", "库存", "件数"], "en": ["Qty", "Stock", "Count"], "ja": ["数量", "在庫"], "ko": ["수량", "재고"]},
    "money": {"zh": ["单价 (元)", "金额", "销售额"], "en": ["Price", "Amount", "Revenue ($)"], "ja": ["単価", "金額"], "ko": ["단가", "금액"]},
    "percent": {"zh": ["增长率", "占比", "完成率"], "en": ["Growth", "Share", "Rate"], "ja": ["成長率", "割合"], "ko": ["성장률", "비율"]},
    "date": {"zh": ["日期", "截止日期", "下单时间"], "en": ["Date", "Due", "Ordered"], "ja": ["日付", "期限"], "ko": ["날짜", "마감일"]},
    "code": {"zh": ["编号", "订单号"], "en": ["ID", "Order #", "SKU"], "ja": ["番号"], "ko": ["번호"]},
    "long": {"zh": ["备注", "说明"], "en": ["Notes", "Description"], "ja": ["備考"], "ko": ["비고"]},
}
GROUPS = {"zh": ["2025 年", "第一季度", "销售情况", "库存"], "en": ["2025", "Q1", "Sales", "Inventory"], "ja": ["2025年", "第1四半期"], "ko": ["2025년", "1분기"]}
TOTAL = {"zh": "合计", "en": "Total", "ja": "合計", "ko": "합계"}
TITLES = {"zh": ["第三季度销售简报", "设备采购清单", "项目进度表"], "en": ["Quarterly Sales Report", "Purchase List", "Project Tracker"],
          "ja": ["四半期売上報告"], "ko": ["분기 매출 보고"]}
PARAS = {"zh": ["本季度整体销量同比增长 18%，其中华东地区贡献最大。下表列出主要商品的销量与单价。", "注：数据截至 9 月 30 日，单价为含税价格。"],
         "en": ["Overall sales grew 18% year over year, led by the East region. The table below lists the main products.",
                "Note: figures as of September 30; prices include tax."],
         "ja": ["今四半期の売上は前年同期比で18%増加しました。"], "ko": ["이번 분기 매출은 전년 대비 18% 증가했습니다."]}
FONTS = {"zh": ["Microsoft YaHei", "SimSun", "DengXian", "SimHei"], "en": ["Segoe UI", "Arial", "Calibri", "Times New Roman", "Consolas", "Verdana"],
         "ja": ["Yu Gothic", "Meiryo", "MS Gothic"], "ko": ["Malgun Gothic"]}
NUMERIC = {"int", "money", "percent"}


def value(rng, kind, lang):
    if kind == "item":
        return rng.choice(ITEMS[lang])
    if kind == "person":
        return rng.choice(PEOPLE[lang])
    if kind == "region":
        return rng.choice(REGIONS[lang])
    if kind == "status":
        return rng.choice(STATUS[lang])
    if kind == "long":
        return rng.choice(LONG[lang])
    if kind == "int":
        n = rng.choice([rng.randint(0, 99), rng.randint(100, 9999), rng.randint(1000, 999999)])
        return f"{n:,}" if n >= 10000 and rng.random() < 0.7 else str(n)
    if kind == "money":
        v = rng.randint(1, 9_999_999) / 100
        prefix = rng.choice(["", "", "¥", "$"]) if lang in ("zh", "en") else ""
        return f"{prefix}{v:,.2f}"
    if kind == "percent":
        return f"{rng.uniform(-30, 99):.1f}%"
    if kind == "date":
        m, d = rng.randint(1, 12), rng.randint(1, 28)
        return rng.choice([f"2025-{m:02d}-{d:02d}", f"2025/{m}/{d}", f"{m:02d}/{d:02d}"])
    if kind == "index":
        return ""
    if kind == "code":
        return f"{rng.choice(['SKU', 'PO', 'INV', 'A', 'TK'])}-{rng.randint(100, 99999)}"
    raise ValueError(kind)


def make_spec(seed):
    rng = random.Random(seed)
    extended = seed >= 10000
    style = (STYLES2 if extended else STYLES)[seed % 10]
    lang = rng.choices(["zh", "en", "mix", "ja", "ko"], [4, 3, 3, 1, 1])[0]
    pick = lambda: ("zh" if rng.random() < 0.5 else "en") if lang == "mix" else lang
    hlang = "zh" if lang == "mix" else lang
    ncols = rng.randint(2, 8)
    nbody = rng.randint(2, 12)
    first = rng.choice(["item", "item", "person", "region", "date", "code"])
    pool = ["int", "money", "percent", "status", "person", "region", "date", "code", "int", "money"]
    kinds = [first] + [rng.choice(pool) for _ in range(ncols - 1)]
    if ncols >= 3 and rng.random() < 0.3:
        kinds[rng.randrange(1, ncols)] = "long"
    headers = [rng.choice(HEADERS[k][hlang]) for k in kinds]
    wrap_headers = set()
    if extended and ncols > 1 and rng.random() < 0.25:
        for c in rng.sample(range(1, ncols), k=min(ncols - 1, rng.randint(1, 2))):
            headers[c] = rng.choice(LONG_HEADERS[hlang])
            wrap_headers.add(c)
    body = [[value(rng, k, pick()) for k in kinds] for _ in range(nbody)]
    if extended and (style == "excel" and rng.random() < 0.5 or rng.random() < 0.15):
        # A leading index column: row numbers.
        kinds = ["index"] + kinds
        headers = [INDEX[hlang]] + headers
        body = [[str(i + 1)] + row for i, row in enumerate(body)]
        wrap_headers = {c + 1 for c in wrap_headers}
        ncols += 1
    for row in body:
        for c in range(1, ncols):
            if rng.random() < 0.07:
                row[c] = ""

    # Logical grid of anchors: (r, c) -> [text, rowspan, colspan]; covered positions map to None.
    cells = {}
    header_rows = 1
    group = None
    if ncols >= 4 and rng.random() < 0.22:
        start = rng.randint(1, ncols - 2)
        span = rng.randint(2, min(3, ncols - start))
        group = (start, span, rng.choice(GROUPS[hlang]))
        header_rows = 2
    if group:
        start, span, name = group
        for c in range(ncols):
            if start <= c < start + span:
                cells[(1, c)] = [headers[c], 1, 1]
            else:
                cells[(0, c)] = [headers[c], 2, 1]
                cells[(1, c)] = None
        cells[(0, start)] = [name, 1, span]
        for c in range(start + 1, start + span):
            cells[(0, c)] = None
    else:
        for c in range(ncols):
            cells[(0, c)] = [headers[c], 1, 1]
    r = header_rows
    i = 0
    while i < nbody:
        row = body[i]
        run = 1
        label = 1 if kinds[0] == "index" else 0
        if first in ("region", "date") and nbody - i >= 2 and rng.random() < 0.18:
            run = rng.randint(2, min(4 if extended else 3, nbody - i))
        for k in range(run):
            for c in range(ncols):
                if c == label:
                    cells[(r + k, c)] = [row[c], run, 1] if k == 0 else None
                else:
                    cells[(r + k, c)] = [body[i + k][c], 1, 1]
        r += run
        i += run
    if ncols >= 3 and rng.random() < 0.15:
        span = rng.randint(1, ncols - 2)
        cells[(r, 0)] = [TOTAL[hlang], 1, span]
        for c in range(1, span):
            cells[(r, c)] = None
        for c in range(span, ncols):
            cells[(r, c)] = [value(rng, kinds[c], pick()) if kinds[c] in NUMERIC else "", 1, 1]
        r += 1
    nrows = r

    font_lang = "zh" if lang == "mix" else lang
    if lang == "en" and rng.random() < 0.3:
        font_lang = "zh"
    spec = {
        "seed": seed, "style": style, "lang": lang, "rows": nrows, "cols": ncols, "kinds": kinds, "header_rows": header_rows,
        "font": rng.choice(FONTS[font_lang]), "size": rng.randint(12, 20), "dsf": rng.choice([1, 1, 1, 1.25, 1.5, 2]),
        "pad_v": rng.randint(2, 10), "pad_h": rng.randint(4, 18), "bold_header": rng.random() < 0.7,
        "align": [("right" if rng.random() < 0.6 else rng.choice(["left", "center"])) if k in NUMERIC else
                  ("center" if rng.random() < 0.2 else "left") for k in kinds],
        "wrap_width": rng.randint(90, 170), "line_color": rng.choice(["#000", "#666", "#999", "#bbb"]),
        "wrap_headers": sorted(wrap_headers), "header_width": rng.randint(60, 100),
        "letters": style == "excel" and rng.random() < 0.4, "icons": style == "list" and rng.random() < 0.7,
        "pill_seed": rng.randint(0, 3),
        "title": rng.choice(TITLES[hlang]) if rng.random() < 0.25 else None,
        "para_above": rng.choice(PARAS[hlang]) if rng.random() < 0.15 else None,
        "para_below": rng.choice(PARAS[hlang]) if rng.random() < 0.15 else None,
        "margin": rng.randint(2, 24),
        "cells": [{"r": rr, "c": cc, "rs": v[1], "cs": v[2], "text": v[0]} for (rr, cc), v in sorted(cells.items()) if v is not None],
    }
    if spec["letters"]:
        # Column letters above the sheet are cells of the screenshot too.
        has_index = kinds[0] == "index"
        for cell in spec["cells"]:
            cell["r"] += 1
        letters = [{"r": 0, "c": c, "rs": 1, "cs": 1, "text": "" if has_index and c == 0 else chr(65 + c - (1 if has_index else 0))}
                   for c in range(ncols)]
        spec["cells"] = letters + spec["cells"]
        spec["rows"] += 1
        spec["header_rows"] += 1
    return spec


def style_css(spec):
    s, lc = spec["style"], spec["line_color"]
    css = [f"body{{margin:30px;font-family:'{spec['font']}',sans-serif;font-size:{spec['size']}px;background:#fff;color:#222}}",
           "table{border-collapse:collapse}",
           f"td,th{{padding:{spec['pad_v']}px {spec['pad_h']}px;font-weight:normal;vertical-align:middle}}"]
    if spec["bold_header"]:
        css.append("th{font-weight:bold}")
    if s == "grid":
        css.append(f"td,th{{border:1px solid {lc}}}")
    elif s == "hlines":
        css.append(f"td,th{{border-bottom:1px solid {lc}}}")
    elif s == "zebra":
        css.append("tr:nth-child(even) td{background:#eef3fb}")
    elif s == "booktabs":
        css.append("table{border-top:2px solid #000;border-bottom:2px solid #000} thead tr:last-child th{border-bottom:1px solid #000}")
    elif s == "grid_light":
        css.append("td,th{border:1px solid #d4d4d4;padding:1px 5px}")
    elif s == "header_bg":
        css.append("th{background:#4472c4;color:#fff} td{border-bottom:1px solid #d9e1f2}")
    elif s == "outer":
        css.append(f"table{{border:1px solid {lc}}}")
    elif s == "dark_grid":
        css.append("body{background:#1e1e1e;color:#ddd} td,th{border:1px solid #555}")
    elif s == "dark_zebra":
        css.append("body{background:#202020;color:#e6e6e6} tr:nth-child(odd) td{background:#2b2b2b} th{background:#333}")
    elif s == "excel":
        css.append(f"body{{font-family:'{spec['font']}',Calibri,DengXian,sans-serif;font-size:15px}} td,th{{border:1px solid #d4d4d4;padding:0 4px;height:20px}}"
                   " th{background:#f3f3f3;font-weight:normal} td.idx,th.idx{background:#f3f3f3;color:#444;text-align:center}")
    elif s == "list":
        css.append("th{color:#666;font-weight:normal;border-bottom:1px solid #e5e5e5;text-align:left} td{padding-top:7px;padding-bottom:7px}"
                   " .ico{display:inline-block;width:14px;height:14px;border-radius:3px;margin-right:8px;vertical-align:-2px}")
    elif s == "cards":
        css.append("table{border-collapse:separate;border-spacing:0 6px} td{background:#f3f5f9} td:first-child{border-radius:8px 0 0 8px}"
                   " td:last-child{border-radius:0 8px 8px 0} th{color:#555}")
    elif s == "pills":
        css.append("td,th{border-bottom:1px solid #e3e3e3} .pill{border-radius:10px;padding:2px 9px;font-size:0.9em}")
    return "".join(css)


def to_html(spec):
    by_row = {}
    for cell in spec["cells"]:
        by_row.setdefault(cell["r"], []).append(cell)
    parts = [f"<!doctype html><html><head><meta charset='utf-8'><style>{style_css(spec)}</style></head><body>"]
    if spec["title"]:
        parts.append(f"<h3 style='margin:0 0 10px'>{html.escape(spec['title'])}</h3>")
    if spec["para_above"]:
        parts.append(f"<p style='max-width:600px'>{html.escape(spec['para_above'])}</p>")
    parts.append("<table><thead>")
    for r in range(spec["rows"]):
        if r == spec["header_rows"]:
            parts.append("</thead><tbody>")
        parts.append("<tr>")
        for cell in sorted(by_row.get(r, []), key=lambda x: x["c"]):
            tag = "th" if r < spec["header_rows"] else "td"
            attrs = ""
            if cell["rs"] > 1:
                attrs += f" rowspan='{cell['rs']}'"
            if cell["cs"] > 1:
                attrs += f" colspan='{cell['cs']}'"
            kind = spec["kinds"][cell["c"]]
            style = f"text-align:{'center' if cell['cs'] > 1 and tag == 'th' else spec['align'][cell['c']]}"
            if kind == "long" and cell["cs"] == 1:
                style += f";width:{spec['wrap_width']}px"
            elif tag == "th" and cell["c"] in spec.get("wrap_headers", []) and cell["cs"] == 1:
                style += f";width:{spec['header_width']}px"
            else:
                style += ";white-space:nowrap"
            content = html.escape(cell["text"])
            if spec["style"] == "pills" and kind == "status" and tag == "td" and content:
                content = f"<span class='pill' style='background:{PILL_COLORS[(spec['pill_seed'] + cell['r']) % 4]}'>{content}</span>"
            if spec.get("icons") and tag == "td" and cell["c"] == 0 and content:
                color = ["#4285f4", "#fbbc04", "#34a853", "#ea4335", "#9aa0a6"][cell["r"] % 5]
                content = f"<span class='ico' style='background:{color}'></span>{content}"
            klass = " class='idx'" if spec["style"] == "excel" and (kind == "index" or spec.get("letters") and r == 0) else ""
            parts.append(f"<{tag}{attrs}{klass} style='{style}'>{content}</{tag}>")
        parts.append("</tr>")
    parts.append("</tbody></table>")
    if spec["para_below"]:
        parts.append(f"<p style='max-width:600px'>{html.escape(spec['para_below'])}</p>")
    parts.append("</body></html>")
    return "".join(parts)


def trim(png, margin):
    img = Image.open(png).convert("RGB")
    bg = Image.new("RGB", img.size, img.getpixel((2, 2)))
    diff = ImageChops.difference(img, bg).convert("L").point(lambda v: 255 if v > 12 else 0)
    box = diff.getbbox()
    if box:
        l, t, r, b = box
        img = img.crop((max(0, l - margin), max(0, t - margin), min(img.width, r + margin), min(img.height, b + margin)))
    img.save(png)
