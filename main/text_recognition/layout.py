# -*- coding: utf-8 -*-
"""把识别出的行排成文本：折行合并、去空格、标点全半角

识别结果里没有段落标记，但同一段被折开的两行有稳定的外形：上一行排到正文右界、下一行
从左界起排、两行挨得近、上一行末尾不是句末标点。四条都成立才合并，宁可少合并一处，
也不要把本来分开的两行粘起来——前者用户一眼看得出，后者要逐行读才发现。

纯函数，输入是 ocr.group_ocr_lines 的行（文字 + 外接框），不依赖 Qt。
"""

import re

MODE_AUTO = "auto"
MODE_SINGLE_LINE = "single_line"
MODE_MULTILINE = "multiline"
LAYOUT_MODES = (MODE_AUTO, MODE_SINGLE_LINE, MODE_MULTILINE)

PUNCT_KEEP = "keep"
PUNCT_HALFWIDTH = "halfwidth"
PUNCT_FULLWIDTH = "fullwidth"
PUNCTUATION_MODES = (PUNCT_KEEP, PUNCT_HALFWIDTH, PUNCT_FULLWIDTH)

# 右界/左界的判定容差，按行高的倍数算：字号越大，同一段各行的边缘越参差
EDGE_TOLERANCE = 0.5
# 行间空隙超过行高的这个倍数就算段落之间，不再合并
GAP_LIMIT = 1.2
# 行尾是这些字符时该行自成一段。西文句点同时是小数点和缩写点，所以只认行尾的
TERMINATORS = "。．.！!？?；;…"

_CJK = re.compile(
    r"[　-〿぀-ヿ㐀-䶿一-鿿"
    r"가-힯豈-﫿＀-￯]"
)
# 行内空白，换行不动
_INLINE_SPACE = re.compile(r"[^\S\r\n]+")

_TO_HALFWIDTH = {
    "，": ",", "。": ".", "！": "!", "？": "?", "；": ";", "：": ":", "、": ",",
    "（": "(", "）": ")", "【": "[", "】": "]", "《": "<", "》": ">",
    "“": '"', "”": '"', "‘": "'", "’": "'", "　": " ",
}
# 引号的全角有开合之分，光看字符定不下来，所以不往回转
_TO_FULLWIDTH = {
    ",": "，", ".": "。", "!": "！", "?": "？", ";": "；", ":": "：",
    "(": "（", ")": "）", "[": "【", "]": "】",
}


def render(lines, mode=MODE_AUTO, remove_spaces=False, punctuation=PUNCT_KEEP):
    """按设置把行排成一段文本。mode 取 LAYOUT_MODES，punctuation 取 PUNCTUATION_MODES"""
    text = _layout(lines, mode)
    if remove_spaces:
        text = _strip_spaces(text)
    return _convert_punctuation(text, punctuation)


def _strip_spaces(text):
    """只去掉挨着中日韩文字的空格。

    识别常在汉字之间塞进空格，那是噪声；而西文的词间空格是内容，一起去掉就读不成句了。
    """
    def replace(match):
        before = text[match.start() - 1] if match.start() else ""
        after = text[match.end()] if match.end() < len(text) else ""
        return "" if _is_cjk(before) or _is_cjk(after) else match.group(0)

    return _INLINE_SPACE.sub(replace, text)


def _layout(lines, mode):
    if not lines:
        return ""
    if mode == MODE_MULTILINE:
        return "\n".join(line["text"] for line in lines)
    if mode == MODE_SINGLE_LINE:
        text = lines[0]["text"]
        for line in lines[1:]:
            text = _merge(text, line["text"])
        return text
    return _auto(lines)


def _auto(lines):
    metrics = _metrics(lines)
    text = lines[0]["text"]
    for previous, current in zip(lines, lines[1:]):
        if _is_wrapped(previous, current, metrics):
            text = _merge(text, current["text"])
        else:
            text += "\n" + current["text"]
    return text


def _metrics(lines):
    """整块文本的左右界和行高中位数。中位数比均值稳：标题和脚注的行高都是离群值"""
    heights = sorted(line["bottom"] - line["top"] for line in lines)
    return {
        "height": heights[len(heights) // 2] or 1.0,
        "left": min(line["left"] for line in lines),
        "right": max(line["right"] for line in lines),
    }


def _is_wrapped(previous, current, metrics):
    """current 是不是 previous 被折下来的下半行"""
    tolerance = metrics["height"] * EDGE_TOLERANCE
    if previous["right"] < metrics["right"] - tolerance:
        return False        # 没排到右界，这是段落的最后一行
    if current["left"] > metrics["left"] + tolerance:
        return False        # 缩进了，这是新段落的开头
    if current["top"] - previous["bottom"] > metrics["height"] * GAP_LIMIT:
        return False        # 行距撑开了
    return previous["text"][-1:] not in TERMINATORS


def _merge(left, right):
    """接上被折开的两行：中日韩直连，西文补空格，断词的连字符去掉"""
    if not left or not right:
        return left or right
    if left.endswith("-") and left[-2:-1].isalpha() and right[:1].isalpha():
        return left[:-1] + right
    if _is_cjk(left[-1]) or _is_cjk(right[0]):
        return left + right
    return left + " " + right


def _is_cjk(char):
    return bool(char) and _CJK.match(char) is not None


def _convert_punctuation(text, punctuation):
    if punctuation == PUNCT_HALFWIDTH:
        return "".join(_TO_HALFWIDTH.get(char, char) for char in text)
    if punctuation == PUNCT_FULLWIDTH:
        return "".join(_fullwidth_at(text, index) for index in range(len(text)))
    return text


def _fullwidth_at(text, index):
    """只转夹在中日韩文字里的标点：小数点、西文括号和缩写点要留着"""
    char = text[index]
    full = _TO_FULLWIDTH.get(char)
    if full is None:
        return char
    before = text[index - 1] if index else ""
    after = text[index + 1] if index + 1 < len(text) else ""
    return full if _is_cjk(before) or _is_cjk(after) else char


__all__ = [
    "LAYOUT_MODES", "MODE_AUTO", "MODE_MULTILINE", "MODE_SINGLE_LINE",
    "PUNCTUATION_MODES", "PUNCT_FULLWIDTH", "PUNCT_HALFWIDTH", "PUNCT_KEEP",
    "render",
]
