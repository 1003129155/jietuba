# -*- coding: utf-8 -*-

from PySide6.QtWidgets import QApplication, QDialog, QWidget

from clipboard.ui.windows.clipboard_window import ClipboardWindow


def _focus_test_window(qapp):
    window = QWidget()
    window.hidden = False
    window.isActiveWindow = lambda: False
    window.hide = lambda: setattr(window, "hidden", True)
    window._owns_window = lambda candidate: ClipboardWindow._owns_window(window, candidate)
    return window


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

    monkeypatch.setattr(QApplication, "activeWindow", staticmethod(lambda: None))
    monkeypatch.setattr(QApplication, "activeModalWidget", staticmethod(lambda: None))
    monkeypatch.setattr(QApplication, "activePopupWidget", staticmethod(lambda: None))

    ClipboardWindow._check_and_hide(window)

    assert window.hidden is True
