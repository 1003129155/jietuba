# -*- coding: utf-8 -*-
"""样式表不能把透明背景传给提示框。

Qt 显示提示框时会沿用悬停控件所有祖先的样式表，优先级还高于全局的
QToolTip 规则。祖先里只要有一条能匹配 QTipLabel 的背景规则，提示框就用它的
背景；透明背景画在提示框这个顶层窗口上就是黑底，配上主题的深色字几乎看不清。

能匹配 QTipLabel 的规则：无选择器的样式表（对整棵子树生效），以及选择器是
*、QWidget、QFrame、QLabel 的规则（QTipLabel 继承自 QLabel）。只给控件自己
设背景请用 core.ui_theme.set_own_style。
"""
import ast
import re
from pathlib import Path

import pytest

MAIN_DIR = Path(__file__).resolve().parents[1]
_TIP_MATCHING_SELECTORS = {"*", "QWidget", "QFrame", "QLabel"}
_BACKGROUND = re.compile(r"background(-color)?\s*:", re.IGNORECASE)
_SEE_THROUGH_BACKGROUND = re.compile(
    r"background(-color)?\s*:\s*(transparent|rgba\()", re.IGNORECASE
)
_RULE = re.compile(r"([^{}]*)\{([^{}]*)\}")


def _literal_text(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.JoinedStr):
        return "".join(
            part.value if isinstance(part, ast.Constant) else "X"
            for part in node.values
        )
    return None


def _problem(sheet):
    sheet = re.sub(r"/\*.*?\*/", "", sheet, flags=re.DOTALL)
    if "{" not in sheet:
        if _BACKGROUND.search(sheet):
            return "无选择器的样式表设置了 background"
        return None
    for selectors, declarations in _RULE.findall(sheet):
        names = {s.strip() for s in selectors.split(",")}
        hit = names & _TIP_MATCHING_SELECTORS
        if hit and _SEE_THROUGH_BACKGROUND.search(declarations):
            return f"选择器 {sorted(hit)} 会匹配提示框，且背景透明"
    return None


def _violations():
    found = []
    for path in sorted(MAIN_DIR.rglob("*.py")):
        rel = path.relative_to(MAIN_DIR)
        if rel.parts[0] in ("tests", "build", "dist") or "site-packages" in rel.parts:
            continue
        # 不少源文件带 BOM，按 utf-8 读会让 ast.parse 报错；解析失败也不能
        # 静默跳过，否则整份文件都漏检。
        tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
        for node in ast.walk(tree):
            if not (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == "setStyleSheet"
                and node.args
            ):
                continue
            text = _literal_text(node.args[0])
            if text is None:
                continue
            reason = _problem(text)
            if reason:
                found.append(f"{rel.as_posix()}:{node.lineno}: {reason}")
    return found


def test_no_stylesheet_passes_a_background_to_tooltips():
    violations = _violations()
    assert not violations, "\n".join(violations)


@pytest.mark.parametrize(
    "sheet",
    [
        "background: transparent;",
        "font-size: 13px; background-color: #fff;",
        "QLabel { background: transparent; }",
        "QWidget, QPushButton { background: rgba(0,0,0,0); }",
        "* { background: transparent; }",
    ],
)
def test_detects_rules_that_reach_the_tooltip(sheet):
    assert _problem(sheet)


@pytest.mark.parametrize(
    "sheet",
    [
        ".QWidget { background: transparent; }",
        "#view { background: transparent; }",
        "QScrollArea { border: none; background: transparent; }",
        "QWidget { background: #FAFAFA; }",
        "color: #333;",
        "QScrollArea > QWidget > QWidget { background: transparent; }",
    ],
)
def test_allows_rules_that_cannot_reach_the_tooltip(sheet):
    assert _problem(sheet) is None
