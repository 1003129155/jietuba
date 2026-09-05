# -*- coding: utf-8 -*-
"""
全模块可导入性测试

单元测试只覆盖了一部分代码，大量 UI 模块从未被任何测试导入过，
因此"某个模块因为循环依赖或误删导入而根本 import 不进来"这类问题
不会被其它测试发现——直到用户点开对应功能才崩。

这里遍历 main/ 下的所有子模块逐个 import，把这层保护补上。
真实发生过、正是被这个检查拦下的两类问题：
1) canvas 与 tools 互相在模块顶层导入，导致先 import tools 必然失败；
2) 清理未使用导入时误删了被其它模块转发引用的符号。
"""
import importlib
import pkgutil

import pytest

# 这些包在导入时会创建 Qt 对象或访问 Windows API，需要 QApplication 已就绪
PACKAGES = [
    "canvas", "capture", "clipboard", "core", "gif", "ocr",
    "pin", "settings", "stitch", "tools", "translation", "ui",
]


def _iter_submodules():
    names = []
    for pkg_name in PACKAGES:
        pkg = importlib.import_module(pkg_name)
        names.append(pkg_name)
        for info in pkgutil.walk_packages(pkg.__path__, pkg_name + "."):
            names.append(info.name)
    return sorted(set(names))


@pytest.fixture(scope="module")
def qapp():
    from PySide6.QtWidgets import QApplication
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture(scope="module")
def module_names(qapp):
    # canvas 需要先于 tools 完成导入：tools 的各工具模块会反向引用 canvas.items。
    # scene.py 已把 tools 的导入延迟到 __init__ 内部来打破这个环，
    # 这里保持与应用一致的导入顺序。
    importlib.import_module("canvas")
    return _iter_submodules()


def test_every_module_imports_cleanly(module_names):
    """main/ 下的每个模块都应能独立导入而不抛异常"""
    failures = []
    for name in module_names:
        try:
            importlib.import_module(name)
        except Exception as exc:      # noqa: BLE001 - 这里就是要捕获任何导入期异常
            failures.append(f"{name}: {exc!r}")

    assert not failures, "以下模块导入失败：\n" + "\n".join(failures)


def test_module_scan_actually_found_modules(module_names):
    """防止遍历逻辑本身失效导致上面的测试空跑而永远通过"""
    assert len(module_names) > 100


def test_tools_package_imports_without_canvas_preloaded():
    """
    tools 必须能在不依赖"canvas 已被导入"的前提下独立导入。

    这是曾经真实存在的缺陷：canvas.scene 在模块顶层 import tools，
    而 tools 的每个工具又 import canvas.items，形成环；
    程序能跑只是因为恰好总是 canvas 先被导入。
    """
    import subprocess
    import sys
    from pathlib import Path

    main_dir = Path(__file__).resolve().parent.parent
    code = (
        "import sys; sys.path.insert(0, r'%s');"
        "from PySide6.QtWidgets import QApplication; QApplication([]);"
        "import tools; print('OK')" % main_dir
    )
    result = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True, text=True,
        env={**__import__("os").environ, "QT_QPA_PLATFORM": "offscreen"},
    )
    assert "OK" in result.stdout, (
        "独立导入 tools 失败（可能又引入了 canvas ↔ tools 循环依赖）：\n"
        + result.stderr
    )
