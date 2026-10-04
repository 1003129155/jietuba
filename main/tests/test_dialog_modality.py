# -*- coding: utf-8 -*-
"""对话框只锁住所属窗口，截图层、钉图等其他顶层窗口照常可用。

exec() 遇到没设过模态的对话框会用应用模态，挡住整个程序：设置里开着一个取色框，新建的
截图层就收不到输入。所以弹模态对话框统一走 ui.dialogs 的 exec_dialog 和文件对话框函数，
判断某个窗口是否被挡住统一用 core.qt_utils.blocking_modal。
"""
import ast
from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QCoreApplication, QEvent, QPoint, Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QColorDialog, QDialog, QFileDialog, QWidget

from core.qt_utils import blocking_modal
from ui import dialogs
from ui.color_picker_button import ColorPickerButton

MAIN_DIR = Path(__file__).resolve().parents[1]
WINDOW_MODAL = Qt.WindowModality.WindowModal
APP_MODAL = Qt.WindowModality.ApplicationModal

# 这些接收者的 exec() 是事件循环，不是对话框
_EVENT_LOOP_RECEIVERS = {"app", "self.app", "loop", "dlg._loop"}
_STATIC_DIALOG_CALLS = {
    "QFileDialog": {
        "getOpenFileName", "getOpenFileNames", "getSaveFileName", "getExistingDirectory",
        "getOpenFileUrl", "getOpenFileUrls", "getSaveFileUrl", "getExistingDirectoryUrl",
    },
    "QColorDialog": {"getColor"},
    "QFontDialog": {"getFont"},
    "QInputDialog": {"getText", "getMultiLineText", "getInt", "getDouble", "getItem"},
    "QMessageBox": {"information", "warning", "critical", "question", "about"},
}


def _modal_calls(tree):
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
            continue
        receiver, name = ast.unparse(node.func.value), node.func.attr
        if name == "exec" and not node.args and receiver not in _EVENT_LOOP_RECEIVERS:
            yield node.lineno, f"{receiver}.exec()"
        elif name in _STATIC_DIALOG_CALLS.get(receiver, ()):
            yield node.lineno, f"{receiver}.{name}()"


def test_scan_catches_application_modal_calls():
    source = (
        "QFileDialog.getSaveFileName(None)\n"
        "QColorDialog.getColor()\n"
        "editor.exec()\n"
        "loop.exec()\n"
        "menu.exec(pos)\n"
        "dialogs.exec_dialog(editor)\n"
    )
    assert [call for _line, call in _modal_calls(ast.parse(source))] == [
        "QFileDialog.getSaveFileName()", "QColorDialog.getColor()", "editor.exec()",
    ]


def _source_trees():
    for path in sorted(MAIN_DIR.rglob("*.py")):
        rel = path.relative_to(MAIN_DIR)
        if rel.parts[0] == "tests" or "site-packages" in rel.parts:
            continue
        yield rel, ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))


def test_modal_dialogs_go_through_window_modal_helpers():
    found = [f"{rel}:{line} {call}" for rel, tree in _source_trees() if rel != Path("ui/dialogs.py")
             for line, call in _modal_calls(tree)]
    assert found == [], "弹模态对话框请用 ui.dialogs 的 exec_dialog 或 get_* 文件对话框函数"


def _application_modal_calls(tree):
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)):
            continue
        args = " ".join(ast.unparse(arg) for arg in node.args)
        if (node.func.attr == "setWindowModality" and "ApplicationModal" in args
                or node.func.attr == "setModal" and args != "False"):
            yield node.lineno, ast.unparse(node)


def test_scan_catches_application_modality():
    source = (
        "dlg.setWindowModality(Qt.WindowModality.ApplicationModal)\n"
        "dlg.setModal(True)\n"
        "dlg.setWindowModality(Qt.WindowModality.WindowModal)\n"
        "dlg.setModal(False)\n"
    )
    assert [line for line, _call in _application_modal_calls(ast.parse(source))] == [1, 2]


def test_only_welcome_and_update_handoff_are_application_modal():
    """向导和已暂停任务的更新交接可用应用模态；普通对话框只锁父窗口。"""
    allowed = {Path("ui/welcome/wizard.py"), Path("ui/update_dialog.py")}
    found = [f"{rel}:{line} {call}" for rel, tree in _source_trees() if rel not in allowed
             for line, call in _application_modal_calls(tree)]
    assert found == [], "应用模态会挡住截图层；确实需要时，像欢迎向导那样在前后暂停、恢复快速截图"


class _Probe(QWidget):
    def __init__(self, parent=None, flags=Qt.WindowType.Window):
        super().__init__(parent, flags)
        self.presses = 0
        self.resize(120, 80)

    def mousePressEvent(self, event):
        self.presses += 1


def _qt_blocks(widget) -> bool:
    """经由窗口系统事件点一下，看 Qt 的模态拦截有没有把它挡掉。"""
    widget.presses = 0
    # 点在右下角，避开放在左上角的子控件
    QTest.mouseClick(widget.windowHandle(), Qt.MouseButton.LeftButton, pos=QPoint(100, 60))
    return widget.presses == 0


@pytest.fixture
def windows(qapp):
    host = _Probe()
    panel = _Probe(host, Qt.WindowType.Tool)
    other = _Probe()
    created = [host, panel, other]
    for window in created:
        window.show()
    qapp.processEvents()
    yield SimpleNamespace(host=host, panel=panel, other=other, created=created)
    for window in reversed(created):
        window.hide()
        window.deleteLater()


def _window_modal_on_host(w):
    return QDialog(w.host)


def _window_modal_on_panel(w):
    return QDialog(w.panel)


def _window_modal_owned_by_host(w):
    dialog = QDialog()
    dialog.winId()
    dialog.windowHandle().setTransientParent(w.host.windowHandle())
    return dialog


def _application_modal(w):
    dialog = QDialog(w.host)
    dialog.setWindowModality(APP_MODAL)
    return dialog


@pytest.mark.parametrize("make_modal", [
    _window_modal_on_host, _window_modal_on_panel, _window_modal_owned_by_host, _application_modal,
])
def test_blocking_modal_matches_qt_input_blocking(windows, qapp, make_modal):
    modal = make_modal(windows)
    windows.created.append(modal)
    if modal.windowModality() == Qt.WindowModality.NonModal:
        modal.setWindowModality(WINDOW_MODAL)
    modal.show()
    qapp.processEvents()

    for window in (windows.host, windows.panel, windows.other):
        assert (blocking_modal(window) is modal) == _qt_blocks(window), window
    assert blocking_modal(modal) is None
    application_modal = modal.windowModality() == APP_MODAL
    assert (blocking_modal() is modal) == application_modal
    # 截图层这类无父窗口只会被应用模态挡住
    assert _qt_blocks(windows.other) == application_modal


def test_exec_dialog_locks_only_the_owner(windows):
    dialog = QDialog(windows.host)
    seen = {}

    def inspect():
        seen.update(modality=dialog.windowModality(),
                    host=_qt_blocks(windows.host), other=_qt_blocks(windows.other))
        dialog.accept()

    QTimer.singleShot(0, inspect)
    assert dialogs.exec_dialog(dialog) == QDialog.DialogCode.Accepted
    assert seen == {"modality": WINDOW_MODAL, "host": True, "other": False}
    dialog.deleteLater()


def test_exec_dialog_owner_locks_window_without_parenting(windows):
    dialog = QColorDialog()
    seen = {}

    def inspect():
        seen.update(parent=dialog.parentWidget(),
                    host=_qt_blocks(windows.host), other=_qt_blocks(windows.other))
        dialog.reject()

    QTimer.singleShot(0, inspect)
    dialogs.exec_dialog(dialog, owner=windows.panel)
    assert seen == {"parent": None, "host": True, "other": False}


def test_exec_dialog_keeps_an_explicit_application_modality(windows):
    dialog = QDialog()
    dialog.setWindowModality(APP_MODAL)
    seen = {}

    def inspect():
        seen.update(modality=dialog.windowModality(), other=_qt_blocks(windows.other))
        dialog.reject()

    QTimer.singleShot(0, inspect)
    dialogs.exec_dialog(dialog)
    assert seen == {"modality": APP_MODAL, "other": True}


def test_save_file_dialog_is_window_modal_and_freed(windows, tmp_path):
    target = tmp_path / "out.png"
    seen = {}

    def answer():
        dialog = QApplication.activeModalWidget()
        seen.update(type=type(dialog), modality=dialog.windowModality(),
                    parent=dialog.parentWidget(), mode=dialog.acceptMode())
        dialog.selectFile(str(target))
        dialog.accept()

    QTimer.singleShot(0, answer)
    path, name_filter = dialogs.get_save_file_name(windows.host, "Save", "", "PNG (*.png);;JPG (*.jpg)")
    assert Path(path) == target
    assert name_filter == "PNG (*.png)"
    assert seen == {"type": QFileDialog, "modality": WINDOW_MODAL, "parent": windows.host,
                    "mode": QFileDialog.AcceptMode.AcceptSave}
    assert windows.host.findChildren(QFileDialog) == []


@pytest.mark.parametrize("ask, mode", [
    (lambda parent: dialogs.get_open_file_name(parent, "Open"), QFileDialog.FileMode.ExistingFile),
    (lambda parent: dialogs.get_existing_directory(parent, "Folder"), QFileDialog.FileMode.Directory),
])
def test_cancelled_file_dialog_returns_empty_and_is_freed(windows, ask, mode):
    seen = {}

    def cancel():
        dialog = QApplication.activeModalWidget()
        seen.update(modality=dialog.windowModality(), mode=dialog.fileMode(),
                    dirs_only=dialog.testOption(QFileDialog.Option.ShowDirsOnly))
        dialog.reject()

    QTimer.singleShot(0, cancel)
    result = ask(windows.host)
    assert result in ("", ("", ""))
    assert seen == {"modality": WINDOW_MODAL, "mode": mode,
                    "dirs_only": mode == QFileDialog.FileMode.Directory}
    assert windows.host.findChildren(QFileDialog) == []


def test_file_dialog_returns_nothing_when_its_parent_closes(qapp):
    host = _Probe()
    host.show()

    def close_host():
        host.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)

    QTimer.singleShot(0, close_host)
    assert dialogs.get_open_file_name(host, "Open") == ("", "")


def test_color_picker_locks_its_window_and_reports_the_pick(windows):
    button = ColorPickerButton(QColor("red"), parent=windows.panel)
    picked = []
    button.color_changed.connect(picked.append)
    seen = {}

    def answer():
        dialog = QApplication.activeModalWidget()
        seen.update(panel=_qt_blocks(windows.panel), other=_qt_blocks(windows.other),
                    found=blocking_modal(windows.panel) is dialog)
        dialog.setCurrentColor(QColor("blue"))
        dialog.accept()

    QTimer.singleShot(0, answer)
    button._pick_color()
    assert seen == {"panel": True, "other": False, "found": True}
    assert picked == [QColor("blue")]


def test_color_picker_survives_its_window_closing_mid_pick(qapp):
    """钉图、GIF 的面板是独立窗口，取色期间宿主被关掉，按钮会随面板删除"""
    panel = _Probe()
    button = ColorPickerButton(QColor("red"), parent=panel)
    panel.show()
    picked = []
    button.color_changed.connect(picked.append)

    def close_panel_then_pick():
        dialog = QApplication.activeModalWidget()
        panel.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        dialog.setCurrentColor(QColor("blue"))
        dialog.accept()

    QTimer.singleShot(0, close_panel_then_pick)
    button._pick_color()
    assert picked == []


def test_screenshot_keys_yield_only_to_dialogs_locking_the_screenshot(windows, qapp):
    from ui.screenshot_window import ScreenshotShortcutHandler

    screenshot_window = windows.other
    screenshot_window._is_closing = False
    handler = ScreenshotShortcutHandler.__new__(ScreenshotShortcutHandler)
    handler._window = screenshot_window
    unrelated = QDialog(windows.host)
    toolbar_dialog = QDialog(screenshot_window)
    for dialog in (unrelated, toolbar_dialog):
        windows.created.append(dialog)
        dialog.setWindowModality(WINDOW_MODAL)

    unrelated.show()
    qapp.processEvents()
    assert handler.is_active()
    toolbar_dialog.show()
    qapp.processEvents()
    assert not handler.is_active()


def test_screenshot_guard_only_stops_for_application_modal(windows, qapp):
    from main_app import MainApp

    dialog = QDialog(windows.host)
    windows.created.append(dialog)
    dialog.setWindowModality(WINDOW_MODAL)
    dialog.show()
    qapp.processEvents()
    assert MainApp._activate_blocking_modal(None) is False

    dialog.hide()
    dialog.setWindowModality(APP_MODAL)
    dialog.show()
    qapp.processEvents()
    assert MainApp._activate_blocking_modal(None) is True


def test_new_screenshot_closes_only_dialogs_left_on_the_screenshot_window(windows, qapp, monkeypatch):
    from main_app import MainApp

    screenshot_window = windows.panel
    left_over = QDialog(screenshot_window)
    unrelated = QDialog(windows.other)
    for dialog in (left_over, unrelated):
        windows.created.append(dialog)
        dialog.setWindowModality(WINDOW_MODAL)
        dialog.show()
    qapp.processEvents()
    monkeypatch.setattr(QTimer, "singleShot", lambda *_args: None)
    app = SimpleNamespace(
        quick_capture=SimpleNamespace(busy=False, set_capture_pending=lambda _pending: None),
        screenshot_window=screenshot_window, clipboard_window=None, _capture_pending=False,
        _capture_and_prepare_window=lambda: None,
    )
    app._activate_blocking_modal = lambda: MainApp._activate_blocking_modal(app)

    MainApp.start_screenshot(app)

    assert app._capture_pending
    assert not left_over.isVisible()
    assert unrelated.isVisible()
