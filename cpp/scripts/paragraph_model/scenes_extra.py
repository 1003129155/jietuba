"""More synthetic scenes, modelled on everyday screenshots: chat apps, spreadsheets, games, papers, slides, phone
apps, mail, shops, forums, docs, news, dialogs and nested lists. Same contract as scenes.py: every visually
separate piece of text is its own block element and never contains <br>; the labels are read back from the DOM.
Heights are estimated from character counts so that content stays inside the viewport."""
import re, urllib.parse
from scenes import scene, Ctx, theme, page, CJK, FONTS, H

DARK_BG = ("#1e1e1e", "#202124", "#0f172a", "#2b2b2b")
AVATARS = ["#f28b82", "#fbbc04", "#34a853", "#4285f4", "#a142f4", "#24c1e0", "#ff8a65", "#9e9d24", "#5c6bc0", "#8d6e63", "#ec407a", "#26a69a"]
MON = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
MONEY = {"en": "$", "zh": "¥", "ja": "¥", "ko": "₩"}

# Fixed interface wording that real apps show in every language; everything else comes from the corpus.
UI = {
    "en": dict(recall="{n} unsent a message", today="Today", yesterday="Yesterday", send="Send", cancel="Cancel", ok="OK",
               replies="{n} replies", fig="Fig.", tab="Table", abstract="Abstract", keywords="Keywords", sep="—", colon=": ",
               wrote="On {d}, {n} &lt;{a}&gt; wrote:", to="To", ago="{n}h ago", by="By {n}", read="{n} min read", reply="Reply",
               page="Page {n}", floor="#{n}", said="{n} wrote:"),
    "zh": dict(recall="“{n}”撤回了一条消息", today="今天", yesterday="昨天", send="发送", cancel="取消", ok="确定",
               replies="{n} 条回复", fig="图", tab="表", abstract="摘要", keywords="关键词", sep="：", colon="：",
               wrote="在 {d}，{n} &lt;{a}&gt; 写道：", to="收件人", ago="{n}小时前", by="记者 {n}", read="阅读约 {n} 分钟", reply="回复",
               page="第 {n} 页", floor="{n}楼", said="{n} 写道："),
    "ja": dict(recall="{n}がメッセージの送信を取り消しました", today="今日", yesterday="昨日", send="送信", cancel="キャンセル", ok="OK",
               replies="{n}件の返信", fig="図", tab="表", abstract="概要", keywords="キーワード", sep="：", colon="：",
               wrote="{d} {n} &lt;{a}&gt;:", to="宛先", ago="{n}時間前", by="{n} 記者", read="{n}分で読めます", reply="返信",
               page="{n} ページ", floor="{n}", said="{n}さんの投稿："),
    "ko": dict(recall="{n}님이 메시지를 삭제했습니다", today="오늘", yesterday="어제", send="보내기", cancel="취소", ok="확인",
               replies="답글 {n}개", fig="그림", tab="표", abstract="초록", keywords="키워드", sep=": ", colon=": ",
               wrote="{d}, {n} &lt;{a}&gt;님이 작성:", to="받는 사람", ago="{n}시간 전", by="{n} 기자", read="{n}분 분량", reply="답글",
               page="{n}페이지", floor="{n}번째", said="{n}님의 글:"),
}

ICONS = {
    "circle": "<circle cx='8' cy='8' r='6.2' fill='none' stroke='{c}' stroke-width='1.6'/>",
    "dot": "<circle cx='8' cy='8' r='6' fill='{c}'/>",
    "square": "<rect x='2' y='2' width='12' height='12' rx='3' fill='none' stroke='{c}' stroke-width='1.6'/>",
    "chev": "<path d='M6 3l5 5-5 5' fill='none' stroke='{c}' stroke-width='1.8'/>",
    "back": "<path d='M10 2L4 8l6 6' fill='none' stroke='{c}' stroke-width='2'/>",
    "down": "<path d='M3 6l5 5 5-5' fill='none' stroke='{c}' stroke-width='1.8'/>",
    "x": "<path d='M3 3l10 10M13 3L3 13' stroke='{c}' stroke-width='1.8'/>",
    "search": "<circle cx='7' cy='7' r='4.5' fill='none' stroke='{c}' stroke-width='1.6'/><path d='M10.5 10.5L14 14' stroke='{c}' stroke-width='1.8'/>",
    "info": "<circle cx='8' cy='8' r='7' fill='{c}'/><rect x='7.1' y='7' width='1.8' height='5' fill='#fff'/><rect x='7.1' y='4' width='1.8' height='1.8' fill='#fff'/>",
    "warn": "<path d='M8 1.5L15 14H1z' fill='{c}'/><rect x='7.1' y='6' width='1.8' height='4.5' fill='#fff'/><rect x='7.1' y='11.3' width='1.8' height='1.6' fill='#fff'/>",
    "star": "<path d='M8 1l2.1 4.6 5 .5-3.8 3.4 1.1 4.9L8 11.9 3.6 14.4l1.1-4.9L.9 6.1l5-.5z' fill='{c}'/>",
    "menu": "<path d='M2 4h12M2 8h12M2 12h12' stroke='{c}' stroke-width='1.6'/>",
    "plus": "<path d='M8 2v12M2 8h12' stroke='{c}' stroke-width='1.8'/>",
    "heart": "<path d='M8 14S1.5 9.6 1.5 5.4A3.3 3.3 0 0 1 8 4a3.3 3.3 0 0 1 6.5 1.4C14.5 9.6 8 14 8 14z' fill='none' stroke='{c}' stroke-width='1.5'/>",
    "home": "<path d='M2 8l6-5.5L14 8v6H2z' fill='none' stroke='{c}' stroke-width='1.6'/>",
    "bell": "<path d='M4 11V7a4 4 0 0 1 8 0v4l1.5 1.5h-11zM6.5 14h3' fill='none' stroke='{c}' stroke-width='1.5'/>",
}


RTL = re.compile("[%s-%s%s-%s%s-%s]" % tuple(map(chr, (0x590, 0x8ff, 0xfb1d, 0xfdff, 0xfe70, 0xfefc))))   # Hebrew, Arabic


class XCtx(Ctx):
    """Ctx without right-to-left paragraphs: mixed-direction lines come back from the DOM out of order and the
    block is dropped as not a vertical stack."""
    def __init__(self, rnd, lang):
        super().__init__(rnd, lang)
        self.paras = [p for p in self.paras if not RTL.search(p)] or self.paras


STOP_EN = {"the", "a", "an", "of", "in", "on", "at", "to", "and", "or", "is", "was", "by", "for", "with", "as", "from", "that",
           "which", "its", "his", "her", "their", "it", "he", "she", "be", "are", "were", "has", "had", "this", "also", "who"}
PUNCT = r"[，。、；：！？（）()「」『』《》〈〉“”\"'‘’,.;:!?\[\]·…—–/]+"


def lab(c, lo=1, hi=3):
    """A label-like phrase cut from the middle of a corpus sentence: menu items, buttons, names of things.
    Sentence starts ("The", "He") make poor labels, so the phrase starts at a random word."""
    for _ in range(10):
        s = H.unescape(c.r.choice(c.paras))
        if c.lang in ("zh", "ja"):
            runs = [r for r in re.split(PUNCT + r"|\s+", s) if len(r) >= 2]
            if not runs: continue
            r = c.r.choice(runs); n = c.r.randint(lo * 2, hi * 3)
            if len(r) > n:
                st = c.r.randint(0, len(r) - n); r = r[st:st + n]
            return H.escape(r)
        runs = [r.split() for r in re.split(PUNCT, s)]
        runs = [r for r in runs if len(r) >= lo]
        if not runs: continue
        w = c.r.choice(runs); n = min(len(w), c.r.randint(lo, hi)); st = c.r.randint(0, len(w) - n); w = w[st:st + n]
        if c.lang == "en":
            while len(w) > 1 and w[-1].lower() in STOP_EN: w.pop()
            while len(w) > 1 and w[0].lower() in STOP_EN: w.pop(0)
            if not w or w[0].lower() in STOP_EN: continue
        out = " ".join(w)
        return H.escape(out[0].upper() + out[1:] if c.lang == "en" else out)
    return c.short(lo, hi)


def ui_font(c):
    """Interface text is set in sans-serif faces."""
    return c.r.choice({"en": ['"Segoe UI"', '"Segoe UI"', 'Arial', 'Verdana', 'Tahoma', 'Calibri'],
                       "zh": ['"Microsoft YaHei"', '"Microsoft YaHei"', 'SimHei', 'SimSun']}.get(c.lang, FONTS[c.lang]))


def icon(sz, color, kind="circle"):
    return f"<svg width='{sz:.0f}' height='{sz:.0f}' viewBox='0 0 16 16' style='flex:none;display:block'>{ICONS[kind].format(c=color)}</svg>"


def is_dark(t): return t["bg"] in DARK_BG


def cl(n, lang):
    """A length given in Latin characters, converted for full-width scripts."""
    return n if lang not in CJK else max(4, n // 2)


def cut(s, n):
    raw = H.unescape(s).strip()
    if len(raw) <= n: return s
    return H.escape(raw[:max(1, n - 1)].rstrip(" ,.;:，、。・（(") + "…")


def trunc(s, n, lang):
    """Cut at a word boundary without an ellipsis: a paragraph that continues elsewhere."""
    raw = H.unescape(s).strip()
    if len(raw) <= n: return s
    if lang in CJK: return H.escape(raw[:n])
    k = raw.rfind(" ", 0, n)
    return H.escape(raw[:k if k > n // 2 else n])


def cpl(width, size, lang, k=None):
    k = k or (1.0 if lang in CJK else 0.55)
    return max(3, int(width / (size * k)))


def fit(s, width, size, lang):
    """Short enough to stay on one line at this width."""
    return cut(s, cpl(width, size, lang, 1.08 if lang in CJK else 0.64))


def nlines(s, width, size, lang):
    return max(1, -(-len(H.unescape(s)) // cpl(width, size, lang, 1.0 if lang in CJK else 0.56)))


def person(c):
    if c.lang == "en":
        w = H.unescape(c.title()).replace("(", " ").split()
        s = " ".join(w[:c.r.choice([1, 2, 2])])
    else:
        s = H.unescape(lab(c, 1, 2)).replace(" ", "" if c.lang != "ko" else " ")
        s = s[:c.r.randint(2, 4)]
    s = s.strip("()（）,，、.·:：「」『』《》\"' ")
    return H.escape(s or "A")


def addr(r):
    name = "".join(r.choice("abcdefghijklmnoprstuvwy") for _ in range(r.randint(3, 8)))
    sep = r.choice(["", ".", "_"]) + "".join(r.choice("abcdeghiklmnorstu") for _ in range(r.randint(0, 5)))
    return f"{name}{sep}@{r.choice(['mail.com', 'outlook.com', 'corp.net', 'lab.org', 'studio.io', 'univ.edu'])}"


def clock(r): return f"{r.randint(0, 23):02d}:{r.randint(0, 59):02d}"


def day(r, lang):
    y, m, d = r.randint(2016, 2025), r.randint(1, 12), r.randint(1, 28)
    if lang == "zh": return r.choice([f"{y}年{m}月{d}日", f"{y}-{m:02d}-{d:02d}", f"{m}月{d}日"])
    if lang == "ja": return r.choice([f"{y}年{m}月{d}日", f"{y}/{m:02d}/{d:02d}", f"{m}月{d}日"])
    if lang == "ko": return r.choice([f"{y}. {m}. {d}.", f"{y}년 {m}월 {d}일", f"{m}월 {d}일"])
    return r.choice([f"{MON[m - 1]} {d}, {y}", f"{m}/{d}/{y}", f"{d} {MON[m - 1]} {y}", f"{MON[m - 1]} {d}"])


def money(r, lang, lo=5, hi=900):
    v = r.uniform(lo, hi) * (130 if lang == "ko" else 7 if lang == "zh" else 110 if lang == "ja" else 1)
    return f"{MONEY[lang]}{v:,.0f}" if lang in ("ko", "ja") else f"{MONEY[lang]}{v:,.2f}"


def base_css(lh): return f"line-height:{lh};overflow:hidden;overflow-wrap:break-word"


# ------------------------------------------------------------------ 1 chat app
@scene("chat_app")
def s_chat_app(rnd, lang):
    c = XCtx(rnd, lang); u = UI[lang]
    dark = rnd.random() < .35
    if dark:
        P = rnd.choice([
            dict(bg="#0e1621", side="#17212b", head="#17212b", fg="#f5f5f5", muted="#7f91a4", line="#0b1119", other="#182533", mine="#2b5278", mine_fg="#ffffff", accent="#6ab3f3", sys="rgba(255,255,255,.08)", sys_fg="#c9d4df", input="#242f3d"),
            dict(bg="#313338", side="#2b2d31", head="#313338", fg="#dbdee1", muted="#949ba4", line="#26272b", other="#383a40", mine="#5865f2", mine_fg="#ffffff", accent="#00a8fc", sys="rgba(255,255,255,.06)", sys_fg="#b5bac1", input="#383a40"),
            dict(bg="#0b141a", side="#111b21", head="#202c33", fg="#e9edef", muted="#8696a0", line="#222d34", other="#202c33", mine="#005c4b", mine_fg="#e9edef", accent="#00a884", sys="#182229", sys_fg="#8696a0", input="#2a3942")])
    else:
        P = rnd.choice([
            dict(bg="#f5f5f5", side="#e9e9e9", head="#f5f5f5", fg="#191919", muted="#9a9a9a", line="#dddddd", other="#ffffff", mine="#95ec69", mine_fg="#111111", accent="#576b95", sys="rgba(0,0,0,.06)", sys_fg="#8a8a8a", input="#ffffff"),
            dict(bg="#cfe0b8", side="#ffffff", head="#ffffff", fg="#000000", muted="#8b9398", line="#e4e4e4", other="#ffffff", mine="#effdde", mine_fg="#000000", accent="#3390ec", sys="rgba(0,0,0,.22)", sys_fg="#ffffff", input="#f1f1f1"),
            dict(bg="#efeae2", side="#ffffff", head="#f0f2f5", fg="#111b21", muted="#667781", line="#e9edef", other="#ffffff", mine="#d9fdd3", mine_fg="#111b21", accent="#008069", sys="#ffffff", sys_fg="#54656f", input="#ffffff"),
            dict(bg="#ffffff", side="#f6f6f6", head="#ffffff", fg="#1d1c1d", muted="#616061", line="#e6e6e6", other="#e9e9eb", mine="#0b84fe", mine_fg="#ffffff", accent="#1264a3", sys="rgba(0,0,0,.05)", sys_fg="#7a7a7a", input="#f4f4f4")])
    style = rnd.choice(["bubble", "bubble", "bubble", "flat"])
    size = rnd.choice([13, 14, 14, 15, 16]); lh = rnd.choice([1.35, 1.4, 1.5]); font = ui_font(c)
    W, Hh = rnd.choice([(1100, 780), (1200, 800), (1000, 760), (1280, 840)])
    side_w = rnd.choice([0, 260, 280, 300]) if W >= 1100 else rnd.choice([0, 0, 240])
    main_w = W - side_w; head_h = 60; input_h = rnd.choice([96, 112, 136]); msg_h = Hh - head_h - input_h
    group_chat = rnd.random() < .7
    me = person(c); people = [person(c) for _ in range(rnd.randint(2, 5) if group_chat else 1)]
    color = {n: AVATARS[i % len(AVATARS)] for i, n in enumerate(rnd.sample([me] + people, len(people) + 1))}
    av_r = rnd.choice(["50%", "6px", "4px"]); av = rnd.choice([34, 36, 40])
    bub_max = int((main_w - 2 * av - 90) * rnd.choice([.6, .68, .76]))
    py, px_ = rnd.choice([(6, 10), (8, 12), (9, 14)]); rad = rnd.choice([4, 8, 12, 16, 18])
    tight = rnd.choice([2, 3, 4, 6]); group_gap = rnd.choice([10, 14, 18])
    av_mode = rnd.choice(["first", "last", "last", "each"]); time_in = rnd.random() < .45
    quote_mode = rnd.choice(["inside", "below"]); mine_avatar = rnd.random() < .6
    sm = size * .78; small = f"font-size:{sm:.1f}px"
    shadow = "box-shadow:0 1px 1px rgba(0,0,0,.12);" if not dark and rnd.random() < .6 else ""

    nmax = [5]                                       # drops to 1 once groups stop fitting

    def text():
        k = rnd.random()
        if k < .35: return lab(c, 2, 7)
        if k < .8: return cut(c.para(1, 1), cl(rnd.choice([60, 90, 140]), lang))
        return cut(c.para(1, 2), cl(240, lang))

    def bubble(mine, width):
        t = text(); w = width - 2 * px_; h = nlines(t, w, size, lang) * size * lh + 2 * py
        bg = P["mine"] if mine else P["other"]; fg = P["mine_fg"] if mine else P["fg"]
        q = below = ""
        if rnd.random() < .14:
            qt = cut(c.para(1, 1), cl(70, lang)); qn = rnd.choice(people + [me])
            if quote_mode == "inside":
                q = (f"<div style='border-left:3px solid {P['accent']};padding:2px 8px;margin:0 0 5px;border-radius:3px;background:rgba(127,127,127,.14)'>"
                     f"<div style='font-weight:600;color:{P['accent']};font-size:{size * .88:.1f}px'>{qn}</div>"
                     f"<div style='font-size:{size * .88:.1f}px'>{qt}</div></div>")
                h += (1 + nlines(qt, w - 14, size * .88, lang)) * size * .88 * lh + 10
            else:
                below = (f"<div style='max-width:{width}px;padding:4px 8px;border-radius:4px;background:{P['sys']};color:{P['sys_fg'] if P['sys'] != '#ffffff' else P['muted']};{small}'>{qn}: {qt}</div>")
                h += nlines(qt, width - 16, sm, lang) * sm * 1.45 + 8 + tight
        tm = ""
        if time_in:
            tc = "rgba(255,255,255,.72)" if mine and fg == "#ffffff" else P["muted"]
            tm = f"<div style='text-align:right;{small};color:{tc};margin-top:1px'>{clock(rnd)}</div>"; h += sm * 1.3
        return (f"<div style='max-width:{width}px;padding:{py}px {px_}px;border-radius:{rad}px;background:{bg};color:{fg};{shadow}'>{q}<div>{t}</div>{tm}</div>{below}"), h

    def avatar(n, show=True):
        if not show: return f"<div style='width:{av}px;flex:none'></div>"
        return f"<div style='width:{av}px;height:{av}px;border-radius:{av_r};background:{color[n]};flex:none'></div>"

    def group_bubble(sender):
        mine = sender == me; n = min(nmax[0], rnd.choice([1, 2, 2, 3, 3, 4]))
        show_name = not mine and group_chat and rnd.random() < .85
        name = f"<div style='{small};color:{P['muted']}'>{sender}</div>"
        side_av = (not mine) or mine_avatar
        rows = []; h = 0
        if av_mode == "each":
            for _ in range(n):
                b, bh = bubble(mine, bub_max)
                col = f"<div style='display:flex;flex-direction:column;gap:3px;align-items:{'flex-end' if mine else 'flex-start'};min-width:0'>{name if show_name else ''}{b}</div>"
                a = avatar(sender) if side_av else ""
                rows.append(f"<div style='display:flex;gap:10px;align-items:flex-start;justify-content:{'flex-end' if mine else 'flex-start'}'>{col + a if mine else a + col}</div>")
                h += max(av, bh + (sm * 1.4 if show_name else 0)) + 14
            return f"<div style='display:flex;flex-direction:column;gap:14px'>{''.join(rows)}</div>", h
        parts = [name] if show_name else []; h = sm * 1.4 if show_name else 0
        for _ in range(n):
            b, bh = bubble(mine, bub_max); parts.append(b); h += bh + tight
        col = f"<div style='display:flex;flex-direction:column;gap:{tight}px;align-items:{'flex-end' if mine else 'flex-start'};min-width:0'>{''.join(parts)}</div>"
        a = avatar(sender) if side_av else ""
        al = "flex-start" if av_mode == "first" else "flex-end"
        return (f"<div style='display:flex;gap:10px;align-items:{al};justify-content:{'flex-end' if mine else 'flex-start'}'>{col + a if mine else a + col}</div>"), max(h, av)

    def group_flat(sender):
        n = min(nmax[0], rnd.choice([1, 2, 3, 3, 4, 5])); w = main_w - 48 - av - 30
        head = (f"<div><span style='font-weight:700'>{sender}</span><span style='{small};color:{P['muted']};margin-left:8px'>"
                f"{rnd.choice([u['today'], u['yesterday'], day(rnd, lang)])} {clock(rnd)}</span></div>")
        msgs = []; h = size * lh
        for _ in range(n):
            t = text(); msgs.append(f"<div style='margin-top:{tight}px'>{t}</div>"); h += nlines(t, w, size, lang) * size * lh + tight
        if rnd.random() < .25:
            msgs.append(f"<div style='margin-top:4px;{small};color:{P['accent']};font-weight:600'>{u['replies'].format(n=rnd.randint(2, 30))}</div>"); h += sm * 1.5
        return (f"<div style='display:flex;gap:10px;align-items:flex-start'>{avatar(sender)}<div style='flex:1;min-width:0'>{head}{''.join(msgs)}</div></div>"), h

    def notice():
        k = rnd.random()
        if k < .5: t = f"{rnd.choice([u['today'], u['yesterday'], day(rnd, lang)])} {clock(rnd)}"
        elif k < .8: t = u["recall"].format(n=rnd.choice(people + [me]))
        else: t = lab(c, 3, 7)
        if style == "flat" and rnd.random() < .6:
            ln = f"<div style='flex:1;height:1px;background:{P['line'] if not dark else P['muted']}'></div>"
            return f"<div style='display:flex;align-items:center;gap:12px'>{ln}<div style='{small};color:{P['muted']};font-weight:600'>{t}</div>{ln}</div>", sm * 1.5
        return (f"<div style='align-self:center;max-width:70%;text-align:center;padding:3px 10px;border-radius:10px;background:{P['sys']};color:{P['sys_fg']};{small}'>{t}</div>"), sm * 1.5 + 6

    items = []; total = 24; last = None; misses = 0
    for _ in range(60):
        if rnd.random() < .16 and last != "sys":
            html, h = notice(); who = "sys"
        else:
            who = rnd.choice([s for s in [me] + people if s != last] or [me])
            html, h = group_flat(who) if style == "flat" else group_bubble(who)
        if total + h + group_gap > msg_h - 16:
            misses += 1; nmax[0] = 1
            if misses > 10: break
            continue
        items.append(html); total += h + group_gap; last = who
    chat_bg = P["bg"] if style == "bubble" else P["head"]
    msgs = (f"<div style='height:{msg_h}px;overflow:hidden;display:flex;flex-direction:column;justify-content:flex-end;gap:{group_gap}px;padding:12px 24px;background:{chat_bg}'>{''.join(items)}</div>")
    title = fit(lab(c, 1, 4) if group_chat else people[0], main_w - 200, size * 1.1, lang)
    sub = f"<div style='{small};color:{P['muted']}'>{fit(lab(c, 1, 3), main_w - 200, sm, lang)}</div>" if rnd.random() < .7 else ""
    head = (f"<div style='height:{head_h}px;display:flex;align-items:center;justify-content:space-between;padding:0 20px;background:{P['head']};border-bottom:1px solid {P['line']}'>"
            f"<div><div style='font-size:{size * 1.1:.1f}px;font-weight:600'>{title}</div>{sub}</div>"
            f"<div style='display:flex;gap:16px'>{icon(18, P['muted'], 'search')}{icon(18, P['muted'], 'menu')}</div></div>")
    tools = "".join(icon(18, P["muted"], k) for k in rnd.sample(["circle", "square", "plus", "heart", "star"], 4))
    inp = (f"<div style='height:{input_h}px;border-top:1px solid {P['line']};background:{P['head']};padding:10px 20px;display:flex;flex-direction:column;gap:8px'>"
           f"<div style='display:flex;gap:14px'>{tools}</div>"
           f"<div style='display:flex;gap:10px;align-items:center'><div style='flex:1;color:{P['muted']};padding:7px 12px;border-radius:8px;background:{P['input']}'>{fit(lab(c, 2, 5), main_w - 180, size, lang)}</div>"
           f"<button style='font:inherit;padding:6px 18px;border:0;border-radius:6px;background:{P['accent']};color:#fff'>{u['send']}</button></div></div>")
    side = ""
    if side_w:
        rows = [f"<div style='margin:12px;padding:6px 10px;border-radius:6px;background:{P['input']};color:{P['muted']};display:flex;gap:6px;align-items:center'>{icon(14, P['muted'], 'search')}<div>{fit(lab(c, 1, 2), side_w - 70, size, lang)}</div></div>"]
        y = 60; tw = side_w - 42 - 34
        for i in range(30):
            if y + 64 > Hh: break
            nm = fit(lab(c, 1, 3) if rnd.random() < .5 else person(c), tw - 60, size, lang)
            badge = (f"<div style='min-width:18px;padding:0 5px;border-radius:9px;background:{P['accent'] if rnd.random() < .5 else '#f04a4a'};color:#fff;font-size:11px;line-height:18px;text-align:center'>{rnd.randint(1, 99)}</div>") if rnd.random() < .3 else ""
            act = f"background:{P['accent']}22;" if i == 1 else ""
            rows.append(f"<div style='display:flex;gap:10px;padding:10px 12px;align-items:center;{act}'>"
                        f"<div style='width:42px;height:42px;border-radius:{av_r};background:{rnd.choice(AVATARS)};flex:none'></div>"
                        f"<div style='flex:1;min-width:0'><div style='display:flex;justify-content:space-between;gap:8px'><div style='font-weight:600'>{nm}</div>"
                        f"<div style='{small};color:{P['muted']}'>{rnd.choice([clock(rnd), u['yesterday'], day(rnd, lang)])}</div></div>"
                        f"<div style='display:flex;justify-content:space-between;gap:8px;margin-top:2px'><div style='color:{P['muted']};font-size:{size * .88:.1f}px'>{fit(c.para(1, 1), tw - (30 if badge else 0), size * .88, lang)}</div>{badge}</div></div></div>")
            y += 64
        side = f"<div style='width:{side_w}px;flex:none;background:{P['side']};border-right:1px solid {P['line']};overflow:hidden'>{''.join(rows)}</div>"
    body = f"<div style='display:flex;width:{W}px;height:{Hh}px'>{side}<div style='width:{main_w}px;display:flex;flex-direction:column'>{head}{msgs}{inp}</div></div>"
    return page(body, base_css(lh), {"bg": P["bg"], "fg": P["fg"]}, font, size), W, Hh


# ------------------------------------------------------------------ 2 spreadsheet
@scene("spreadsheet")
def s_spreadsheet(rnd, lang):
    c = XCtx(rnd, lang); dark = rnd.random() < .22
    P = (dict(bg="#1f1f1f", fg="#e3e3e3", grid="#3b3b3b", head="#2b2b2b", hfg="#b8b8b8", accent="#3fb67a", chrome="#2b2b2b", sel="#2f3d34", hdr="#26352c", frz="#777")
         if dark else dict(bg="#ffffff", fg="#000000", grid="#d6d6d6", head="#f3f3f3", hfg="#444444", accent="#217346", chrome="#f3f3f3", sel="#d3ebdc", hdr=rnd.choice(["#ddebf7", "#e2efda", "#fce4d6", "#f2f2f2", "#4472c4"]), frz="#9e9e9e"))
    hdr_fg = "#ffffff" if P["hdr"] == "#4472c4" else P["fg"]
    size = rnd.choice([11, 12, 12, 13, 14]); font = ui_font(c); lh = 1.25
    W, Hh = rnd.choice([(1200, 800), (1280, 820), (1100, 760), (1280, 760)])
    sm = size * .92; s = size / 12
    tabs = "".join(f"<div style='padding:5px 12px;{('border-bottom:2px solid ' + P['accent'] + ';color:' + P['accent']) if i == 1 else ''}'>{fit(lab(c, 1, 1), 90, size, lang)}</div>" for i in range(rnd.randint(6, 9)))
    tool_items = []
    for i in range(rnd.randint(6, 10)):
        if rnd.random() < .3: tool_items.append(f"<div style='padding:2px 8px;border:1px solid {P['grid']};background:{P['bg']}'>{fit(lab(c, 1, 1), 80, size, lang)}</div>")
        else: tool_items.append(icon(16, P["hfg"], rnd.choice(["square", "circle", "plus", "down", "star"])))
    nm_cell = f"{chr(65 + rnd.randint(0, 5))}{rnd.randint(2, 30)}"
    chrome = (f"<div style='background:{P['chrome']};border-bottom:1px solid {P['grid']}'><div style='display:flex;gap:4px;padding:2px 8px 0'>{tabs}</div>"
              f"<div style='display:flex;gap:14px;align-items:center;padding:6px 12px;border-top:1px solid {P['grid']}'>{''.join(tool_items)}</div></div>"
              f"<div style='display:flex;align-items:center;border-bottom:1px solid {P['grid']};height:26px'>"
              f"<div style='width:90px;padding:0 6px;border-right:1px solid {P['grid']}'>{nm_cell}</div><div style='padding:0 10px;color:{P['hfg']};font-style:italic'>fx</div>"
              f"<div style='flex:1;padding:0 8px;border-left:1px solid {P['grid']}'>{fit(lab(c, 2, 6), W - 200, size, lang)}</div></div>")
    rowhdr = int(30 * s + 8)
    widths = {"text": (90, 160), "long": (180, 280), "num": (70, 100), "money": (90, 120), "pct": (60, 80), "date": (85, 110), "code": (70, 90), "int": (50, 70)}
    cols = []; total = rowhdr
    for i in range(10):
        k = "text" if i == 0 else rnd.choice(["text", "text", "num", "money", "pct", "date", "long", "code", "int"])
        w = int(rnd.randint(*widths[k]) * s)
        if total + w > W - 30: break
        cols.append((k, w)); total += w
    while total + 70 * s <= W - 10: cols.append(("empty", int(70 * s))); total += int(70 * s)
    if W - total > 8: cols.append(("empty", W - total)); total = W
    data_cols = [i for i, (k, _) in enumerate(cols) if k != "empty"]
    bord = f"border:1px solid {P['grid']}"
    va = rnd.choice(["bottom", "bottom", "middle", "top"])

    def td(txt, align="left", extra=""):
        return f"<td style='{bord};padding:1px 4px;text-align:{align};vertical-align:{va};{extra}'>{txt}</td>"

    def rownum(i, sel=False):
        return f"<td style='{bord};background:{P['sel'] if sel else P['head']};color:{P['hfg']};text-align:center;font-size:{sm:.1f}px'>{i}</td>"

    def value(k, w):
        if k == "text": return fit(lab(c, 1, 3), w - 8, size, lang), "left"
        if k == "long": return cut(c.para(1, 1), cpl(w - 8, size, lang) * rnd.choice([2, 3])), "left"
        if k == "num": return f"{rnd.uniform(0, 10 ** rnd.randint(1, 5)):,.2f}", "right"
        if k == "int": return str(rnd.randint(0, 999)), "right"
        if k == "money":
            v = money(rnd, lang, 1, 9000)
            return (f"<span style='color:#c00000'>({v})</span>" if rnd.random() < .12 else v), "right"
        if k == "pct": return f"{rnd.uniform(-20, 120):.1f}%", "right"
        if k == "date": return f"{rnd.randint(2018, 2025)}/{rnd.randint(1, 12):02d}/{rnd.randint(1, 28):02d}", "right"
        if k == "code": return f"{''.join(rnd.choice('ABCDEFGHJKLMNPRSTUVWXYZ') for _ in range(2))}-{rnd.randint(100, 9999)}", "center"
        return "", "left"

    sel_r = rnd.randint(2, 8); sel_c = rnd.choice(data_cols)
    letters = "".join(f"<th style='{bord};background:{P['sel'] if j == sel_c else P['head']};color:{P['hfg']};font-weight:400;font-size:{sm:.1f}px;height:{size + 8}px'>{chr(65 + j)}</th>" for j in range(len(cols)))
    rows = [f"<tr><th style='{bord};background:{P['head']}'></th>{letters}</tr>"]
    base = int(size * lh + 6)
    avail = Hh - 34 - 36 - 26 - (size + 10) - 54
    used = 0; r = 1
    if rnd.random() < .45:
        rows.append(f"<tr style='height:{int(base * 1.5)}px'>{rownum(r)}<td colspan='{len(data_cols)}' style='{bord};text-align:center;font-weight:700;font-size:{size * 1.3:.1f}px;vertical-align:middle'>{fit(c.title(), sum(cols[j][1] for j in data_cols) - 20, size * 1.3, lang)}</td>"
                    + "".join(td("") for _ in range(len(cols) - len(data_cols))) + "</tr>")
        used += base * 1.5; r += 1
    heads = []
    hh = base
    for j, (k, w) in enumerate(cols):
        if k == "empty": heads.append(td("", extra=f"border-bottom:2px solid {P['frz']}")); continue
        t = lab(c, 1, 3) if k != "long" else lab(c, 1, 2)
        t = cut(t, cpl(w - 8, size, lang) * 2); hh = max(hh, nlines(t, w - 8, size, lang) * size * lh + 6)
        heads.append(f"<td style='{bord};border-bottom:2px solid {P['frz']};background:{P['hdr']};color:{hdr_fg};font-weight:700;padding:2px 4px;text-align:{'right' if k in ('num', 'money', 'pct', 'int') and rnd.random() < .5 else 'left'};vertical-align:bottom'>{t}</td>")
    rows.append(f"<tr style='height:{hh:.0f}px'>{rownum(r)}{''.join(heads)}</tr>"); used += hh; r += 1
    merge = cols[0][0] == "text" and rnd.random() < .5
    run_left = 0
    nrows = 0
    while True:
        cells = []; h = base; saved = run_left
        for j, (k, w) in enumerate(cols):
            if j == 0 and merge:
                if run_left > 0: run_left -= 1; continue
                span = rnd.choice([1, 2, 2, 3])
                run_left = span - 1
                t, al = value(k, w)
                cells.append(f"<td rowspan='{span}' style='{bord};padding:1px 4px;vertical-align:middle;text-align:{rnd.choice(['left', 'center'])}'>{t}</td>")
                continue
            if k != "empty" and rnd.random() < .05: cells.append(td("")); continue
            t, al = value(k, w)
            if k == "long": h = max(h, nlines(t, w - 8, size, lang) * size * lh + 6)
            extra = f"outline:2px solid {P['accent']};outline-offset:-2px;" if (r == sel_r and j == sel_c) else ""
            cells.append(td(t, al, extra))
        if used + h > avail - base * 2: run_left = saved; break
        rows.append(f"<tr style='height:{h:.0f}px'>{rownum(r, r == sel_r)}{''.join(cells)}</tr>"); used += h; r += 1; nrows += 1
        if nrows >= 26: break

    def first_cell(html):
        """Rows after the data still sit under a merged run of the first column."""
        nonlocal run_left
        if run_left > 0: run_left -= 1; return ""
        return html
    if rnd.random() < .45:
        line = f"border-top:2px solid {P['fg']}"
        cells = [first_cell(td(f"<b>{fit(lab(c, 1, 1), cols[0][1] - 8, size, lang)}</b>", extra=line))]
        for k, w in cols[1:]:
            cells.append(td(f"<b>{rnd.uniform(100, 99999):,.2f}</b>", "right", line) if k in ("num", "money", "int") else td("", extra=line))
        rows.append(f"<tr style='height:{base}px'>{rownum(r)}{''.join(cells)}</tr>"); used += base; r += 1
    while used + base <= avail:
        rows.append(f"<tr style='height:{base}px'>{rownum(r)}{first_cell(td(''))}{''.join(td('') for _ in cols[1:])}</tr>"); used += base; r += 1
    colgroup = f"<col style='width:{rowhdr}px'>" + "".join(f"<col style='width:{w}px'>" for _, w in cols)
    table = f"<table style='border-collapse:collapse;table-layout:fixed;width:{rowhdr + sum(w for _, w in cols)}px;background:{P['bg']}'><colgroup>{colgroup}</colgroup>{''.join(rows)}</table>"
    sheets = "".join(f"<div style='padding:3px 14px;{('background:' + P['bg'] + ';color:' + P['accent'] + ';border-bottom:2px solid ' + P['accent'] + ';font-weight:600') if i == 0 else ''}'>{fit(lab(c, 1, 1), 110, size, lang)}</div>" for i in range(rnd.randint(2, 5)))
    stats = "".join(f"<div>{fit(lab(c, 1, 1), 80, sm, lang)}{UI[lang]['colon']}{rnd.uniform(10, 9999):,.1f}</div>" for _ in range(rnd.randint(1, 3)))
    bottom = (f"<div style='position:absolute;left:0;right:0;bottom:0;background:{P['chrome']};border-top:1px solid {P['grid']}'>"
              f"<div style='display:flex;align-items:center;gap:2px;padding:0 8px;height:28px'>{icon(14, P['hfg'], 'back')}{icon(14, P['hfg'], 'chev')}{sheets}{icon(14, P['hfg'], 'plus')}</div>"
              f"<div style='display:flex;justify-content:space-between;padding:0 12px;height:24px;align-items:center;font-size:{sm:.1f}px;color:{P['hfg']};border-top:1px solid {P['grid']}'>"
              f"<div>{fit(lab(c, 1, 1), 120, sm, lang)}</div><div style='display:flex;gap:28px'>{stats}<div>100%</div></div></div></div>")
    body = f"<div style='position:relative;width:{W}px;height:{Hh}px'>{chrome}{table}{bottom}</div>"
    return page(body, base_css(lh), {"bg": P["bg"], "fg": P["fg"]}, font, size), W, Hh


# ------------------------------------------------------------------ 3 game ui
@scene("game_ui")
def s_game(rnd, lang):
    c = XCtx(rnd, lang); font = c.font()
    W, Hh = rnd.choice([(1280, 720), (1200, 675), (1280, 800), (1152, 648)])
    size = rnd.choice([14, 15, 16, 17, 18]); lh = 1.4
    sky = rnd.choice(["linear-gradient(180deg,#1c2a4a 0%,#41618f 45%,#b98a5e 100%)", "radial-gradient(ellipse at 70% 30%,#6a8f5c,#1d2b1a 72%)",
                      "linear-gradient(135deg,#2b1b3d,#6b2f4f 50%,#d07a4a)", "linear-gradient(0deg,#0b1a12,#24563f 60%,#7fb3a0)", "linear-gradient(180deg,#0a0f1f,#1d2b55 60%,#3f5f8f)"])
    pattern = rnd.choice(["", "repeating-linear-gradient(45deg,rgba(255,255,255,.045) 0 12px,transparent 12px 24px),",
                          "repeating-radial-gradient(circle at 20% 80%,rgba(0,0,0,.12) 0 6px,transparent 6px 18px),"])
    scenery = "".join(f"<div style='position:absolute;left:{rnd.randint(-200, W - 200)}px;bottom:0;width:{rnd.randint(300, 700)}px;height:{rnd.randint(120, 380)}px;background:rgba(0,0,0,{rnd.uniform(.15, .4):.2f});clip-path:polygon(0 100%,{rnd.randint(25, 75)}% 0,100% 100%)'></div>" for _ in range(rnd.randint(2, 5)))
    if rnd.random() < .5: scenery += f"<div style='position:absolute;left:{rnd.randint(100, W - 200)}px;top:{rnd.randint(30, 160)}px;width:90px;height:90px;border-radius:50%;background:rgba(255,240,200,.55)'></div>"
    style = rnd.choice(["scifi", "fantasy", "minimal", "parchment"])
    panel = {"scifi": "background:rgba(8,14,30,.8);border:1px solid rgba(120,190,255,.55);border-radius:4px;box-shadow:0 0 12px rgba(60,160,255,.25)",
             "fantasy": "background:linear-gradient(180deg,rgba(42,30,16,.93),rgba(22,15,8,.9));border:2px solid #b8913d;border-radius:3px;box-shadow:0 0 0 1px #000",
             "minimal": "background:rgba(0,0,0,.58);border-radius:12px",
             "parchment": "background:rgba(238,226,196,.95);border:2px solid #6b4f2a;border-radius:4px;box-shadow:2px 3px 8px rgba(0,0,0,.5)"}[style]
    paper = style == "parchment"
    fg = "#2b1d0e" if paper else "#f2efe6"; muted = "#6b5636" if paper else "#b9b2a2"; gold = "#8a3b00" if paper else "#ffd36b"
    ts = "" if paper else rnd.choice(["text-shadow:-1px -1px 0 #000,1px -1px 0 #000,-1px 1px 0 #000,1px 1px 0 #000;",
                                       "text-shadow:2px 2px 2px rgba(0,0,0,.9);", "text-shadow:0 0 5px rgba(0,0,0,.95),0 0 2px #000;", ""])
    have = {k: rnd.random() < p for k, p in (("dialog", .6), ("quest", .65), ("tip", .6), ("menu", .45), ("hud", .6), ("chat", .55))}
    if have["dialog"]: have["chat"] = False
    while sum(have[k] for k in ("dialog", "quest", "tip", "menu", "chat")) < 3:
        have[rnd.choice(["quest", "tip", "menu", "quest" if have["dialog"] else "chat"])] = True
    parts = []
    dlg_h = rnd.randint(140, 180) if have["dialog"] else 0
    chat_h = rnd.randint(170, 240) if have["chat"] else 0
    floor = Hh - dlg_h - 56 if have["dialog"] else (Hh - chat_h - 50 if have["chat"] else Hh - 24)
    top_l = 20
    if have["hud"]:
        bars = "".join(f"<div style='position:relative;width:240px;height:{size + 4}px;margin-top:4px;background:rgba(0,0,0,.55);border:1px solid rgba(255,255,255,.35)'>"
                       f"<div style='position:absolute;left:0;top:0;bottom:0;width:{rnd.randint(25, 100)}%;background:{col}'></div>"
                       f"<div style='position:relative;text-align:center;font-size:{size * .78:.1f}px;line-height:{size + 4}px;color:#fff;text-shadow:0 0 3px #000'>{v} / {mx}</div></div>"
                       for col, v, mx in (("#b3261e", rnd.randint(100, 2000), rnd.randint(2000, 4000)), ("#1e5bb3", rnd.randint(50, 500), rnd.randint(500, 900)))[:rnd.randint(1, 2)])
        parts.append(f"<div style='position:absolute;left:20px;top:16px;color:#fff;{ts or 'text-shadow:0 0 3px #000;'}'>"
                     f"<div style='display:flex;gap:10px;align-items:baseline'><div style='font-weight:700'>{fit(person(c), 160, size, lang)}</div><div style='font-size:{size * .85:.1f}px;color:#ffd36b'>Lv. {rnd.randint(1, 99)}</div></div>{bars}</div>")
        top_l = 16 + size * 1.4 + (size + 8) * 2 + 20
    qw = rnd.randint(300, 360); tw = rnd.randint(280, 330)
    if have["quest"]:
        avail = floor - top_l - 40; used = size * 1.6 + 30; qs = []
        for i in range(6):
            desc = cut(c.para(1, 2), cpl(qw - 44, size * .88, lang) * rnd.choice([2, 3]))
            objs = [f"<div style='font-size:{size * .86:.1f}px;margin-top:2px'>{rnd.choice(['◇', '◆', '□', '•', '-'])} {fit(lab(c, 1, 4), qw - 110, size * .86, lang)} <span style='color:{gold}'>{rnd.randint(0, 5)}/{rnd.randint(5, 10)}</span></div>" for _ in range(rnd.randint(0, 2))]
            h = size * lh + nlines(desc, qw - 44, size * .88, lang) * size * .88 * lh + len(objs) * size * .86 * lh + 22
            if used + h > avail: continue
            selq = f"background:{'rgba(0,0,0,.08)' if paper else 'rgba(255,255,255,.08)'};border-left:3px solid {gold};" if i == 0 else "border-left:3px solid transparent;"
            qs.append(f"<div style='padding:6px 10px;margin:0 0 6px;{selq}'><div style='display:flex;justify-content:space-between;gap:10px'><div style='color:{gold};font-weight:700'>{fit(lab(c, 2, 4), qw - 120, size, lang)}</div>"
                      f"<div style='color:{muted};font-size:{size * .82:.1f}px'>Lv. {rnd.randint(1, 60)}</div></div>"
                      f"<div style='color:{muted};font-size:{size * .88:.1f}px;margin-top:3px'>{desc}</div>{''.join(objs)}</div>")
            used += h
        parts.append(f"<div style='position:absolute;left:20px;top:{top_l:.0f}px;width:{qw}px;padding:12px;{panel};color:{fg};{ts}'>"
                     f"<div style='font-size:{size * 1.15:.1f}px;font-weight:700;letter-spacing:.06em;border-bottom:1px solid {muted};padding-bottom:6px;margin-bottom:8px'>{fit(lab(c, 1, 2), qw - 30, size * 1.15, lang)}</div>{''.join(qs)}</div>")
    if have["tip"]:
        rar = rnd.choice(["#ffffff", "#1eff00", "#4aa3ff", "#c06bff", "#ff8000"]) if not paper else rnd.choice(["#1d5c1d", "#1d3f8a", "#6a1b9a", "#a04000"])
        stats = []
        for _ in range(rnd.randint(2, 5)):
            sc = "#7dff7d" if (rnd.random() < .5 and not paper) else fg
            stats.append(f"<div style='color:{sc}'>+{rnd.randint(1, 120)} {fit(lab(c, 1, 2), tw - 90, size * .92, lang)}</div>")
        desc = cut(c.para(1, 2), cpl(tw - 30, size * .88, lang) * rnd.choice([2, 3]))
        y0 = 24 if not have["hud"] or rnd.random() < .5 else 90
        parts.append(f"<div style='position:absolute;right:24px;top:{y0}px;width:{tw}px;padding:12px 14px;{panel};color:{fg};font-size:{size * .92:.1f}px;{ts}'>"
                     f"<div style='color:{rar};font-weight:700;font-size:{size * 1.12:.1f}px'>{fit(lab(c, 1, 4), tw - 30, size * 1.12, lang)}</div>"
                     f"<div style='display:flex;justify-content:space-between;color:{muted};margin:2px 0 6px'><div>{fit(lab(c, 1, 1), tw / 2 - 20, size * .92, lang)}</div><div>{fit(lab(c, 1, 1), tw / 2 - 20, size * .92, lang)}</div></div>"
                     f"<div style='display:flex;justify-content:space-between'><div>{rnd.randint(10, 80)} - {rnd.randint(81, 200)} {fit(lab(c, 1, 1), 90, size * .92, lang)}</div><div>{rnd.uniform(1, 4):.2f}</div></div>"
                     f"{''.join(stats)}<div style='color:{gold};font-style:italic;margin-top:8px'>{desc}</div>"
                     f"<div style='color:{muted};margin-top:6px'>{fit(lab(c, 1, 2), tw - 80, size * .92, lang)} {rnd.randint(1, 70)}</div>"
                     f"<div style='display:flex;gap:6px;align-items:center;justify-content:flex-end;margin-top:4px'>{icon(12, '#e8b923', 'dot')}<div>{rnd.randint(1, 9999):,}</div></div></div>")
    if have["menu"]:
        x0 = 20 + qw + 40 if have["quest"] else 60
        x1 = W - tw - 64 if have["tip"] else W - 60
        mw = min(rnd.randint(320, 420), x1 - x0)
        n = rnd.randint(3, 5); long = rnd.random() < .4
        opts = []
        for i in range(n):
            t = cut(c.para(1, 1), cpl(mw - 60, size, lang) * 2) if long else fit(lab(c, 2, 5), mw - 60, size, lang)
            pre = rnd.choice(["", f"{i + 1}. ", "▶ " if i == 0 else ""])
            hl = f"background:{'rgba(0,0,0,.12)' if paper else 'rgba(255,255,255,.14)'};" if i == 0 else ""
            opts.append(f"<div style='padding:7px 14px;margin:0 0 6px;{hl}border-radius:3px'>{pre}{t}</div>")
        mh = n * (size * lh * (2 if long else 1) + 20) + 30
        ytop = max(top_l if have["quest"] else 40, (floor - mh) / 2)
        parts.append(f"<div style='position:absolute;left:{(x0 + x1 - mw) / 2:.0f}px;top:{ytop:.0f}px;width:{mw}px;padding:12px;{panel};color:{fg};{ts}'>{''.join(opts)}</div>")
    elif not have["dialog"]:
        keys = "".join(f"<div style='display:flex;gap:6px;align-items:center'><div style='border:1px solid #fff;border-radius:3px;padding:0 6px;font-size:{size * .8:.1f}px'>{k}</div><div>{fit(lab(c, 1, 2), 140, size * .9, lang)}</div></div>" for k in rnd.sample(["E", "F", "Q", "R", "Tab", "Esc"], rnd.randint(2, 4)))
        parts.append(f"<div style='position:absolute;right:30px;bottom:24px;display:flex;gap:26px;color:#fff;font-size:{size * .9:.1f}px;text-shadow:0 0 4px #000'>{keys}</div>")
    if have["chat"]:
        cw = rnd.randint(380, 470); fs = size * .85; used = fs * 1.4 + 26; rows = []
        for _ in range(16):
            msg = cut(c.para(1, 1), cpl(cw - 24, fs, lang) * rnd.choice([1, 1, 2]))
            col = rnd.choice(["#ffffff", "#9fd4ff", "#7dff7d", "#ffb36b", "#ff9de2", "#ffffff"])
            t = f"[{fit(lab(c, 1, 1), 70, fs, lang)}] {person(c)}: {msg}" if rnd.random() < .8 else msg
            h = nlines(t, cw - 24, fs, lang) * fs * 1.35 + 2
            if used + h > chat_h: continue
            rows.append(f"<div style='color:{col};margin-top:2px'>{t}</div>"); used += h
        tabs = "".join(f"<div style='padding:2px 10px;{'background:rgba(255,255,255,.18);' if i == 0 else ''}border-radius:3px 3px 0 0'>{fit(lab(c, 1, 1), 70, fs, lang)}</div>" for i in range(rnd.randint(2, 4)))
        parts.append(f"<div style='position:absolute;left:20px;bottom:24px;width:{cw}px;font-size:{fs:.1f}px;line-height:1.35;color:#fff;text-shadow:0 0 3px #000,1px 1px 1px #000'>"
                     f"<div style='display:flex;gap:2px'>{tabs}</div><div style='height:{chat_h - 30 - fs * 1.4:.0f}px;padding:4px 10px;background:rgba(0,0,0,.38);display:flex;flex-direction:column;justify-content:flex-end;overflow:hidden'>{''.join(rows)}</div>"
                     f"<div style='margin-top:4px;padding:3px 10px;background:rgba(0,0,0,.55);border:1px solid rgba(255,255,255,.3);color:#cfcfcf'>{fit(lab(c, 1, 3), cw - 30, fs, lang)}</div></div>")
    if have["dialog"]:
        dw = W - 120; dsz = size * rnd.choice([1.0, 1.1, 1.2])
        line = cut(c.para(2, 3), int(cpl(dw - 70, dsz, lang) * rnd.choice([2, 2.6, 3])))
        plate = (f"<div style='position:absolute;left:{rnd.choice([0, 24, 40])}px;top:-{size * 1.2:.0f}px;padding:4px 18px;{panel};color:{gold};font-weight:700;font-size:{size * 1.05:.1f}px;{ts}'>{fit(person(c), 220, size * 1.05, lang)}</div>")
        parts.append(f"<div style='position:absolute;left:60px;top:{Hh - dlg_h - 24}px;width:{dw}px;height:{dlg_h}px;{panel}'>{plate}"
                     f"<div style='padding:{size * 1.6:.0f}px 34px 0;font-size:{dsz:.1f}px;line-height:1.6;color:{fg};{ts}'>{line}</div>"
                     f"<div style='position:absolute;right:22px;bottom:14px'>{icon(14, gold, 'down')}</div></div>")
    body = f"<div style='position:relative;width:{W}px;height:{Hh}px;background:{pattern}{sky};overflow:hidden'>{scenery}{''.join(parts)}</div>"
    return page(body, base_css(lh), {"bg": "#000", "fg": "#fff"}, font, size), W, Hh


# ------------------------------------------------------------------ 4 two-column paper
@scene("paper_2col")
def s_paper(rnd, lang):
    c = XCtx(rnd, lang); u = UI[lang]; cj = lang in CJK
    size = rnd.choice([11, 11.5, 12, 12.5, 13]); lh = rnd.choice([1.22, 1.3, 1.36]) if not cj else rnd.choice([1.4, 1.5, 1.6])
    font = rnd.choice(['"Times New Roman"', '"Times New Roman"', 'Georgia', c.font()]) if lang == "en" else c.font()
    W, Hh = rnd.choice([(1000, 900), (1100, 900), (960, 880), (1200, 900), (1000, 860)])
    kf = 1.0 if cj else (.47 if "Times" in font else .53)   # average advance in em, justified text

    def nl(s, width, sz, lang):
        return max(1, -(-len(H.unescape(s)) // max(3, int(width / (sz * kf)))))
    viewer = rnd.choice(["#525659", "#e8e8e8", "#3b3b3b", "#d9dce0"])
    pw = min(W - 40, rnd.randint(760, 900)); ph = Hh - 24; mx = rnd.randint(44, 62); my = rnd.randint(26, 38)
    gap = rnd.randint(18, 30); inner = pw - 2 * mx; colw = (inner - gap) / 2
    layout = rnd.choice(["wide", "ieee"]); indent = rnd.choice(["1em", "1.5em", "2em"]) if not cj else "2em"
    upper = lang == "en" and rnd.random() < .5
    L = size * lh
    top = []; top_h = 0
    top.append(f"<div style='display:flex;justify-content:space-between;font-size:{size * .82:.1f}px;color:#444;border-bottom:1px solid #999;padding-bottom:3px;margin-bottom:14px'>"
               f"<div style='font-style:italic'>{fit(lab(c, 2, 5), inner / 2, size * .82, lang)}</div><div>{rnd.randint(1, 60)} ({rnd.randint(1, 12)}), {rnd.randint(2000, 2025)}</div></div>")
    top_h += size * .82 * 1.3 + 20
    title = cut(lab(c, 5, 12), cl(110, lang)); tsz = size * rnd.choice([1.6, 1.8, 2.0])
    top.append(f"<div style='text-align:center;font-size:{tsz:.1f}px;font-weight:700;line-height:1.25;margin:0 0 10px'>{title}</div>")
    top_h += nl(title, inner, tsz, lang) * tsz * 1.25 + 10
    na = rnd.randint(2, 5); nf = rnd.randint(1, min(2, na))
    auth = ", ".join(f"{person(c)}<sup>{rnd.randint(1, nf)}</sup>" for _ in range(na))
    top.append(f"<div style='text-align:center;font-size:{size * 1.1:.1f}px;margin:0 0 4px'>{auth}</div>"); top_h += size * 1.1 * 1.3 * 2 + 4
    for i in range(1, nf + 1):
        top.append(f"<div style='text-align:center;font-size:{size * .9:.1f}px;font-style:italic'><sup>{i}</sup>{fit(lab(c, 3, 7), inner - 40, size * .9, lang)}</div>"); top_h += size * .9 * 1.3
    if rnd.random() < .5:
        top.append(f"<div style='text-align:center;font-size:{size * .85:.1f}px;margin-top:2px'>{addr(rnd)}</div>"); top_h += size * 1.2
    ab_txt = c.para(3, 5); kw = (", " if not cj else "；").join(fit(c.title(), 160, size, lang) for _ in range(rnd.randint(3, 5)))
    if layout == "wide":
        am = rnd.randint(20, 50)
        top.append(f"<div style='margin:14px {am}px 4px;text-align:justify;font-size:{size * .95:.1f}px'><b>{u['abstract']}</b>{u['sep']}{ab_txt}</div>")
        top.append(f"<div style='margin:0 {am}px 14px;font-size:{size * .95:.1f}px'><b>{u['keywords']}</b>{u['sep']}{kw}</div>")
        top_h += (nl(ab_txt, inner - 2 * am, size * .95, lang) + nl(kw, inner - 2 * am, size * .95, lang)) * size * .95 * lh + 34
    else:
        top_h += 14
    foot_h = size * 1.4 + 10
    cap_lines = (ph - 2 * my - top_h - foot_h - 6) / L * .95
    sec = [0]; fig = [0]; eq = [0]

    def heading():
        sec[0] += 1; t = fit(lab(c, 1, 3), colw - 30, size * 1.05, lang)
        if upper: t = t.upper()
        return f"<div style='font-weight:700;font-size:{size * 1.05:.1f}px;margin:{L * .6:.0f}px 0 {L * .3:.0f}px;{'text-align:center;' if upper and rnd.random() < .5 else ''}'>{sec[0]} {t}</div>", 1.9

    def fill(cap, first):
        out = []; used = 0; last_head = False
        if first and layout == "ieee":
            ab = f"<div style='font-weight:700;font-size:{size * .92:.1f}px;text-align:justify;margin:0 0 6px'><i>{u['abstract']}</i>{u['sep']}{ab_txt}</div>"
            kwd = f"<div style='font-weight:700;font-size:{size * .92:.1f}px;text-align:justify;margin:0 0 4px'><i>{u['keywords']}</i>{u['sep']}{kw}</div>"
            out += [ab, kwd]; used += nl(ab_txt, colw, size * .92, lang) + nl(kw, colw, size * .92, lang) + 1
        note = ""
        if first and rnd.random() < .55:
            nt = c.para(1, 2)
            note = (f"<div style='margin-top:auto;padding-top:6px'><div style='width:40%;border-top:1px solid #555;margin-bottom:3px'></div>"
                    f"<div style='font-size:{size * .82:.1f}px;text-align:justify;line-height:1.25'><sup>1</sup>{nt}</div></div>")
            cap -= nl(nt, colw, size * .82, lang) * .82 + 1.2
        if not first and rnd.random() < .5:
            t = c.para(2, 4); n = nl(t, colw, size, lang)
            out.append(f"<p style='margin:0;text-align:justify'>{t}</p>"); used += n
        for _ in range(30):
            k = rnd.random()
            if k < .15 and not last_head:
                h, n = heading(); last_head = True
                if used + n + 3 > cap: break
                out.append(h); used += n; continue
            if k < .25 and used + 9 < cap:
                fig[0] += 1; fh = rnd.randint(int(L * 5), int(L * 9)); ct = cut(c.para(1, 2), cl(200, lang))
                bars = "".join(f"<rect x='{10 + i * 30}' y='{100 - v}' width='18' height='{v}' fill='{rnd.choice(['#4472c4', '#ed7d31', '#a5a5a5', '#5b9bd5'])}'/>" for i, v in enumerate(rnd.randint(15, 90) for _ in range(rnd.randint(4, 8))))
                n = fh / L + nl(ct, colw, size * .88, lang) * .88 + 1.2
                if used + n > cap: continue
                out.append(f"<div style='margin:{L * .4:.0f}px 0 4px;height:{fh}px;border:1px solid #bbb;background:#fafafa'><svg width='100%' height='100%' viewBox='0 0 260 110' preserveAspectRatio='none'>{bars}<path d='M5 100H255' stroke='#333'/></svg></div>"
                           f"<div style='font-size:{size * .88:.1f}px;text-align:justify;margin:0 0 {L * .5:.0f}px'>{u['fig']} {fig[0]}. {ct}</div>")
                used += n; last_head = False; continue
            if k < .29:
                eq[0] += 1; v = rnd.sample("xyzabkntw", 3)
                out.append(f"<div style='display:flex;justify-content:space-between;margin:{L * .3:.0f}px 0'><div style='flex:1;text-align:center;font-style:italic;font-family:\"Times New Roman\"'>{v[0]}({v[1]}) = {rnd.randint(2, 9)}{v[2]} + {v[1]}<sup>{rnd.randint(2, 3)}</sup></div><div>({eq[0]})</div></div>")
                used += 1.6; continue
            t = c.para(2, 5); n = nl(t, colw, size, lang)
            css = f"margin:0;text-indent:{indent};text-align:justify" if not (last_head and rnd.random() < .5) else "margin:0;text-align:justify"
            if used + n > cap:
                rest = int(cap - used)
                if rest >= 2: out.append(f"<p style='{css}'>{trunc(t, (rest - 1) * int(colw / (size * kf)), lang)}</p>")
                break
            out.append(f"<p style='{css}'>{t}</p>"); used += n; last_head = False
        return "".join(out) + note

    col1 = fill(cap_lines, True); col2 = fill(cap_lines, False)
    cols = (f"<div style='display:flex;gap:{gap}px;flex:1;min-height:0'><div style='width:{colw:.0f}px;display:flex;flex-direction:column;overflow:hidden'>{col1}</div>"
            f"<div style='width:{colw:.0f}px;display:flex;flex-direction:column;overflow:hidden'>{col2}</div></div>")
    foot = f"<div style='text-align:center;font-size:{size * .9:.1f}px;margin-top:8px'>{rnd.randint(1, 400)}</div>"
    paper = (f"<div style='width:{pw}px;height:{ph}px;margin:0 auto;background:#fff;color:#111;box-shadow:0 2px 8px rgba(0,0,0,.35);padding:{my}px {mx}px;display:flex;flex-direction:column;overflow:hidden'>"
             f"{''.join(top)}{cols}{foot}</div>")
    return page(f"<div style='width:{W}px;height:{Hh}px;padding-top:12px;background:{viewer}'>{paper}</div>", base_css(lh), {"bg": viewer, "fg": "#111"}, font, size), W, Hh


# ------------------------------------------------------------------ 5 slides
BULLETS = [["•", "–", "▪"], ["■", "○", "–"], ["▸", "•", "·"], ["●", "◦", "▪"], ["◆", "▶", "•"], ["➢", "•", "–"]]


@scene("slides")
def s_slides(rnd, lang):
    c = XCtx(rnd, lang); font = c.font()
    W, Hh = rnd.choice([(1280, 720), (1120, 630), (960, 540), (1200, 675)]); k = W / 1280
    T = rnd.choice([
        dict(bg="#ffffff", fg="#222222", title="#1f3864", accent="#2f5597", muted="#7f7f7f", band=None),
        dict(bg="#1f2a44", fg="#e8ecf4", title="#ffffff", accent="#f2c14e", muted="#9aa6bf", band=None),
        dict(bg="#ffffff", fg="#333333", title="#ffffff", accent="#c55a11", muted="#808080", band="#c55a11"),
        dict(bg="linear-gradient(135deg,#f7f9fc,#dfe8f5)", fg="#1c2733", title="#0b3d91", accent="#0b84a5", muted="#6b7a8c", band=None),
        dict(bg="#111111", fg="#f0f0f0", title="#ffffff", accent="#3ddc97", muted="#9a9a9a", band=None),
        dict(bg="#fbf7ef", fg="#3a3226", title="#7a3e00", accent="#b5651d", muted="#8c7b65", band="#3a3226")])
    kind = rnd.choice(["title", "bullets", "bullets", "bullets", "two", "two", "agenda"])
    pad = int(rnd.choice([48, 56, 64]) * k); tsz = rnd.choice([34, 38, 42]) * k; bsz = rnd.choice([20, 22, 24, 26]) * k; lh = rnd.choice([1.25, 1.3, 1.4])
    marks = rnd.choice(BULLETS); mode = rnd.choice(["text", "text", "css"]); sp = rnd.choice([.25, .4, .6]); ordered = rnd.random() < .2
    num = rnd.randint(2, 40)
    fsz = 13 * k
    footer = (f"<div style='position:absolute;left:{pad}px;right:{pad}px;bottom:{int(18 * k)}px;display:flex;justify-content:space-between;align-items:center;font-size:{fsz:.1f}px;color:{T['muted']}'>"
              f"<div>{fit(lab(c, 1, 4), W / 3, fsz, lang)}</div>" + (f"<div>{day(rnd, lang)}</div>" if rnd.random() < .5 else "") + f"<div>{num if kind != 'title' else 1}</div></div>")
    body_w = W - 2 * pad

    def item(level, text, fs, ordinal):
        if mode == "css":
            lt = "decimal" if ordered and level == 0 else ["disc", "circle", "square"][level]
            return f"<li style='margin:0 0 {sp}em {1.2 + level * 1.5:.1f}em;list-style-type:{lt};font-size:{fs:.1f}px'>{text}</li>"
        m = f"{ordinal}." if ordered and level == 0 else marks[level]
        mw = 1.2 if len(m) == 1 else 1.8
        return (f"<div style='font-size:{fs:.1f}px;padding-left:{level * 1.6 + mw:.1f}em;text-indent:-{mw}em;margin:0 0 {sp}em'>"
                f"<span style='display:inline-block;width:{mw}em;text-indent:0;color:{T['accent']}'>{m}</span>{text}</div>")

    def bullets(width, height, max_level=2):
        out = []; used = 0; level = 0; i = 0
        for _ in range(14):
            fs = bsz * (1, .86, .76)[level]
            t = cut(c.para(1, 1), cl(rnd.choice([70, 110, 160]) if level == 0 else rnd.choice([50, 80]), lang)) if rnd.random() < .8 else lab(c, 2, 6)
            h = nlines(t, width - (level * 1.6 + 1.3) * fs, fs, lang) * fs * lh + sp * fs
            if used + h > height: break
            i += 1; out.append(item(level, t, fs, i)); used += h
            r = rnd.random()
            level = min(max_level, level + 1) if r < .35 else (max(0, level - 1) if r < .6 else level)
            if len(out) == 1 and level > 0 and rnd.random() < .5: level = 0
        inner = "".join(out)
        if mode == "css": inner = f"<style>.sl li::marker{{color:{T['accent']}}}</style><ul class='sl' style='margin:0;padding:0'>{inner}</ul>"
        return inner

    def title_bar(t):
        n = nlines(t, body_w, tsz, lang)
        if T["band"]:
            return (f"<div style='background:{T['band']};color:#ffffff;padding:{int(20 * k)}px {pad}px;font-size:{tsz:.1f}px;font-weight:700;line-height:1.2'>{t}</div>"), n * tsz * 1.2 + 40 * k
        bar = f"border-left:{int(6 * k)}px solid {T['accent']};padding-left:{int(16 * k)}px;" if rnd.random() < .4 else ""
        return (f"<div style='padding:{pad * .7:.0f}px {pad}px 0'><div style='{bar}color:{T['title']};font-size:{tsz:.1f}px;font-weight:700;line-height:1.2'>{t}</div></div>"), n * tsz * 1.2 + pad * .7
    title_txt = fit(lab(c, 2, 6), body_w * rnd.choice([1, 1.6]), tsz, lang)
    if kind == "title":
        big = cut(lab(c, 4, 10), cl(70, lang)); tz = tsz * 1.35
        sub = cut(c.para(1, 1), cl(110, lang))
        al = rnd.choice(["center", "left"])
        inner = (f"<div style='position:absolute;left:{pad * 1.5:.0f}px;right:{pad * 1.5:.0f}px;top:{Hh * .26:.0f}px;text-align:{al}'>"
                 f"<div style='font-size:{tz:.1f}px;font-weight:800;line-height:1.15;color:{T['title'] if not T['band'] else T['accent']}'>{big}</div>"
                 f"<div style='height:{int(4 * k)}px;width:{int(120 * k)}px;background:{T['accent']};margin:{int(22 * k)}px {'auto' if al == 'center' else '0'}'></div>"
                 f"<div style='font-size:{bsz * 1.05:.1f}px;color:{T['fg']}'>{sub}</div>"
                 f"<div style='font-size:{bsz * .85:.1f}px;color:{T['fg']};margin-top:{int(36 * k)}px;font-weight:600'>{person(c)}</div>"
                 f"<div style='font-size:{bsz * .75:.1f}px;color:{T['muted']};margin-top:{int(6 * k)}px'>{fit(lab(c, 2, 6), body_w, bsz * .75, lang)}</div>"
                 f"<div style='font-size:{bsz * .75:.1f}px;color:{T['muted']};margin-top:{int(4 * k)}px'>{day(rnd, lang)}</div></div>")
    elif kind == "bullets":
        tb, th = title_bar(title_txt); bh = Hh - th - pad * .6 - 60 * k
        sub = ""
        if rnd.random() < .3:
            sub = f"<div style='font-size:{bsz * .9:.1f}px;color:{T['muted']};margin:0 0 {int(14 * k)}px'>{fit(c.para(1, 1), body_w, bsz * .9, lang)}</div>"; bh -= bsz * 1.4 + 14 * k
        inner = tb + f"<div style='padding:{pad * .5:.0f}px {pad}px 0;color:{T['fg']};line-height:{lh}'>{sub}{bullets(body_w, bh)}</div>"
    elif kind == "two":
        tb, th = title_bar(title_txt); bh = Hh - th - pad * .6 - 60 * k; cw = (body_w - 40 * k) / 2
        def col():
            head = f"<div style='font-size:{bsz * 1.05:.1f}px;font-weight:700;color:{T['accent']};margin:0 0 {int(10 * k)}px;padding-bottom:{int(6 * k)}px;border-bottom:2px solid {T['accent']}'>{fit(lab(c, 1, 3), cw, bsz * 1.05, lang)}</div>"
            return head + bullets(cw, bh - bsz * 2.2, 1)
        if rnd.random() < .35:
            cap = cut(c.para(1, 1), cl(120, lang))
            right = (f"<div style='height:{bh * .72:.0f}px;border-radius:{int(8 * k)}px;background:linear-gradient(135deg,{T['accent']}55,{T['muted']}55)'></div>"
                     f"<div style='font-size:{bsz * .72:.1f}px;color:{T['muted']};margin-top:{int(8 * k)}px'>{cap}</div>")
        else:
            right = col()
        inner = tb + (f"<div style='display:flex;gap:{int(40 * k)}px;padding:{pad * .5:.0f}px {pad}px 0;color:{T['fg']};line-height:{lh}'>"
                      f"<div style='width:{cw:.0f}px'>{col()}</div><div style='width:{cw:.0f}px'>{right}</div></div>")
    else:
        tb, th = title_bar(title_txt); bh = Hh - th - pad * .6 - 60 * k
        n = rnd.randint(4, 6); two = n > 4 and rnd.random() < .6
        cw = (body_w - 40 * k) / 2 if two else body_w
        rows = []
        for i in range(n):
            d = fit(c.para(1, 1), cw - bsz * 2.6, bsz * .7, lang) if rnd.random() < .7 else ""
            rows.append(f"<div style='display:flex;gap:{int(18 * k)}px;align-items:flex-start;margin:0 0 {int(rnd.choice([18, 26, 34]) * k)}px;width:{cw:.0f}px'>"
                        f"<div style='font-size:{bsz * 1.6:.1f}px;font-weight:800;color:{T['accent']};line-height:1;min-width:{bsz * 2:.0f}px;white-space:nowrap;flex:none'>{i + 1:02d}</div>"
                        f"<div><div style='font-size:{bsz:.1f}px;font-weight:600'>{fit(lab(c, 2, 5), cw - bsz * 2.6, bsz, lang)}</div>"
                        + (f"<div style='font-size:{bsz * .7:.1f}px;color:{T['muted']};margin-top:2px'>{d}</div>" if d else "") + "</div></div>")
        inner = tb + f"<div style='display:flex;flex-wrap:wrap;column-gap:{int(40 * k)}px;padding:{pad * .6:.0f}px {pad}px 0;color:{T['fg']};line-height:{lh}'>{''.join(rows)}</div>"
    body = f"<div style='position:relative;width:{W}px;height:{Hh}px;background:{T['bg']};overflow:hidden'>{inner}{footer}</div>"
    return page(body, base_css(lh), {"bg": "#000" if T["bg"].startswith("#1") else "#fff", "fg": T["fg"]}, font, bsz), W, Hh


# ------------------------------------------------------------------ 6 mobile app
@scene("mobile_app")
def s_mobile(rnd, lang):
    c = XCtx(rnd, lang); font = ui_font(c); dark = rnd.random() < .3
    W, Hh = rnd.choice([(390, 844), (375, 812), (393, 852), (414, 896), (360, 800)])
    size = rnd.choice([15, 16, 16, 17]); lh = 1.35
    P = (dict(bg="#000000", group="#1c1c1e", fg="#ffffff", muted="#8e8e93", line="#38383a", accent=rnd.choice(["#0a84ff", "#30d158", "#ff9f0a"]), bar="#121212")
         if dark else rnd.choice([dict(bg="#f2f2f7", group="#ffffff", fg="#000000", muted="#8a8a8e", line="#d8d8dc", accent="#007aff", bar="#f9f9f9"),
                                  dict(bg="#ffffff", group="#f3edf7", fg="#1d1b20", muted="#625b71", line="#e7e0ec", accent="#6750a4", bar="#f3edf7"),
                                  dict(bg="#ededed", group="#ffffff", fg="#191919", muted="#888888", line="#e5e5e5", accent="#07c160", bar="#f7f7f7")]))
    sm = size * .8; mu = f"color:{P['muted']}"
    status = (f"<div style='height:46px;display:flex;justify-content:space-between;align-items:center;padding:0 26px;font-size:{size:.0f}px;font-weight:600'>"
              f"<div>{rnd.randint(7, 23)}:{rnd.randint(10, 59)}</div><div style='display:flex;gap:6px;align-items:center'>{icon(16, P['fg'], 'dot')}{icon(16, P['fg'], 'circle')}"
              f"<div style='width:24px;height:12px;border:1.5px solid {P['fg']};border-radius:3px;padding:1px'><div style='width:{rnd.randint(30, 100)}%;height:100%;background:{P['fg']}'></div></div></div></div>")
    large = rnd.random() < .5
    if large:
        nav = (f"<div style='display:flex;justify-content:space-between;padding:6px 16px;color:{P['accent']}'><div style='display:flex;gap:4px;align-items:center'>{icon(16, P['accent'], 'back')}<div>{fit(lab(c, 1, 1), 120, size, lang)}</div></div>"
               f"<div>{fit(lab(c, 1, 1), 100, size, lang)}</div></div><div style='padding:2px 16px 8px;font-size:{size * 2:.0f}px;font-weight:700;line-height:1.2'>{fit(lab(c, 1, 3), W - 32, size * 2, lang)}</div>")
        nav_h = size * 1.35 + 12 + size * 2.4 + 10
    else:
        nav = (f"<div style='height:52px;display:flex;align-items:center;justify-content:space-between;padding:0 14px;border-bottom:1px solid {P['line']}'>{icon(20, P['fg'], 'back')}"
               f"<div style='font-size:{size * 1.06:.1f}px;font-weight:600'>{fit(lab(c, 1, 3), W - 120, size * 1.06, lang)}</div>{icon(20, P['fg'], rnd.choice(['menu', 'plus', 'search']))}</div>")
        nav_h = 52
    tabs_n = rnd.choice([4, 4, 5]); tab_h = rnd.choice([56, 64, 82])
    tabs = "".join(f"<div style='flex:1;display:flex;flex-direction:column;align-items:center;gap:3px;color:{P['accent'] if i == 0 else P['muted']}'>{icon(22, P['accent'] if i == 0 else P['muted'], ic)}"
                   f"<div style='font-size:11px;line-height:1.2'>{fit(lab(c, 1, 1), W / tabs_n - 6, 11, lang)}</div></div>" for i, ic in enumerate(rnd.sample(["home", "search", "bell", "heart", "star", "circle", "square"], tabs_n)))
    tabbar = f"<div style='position:absolute;left:0;right:0;bottom:0;height:{tab_h}px;background:{P['bar']};border-top:1px solid {P['line']};display:flex;padding-top:8px'>{tabs}</div>"
    budget = Hh - 46 - nav_h - tab_h - 10
    secs = []; used = 0
    inset = rnd.random() < .6; gm = 16 if inset else 0; gr = 12 if inset else 0

    def header():
        return f"<div style='padding:18px {16 + gm}px 6px;font-size:{sm:.1f}px;{mu};{'text-transform:uppercase;' if lang == 'en' else ''}'>{fit(lab(c, 1, 3), W - 60, sm, lang)}</div>", sm * 1.35 + 24

    def list_group():
        rows = []; h = 0; n = rnd.randint(2, 5); av = rnd.choice([40, 44, 52]); tw = W - 2 * gm - av - 48
        for i in range(n):
            sub = cut(c.para(1, 1), cpl(tw, size * .88, lang) * rnd.choice([1, 2])) if rnd.random() < .85 else ""
            sl = nlines(sub, tw, size * .88, lang) if sub else 0
            rows.append(f"<div style='display:flex;gap:12px;padding:11px 14px;{('border-top:1px solid ' + P['line']) if i else ''}'>"
                        f"<div style='width:{av}px;height:{av}px;border-radius:{rnd.choice(['50%', '10px'])};background:{rnd.choice(AVATARS)};flex:none'></div>"
                        f"<div style='flex:1;min-width:0'><div style='display:flex;justify-content:space-between;gap:8px'><div style='font-weight:600'>{fit(lab(c, 1, 3), tw - 70, size, lang)}</div>"
                        f"<div style='font-size:{sm * .95:.1f}px;{mu}'>{rnd.choice([clock(rnd), day(rnd, lang), UI[lang]['yesterday']])}</div></div>"
                        + (f"<div style='font-size:{size * .88:.1f}px;{mu};margin-top:2px'>{sub}</div>" if sub else "") + "</div></div>")
            h += max(av, size * lh + sl * size * .88 * lh) + 23
        return f"<div style='margin:0 {gm}px;background:{P['group']};border-radius:{gr}px;overflow:hidden'>{''.join(rows)}</div>", h

    def settings_group():
        rows = []; h = 0
        for i in range(rnd.randint(2, 5)):
            desc = cut(c.para(1, 1), cpl(W - 2 * gm - 110, sm, lang) * 2) if rnd.random() < .35 else ""
            ctl = rnd.choice([f"<div style='width:50px;height:30px;border-radius:15px;background:{P['accent'] if rnd.random() < .6 else P['line']};flex:none'></div>",
                              f"<div style='display:flex;gap:4px;align-items:center;{mu}'><div>{fit(lab(c, 1, 1), 90, size, lang)}</div>{icon(14, P['muted'], 'chev')}</div>",
                              icon(14, P["muted"], "chev")])
            rows.append(f"<div style='display:flex;justify-content:space-between;align-items:center;gap:12px;padding:12px 16px;{('border-top:1px solid ' + P['line']) if i else ''}'>"
                        f"<div style='min-width:0'><div>{fit(lab(c, 1, 4), W - 2 * gm - 130, size, lang)}</div>" + (f"<div style='font-size:{sm:.1f}px;{mu};margin-top:2px'>{desc}</div>" if desc else "") + f"</div>{ctl}</div>")
            h += size * lh + 25 + (nlines(desc, W - 2 * gm - 110, sm, lang) * sm * lh + 2 if desc else 0)
        return f"<div style='margin:0 {gm}px;background:{P['group']};border-radius:{gr}px'>{''.join(rows)}</div>", h

    def card():
        ih = rnd.choice([0, 110, 140]); tw = W - 32 - 28
        t = cut(c.para(1, 1), cpl(tw, size * 1.1, lang) * 2); d = cut(c.para(1, 2), cpl(tw, size * .9, lang) * rnd.choice([2, 3]))
        h = ih + nlines(t, tw, size * 1.1, lang) * size * 1.1 * lh + nlines(d, tw, size * .9, lang) * size * .9 * lh + 36 + 42 + 16
        img = f"<div style='height:{ih}px;background:linear-gradient(135deg,{rnd.choice(AVATARS)},{rnd.choice(AVATARS)})'></div>" if ih else ""
        return (f"<div style='margin:8px 16px;background:{P['group']};border-radius:14px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,.1)'>{img}<div style='padding:14px'>"
                f"<div style='font-size:{size * 1.1:.1f}px;font-weight:700'>{t}</div><div style='font-size:{size * .9:.1f}px;{mu};margin-top:4px'>{d}</div>"
                f"<div style='display:flex;justify-content:space-between;align-items:center;margin-top:10px'><div style='font-size:{sm:.1f}px;{mu}'>{fit(lab(c, 1, 2), tw - 120, sm, lang)}</div>"
                f"<button style='font:inherit;font-size:{sm:.1f}px;padding:6px 14px;border:0;border-radius:16px;background:{P['accent']};color:#fff'>{fit(lab(c, 1, 1), 80, sm, lang)}</button></div></div></div>"), h

    def button():
        return (f"<button style='display:block;width:calc(100% - 32px);margin:14px 16px;height:50px;border:0;border-radius:12px;background:{P['accent']};color:#fff;font:inherit;font-weight:600'>{fit(lab(c, 1, 3), W - 60, size, lang)}</button>"), 78

    def notice():
        t = cut(c.para(1, 1), cpl(W - 90, size * .88, lang) * 2)
        return (f"<div style='margin:10px 16px;padding:10px 12px;border-radius:10px;background:{P['accent']}1f;display:flex;gap:10px;align-items:flex-start'>{icon(18, P['accent'], 'info')}"
                f"<div style='font-size:{size * .88:.1f}px'>{t}</div></div>"), nlines(t, W - 90, size * .88, lang) * size * .88 * lh + 40

    def chips():
        out = []; w = 16
        for _ in range(8):
            t = fit(lab(c, 1, 1), 90, sm, lang); cw = len(H.unescape(t)) * sm * (1.0 if lang in CJK else .6) + 30
            if w + cw > W - 16: break
            out.append(f"<div style='padding:5px 12px;border-radius:16px;font-size:{sm:.1f}px;background:{P['group'] if out else P['accent']};color:{P['fg'] if out else '#fff'};border:1px solid {P['line']};white-space:nowrap'>{t}</div>"); w += cw + 8
        return f"<div style='display:flex;gap:8px;padding:10px 16px'>{''.join(out)}</div>", sm * lh + 32

    if rnd.random() < .5:
        secs.append(f"<div style='margin:4px 16px 8px;padding:8px 12px;border-radius:10px;background:{P['group'] if not inset else P['line']};{mu};display:flex;gap:8px;align-items:center'>{icon(16, P['muted'], 'search')}<div>{fit(lab(c, 1, 3), W - 90, size, lang)}</div></div>"); used += size * lh + 28
    makers = [list_group, settings_group, card, notice, chips, button]
    weights = [4, 4, 2, 1, 1, 1]
    limit = {card: 2, notice: 1, chips: 1, button: 1}; count = {}
    for _ in range(24):
        f = rnd.choices(makers, weights)[0]
        if count.get(f, 0) >= limit.get(f, 9): continue
        html, h = f()
        if f in (list_group, settings_group) and rnd.random() < .6:   # a group usually has a caption
            hh, hgt = header(); html = hh + html; h += hgt
        if used + h > budget: continue
        secs.append(html); used += h; count[f] = count.get(f, 0) + 1
    body = f"<div style='position:relative;width:{W}px;height:{Hh}px;background:{P['bg']};color:{P['fg']}'>{status}{nav}<div style='height:{budget:.0f}px;overflow:hidden'>{''.join(secs)}</div>{tabbar}</div>"
    return page(body, base_css(lh), {"bg": P["bg"], "fg": P["fg"]}, font, size), W, Hh


# ------------------------------------------------------------------ 7 email
def quote_marks_bg(L, size, color):
    """A '>' at the start of every line, the way plain-text replies quote; drawn as a background so the
    quoted paragraph stays one block."""
    svg = (f"<svg xmlns='http://www.w3.org/2000/svg' width='{size:.0f}' height='{L}'><text x='0' y='{(L - size) / 2 + size * .82:.1f}' "
           f"font-family='Arial' font-size='{size:.0f}' fill='{color}'>&gt;</text></svg>")
    return 'url("data:image/svg+xml,' + urllib.parse.quote(svg) + '") repeat-y 0 0'


@scene("email")
def s_email(rnd, lang):
    c = XCtx(rnd, lang); u = UI[lang]; t = theme(rnd); dark = is_dark(t); font = ui_font(c)
    size = rnd.choice([13, 14, 14, 15]); lh = rnd.choice([1.4, 1.5, 1.6])
    W, Hh = rnd.choice([(1200, 800), (1280, 820), (1100, 760), (1280, 860)])
    fold_w = rnd.choice([0, 180, 200]) if W >= 1200 else 0; list_w = rnd.choice([300, 330, 360]); read_w = W - fold_w - list_w
    sel = "#26364d" if dark else rnd.choice(["#cfe3ff", "#e8f0fe", "#dbeafe"]); sm = size * .86; mu = f"color:{t['muted']}"
    top = (f"<div style='height:50px;display:flex;align-items:center;gap:20px;padding:0 16px;border-bottom:1px solid {t['line']};background:{t['card']}'>"
           f"{icon(18, t['fg'], 'menu')}<div style='font-weight:700;font-size:{size * 1.2:.1f}px'>{fit(c.title(), 160, size * 1.2, lang)}</div>"
           f"<div style='flex:1;max-width:520px;display:flex;gap:8px;align-items:center;padding:7px 12px;border-radius:8px;background:{t['bg']};{mu}'>{icon(15, t['muted'], 'search')}<div>{fit(lab(c, 1, 3), 400, size, lang)}</div></div></div>")
    col_h = Hh - 50
    folders = ""
    if fold_w:
        rows = [f"<button style='font:inherit;margin:12px;padding:9px 18px;border:0;border-radius:14px;background:{t['accent']};color:#fff;display:block'>{fit(lab(c, 1, 1), fold_w - 60, size, lang)}</button>"]
        for i in range(rnd.randint(6, 10)):
            cnt = f"<div style='font-size:{sm:.1f}px;{mu}'>{rnd.randint(1, 999)}</div>" if rnd.random() < .4 else ""
            rows.append(f"<div style='display:flex;justify-content:space-between;gap:6px;padding:6px 16px;{('background:' + sel + ';font-weight:700;') if i == 0 else ''}border-radius:0 16px 16px 0'>"
                        f"<div>{fit(lab(c, 1, 1), fold_w - 70, size, lang)}</div>{cnt}</div>")
        folders = f"<div style='width:{fold_w}px;flex:none;border-right:1px solid {t['line']}'>{''.join(rows)}</div>"
    items = [f"<div style='display:flex;justify-content:space-between;padding:12px 14px;border-bottom:1px solid {t['line']}'><div style='font-weight:700;font-size:{size * 1.1:.1f}px'>{fit(lab(c, 1, 2), list_w - 120, size * 1.1, lang)}</div>"
             f"<div style='{mu};font-size:{sm:.1f}px'>{fit(lab(c, 1, 1), 80, sm, lang)}</div></div>"]
    used = size * 1.1 * lh + 25; tw = list_w - 28
    for i in range(14):
        pl = rnd.choice([1, 1, 2]); prev = cut(c.para(1, 2), cpl(tw, sm, lang) * pl)
        h = size * lh * 2 + nlines(prev, tw, sm, lang) * sm * lh + 22
        if used + h > col_h: break
        unread = rnd.random() < .35
        items.append(f"<div style='padding:10px 14px;border-bottom:1px solid {t['line']};{('background:' + sel + ';') if i == 1 else ''}'>"
                     f"<div style='display:flex;justify-content:space-between;gap:8px'><div style='{'font-weight:700;' if unread else ''}'>{fit(person(c), tw - 90, size, lang)}</div><div style='font-size:{sm:.1f}px;{mu}'>{rnd.choice([clock(rnd), day(rnd, lang)])}</div></div>"
                     f"<div style='{'font-weight:600;' if unread else ''}'>{fit(lab(c, 2, 7), tw, size, lang)}</div><div style='font-size:{sm:.1f}px;{mu}'>{prev}</div></div>")
        used += h
    mlist = f"<div style='width:{list_w}px;flex:none;border-right:1px solid {t['line']};overflow:hidden'>{''.join(items)}</div>"
    rw = read_w - 64; parts = []; used = 0
    subj = cut(c.para(1, 1), cl(120, lang)); ssz = size * rnd.choice([1.4, 1.6])
    parts.append(f"<div style='font-size:{ssz:.1f}px;font-weight:600;line-height:1.3;margin:0 0 12px'>{subj}</div>"); used += nlines(subj, rw, ssz, lang) * ssz * 1.3 + 12
    if rnd.random() < .4:
        parts.append("<div style='display:flex;gap:6px;margin:0 0 12px'>" + "".join(f"<div style='font-size:{sm * .92:.1f}px;padding:1px 8px;border-radius:4px;background:{t['line']}'>{fit(lab(c, 1, 1), 90, sm, lang)}</div>" for _ in range(rnd.randint(1, 3))) + "</div>"); used += sm * lh + 14
    sender = person(c); sa = addr(rnd)
    parts.append(f"<div style='display:flex;gap:12px;align-items:flex-start;margin:0 0 18px'><div style='width:40px;height:40px;border-radius:50%;background:{rnd.choice(AVATARS)};flex:none'></div>"
                 f"<div style='flex:1;min-width:0'><div><b>{sender}</b> <span style='{mu};font-size:{sm:.1f}px'>&lt;{sa}&gt;</span></div>"
                 f"<div style='font-size:{sm:.1f}px;{mu}'>{u['to']}{u['colon']}{person(c)}, {person(c)}</div></div><div style='font-size:{sm:.1f}px;{mu}'>{day(rnd, lang)} {clock(rnd)}</div></div>")
    used += 2 * size * lh + 22
    budget = col_h - 56
    body_p = [f"<div style='margin:0 0 1em'>{fit(person(c), rw, size, lang)}{',' if lang == 'en' else '：' if lang != 'ko' else ','}</div>"]; used += size * lh * 2
    q_style = rnd.choice(["bar", "gt", "none"])
    want_quote = q_style != "none"
    reserve = (size * lh * 7) if want_quote else 0
    for _ in range(5):
        p = c.para(1, 3); h = nlines(p, rw, size, lang) * size * lh + size
        if used + h > budget - reserve - size * lh * 4: break
        body_p.append(f"<p style='margin:0 0 1em'>{p}</p>"); used += h
    sig = [f"<div>{fit(lab(c, 1, 2), rw, size, lang)}</div>", f"<div style='margin-top:.6em;font-weight:600'>{sender}</div>", f"<div style='font-size:{sm:.1f}px;{mu}'>{fit(lab(c, 2, 6), rw, sm, lang)}</div>"]
    if rnd.random() < .5: sig.append(f"<div style='font-size:{sm:.1f}px;{mu}'>+{rnd.randint(1, 99)} {rnd.randint(100, 999)} {rnd.randint(1000, 9999)}</div>")
    used += size * lh * 4.5
    quote = ""
    if want_quote and budget - used > size * lh * 4:
        Lp = round(size * lh); head = u["wrote"].format(d=day(rnd, lang), n=person(c), a=addr(rnd))
        qs = []; qused = Lp * 2
        for _ in range(4):
            p = c.para(1, 3); h = nlines(p, rw - 24, size, lang) * Lp + Lp
            if used + qused + h > budget: break
            qs.append(p); qused += h
        if not qs: qs = [cut(c.para(1, 1), cpl(rw - 24, size, lang) * 2)]
        if q_style == "bar":
            inner = "".join(f"<p style='margin:0 0 .8em'>{p}</p>" for p in qs)
            box = f"<div style='border-left:3px solid {t['line'] if not dark else t['muted']};padding-left:12px;{mu}'>{inner}</div>"
        else:
            inner = "".join(f"<p style='margin:0 0 {Lp if i < len(qs) - 1 else 0}px'>{p}</p>" for i, p in enumerate(qs))
            box = f"<div style='padding-left:{size * 1.1:.0f}px;line-height:{Lp}px;background:{quote_marks_bg(Lp, size, t['muted'])};{mu}'>{inner}</div>"
        quote = f"<div style='margin-top:1.4em'><div style='font-size:{sm:.1f}px;{mu};margin:0 0 .6em'>{head}</div>{box}</div>"
    reader = (f"<div style='flex:1;padding:20px 32px;overflow:hidden'>{''.join(parts)}<div style='line-height:{lh}'>{''.join(body_p)}{''.join(sig)}{quote}</div></div>")
    body = f"{top}<div style='display:flex;height:{col_h}px'>{folders}{mlist}{reader}</div>"
    return page(f"<div style='width:{W}px;height:{Hh}px'>{body}</div>", base_css(lh), t, font, size), W, Hh


# ------------------------------------------------------------------ 8 e-commerce product page
@scene("ecommerce")
def s_shop(rnd, lang):
    c = XCtx(rnd, lang); t = theme(rnd); font = ui_font(c); dark = is_dark(t)
    size = rnd.choice([13, 14, 14, 15]); lh = 1.45; sm = size * .86; mu = f"color:{t['muted']}"
    W, Hh = rnd.choice([(1200, 860), (1280, 860), (1100, 820), (1280, 800)])
    price_c = rnd.choice(["#e4393c", "#ff5000", "#b12704", t["fg"]]); star_c = "#ffa41c"
    view = rnd.choice(["buy", "buy", "detail"])
    head = (f"<div style='display:flex;align-items:center;gap:24px;padding:10px 28px;border-bottom:1px solid {t['line']}'><div style='font-weight:800;font-size:{size * 1.5:.1f}px;color:{price_c if price_c != t['fg'] else t['accent']}'>{fit(c.title(), 200, size * 1.5, lang)}</div>"
            f"<div style='flex:1;display:flex;border:2px solid {price_c if price_c != t['fg'] else t['accent']};border-radius:6px;overflow:hidden'><div style='flex:1;padding:6px 10px;{mu}'>{fit(lab(c, 2, 4), 400, size, lang)}</div>"
            f"<div style='padding:6px 18px;background:{price_c if price_c != t['fg'] else t['accent']};color:#fff'>{fit(lab(c, 1, 1), 70, size, lang)}</div></div>{icon(22, t['fg'], 'heart')}{icon(22, t['fg'], 'square')}</div>")
    nav = "<div style='display:flex;gap:22px;padding:8px 28px;font-size:{0:.1f}px;border-bottom:1px solid {1}'>".format(size * .95, t['line']) + "".join(f"<div>{fit(lab(c, 1, 1), 110, size * .95, lang)}</div>" for _ in range(rnd.randint(5, 8))) + "</div>"
    crumbs = " › ".join(fit(lab(c, 1, 2), 140, sm, lang) for _ in range(rnd.randint(2, 4)))
    bc = f"<div style='padding:10px 28px;font-size:{sm:.1f}px;{mu}'>{crumbs}</div>"
    stars = lambda r: f"<span style='color:{star_c};letter-spacing:1px'>{'★' * r}{'☆' * (5 - r)}</span>"

    def specs(width, height):
        rows = []; used = 0
        for i in range(14):
            v = rnd.choice([fit(lab(c, 1, 3), width * .6, size, lang), f"{rnd.randint(1, 999)} {rnd.choice(['mm', 'g', 'kg', 'cm', 'W', 'mAh', 'GB'])}", cut(c.para(1, 1), cpl(width * .6, size, lang) * 2)])
            h = nlines(v, width * .62, size, lang) * size * lh + 17
            if used + h > height: break
            rows.append(f"<tr><th style='text-align:left;font-weight:400;{mu};padding:8px 12px;width:35%;vertical-align:top;background:{t['card'] if not dark else t['line']};border-bottom:1px solid {t['line']}'>{fit(lab(c, 1, 2), width * .3, size, lang)}</th>"
                        f"<td style='padding:8px 12px;border-bottom:1px solid {t['line']}'>{v}</td></tr>")
            used += h
        return f"<table style='border-collapse:collapse;width:{width:.0f}px;table-layout:fixed;border-top:1px solid {t['line']}'>{''.join(rows)}</table>"

    def reviews(width, height):
        out = []; used = 0
        for _ in range(6):
            body = c.para(1, 3); title = fit(lab(c, 2, 6), width, size, lang)
            h = nlines(body, width, size, lang) * size * lh + size * lh * 3 + sm * lh + 34
            if used + h > height: break
            out.append(f"<div style='padding:12px 0;border-bottom:1px solid {t['line']}'><div style='display:flex;gap:8px;align-items:center'><div style='width:26px;height:26px;border-radius:50%;background:{rnd.choice(AVATARS)}'></div><div>{person(c)}</div></div>"
                       f"<div style='margin-top:4px'>{stars(rnd.randint(1, 5))} <b>{title}</b></div><div style='font-size:{sm:.1f}px;{mu}'>{day(rnd, lang)}</div>"
                       f"<div style='margin-top:4px'>{body}</div><div style='font-size:{sm:.1f}px;{mu};margin-top:6px'>{fit(lab(c, 1, 3), width, sm, lang)} ({rnd.randint(0, 300)})</div></div>")
            used += h
        return "".join(out)
    top_h = 50 + 40 + 36
    if view == "buy":
        img = rnd.randint(340, 420); iw = W - 56 - img - 40; parts = []; used = 0
        parts.append(f"<div style='font-size:{sm:.1f}px;color:{t['accent']}'>{fit(lab(c, 1, 3), iw, sm, lang)}</div>"); used += sm * lh
        tt = cut(c.para(1, 1), cpl(iw, size * 1.45, lang) * 2); parts.append(f"<h1 style='font-size:{size * 1.45:.1f}px;line-height:1.3;margin:4px 0 6px'>{tt}</h1>"); used += nlines(tt, iw, size * 1.45, lang) * size * 1.45 * 1.3 + 10
        parts.append(f"<div style='font-size:{sm:.1f}px'>{stars(rnd.randint(3, 5))} {rnd.uniform(3, 5):.1f} <span style='{mu}'>({rnd.randint(10, 9999):,})</span></div>"); used += sm * lh
        parts.append(f"<div style='margin:10px 0;padding:10px 14px;background:{t['card'] if not dark else t['line']};border-radius:6px'><span style='font-size:{size * 2:.0f}px;font-weight:700;color:{price_c}'>{money(rnd, lang)}</span>"
                     f" <span style='{mu};text-decoration:line-through;margin-left:8px'>{money(rnd, lang, 900, 1500)}</span> <span style='margin-left:8px;padding:1px 6px;background:{price_c};color:#fff;border-radius:3px;font-size:{sm:.1f}px'>-{rnd.randint(5, 60)}%</span></div>")
        used += size * 2 * 1.3 + 40
        if rnd.random() < .5: parts.append(f"<div style='font-size:{sm:.1f}px;color:{price_c}'>{fit(lab(c, 2, 6), iw, sm, lang)}</div>"); used += sm * lh + 4
        for _ in range(rnd.randint(1, 2)):
            chips = "".join(f"<div style='padding:5px 12px;border:1px solid {t['accent'] if i == 0 else t['line']};border-radius:4px;font-size:{sm:.1f}px'>{fit(lab(c, 1, 2), 110, sm, lang)}</div>" for i in range(rnd.randint(2, 5)))
            parts.append(f"<div style='margin-top:10px;font-size:{sm:.1f}px;{mu}'>{fit(lab(c, 1, 1), iw, sm, lang)}</div><div style='display:flex;flex-wrap:wrap;gap:8px;margin-top:4px'>{chips}</div>"); used += sm * lh * 2 + 34
        btns = (f"<div style='display:flex;gap:12px;margin:14px 0'><button style='font:inherit;padding:10px 28px;border:0;border-radius:22px;background:#ffd814;color:#111'>{fit(lab(c, 1, 2), 140, size, lang)}</button>"
                f"<button style='font:inherit;padding:10px 28px;border:0;border-radius:22px;background:{price_c if price_c != t['fg'] else '#ffa41c'};color:#fff'>{fit(lab(c, 1, 2), 140, size, lang)}</button></div>")
        used += 70
        avail = Hh - top_h - 30 - used - 60
        bl = []
        for _ in range(rnd.randint(3, 6)):
            b = cut(c.para(1, 1), cpl(iw - 24, size, lang) * rnd.choice([1, 2]))
            h = nlines(b, iw - 24, size, lang) * size * lh + 4
            if h > avail: break
            avail -= h
            bl.append(f"<li style='margin:0 0 4px'>{b}</li>" if rnd.random() < .5 else f"<div style='padding-left:1.1em;text-indent:-1.1em;margin:0 0 4px'>• {b}</div>")
        bul = "".join(x if x.startswith("<div") else f"<ul style='margin:0;padding-left:1.3em'>{x}</ul>" for x in bl)
        parts.append(f"<div style='margin-top:8px'>{bul}</div>")
        parts.insert(len(parts) - 1, btns) if rnd.random() < .5 else parts.append(btns)
        thumbs = "".join(f"<div style='width:56px;height:56px;border:1px solid {t['line'] if i else t['accent']};border-radius:4px;background:linear-gradient(135deg,{rnd.choice(AVATARS)}66,{rnd.choice(AVATARS)}66)'></div>" for i in range(5))
        main = (f"<div style='display:flex;gap:40px;padding:0 28px'><div style='width:{img}px;flex:none'><div style='height:{img}px;border-radius:8px;background:radial-gradient(circle at 40% 40%,{rnd.choice(AVATARS)},{t['line']})'></div>"
                f"<div style='display:flex;gap:8px;margin-top:10px'>{thumbs}</div></div><div style='width:{iw}px'>{''.join(parts)}</div></div>")
        info_h = Hh - top_h - 30 - 60 - avail + 8
        rest = Hh - top_h - 30 - max(img + 66, info_h * .88) - 56   # the per-item estimates run high
        if rest > size * 5:
            half = (W - 56 - 40) / 2
            main += (f"<div style='margin:22px 28px 0;padding-top:10px;border-top:1px solid {t['line']};font-weight:700;font-size:{size * 1.1:.1f}px'>{fit(lab(c, 1, 3), W / 2, size * 1.1, lang)}</div>"
                     f"<div style='display:flex;gap:40px;padding:8px 28px 0'>{specs(half, rest - size * 2)}{specs(half, rest - size * 2)}</div>")
    else:
        tabs = "".join(f"<div style='padding:10px 4px;{('border-bottom:3px solid ' + price_c + ';font-weight:700') if i == 0 else mu}'>{fit(lab(c, 1, 2), 140, size, lang)}{(' (' + str(rnd.randint(10, 999)) + ')') if i == 2 else ''}</div>" for i in range(3))
        sw = rnd.randint(380, 460); rw = W - 56 - sw - 40; avail = Hh - top_h - 30 - 60
        rhead = f"<div style='display:flex;gap:16px;align-items:center;padding-bottom:10px;border-bottom:1px solid {t['line']}'><div style='font-size:{size * 2.4:.0f}px;font-weight:700;line-height:1'>{rnd.uniform(3, 5):.1f}</div><div><div>{stars(rnd.randint(3, 5))}</div><div style='font-size:{sm:.1f}px;{mu}'>{rnd.randint(10, 9999):,} {fit(lab(c, 1, 1), 120, sm, lang)}</div></div></div>"
        main = (f"<div style='display:flex;gap:28px;padding:0 28px;border-bottom:1px solid {t['line']};margin-bottom:14px'>{tabs}</div>"
                f"<div style='display:flex;gap:40px;padding:0 28px'><div style='width:{sw}px'><div style='font-weight:700;font-size:{size * 1.15:.1f}px;margin-bottom:8px'>{fit(lab(c, 1, 3), sw, size * 1.15, lang)}</div>{specs(sw, avail - 40)}</div>"
                f"<div style='width:{rw}px'>{rhead}{reviews(rw, avail - size * 3.2)}</div></div>")
    body = f"<div style='width:{W}px;height:{Hh}px;background:{t['bg']}'>{head}{nav}{bc}{main}</div>"
    return page(body, base_css(lh), t, font, size), W, Hh


# ------------------------------------------------------------------ 9 forum thread
@scene("forum")
def s_forum(rnd, lang):
    c = XCtx(rnd, lang); u = UI[lang]; t = theme(rnd); font = ui_font(c); dark = is_dark(t)
    size = rnd.choice([13, 14, 14, 15]); lh = rnd.choice([1.45, 1.55, 1.65]); sm = size * .84; mu = f"color:{t['muted']}"
    W, Hh = rnd.choice([(1000, 860), (1100, 860), (1200, 840), (1000, 800)])
    style = rnd.choice(["classic", "modern", "modern"])
    cw = min(W - 48, rnd.randint(820, 1000)); parts = []
    tt = cut(c.para(1, 1), cl(110, lang)); tsz = size * rnd.choice([1.5, 1.7])
    parts.append(f"<h1 style='font-size:{tsz:.1f}px;line-height:1.3;margin:0 0 6px'>{tt}</h1>")
    tags = "".join(f"<div style='font-size:{sm * .95:.1f}px;padding:1px 8px;border-radius:10px;background:{t['line']}'>{fit(lab(c, 1, 1), 100, sm, lang)}</div>" for _ in range(rnd.randint(1, 3)))
    parts.append(f"<div style='display:flex;gap:10px;align-items:center;margin:0 0 14px'>{tags}<div style='font-size:{sm:.1f}px;{mu}'>{rnd.randint(10, 9999):,} · {u['ago'].format(n=rnd.randint(1, 23))}</div></div>")
    used = nlines(tt, cw, tsz, lang) * tsz * 1.3 + 50 + 30; budget = Hh - 40

    def quote():
        q = cut(c.para(1, 2), cl(220, lang))
        return (f"<blockquote style='margin:0 0 .8em;padding:8px 12px;border-left:3px solid {t['accent']};background:{t['card'] if dark else '#f3f4f6'};font-size:{size * .94:.1f}px'>"
                f"<div style='font-weight:600;font-size:{sm:.1f}px;margin:0 0 3px'>{UI[lang]['said'].format(n=person(c))}</div><div style='{mu}'>{q}</div></blockquote>"), q

    def body_html(width, maxh):
        out = []; h = 0
        if rnd.random() < .3:
            qh, q = quote(); out.append(qh); h += (nlines(q, width - 30, size * .94, lang) + 1) * size * lh + 30
        for _ in range(rnd.randint(1, 4)):
            p = c.para(1, 3); ph = nlines(p, width, size, lang) * size * lh + size * .7
            if h + ph > maxh:
                if out: break
                p = cut(p, cpl(width, size, lang) * 2); ph = nlines(p, width, size, lang) * size * lh + size * .7
                if h + ph > maxh: break
            out.append(f"<p style='margin:0 0 .7em'>{p}</p>"); h += ph
        return "".join(out), h

    if style == "classic":
        uw = rnd.randint(140, 170); bw = cw - uw - 40
        for i in range(8):
            maxh = budget - used
            if maxh < 170: break
            b, bh = body_html(bw, maxh - 60)
            if not b: break
            sig = f"<div style='margin-top:8px;padding-top:6px;border-top:1px dashed {t['line']};font-size:{sm:.1f}px;{mu}'>{fit(lab(c, 2, 6), bw, sm, lang)}</div>" if rnd.random() < .4 else ""
            h = max(bh + 40 + (sm * lh + 14 if sig else 0), 150) + 14
            parts.append(f"<div style='display:flex;border:1px solid {t['line']};margin:0 0 10px;background:{t['card']}'>"
                         f"<div style='width:{uw}px;flex:none;padding:12px;border-right:1px solid {t['line']};background:{t['bg']}'><div style='width:64px;height:64px;background:{rnd.choice(AVATARS)};border-radius:4px'></div>"
                         f"<div style='font-weight:700;color:{t['accent']};margin-top:6px'>{fit(person(c), uw - 24, size, lang)}</div><div style='font-size:{sm:.1f}px;{mu}'>{fit(lab(c, 1, 2), uw - 24, sm, lang)}</div>"
                         f"<div style='font-size:{sm:.1f}px;{mu}'>{fit(lab(c, 1, 1), uw - 70, sm, lang)}{u['colon']}{rnd.randint(1, 9999)}</div></div>"
                         f"<div style='flex:1;padding:10px 16px'><div style='display:flex;justify-content:space-between;font-size:{sm:.1f}px;{mu};padding-bottom:6px;margin-bottom:8px;border-bottom:1px solid {t['line']}'>"
                         f"<div>{day(rnd, lang)} {clock(rnd)}</div><div>{u['floor'].format(n=i + 1)}</div></div>{b}{sig}</div></div>")
            used += h
    else:
        def post(depth, width):
            av = 32 if depth else 40
            b, bh = body_html(width - av - 12, budget - used - 60)
            acts = "".join(f"<div style='display:flex;gap:4px;align-items:center'>{icon(14, t['muted'], k)}<div>{v}</div></div>" for k, v in (("heart", rnd.randint(0, 999)), ("circle", u["reply"]), ("square", rnd.randint(0, 99)))[:rnd.randint(2, 3)])
            html = (f"<div style='display:flex;gap:12px;margin:0 0 12px'><div style='width:{av}px;height:{av}px;border-radius:50%;background:{rnd.choice(AVATARS)};flex:none'></div><div style='flex:1;min-width:0'>"
                    f"<div style='font-size:{sm:.1f}px;margin:0 0 4px'><b style='font-size:{size:.0f}px'>{person(c)}</b> <span style='{mu}'>· {u['ago'].format(n=rnd.randint(1, 23))}</span></div>{b}"
                    f"<div style='display:flex;gap:18px;font-size:{sm:.1f}px;{mu}'>{acts}</div></div></div>")
            return html, bh + size * lh + sm * lh + 24, bool(b)
        for i in range(10):
            depth = 0 if i == 0 else rnd.choice([0, 1, 1, 2, 2, 3])
            ind = depth * rnd.choice([24, 32, 40]); width = cw - ind
            html, h, ok = post(depth, width)
            if not ok or used + h > budget: break
            guide = "".join(f"<div style='position:absolute;left:{d * (ind // depth if depth else 0) + 18}px;top:0;bottom:0;width:2px;background:{t['line']}'></div>" for d in range(depth))
            parts.append(f"<div style='position:relative;padding-left:{ind}px'>{guide}{html}</div>"); used += h
    body = f"<div style='width:{cw}px;margin:0 auto;padding:22px 0'>{''.join(parts)}</div>"
    return page(f"<div style='width:{W}px;height:{Hh}px'>{body}</div>", base_css(lh), t, font, size), W, Hh


# ------------------------------------------------------------------ 10 technical docs
@scene("docs_page")
def s_docs(rnd, lang):
    c = XCtx(rnd, lang); t = theme(rnd); font = ui_font(c); dark = is_dark(t)
    size = rnd.choice([14, 15, 15, 16]); lh = rnd.choice([1.5, 1.6, 1.7]); sm = size * .88; mu = f"color:{t['muted']}"
    W, Hh = rnd.choice([(1280, 860), (1200, 840), (1280, 800), (1100, 820)])
    nav_w = rnd.randint(220, 260); toc_w = rnd.randint(190, 230) if W >= 1200 and rnd.random() < .7 else 0
    main_w = min(760, W - nav_w - toc_w - 100); bar_h = 56
    top = (f"<div style='height:{bar_h}px;display:flex;align-items:center;gap:26px;padding:0 24px;border-bottom:1px solid {t['line']}'>"
           f"<div style='font-weight:800;font-size:{size * 1.2:.1f}px'>{fit(c.title(), 180, size * 1.2, lang)}</div>"
           + "".join(f"<div style='font-size:{sm:.1f}px'>{fit(lab(c, 1, 1), 90, sm, lang)}</div>" for _ in range(rnd.randint(2, 4)))
           + f"<div style='margin-left:auto;width:240px;display:flex;gap:8px;align-items:center;padding:6px 10px;border:1px solid {t['line']};border-radius:6px;{mu};font-size:{sm:.1f}px'>{icon(14, t['muted'], 'search')}<div>{fit(lab(c, 1, 2), 160, sm, lang)}</div></div>"
           f"<div style='font-size:{sm * .9:.1f}px;padding:2px 8px;border-radius:10px;background:{t['line']}'>v{rnd.randint(1, 9)}.{rnd.randint(0, 20)}</div></div>")
    side_h = Hh - bar_h - 20
    nav = []; used = 0
    for g in range(6):
        if used + 40 > side_h: break
        nav.append(f"<div style='font-size:{sm * .9:.1f}px;font-weight:700;{mu};margin:{16 if g else 4}px 0 4px;{'text-transform:uppercase;letter-spacing:.04em;' if lang == 'en' else ''}'>{fit(lab(c, 1, 3), nav_w - 30, sm, lang)}</div>"); used += sm * lh + 20
        for i in range(rnd.randint(2, 6)):
            if used + sm * lh + 8 > side_h: break
            lvl = 1 if (i and rnd.random() < .25) else 0
            act = f"background:{t['accent']}1f;color:{t['accent']};font-weight:600;" if g == 1 and i == 1 else ""
            nav.append(f"<div style='font-size:{sm:.1f}px;padding:3px 8px 3px {8 + lvl * 16}px;border-radius:4px;{act}'>{fit(lab(c, 1, 4), nav_w - 40 - lvl * 16, sm, lang)}</div>"); used += sm * lh + 6
    navh = f"<div style='width:{nav_w}px;flex:none;padding:12px 16px;border-right:1px solid {t['line']};overflow:hidden'>{''.join(nav)}</div>"
    parts = []; used = 0; budget = Hh - bar_h - 50
    crumb = " / ".join(fit(lab(c, 1, 2), 140, sm, lang) for _ in range(rnd.randint(2, 3)))
    parts.append(f"<div style='font-size:{sm:.1f}px;{mu};margin:0 0 6px'>{crumb}</div>"); used += sm * lh + 6
    h1 = fit(lab(c, 2, 5), main_w, size * 2, lang); parts.append(f"<h1 style='font-size:{size * 2:.0f}px;line-height:1.25;margin:0 0 12px'>{h1}</h1>"); used += size * 2.5 + 12
    heads = []

    def para():
        p = c.para(1, 4); return f"<p style='margin:0 0 1em'>{p}</p>", nlines(p, main_w, size, lang) * size * lh + size

    def h2():
        h = fit(lab(c, 2, 5), main_w, size * 1.4, lang); heads.append(h)
        return f"<h2 style='font-size:{size * 1.4:.1f}px;margin:1.1em 0 .5em;padding-bottom:4px;border-bottom:1px solid {t['line']}'>{h}</h2>", size * 1.4 * 1.3 + size * 2.4

    def admon():
        kind = rnd.choice([("#2563eb", "info"), ("#16a34a", "info"), ("#d97706", "warn"), ("#dc2626", "warn")])
        p = c.para(1, 2); bg = kind[0] + ("26" if dark else "14")
        ttl = f"<div style='font-weight:700;color:{kind[0]};margin:0 0 2px'>{fit(lab(c, 1, 2), main_w - 80, size, lang)}</div>" if rnd.random() < .7 else ""
        return (f"<div style='display:flex;gap:10px;padding:12px 14px;margin:0 0 1em;border-left:4px solid {kind[0]};background:{bg};border-radius:4px'>{icon(18, kind[0], kind[1])}"
                f"<div style='flex:1'>{ttl}<div>{p}</div></div></div>"), nlines(p, main_w - 60, size, lang) * size * lh + (size * lh if ttl else 0) + 24 + size

    def dl():
        out = []; h = 0
        for _ in range(rnd.randint(2, 4)):
            d = c.para(1, 1); term = fit(lab(c, 1, 3), main_w, size, lang)
            code = rnd.random() < .5
            out.append(f"<dt style='font-weight:700;{('font-family:Consolas,monospace;font-size:' + str(round(size * .92)) + 'px;') if code else ''}margin-top:.5em'>{term}</dt><dd style='margin:2px 0 0 1.8em;{mu}'>{d}</dd>")
            h += (1 + nlines(d, main_w - 30, size, lang)) * size * lh + size * .5
        return f"<dl style='margin:0 0 1em'>{''.join(out)}</dl>", h + size

    def table():
        rows = []; h = sm * lh + 18; w3 = main_w - 300
        for _ in range(rnd.randint(2, 4)):
            d = cut(c.para(1, 1), cpl(w3, sm, lang) * 2)
            rows.append(f"<tr><td style='padding:6px 10px;border-bottom:1px solid {t['line']};font-family:Consolas,monospace;vertical-align:top'>{''.join(rnd.choice('abcdefghiklmnoprstu') for _ in range(rnd.randint(3, 9)))}</td>"
                        f"<td style='padding:6px 10px;border-bottom:1px solid {t['line']};{mu};vertical-align:top'>{rnd.choice(['string', 'int', 'bool', 'float', 'list'])}</td><td style='padding:6px 10px;border-bottom:1px solid {t['line']}'>{d}</td></tr>")
            h += nlines(d, w3, sm, lang) * sm * lh + 13
        hd = "".join(f"<th style='text-align:left;padding:6px 10px;border-bottom:2px solid {t['line']}'>{fit(lab(c, 1, 1), 140, sm, lang)}</th>" for _ in range(3))
        return f"<table style='border-collapse:collapse;width:{main_w}px;table-layout:fixed;font-size:{sm:.1f}px;margin:0 0 1em'><colgroup><col style='width:150px'><col style='width:110px'><col></colgroup><tr>{hd}</tr>{''.join(rows)}</table>", h + size

    def code():
        n = rnd.randint(2, 5)
        lines = "\n".join(f"{rnd.choice(['$ ', '', '  '])}{''.join(rnd.choice('abcdefghiklmnoprstu') for _ in range(rnd.randint(3, 8)))}({rnd.randint(0, 99)}) = {fit(lab(c, 1, 2), 200, sm, lang) if lang == 'en' else rnd.randint(0, 999)}" for _ in range(n))
        return f"<pre style='margin:0 0 1em;padding:12px 14px;border-radius:6px;background:{t['card'] if dark else '#f6f8fa'};font-size:{sm:.1f}px;font-family:Consolas,monospace'>{lines}</pre>", n * sm * 1.3 + 24 + size
    order = [para] + rnd.sample([admon, rnd.choice([dl, table]), h2, para, para] + ([code] if rnd.random() < .5 else []), 5)
    for _ in range(12): order.append(rnd.choices([para, h2, admon, dl, table], [4, 2, 1.2, 1, 1])[0])
    for f in order:
        html, h = f()
        if used + h > budget: continue
        parts.append(html); used += h
    toc = ""
    if toc_w:
        items = [f"<div style='font-size:{sm:.1f}px;font-weight:700;margin:0 0 6px'>{fit(lab(c, 1, 2), toc_w - 20, sm, lang)}</div>"]
        for h in (heads or [fit(lab(c, 2, 4), toc_w, sm, lang)]) + [fit(lab(c, 2, 5), toc_w, sm, lang) for _ in range(rnd.randint(1, 4))]:
            sub = rnd.random() < .3
            items.append(f"<div style='font-size:{sm * .95:.1f}px;{mu};padding:3px 0 3px {12 + (14 if sub else 0)}px;border-left:2px solid {t['line']}'>{cut(h, cpl(toc_w - 30, sm, lang) * 2)}</div>")
        toc = f"<div style='width:{toc_w}px;flex:none;padding:24px 16px 0 0'>{''.join(items)}</div>"
    main = f"<div style='flex:1;display:flex;justify-content:center;padding:24px 30px;overflow:hidden'><div style='width:{main_w}px'>{''.join(parts)}</div></div>"
    body = f"{top}<div style='display:flex;height:{Hh - bar_h}px'>{navh}{main}{toc}</div>"
    return page(f"<div style='width:{W}px;height:{Hh}px'>{body}</div>", base_css(lh), t, font, size), W, Hh


# ------------------------------------------------------------------ 11 news article
@scene("news")
def s_news(rnd, lang):
    c = XCtx(rnd, lang); u = UI[lang]; t = theme(rnd); font = c.font(); dark = is_dark(t)
    size = rnd.choice([15, 16, 17, 18]); lh = rnd.choice([1.6, 1.7, 1.8]); sm = size * .8; mu = f"color:{t['muted']}"
    W, Hh = rnd.choice([(1200, 860), (1280, 860), (1100, 840), (1280, 800)])
    serif = rnd.choice(['Georgia', '"Times New Roman"', font]) if lang == "en" else rnd.choice(FONTS[lang])
    side_w = rnd.choice([0, 280, 300, 320]) if W >= 1200 else rnd.choice([0, 260])
    art_w = min(rnd.randint(620, 720), W - side_w - 100); accent = rnd.choice(["#c00", "#0a5ea8", "#b45309", t["accent"]])
    mast = (f"<div style='display:flex;justify-content:space-between;align-items:flex-end;padding:14px 32px 8px;border-bottom:3px double {t['fg']}'>"
            f"<div style='font-family:{serif};font-size:{size * 2:.0f}px;font-weight:800;line-height:1.1'>{fit(c.title(), 420, size * 2, lang)}</div><div style='font-size:{sm:.1f}px;{mu}'>{day(rnd, lang)}</div></div>"
            f"<div style='display:flex;gap:22px;padding:8px 32px;font-size:{sm:.1f}px;border-bottom:1px solid {t['line']}'>" + "".join(f"<div>{fit(lab(c, 1, 1), 100, sm, lang)}</div>" for _ in range(rnd.randint(5, 9))) + "</div>")
    top_h = size * 2.2 + 26 + sm * lh + 18; budget = Hh - top_h - 40
    parts = []; used = 0
    parts.append(f"<div style='font-size:{sm:.1f}px;font-weight:700;color:{accent};{'text-transform:uppercase;letter-spacing:.06em;' if lang == 'en' else ''}margin:0 0 6px'>{fit(lab(c, 1, 2), art_w, sm, lang)}</div>"); used += sm * lh + 6
    hsz = size * rnd.choice([1.7, 1.9, 2.2]); hl = cut(c.para(1, 1), cpl(art_w, hsz, lang) * rnd.choice([2, 3]))
    parts.append(f"<h1 style='font-family:{serif};font-size:{hsz:.0f}px;line-height:1.18;margin:0 0 10px'>{hl}</h1>"); used += nlines(hl, art_w, hsz, lang) * hsz * 1.18 + 10
    lede = cut(c.para(1, 2), cl(260, lang)); parts.append(f"<div style='font-size:{size * 1.12:.1f}px;{mu};line-height:1.5;margin:0 0 12px'>{lede}</div>"); used += nlines(lede, art_w, size * 1.12, lang) * size * 1.12 * 1.5 + 12
    au = u["by"].format(n=person(c))
    if rnd.random() < .5:
        parts.append(f"<div style='font-size:{sm:.1f}px;margin:0 0 16px;padding:8px 0;border-top:1px solid {t['line']};border-bottom:1px solid {t['line']}'><b>{au}</b> <span style='{mu}'>· {day(rnd, lang)} {clock(rnd)} · {u['read'].format(n=rnd.randint(2, 15))}</span></div>"); used += sm * lh + 34
    else:
        parts.append(f"<div style='display:flex;gap:10px;align-items:center;margin:0 0 16px'><div style='width:36px;height:36px;border-radius:50%;background:{rnd.choice(AVATARS)}'></div><div><div style='font-size:{sm:.1f}px;font-weight:700'>{au}</div>"
                     f"<div style='font-size:{sm * .95:.1f}px;{mu}'>{day(rnd, lang)} {clock(rnd)}</div></div><div style='margin-left:auto;display:flex;gap:10px'>{icon(18, t['muted'], 'circle')}{icon(18, t['muted'], 'square')}</div></div>"); used += 54
    fig_first = rnd.random() < .5

    def figure():
        fh = rnd.randint(120, 220); cap = cut(c.para(1, 1), cl(170, lang))
        credit = f"<span style='{mu}'> {fit(lab(c, 1, 2), 200, sm, lang)}</span>" if rnd.random() < .5 else ""
        sep = f"<div style='font-size:{sm * .9:.1f}px;{mu};margin-top:2px'>{fit(lab(c, 1, 3), art_w, sm * .9, lang)}</div>" if not credit and rnd.random() < .5 else ""
        return (f"<div style='margin:4px 0 18px'><div style='height:{fh}px;background:linear-gradient(160deg,{rnd.choice(AVATARS)},{rnd.choice(AVATARS)} 60%,#333)'></div>"
                f"<div style='font-size:{sm:.1f}px;margin-top:6px;line-height:1.45'>{cap}{credit}</div>{sep}</div>"), fh + nlines(cap, art_w, sm, lang) * sm * 1.45 + 30 + (sm * lh if sep else 0)

    def pull():
        q = cut(c.para(1, 1), cl(150, lang)); qs = size * 1.35
        st = rnd.choice([f"border-top:3px solid {accent};border-bottom:1px solid {t['line']};padding:12px 0;text-align:center", f"border-left:4px solid {accent};padding:4px 0 4px 18px"])
        return (f"<div style='{st};margin:6px 0 18px'><div style='font-family:{serif};font-size:{qs:.1f}px;font-style:italic;line-height:1.4'>“{q}”</div>"
                f"<div style='font-size:{sm:.1f}px;{mu};margin-top:6px'>— {person(c)}</div></div>"), nlines(q, art_w, qs, lang) * qs * 1.4 + sm * lh + 52

    def para():
        p = c.para(1, 4); return f"<p style='margin:0 0 1em'>{p}</p>", nlines(p, art_w, size, lang) * size * lh + size
    order = ([figure] if fig_first else []) + [para, para] + rnd.sample([pull, para, para, figure, para], 5) + [para] * 3
    for f in order:
        html, h = f()
        if used + h > budget: continue
        parts.append(html); used += h
    side = ""
    if side_w:
        items = [f"<div style='font-weight:800;font-size:{size * 1.05:.1f}px;padding-bottom:6px;border-bottom:2px solid {t['fg']};margin-bottom:8px'>{fit(lab(c, 1, 2), side_w, size * 1.05, lang)}</div>"]; sh = 40
        rank = rnd.random() < .6
        for i in range(8):
            tt = cut(c.para(1, 1), cpl(side_w - 40, size * .9, lang) * 2); h = nlines(tt, side_w - 40, size * .9, lang) * size * .9 * 1.4 + sm * lh + 22
            if sh + h > Hh - top_h - 60: break
            num = f"<div style='font-size:{size * 1.6:.0f}px;font-weight:800;color:{accent};width:30px;flex:none;line-height:1'>{i + 1}</div>" if rank else ""
            items.append(f"<div style='display:flex;gap:10px;padding:8px 0;border-bottom:1px solid {t['line']}'>{num}<div><div style='font-size:{size * .9:.1f}px;font-weight:600;line-height:1.4'>{tt}</div>"
                         f"<div style='font-size:{sm * .95:.1f}px;{mu};margin-top:2px'>{u['ago'].format(n=rnd.randint(1, 23))}</div></div></div>"); sh += h
        side = f"<div style='width:{side_w}px;flex:none'>{''.join(items)}</div>"
    body = f"{mast}<div style='display:flex;gap:56px;justify-content:center;padding:22px 32px 0'><div style='width:{art_w}px'>{''.join(parts)}</div>{side}</div>"
    return page(f"<div style='width:{W}px;height:{Hh}px'>{body}</div>", base_css(lh), t, font, size), W, Hh


# ------------------------------------------------------------------ 12 form dialog
@scene("form_dialog")
def s_form(rnd, lang):
    c = XCtx(rnd, lang); u = UI[lang]; font = ui_font(c); dark = rnd.random() < .28
    P = (dict(bg="#202020", card="#2b2b2b", fg="#ececec", muted="#9a9a9a", line="#454545", field="#1f1f1f", accent=rnd.choice(["#4cc2ff", "#60a5fa", "#a78bfa"]), err="#ff6b6b")
         if dark else dict(bg="#f3f3f3", card="#ffffff", fg="#1b1b1b", muted="#6b6b6b", line="#cfcfcf", field="#ffffff", accent=rnd.choice(["#0067c0", "#2563eb", "#16a34a", "#7c3aed"]), err="#c42b1c"))
    size = rnd.choice([13, 14, 14, 15]); lh = 1.4; sm = size * .86; mu = f"color:{P['muted']}"
    W, Hh = rnd.choice([(1000, 800), (1100, 820), (960, 760), (1200, 860)])
    dw = rnd.randint(440, 620); pad = rnd.randint(20, 28); iw = dw - 2 * pad
    win = rnd.random() < .35
    fh = rnd.choice([30, 34, 38]); rad = rnd.choice([3, 4, 6, 8])
    fields = []; used = 0; budget = Hh - 120 - (90 if not win else 110)

    def box(text, filled, w=None, h=None, err=False):
        bc = P["err"] if err else P["line"]
        return (f"<div style='display:flex;align-items:{'flex-start' if h else 'center'};justify-content:space-between;gap:6px;{('width:' + str(w) + 'px;') if w else ''}height:{h or fh}px;padding:{'6px 10px' if h else '0 10px'};border:1px solid {bc};{('border-bottom:2px solid ' + P['accent'] + ';') if (not err and rnd.random() < .2) else ''}border-radius:{rad}px;background:{P['field']}'>"
                f"<div style='{'' if filled else mu}'>{text}</div></div>")

    def label(w):
        req = f"<span style='color:{P['err']}'> *</span>" if rnd.random() < .3 else ""
        return f"<label style='display:block;font-weight:600;margin:0 0 5px'>{fit(lab(c, 1, 3), w - 20, size, lang)}{req}</label>"

    def text_field(w):
        filled = rnd.random() < .4; err = rnd.random() < .18
        v = fit(lab(c, 1, 4) if filled else lab(c, 2, 5), w - 30, size, lang)
        out = label(w) + box(v, filled, err=err); h = size * lh + 5 + fh
        if err:
            e = cut(c.para(1, 1), cpl(w - 24, sm, lang) * rnd.choice([1, 2]))
            out += f"<div style='display:flex;gap:6px;margin-top:5px;color:{P['err']};font-size:{sm:.1f}px'>{icon(14, P['err'], 'warn')}<div>{e}</div></div>"; h += nlines(e, w - 24, sm, lang) * sm * lh + 5
        elif rnd.random() < .5:
            hp = cut(c.para(1, 1), cpl(w, sm, lang) * rnd.choice([1, 2, 2]))
            out += f"<div style='margin-top:5px;font-size:{sm:.1f}px;{mu}'>{hp}</div>"; h += nlines(hp, w, sm, lang) * sm * lh + 5
        return out, h

    def field():
        k = rnd.random()
        if k < .4:
            html, h = text_field(iw); return f"<div style='margin:0 0 16px'>{html}</div>", h + 16
        if k < .55:
            w2 = (iw - 16) / 2; a, ha = text_field(w2); b, hb = text_field(w2)
            return f"<div style='display:flex;gap:16px;margin:0 0 16px'><div style='width:{w2:.0f}px'>{a}</div><div style='width:{w2:.0f}px'>{b}</div></div>", max(ha, hb) + 16
        if k < .65:
            v = fit(lab(c, 1, 3), iw - 60, size, lang)
            return (f"<div style='margin:0 0 16px'>{label(iw)}<div style='display:flex;align-items:center;justify-content:space-between;height:{fh}px;padding:0 10px;border:1px solid {P['line']};border-radius:{rad}px;background:{P['field']}'><div>{v}</div>{icon(14, P['muted'], 'down')}</div></div>"), size * lh + fh + 21
        if k < .75:
            ph = cut(c.para(1, 1), cpl(iw - 20, size, lang) * 2); th = rnd.choice([70, 84, 96])
            return (f"<div style='margin:0 0 16px'>{label(iw)}<div style='position:relative;height:{th}px;padding:6px 10px;border:1px solid {P['line']};border-radius:{rad}px;background:{P['field']}'><div style='{mu}'>{ph}</div>"
                    f"<div style='position:absolute;right:8px;bottom:4px;font-size:{sm * .9:.1f}px;{mu}'>0/{rnd.choice([100, 200, 500])}</div></div></div>"), size * lh + th + 21
        if k < .9:
            out = []; h = 0
            for _ in range(rnd.randint(1, 3)):
                lt = cut(c.para(1, 1), cpl(iw - 30, size, lang) * rnd.choice([1, 2])) if rnd.random() < .6 else fit(lab(c, 2, 6), iw - 30, size, lang)
                on = rnd.random() < .5
                mark = f"<div style='width:16px;height:16px;margin-top:{(size * lh - 16) / 2:.0f}px;border:1.5px solid {P['accent'] if on else P['muted']};border-radius:3px;background:{P['accent'] if on else 'transparent'};flex:none'></div>"
                out.append(f"<div style='display:flex;gap:10px;align-items:flex-start;margin:0 0 8px'>{mark}<div>{lt}</div></div>"); h += nlines(lt, iw - 30, size, lang) * size * lh + 8
            return f"<div style='margin:0 0 10px'>{''.join(out)}</div>", h + 10
        opts = "".join(f"<div style='display:flex;gap:8px;align-items:center'><div style='width:16px;height:16px;border-radius:50%;border:{'5px solid ' + P['accent'] if i == 0 else '1.5px solid ' + P['muted']};flex:none'></div><div>{fit(lab(c, 1, 2), iw / 3 - 30, size, lang)}</div></div>" for i in range(rnd.randint(2, 3)))
        return f"<div style='margin:0 0 16px'>{label(iw)}<div style='display:flex;gap:22px;flex-wrap:wrap'>{opts}</div></div>", size * lh * 2 + 21

    title = fit(lab(c, 2, 5), iw - 40, size * 1.3, lang)
    desc = cut(c.para(1, 2), cpl(iw, size, lang) * rnd.choice([1, 2, 3])) if rnd.random() < .7 else ""
    used += nlines(desc, iw, size, lang) * size * lh + 12 if desc else 0
    if rnd.random() < .2:
        al = cut(c.para(1, 1), cpl(iw - 50, size, lang) * 2)
        fields.append(f"<div style='display:flex;gap:10px;padding:10px 12px;margin:0 0 16px;border-radius:{rad}px;background:{P['err']}1f;border:1px solid {P['err']}66'>{icon(16, P['err'], 'warn')}<div>{al}</div></div>")
        used += nlines(al, iw - 50, size, lang) * size * lh + 38
    for _ in range(10):
        html, h = field()
        if used + h > budget: continue
        fields.append(html); used += h
    ok = rnd.choice([u["ok"], fit(lab(c, 1, 2), 120, size, lang)]); cancel = rnd.choice([u["cancel"], fit(lab(c, 1, 2), 120, size, lang)])
    link = f"<div style='color:{P['accent']};font-size:{sm:.1f}px'>{fit(lab(c, 1, 3), iw / 2 - 20, sm, lang)}</div>" if rnd.random() < .4 else "<div></div>"
    btn = lambda txt, primary: (f"<button style='font:inherit;min-width:88px;padding:6px 16px;border-radius:{rad}px;border:1px solid {P['accent'] if primary else P['line']};"
                                f"background:{P['accent'] if primary else P['card']};color:{'#fff' if primary else P['fg']}'>{txt}</button>")
    order = btn(ok, True) + btn(cancel, False) if rnd.random() < .5 else btn(cancel, False) + btn(ok, True)
    footer = f"<div style='display:flex;justify-content:space-between;align-items:center;padding:14px {pad}px;{('background:' + P['bg'] + ';border-top:1px solid ' + P['line']) if win else ''}'>{link}<div style='display:flex;gap:10px'>{order}</div></div>"
    if win:
        head = (f"<div style='display:flex;justify-content:space-between;align-items:center;height:34px;padding:0 0 0 12px;font-size:{sm:.1f}px'><div>{fit(lab(c, 1, 3), dw - 120, sm, lang)}</div>"
                f"<div style='width:46px;height:34px;display:flex;align-items:center;justify-content:center'>{icon(10, P['fg'], 'x')}</div></div>"
                f"<div style='padding:6px {pad}px 0'><div style='font-size:{size * 1.3:.1f}px;font-weight:600;margin:0 0 6px'>{title}</div>")
    else:
        head = (f"<div style='padding:{pad}px {pad}px 0'><div style='display:flex;justify-content:space-between;align-items:flex-start;gap:10px;margin:0 0 6px'>"
                f"<div style='font-size:{size * 1.3:.1f}px;font-weight:600'>{title}</div>{icon(14, P['muted'], 'x')}</div>")
    head += (f"<div style='{mu};margin:0 0 14px'>{desc}</div>" if desc else "") + f"{''.join(fields)}</div>"
    dlg = (f"<div style='position:absolute;left:{(W - dw) // 2}px;top:{rnd.randint(30, 60)}px;width:{dw}px;background:{P['card']};border-radius:{rad + 4 if not win else 8}px;"
           f"box-shadow:0 12px 40px rgba(0,0,0,.35);border:1px solid {P['line']};overflow:hidden'>{head}{footer}</div>")
    shapes = "".join(f"<div style='position:absolute;left:{rnd.randint(0, W - 420)}px;top:{rnd.randint(0, Hh - 60)}px;width:{rnd.randint(120, 400)}px;height:{rnd.randint(14, 60)}px;border-radius:4px;background:{P['line']}'></div>" for _ in range(rnd.randint(6, 14)))
    dim = f"<div style='position:absolute;inset:0;background:rgba(0,0,0,{rnd.uniform(.25, .5):.2f})'></div>" if rnd.random() < .7 else ""
    body = f"<div style='position:relative;width:{W}px;height:{Hh}px;background:{P['bg']}'>{shapes}{dim}{dlg}</div>"
    return page(body, base_css(lh), {"bg": P["bg"], "fg": P["fg"]}, font, size), W, Hh


# ------------------------------------------------------------------ 13 nested lists
ROMAN = ["i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x"]
CN_NUM = "一二三四五六七八九十"
KO_NUM = "가나다라마바사아자차"
CIRCLED = "①②③④⑤⑥⑦⑧⑨⑩"


def marker(kind, i):
    return {"bullet": "•", "circle": "◦", "square": "▪", "dash": "–", "tri": "‣", "kdot": "・",
            "dec": f"{i}.", "paren": f"({i})", "rparen": f"{i})", "alpha": f"{chr(96 + i)}.", "Alpha": f"{chr(64 + i)}.",
            "aparen": f"({chr(96 + i)})", "roman": f"{ROMAN[i - 1]}.", "circled": CIRCLED[i - 1], "cn": f"{CN_NUM[i - 1]}、",
            "cnparen": f"（{CN_NUM[i - 1]}）", "ko": f"{KO_NUM[i - 1]}.", "koparen": f"({KO_NUM[i - 1]})"}[kind]


SCHEMES = {
    "en": [["bullet", "circle", "square"], ["dec", "alpha", "roman"], ["dec", "paren", "aparen"], ["Alpha", "dec", "alpha"], ["dash", "bullet", "circle"], ["dec", "bullet", "dash"]],
    "zh": [["cn", "cnparen", "dec"], ["dec", "paren", "circled"], ["bullet", "circle", "square"], ["dec", "rparen", "bullet"], ["cn", "dec", "paren"]],
    "ja": [["dec", "paren", "circled"], ["kdot", "dash", "bullet"], ["circled", "kdot", "dash"], ["bullet", "circle", "square"], ["dec", "rparen", "kdot"]],
    "ko": [["dec", "ko", "rparen"], ["bullet", "dash", "circle"], ["dec", "paren", "koparen"], ["square", "dash", "bullet"], ["dec", "ko", "circled"]],
}
CSS_TYPES = {"bullet": "disc", "circle": "circle", "square": "square", "dec": "decimal", "alpha": "lower-alpha", "Alpha": "upper-alpha", "roman": "lower-roman",
             "cn": "cjk-ideographic", "ko": "korean-hangul-formal"}


@scene("nested_lists")
def s_lists(rnd, lang):
    c = XCtx(rnd, lang); t = theme(rnd); font = c.font()
    size = rnd.choice([14, 15, 16, 17]); lh = rnd.choice([1.4, 1.5, 1.6, 1.75])
    W, Hh = rnd.choice([(1000, 860), (1100, 860), (900, 800), (1200, 860)])
    width = min(W - 80, rnd.randint(560, 860)); ml = rnd.choice(["margin:0 auto", "margin:0 0 0 48px"])
    ind = rnd.choice([1.5, 1.8, 2.2, 2.6]); loose = rnd.choice([0, .15, .35, .6])
    budget = Hh - 70; used = 0; parts = []
    tsz = size * rnd.choice([1.6, 1.9]); title = fit(c.title(), width, tsz, lang)
    parts.append(f"<h1 style='font-size:{tsz:.0f}px;margin:0 0 .6em;line-height:1.25'>{title}</h1>"); used += tsz * 1.25 + tsz * .6

    def para():
        p = c.para(1, 3); return f"<p style='margin:0 0 {rnd.choice([.6, .9])}em;text-align:{rnd.choice(['left', 'justify'])}'>{p}</p>", nlines(p, width, size, lang) * size * lh + size * .8

    def h2():
        h = fit(lab(c, 2, 5), width, size * 1.25, lang)
        return f"<h2 style='font-size:{size * 1.25:.0f}px;margin:.8em 0 .4em'>{h}</h2>", size * 1.25 * 1.3 + size * 1.5

    def lst():
        scheme = rnd.choice(SCHEMES[lang]); mode = rnd.choice(["text", "text", "text", "css", "flat"])
        if mode == "css" and not all(k in CSS_TYPES for k in scheme): mode = "text"
        hang = mode != "flat"
        items = []; h = 0; counters = [0, 0, 0, 0]; level = 0; nmax = rnd.randint(4, 10)
        for n in range(nmax):
            counters[level] += 1
            for d in range(level + 1, 4): counters[d] = 0
            i = min(counters[level], 10)
            k = scheme[level]; m = marker(k, i)
            txt = c.para(1, 2) if level == 0 else (c.para(1, 1) if rnd.random() < .6 else lab(c, 2, 6))
            fs = size if level == 0 or rnd.random() < .7 else size * .94
            avail = width - level * ind * fs - ind * fs
            ih = nlines(txt, avail, fs, lang) * fs * lh + loose * fs
            cont = ""
            if level == 0 and rnd.random() < .12:
                cp = c.para(1, 2); cont = cp; ih += nlines(cp, avail, fs, lang) * fs * lh + .3 * fs
            if h + ih > budget - used - 30: break
            if mode == "css":
                body = f"<p style='margin:0'>{txt}</p><p style='margin:.3em 0 0'>{cont}</p>" if cont else txt
                items.append(f"<li style='margin:0 0 {loose}em {level * ind + ind:.2f}em;list-style-type:{CSS_TYPES[k]};font-size:{fs:.1f}px'>{body}</li>")
            else:
                mw = max(1.2, len(m) * (.62 if lang == "en" or not any(ord(ch) > 0x2e80 for ch in m) else 1.05) + .45)
                lead = f"<span style='display:inline-block;width:{mw:.2f}em;text-indent:0'>{m}</span>" if hang else f"{m} "
                pad_l = level * ind + (mw if hang else 0) + (0 if hang else 0)
                st = f"padding-left:{pad_l:.2f}em;text-indent:-{mw:.2f}em" if hang else f"padding-left:{level * ind:.2f}em"
                items.append(f"<div style='font-size:{fs:.1f}px;{st};margin:0 0 {loose}em'>{lead}{txt}</div>")
                if cont: items.append(f"<div style='font-size:{fs:.1f}px;padding-left:{level * ind + (mw if hang else 1.2):.2f}em;margin:{.3 - loose:.2f}em 0 {loose}em'>{cont}</div>")
            h += ih
            r = rnd.random()
            if r < .35 and level < 2: level += 1
            elif r < .6 and level > 0: level -= rnd.randint(1, level)
        if not items: return "", 0
        html = "".join(items)
        if mode == "css": html = f"<ul style='margin:0 0 .8em;padding:0'>{html}</ul>"
        else: html = f"<div style='margin:0 0 .8em'>{html}</div>"
        return html, h + size * .8

    order = [para, lst] + [rnd.choices([para, h2, lst], [2, 1, 3])[0] for _ in range(10)]
    for f in order:
        html, h = f()
        if not html or used + h > budget: continue
        parts.append(html); used += h
    return page(f"<div style='width:{width}px;{ml};padding:28px 0'>{''.join(parts)}</div>", base_css(lh), t, font, size), W, Hh
