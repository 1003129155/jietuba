# -*- coding: utf-8 -*-

from PySide6.QtWidgets import QApplication, QDialog, QWidget

from clipboard.ui.windows.clipboard_window import ClipboardWindow


class _Config:
    """只提供 _check_and_hide 会读到的那一项设置。"""

    def __init__(self, close_after_paste=True):
        self._close_after_paste = close_after_paste

    def get_clipboard_close_after_paste(self):
        return self._close_after_paste


def _focus_test_window(qapp, close_after_paste=True):
    window = QWidget()
    window.hidden = False
    window.config = _Config(close_after_paste)
    window.isActiveWindow = lambda: False
    window.hide = lambda: setattr(window, "hidden", True)
    window._owns_window = lambda candidate: ClipboardWindow._owns_window(window, candidate)
    return window


def _focus_moves_outside(monkeypatch):
    monkeypatch.setattr(QApplication, "activeWindow", staticmethod(lambda: None))
    monkeypatch.setattr(QApplication, "activeModalWidget", staticmethod(lambda: None))
    monkeypatch.setattr(QApplication, "activePopupWidget", staticmethod(lambda: None))


def test_deactivation_does_not_hide_for_owned_confirmation_dialog(monkeypatch, qapp):
    window = _focus_test_window(qapp)
    dialog = QDialog(window)

    monkeypatch.setattr(QApplication, "activeWindow", staticmethod(lambda: dialog))
    monkeypatch.setattr(QApplication, "activeModalWidget", staticmethod(lambda: dialog))
    monkeypatch.setattr(QApplication, "activePopupWidget", staticmethod(lambda: None))

    ClipboardWindow._check_and_hide(window)

    assert window.hidden is False


def test_deactivation_hides_when_focus_moves_outside_owned_windows(monkeypatch, qapp):
    window = _focus_test_window(qapp)
    _focus_moves_outside(monkeypatch)

    ClipboardWindow._check_and_hide(window)

    assert window.hidden is True


def test_persistent_window_stays_visible_when_focus_moves_away(monkeypatch, qapp):
    """关掉"粘贴后关闭"时窗口常驻——粘贴本身就会把焦点交给目标窗口。"""
    window = _focus_test_window(qapp, close_after_paste=False)
    _focus_moves_outside(monkeypatch)

    ClipboardWindow._check_and_hide(window)

    assert window.hidden is False
