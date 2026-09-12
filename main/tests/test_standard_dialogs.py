# -*- coding: utf-8 -*-

from PySide6.QtWidgets import QDialog, QDialogButtonBox, QWidget

from ui.dialogs import StandardDialog, show_confirm_dialog, show_custom_confirm_dialog


def _close_from_title_bar(dialog):
    dialog.reject()
    return QDialog.DialogCode.Rejected


def test_confirm_dialog_title_bar_close_means_no(monkeypatch, qapp):
    monkeypatch.setattr(StandardDialog, "exec", _close_from_title_bar)
    parent = QWidget()

    result = show_confirm_dialog(parent, "Delete", "Delete this item?")

    assert result is False


def test_custom_dialog_title_bar_close_uses_reject_action(monkeypatch, qapp):
    monkeypatch.setattr(StandardDialog, "exec", _close_from_title_bar)
    parent = QWidget()
    buttons = [
        {
            "id": "save",
            "text": "Save",
            "role": QDialogButtonBox.ButtonRole.AcceptRole,
        },
        {
            "id": "cancel",
            "text": "Cancel",
            "role": QDialogButtonBox.ButtonRole.RejectRole,
        },
    ]

    result = show_custom_confirm_dialog(parent, "Save", "Save changes?", buttons)

    assert result == "cancel"
