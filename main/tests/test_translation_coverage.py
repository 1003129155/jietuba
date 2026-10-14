"""源码里写着原文的翻译调用，四种语言的翻译文件都要有同一上下文的条目，否则界面会露出原文的语言。

识别的写法：类里的 self.tr("…")（上下文为类名）；X = make_tr("上下文") 之后的 X("…")；
从 core.i18n 导入的 tr("…", "上下文")；QCoreApplication.translate("上下文", "…")；
只转发 QCoreApplication.translate("上下文", 参数) 的函数；别的对象的 obj.tr("…")（按 RECEIVER_CONTEXTS）。
"""

import ast
import xml.etree.ElementTree as ET
from pathlib import Path

MAIN = Path(__file__).resolve().parents[1]
LANGUAGES = ("zh", "en", "ja", "ko")
SKIP_DIRS = {"tests", "scripts", "translations", "__pycache__"}
# obj.tr() 的上下文是 obj 的类名，静态看不出来，按 (路径前缀, 接收者) 登记；没登记的接收者测试会报出来
RECEIVER_CONTEXTS = {
    ("ui/settings_ui/", "dialog"): "SettingsDialog",
    ("clipboard/ui/forms/", "dialog"): "ManageDialog",
    ("clipboard/controllers/clipboard_controller.py", "parent_widget"): "GroupBar",
    ("pin/pin_context_menu.py", "self.parent"): "PinWindow",
    ("pin/pin_ocr_manager.py", "self._win"): "PinWindow",
    ("translation/translation_manager.py", "self._dialog"): "TranslationLoadingDialog",
}


def _is_text(node):
    return isinstance(node, ast.Constant) and isinstance(node.value, str)


def _dotted(node):
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        owner = _dotted(node.value)
        return f"{owner}.{node.attr}" if owner else None
    return None


def _forwarded_context(func):
    """def f(text): return QCoreApplication.translate("上下文", text) 的上下文，其他函数返回 None。"""
    body = [stmt for stmt in func.body if not (isinstance(stmt, ast.Expr) and _is_text(stmt.value))]
    if len(func.args.args) != 1 or len(body) != 1 or not isinstance(body[0], ast.Return):
        return None
    call = body[0].value
    if isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute) and call.func.attr == "translate" \
            and len(call.args) == 2 and _is_text(call.args[0]) \
            and getattr(call.args[1], "id", None) == func.args.args[0].arg:
        return call.args[0].value
    return None


def _receiver_context(relative, receiver):
    for (prefix, name), context in RECEIVER_CONTEXTS.items():
        if name == receiver and relative.startswith(prefix):
            return context
    return None


def _calls_in(path):
    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
    relative = path.relative_to(MAIN).as_posix()
    aliases = {}
    global_tr = False
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign) and isinstance(node.value, ast.Call) \
                and getattr(node.value.func, "id", None) == "make_tr" and node.value.args and _is_text(node.value.args[0]):
            aliases.update({t.id: node.value.args[0].value for t in node.targets if isinstance(t, ast.Name)})
        if isinstance(node, ast.FunctionDef) and (context := _forwarded_context(node)):
            aliases[node.name] = context
        if isinstance(node, ast.ImportFrom) and node.module == "core.i18n":
            global_tr |= any(alias.name == "tr" and alias.asname is None for alias in node.names)

    found = []

    def visit(node, cls):
        if isinstance(node, ast.ClassDef):
            cls = node.name
        if isinstance(node, ast.Call) and node.args and _is_text(node.args[0]):
            func, text = node.func, node.args[0].value
            receiver = _dotted(func.value) if isinstance(func, ast.Attribute) and func.attr == "tr" else None
            if receiver == "self" and cls:
                found.append((cls, text))
            elif receiver and receiver != "self":
                found.append((_receiver_context(relative, receiver) or f"<未登记的接收者 {receiver}>", text))
            elif isinstance(func, ast.Name) and func.id in aliases:
                found.append((aliases[func.id], text))
            elif isinstance(func, ast.Name) and func.id == "tr" and global_tr:
                context = node.args[1].value if len(node.args) > 1 and _is_text(node.args[1]) else "I18nManager"
                found.append((context, text))
            elif isinstance(func, ast.Attribute) and func.attr == "translate" and len(node.args) > 1 and _is_text(node.args[1]):
                found.append((node.args[0].value, node.args[1].value))
        for child in ast.iter_child_nodes(node):
            visit(child, cls)

    visit(tree, None)
    return [(context, text, path.relative_to(MAIN)) for context, text in found]


def _entries(language):
    root = ET.parse(MAIN / "translations" / f"app_{language}.xml").getroot()
    return {
        (context.findtext("name"), message.findtext("source")): message.findtext("translation") or ""
        for context in root.findall("context")
        for message in context.findall("message")
    }


def _source_files():
    for path in MAIN.rglob("*.py"):
        if not SKIP_DIRS.intersection(path.relative_to(MAIN).parts):
            yield path


def test_every_literal_translation_call_has_an_entry_in_every_language():
    calls = [call for path in _source_files() for call in _calls_in(path)]
    assert len(calls) > 300  # 识别失灵时别悄悄通过
    entries = {language: _entries(language) for language in LANGUAGES}
    missing = sorted({
        f"[{context}] {text!r} @ {path}: {', '.join(lang for lang in LANGUAGES if (context, text) not in entries[lang])}"
        for context, text, path in calls
        if any((context, text) not in entries[lang] for lang in LANGUAGES)
    })
    assert not missing, "翻译文件缺条目：\n" + "\n".join(missing)


def test_translation_files_use_real_line_breaks():
    # 代码里的 "\n" 是真换行，翻译文件写成字面的反斜杠加 n 就对不上，界面会显示原文
    literal = chr(92) + "n"
    bad = [
        f"{language} [{context}] {source[:50]!r}"
        for language in LANGUAGES
        for (context, source), translation in _entries(language).items()
        if (literal in source or literal in translation)
        # 分隔符输入提示中的 \\n 必须按字面显示，译文也须保留一次。
        and not (context == "ClipboardWindow" and source == "Custom, \\n for a new line"
                 and translation.count(literal) == 1)
    ]
    assert not bad, "\n".join(bad)
