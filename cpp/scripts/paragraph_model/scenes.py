"""Synthetic page scenes for training and testing the paragraph model. Text is real (Wikipedia intros fetched by
sources.py); the layout parameters are random. A scene returns (html, viewport_width, viewport_height). The ground
truth always comes from the rendered DOM (extract.js), never from the generator, so a scene cannot bias the labels:
every paragraph has to be one block element, without <br> inside it."""
import json, random, re, html as H

CORPUS = {}
SCENES = {}


def scene(name):
    def register(function):
        SCENES[name] = function
        return function
    return register


def load_corpus(path):
    CORPUS.clear()
    CORPUS.update(json.load(open(path, encoding="utf-8")))
FONTS = {
    "en": ['"Segoe UI"', 'Arial', '"Times New Roman"', 'Georgia', 'Verdana', 'Calibri', 'Tahoma'],
    "zh": ['"Microsoft YaHei"', 'SimSun', 'SimHei', '"Microsoft YaHei"', 'KaiTi'],
    "ja": ['Meiryo', '"Yu Gothic"', '"Yu Gothic"', 'Meiryo'],
    "ko": ['"Malgun Gothic"', '"Malgun Gothic"'],
}
CJK = ("zh", "ja", "ko")


def sentences(text, lang):
    parts = re.split(r"(?<=[。！？.!?])\s*", text) if True else [text]
    return [p for p in parts if p.strip()]


class Ctx:
    def __init__(self, rnd, lang):
        self.r, self.lang = rnd, lang
        self.paras = CORPUS[lang]["paragraphs"]
        self.titles = CORPUS[lang]["titles"]

    def para(self, lo=1, hi=5):
        """A paragraph of lo..hi sentences from a random intro."""
        p = self.r.choice(self.paras)
        ss = sentences(p, self.lang)
        n = self.r.randint(lo, hi)
        start = self.r.randint(0, max(0, len(ss) - n))
        t = "" if self.lang in CJK else " "
        out = t.join(ss[start:start + n]) if t else "".join(ss[start:start + n])
        return H.escape(out.strip())

    def short(self, lo=2, hi=6):
        """A short phrase: label, title, button caption."""
        p = self.r.choice(self.paras)
        ss = sentences(p, self.lang)
        s = self.r.choice(ss)
        if self.lang in CJK:
            n = self.r.randint(lo * 2, hi * 3)
            return H.escape(s[:n].strip("，、。 ,"))
        w = s.split()
        n = self.r.randint(lo, hi)
        return H.escape(" ".join(w[:n]).strip(",.;: "))

    def title(self):
        return H.escape(self.r.choice(self.titles))

    def font(self):
        return self.r.choice(FONTS[self.lang])


def pick(r, *vals): return r.choice(vals)


def theme(r):
    if r.random() < 0.28:
        return {"bg": r.choice(["#1e1e1e", "#202124", "#0f172a", "#2b2b2b"]), "fg": r.choice(["#e6e6e6", "#d4d4d4", "#e2e8f0"]),
                "muted": "#8b93a1", "card": "#2a2d33", "line": "#3a3f47", "accent": "#6aa9ff"}
    return {"bg": r.choice(["#ffffff", "#fafafa", "#f5f6f8", "#fffdf6"]), "fg": r.choice(["#202020", "#222", "#333", "#111"]),
            "muted": "#6b7280", "card": "#ffffff", "line": "#e1e4e8", "accent": "#2563eb"}


def page(body, css, t, font, size):
    return (f"<!doctype html><html><head><meta charset=utf-8><style>*{{box-sizing:border-box}}html,body{{margin:0}}"
            f"body{{background:{t['bg']};color:{t['fg']};font-family:{font},sans-serif;font-size:{size}px;{css}}}"
            f"p,h1,h2,h3,ul,ol,blockquote{{margin:0}}</style></head><body>{body}</body></html>")


# ---------------------------------------------------------------- templates
@scene("article")
def t_article(rnd, lang):
    c = Ctx(rnd, lang); t = theme(rnd); size = rnd.choice([14, 15, 16, 17, 18, 20])
    lh = rnd.choice([1.3, 1.5, 1.6, 1.8, 2.0]); width = rnd.randint(480, 900)
    style = rnd.choice(["gap", "gap", "tight", "indent"] if lang not in CJK else ["gap", "indent", "indent", "tight"])
    align = rnd.choice(["left", "left", "justify", "justify"])
    pmargin = {"gap": f"margin:0 0 {rnd.choice([0.6, 1, 1.4])}em", "tight": "margin:0 0 .35em", "indent": "margin:0;text-indent:2em"}[style]
    ml = rnd.choice(["margin:0 auto", "margin:0 0 0 40px", "margin:0 auto"])
    parts = [f"<h1 style='font-size:{size * rnd.choice([1.6, 2, 2.4]):.0f}px;margin:0 0 .5em'>{c.title()}</h1>"]
    if rnd.random() < .5: parts.append(f"<div style='color:{t['muted']};font-size:{size * .8:.0f}px;margin:0 0 1.2em'>{c.short(3, 6)} · {rnd.randint(2018, 2025)}-0{rnd.randint(1, 9)}-1{rnd.randint(0, 9)}</div>")
    for i in range(rnd.randint(4, 8)):
        k = rnd.random()
        if k < .12: parts.append(f"<h2 style='font-size:{size * 1.35:.0f}px;margin:1em 0 .5em'>{c.short(2, 5)}</h2>")
        elif k < .24:
            items = "".join(f"<li style='margin:0 0 {rnd.choice([0, .25, .5])}em'>{c.para(1, 2)}</li>" for _ in range(rnd.randint(2, 5)))
            tag = rnd.choice(["ul", "ol"]); parts.append(f"<{tag} style='margin:0 0 1em;padding-left:2em'>{items}</{tag}>")
        elif k < .32: parts.append(f"<blockquote style='margin:0 0 1em;padding-left:1.2em;border-left:3px solid {t['line']};color:{t['muted']}'>{c.para(1, 3)}</blockquote>")
        else: parts.append(f"<p style='{pmargin};text-align:{align}'>{c.para(1, 6)}</p>")
    css = f"line-height:{lh};padding:30px 24px"
    return page(f"<div style='width:{width}px;{ml}'>{''.join(parts)}</div>", css, t, c.font(), size), 1000, 760


@scene("columns")
def t_columns(rnd, lang):
    c = Ctx(rnd, lang); t = theme(rnd); size = rnd.choice([13, 14, 15, 16]); n = rnd.choice([2, 2, 3])
    lh = rnd.choice([1.4, 1.5, 1.7]); align = rnd.choice(["left", "justify"]); gap = rnd.choice([24, 32, 48])
    cols = []
    for _ in range(n):
        items = []
        for _ in range(rnd.randint(2, 4)):
            if rnd.random() < .5: items.append(f"<h3 style='font-size:{size * 1.2:.0f}px;margin:0 0 .3em'>{c.short(2, 5)}</h3>")
            items.append(f"<p style='margin:0 0 1em;text-align:{align}'>{c.para(1, 4)}</p>")
        cols.append(f"<div>{''.join(items)}</div>")
    body = (f"<h1 style='font-size:{size * 2:.0f}px;margin:0 0 .6em;text-align:{rnd.choice(['left', 'center'])}'>{c.title()}</h1>"
            f"<div style='display:grid;grid-template-columns:repeat({n},1fr);gap:{gap}px'>{''.join(cols)}</div>")
    return page(f"<div style='padding:28px 32px'>{body}</div>", f"line-height:{lh}", t, c.font(), size), 1100, 760


@scene("chat")
def t_chat(rnd, lang):
    c = Ctx(rnd, lang); t = theme(rnd); size = rnd.choice([13, 14, 15, 16]); lh = rnd.choice([1.3, 1.4, 1.5])
    mine_bg = rnd.choice(["#95ec69", "#d9fdd3", "#2b6cb0", "#0b93f6"]); mine_fg = "#fff" if mine_bg in ("#2b6cb0", "#0b93f6") else "#111"
    other_bg = t["card"] if t["bg"] != "#ffffff" else "#f1f1f1"
    width = rnd.randint(520, 760); rows = []
    last = None
    for i in range(rnd.randint(7, 12)):
        side = rnd.choice(["l", "r", "l", "r", last or "l"]); last = side
        gap = rnd.choice([4, 8, 8, 14, 22]); pad = rnd.choice([8, 10, 12])
        msg = c.para(1, 3) if rnd.random() < .7 else c.short(2, 6)
        name = f"<div style='font-size:{size * .8:.0f}px;color:{t['muted']};margin:0 0 3px'>{c.short(1, 2)}</div>" if side == "l" and rnd.random() < .6 else ""
        stamp = f"<div style='font-size:{size * .75:.0f}px;color:{t['muted']};text-align:center;margin:{gap + 6}px 0 {gap}px'>{rnd.randint(8, 22)}:{rnd.randint(10, 59)}</div>" if rnd.random() < .14 else ""
        bubble = (f"<div style='max-width:62%;padding:{pad}px {pad + 4}px;border-radius:8px;"
                  f"background:{mine_bg if side == 'r' else other_bg};color:{mine_fg if side == 'r' else t['fg']}'>{msg}</div>")
        avatar = f"<div style='width:36px;height:36px;border-radius:50%;background:{t['line']};flex:none'></div>"
        row = (f"<div style='display:flex;gap:10px;margin:{gap}px 0 0;justify-content:{'flex-start' if side == 'l' else 'flex-end'}'>"
               + (avatar + f"<div>{name}{bubble}</div>" if side == "l" else f"<div>{bubble}</div>" + avatar) + "</div>")
        rows.append(stamp + row)
    return page(f"<div style='width:{width}px;margin:0 auto;padding:20px 16px'>{''.join(rows)}</div>", f"line-height:{lh}", t, c.font(), size), 900, 760


@scene("settings")
def t_settings(rnd, lang):
    c = Ctx(rnd, lang); t = theme(rnd); size = rnd.choice([13, 14, 15]); width = rnd.randint(560, 760)
    pad = rnd.choice([10, 14, 18]); rows = []
    for g in range(rnd.randint(2, 3)):
        rows.append(f"<div style='font-size:{size * .85:.0f}px;color:{t['accent']};font-weight:600;margin:{22 if g else 4}px 0 6px'>{c.short(1, 3)}</div>")
        for _ in range(rnd.randint(3, 5)):
            desc = f"<div style='font-size:{size * .86:.0f}px;color:{t['muted']};margin-top:{rnd.choice([2, 4, 6])}px'>{c.para(1, 2)}</div>" if rnd.random() < .7 else ""
            ctl = rnd.choice([f"<div style='color:{t['muted']}'>{c.short(1, 2)}</div>", f"<div style='width:36px;height:20px;border-radius:10px;background:{t['accent']}'></div>",
                              f"<div style='padding:4px 12px;border:1px solid {t['line']};border-radius:4px'>{c.short(1, 2)}</div>"])
            rows.append(f"<div style='display:flex;justify-content:space-between;align-items:center;gap:20px;padding:{pad}px 12px;border-bottom:1px solid {t['line']}'>"
                        f"<div style='max-width:{width - 200}px'><div style='font-size:{size}px'>{c.short(2, 5)}</div>{desc}</div>{ctl}</div>")
    return page(f"<div style='width:{width}px;margin:0 auto;padding:20px 0'><h1 style='font-size:{size * 1.5:.0f}px;margin:0 0 .6em'>{c.title()}</h1>{''.join(rows)}</div>",
                "line-height:1.4", t, c.font(), size), 900, 760


@scene("table")
def t_table(rnd, lang):
    c = Ctx(rnd, lang); t = theme(rnd); size = rnd.choice([12, 13, 14, 15]); ncol = rnd.randint(3, 5); pad = rnd.choice([6, 8, 12])
    widths = [rnd.choice([110, 150, 200, 260]) for _ in range(ncol)]
    head = "".join(f"<th style='text-align:left;padding:{pad}px;border:1px solid {t['line']};width:{w}px'>{c.short(1, 2)}</th>" for w in widths)
    body = []
    for r_ in range(rnd.randint(5, 9)):
        cells = []
        for w in widths:
            k = rnd.random()
            txt = c.para(1, 2) if k < .45 else (str(rnd.randint(1, 9999)) if k < .6 else c.short(1, 4))
            al = rnd.choice(["left", "left", "center", "right"]) if txt.isdigit() else "left"
            zebra = f"background:{t['card']};" if r_ % 2 else ""
            cells.append(f"<td style='padding:{pad}px;border:1px solid {t['line']};vertical-align:{rnd.choice(['top', 'middle'])};text-align:{al};{zebra}'>{txt}</td>")
        body.append(f"<tr>{''.join(cells)}</tr>")
    return page(f"<div style='padding:24px'><table style='border-collapse:collapse;table-layout:fixed;width:{sum(widths)}px'><thead><tr>{head}</tr></thead><tbody>{''.join(body)}</tbody></table></div>",
                "line-height:1.35", t, c.font(), size), 1000, 760


@scene("feed")
def t_feed(rnd, lang):
    c = Ctx(rnd, lang); t = theme(rnd); size = rnd.choice([13, 14, 15]); width = rnd.randint(560, 800)
    cards = []
    sep = rnd.choice(["border", "card", "none"])
    for _ in range(rnd.randint(5, 8)):
        tags = "".join(f"<span style='font-size:{size * .8:.0f}px;padding:2px 8px;border-radius:10px;background:{t['line']}'>{c.short(1, 1)}</span>" for _ in range(rnd.randint(0, 3)))
        inner = (f"<div style='font-size:{size * 1.25:.0f}px;font-weight:600;margin:0 0 {rnd.choice([3, 6, 8])}px'>{c.para(1, 1)}</div>"
                 f"<div style='color:{t['fg']};margin:0 0 {rnd.choice([4, 6, 10])}px'>{c.para(1, 3)}</div>"
                 f"<div style='font-size:{size * .82:.0f}px;color:{t['muted']};margin:0 0 {rnd.choice([0, 6])}px'>{c.short(2, 3)} · {rnd.randint(1, 59)} min · {rnd.randint(0, 400)} comments</div>"
                 + (f"<div style='display:flex;gap:6px'>{tags}</div>" if tags else ""))
        st = {"border": f"padding:14px 0;border-bottom:1px solid {t['line']}", "card": f"padding:14px 16px;margin:0 0 12px;border:1px solid {t['line']};border-radius:8px;background:{t['card']}", "none": "padding:8px 0"}[sep]
        cards.append(f"<div style='{st}'>{inner}</div>")
    return page(f"<div style='width:{width}px;margin:0 auto;padding:16px'>{''.join(cards)}</div>", "line-height:1.45", t, c.font(), size), 900, 760


@scene("subtitle")
def t_subtitle(rnd, lang):
    c = Ctx(rnd, lang); size = rnd.choice([20, 22, 24, 26, 30]); w, h = 1000, 640
    grads = ["linear-gradient(160deg,#274472,#6a9bd1 55%,#c7b299)", "linear-gradient(200deg,#1b2735,#3b4b61 60%,#8a7f6a)", "linear-gradient(120deg,#5b8f4a,#d9c27a 70%,#8b5a3c)", "radial-gradient(circle at 30% 30%,#e2b4c8,#5b3b6d 80%)"]
    outline = "text-shadow:-2px -2px 0 #000,2px -2px 0 #000,-2px 2px 0 #000,2px 2px 0 #000" if rnd.random() < .6 else "text-shadow:0 2px 6px rgba(0,0,0,.8)"
    choices = ""
    if rnd.random() < .7:
        n = rnd.randint(3, 4); gap = rnd.choice([6, 10, 16])
        opts = "".join(f"<div style='padding:8px 18px;margin:0 0 {gap}px;background:rgba(0,0,0,.55);border:1px solid rgba(255,255,255,.4);color:#fff;font-size:{size - 2}px'>{c.short(2, 5)}</div>" for _ in range(n))
        choices = f"<div style='position:absolute;left:{w // 2 - 210}px;top:70px;width:420px'>{opts}</div>"
    name = f"<div style='display:inline-block;padding:3px 14px;background:rgba(20,20,40,.85);color:#ffd36b;font-size:{size - 2}px;margin:0 0 6px'>{c.short(1, 2)}</div>"
    dialog = (f"<div style='position:absolute;left:60px;right:60px;bottom:40px'>{name}"
              f"<div style='padding:16px 22px;background:rgba(10,10,30,.6);color:#fff;border-radius:6px;{outline};line-height:1.6'>{c.para(1, 2)}</div></div>")
    return page(f"<div style='position:relative;width:{w}px;height:{h}px;background:{rnd.choice(grads)}'>{choices}{dialog}</div>", "", {"bg": "#000", "fg": "#fff"}, c.font(), size), w, h


@scene("doc")
def t_doc(rnd, lang):
    c = Ctx(rnd, lang); t = {"bg": "#ffffff", "fg": "#111", "muted": "#555", "line": "#ccc", "card": "#fff", "accent": "#000"}
    size = rnd.choice([13, 14, 15]); width = rnd.randint(600, 760); lh = rnd.choice([1.35, 1.45, 1.55])
    indent = rnd.choice(["text-indent:1.5em;margin:0", "text-indent:2em;margin:0", "margin:0 0 .8em"]) if lang not in CJK else rnd.choice(["text-indent:2em;margin:0", "margin:0 0 .6em"])
    parts = [f"<div style='text-align:center;font-size:{size * 1.7:.0f}px;font-weight:700;margin:0 0 .5em'>{c.title()}</div>",
             f"<div style='text-align:center;margin:0 0 .15em'>{c.short(2, 3)}</div><div style='text-align:center;margin:0 0 1.2em;color:{t['muted']}'>{c.short(3, 5)}</div>",
             f"<div style='margin:0 0 .3em;font-weight:700'>{c.short(1, 1)}</div><p style='{indent};text-align:justify;margin-bottom:1em'>{c.para(2, 4)}</p>"]
    for i in range(1, rnd.randint(2, 4)):
        parts.append(f"<div style='font-weight:700;font-size:{size * 1.1:.0f}px;margin:1em 0 .4em'>{i} {c.short(1, 3)}</div>")
        for _ in range(rnd.randint(1, 3)): parts.append(f"<p style='{indent};text-align:justify'>{c.para(2, 5)}</p>")
        if rnd.random() < .5: parts.append(f"<div style='text-align:center;font-size:{size * .9:.0f}px;color:{t['muted']};margin:.8em 0'>Figure {i}: {c.short(3, 6)}</div>")
    refs = "".join(f"<div style='padding-left:2em;text-indent:-2em;font-size:{size * .88:.0f}px;margin:0 0 .3em'>[{i}] {c.para(1, 1)}</div>" for i in range(1, rnd.randint(3, 5)))
    parts.append(f"<div style='font-weight:700;margin:1em 0 .4em'>References</div>{refs}")
    return page(f"<div style='width:{width}px;margin:0 auto;padding:36px 20px'>{''.join(parts)}</div>", f"line-height:{lh}", t, c.font(), size), 900, 760


def make(name, seed, lang):
    rnd = random.Random(f"{name}:{seed}:{lang}")
    return SCENES[name](rnd, lang)
