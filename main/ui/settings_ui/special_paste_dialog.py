# -*- coding: utf-8 -*-
"""编辑文本和文件特殊粘贴菜单的顺序与开关。"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication, QCheckBox, QDialog, QHBoxLayout, QStackedWidget, QVBoxLayout

from core.i18n import make_tr
from core.ui_scale import configure_dialog_controls, dialog_scaled, scale_dialog_font
from settings import special_paste
from ui.fluent_lite import (
    BodyLabel, CaptionLabel, PrimaryPushButton, PushButton, SegmentedWidget, TransparentPushButton,
    scrollbar_qss, ui_tokens,
)
from ui.reorderable_rows import DraggableRow, ReorderableRowList

_tr = make_tr("SpecialPasteDialog")
_menu_tr = make_tr("ClipboardWindow")


class _Row(DraggableRow):
    def __init__(self, option, enabled, parent=None):
        super().__init__(parent)
        self.key = option.key
        self.check = QCheckBox(self)
        self.check.setChecked(enabled)
        self.check.setAccessibleName(_menu_tr(option.label))

        name = BodyLabel(_menu_tr(option.label), self)
        description = CaptionLabel(_tr(option.description), self)
        column = QVBoxLayout()
        column.setSpacing(dialog_scaled(2))
        column.addWidget(name)
        column.addWidget(description)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(dialog_scaled(8), dialog_scaled(6), dialog_scaled(8), dialog_scaled(6))
        layout.setSpacing(dialog_scaled(10))
        layout.addWidget(self.grip)
        layout.addWidget(self.check)
        layout.addLayout(column, 1)


class SpecialPasteDialog(QDialog):
    def __init__(self, entries, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Manage Special Paste"))
        self.setWindowFlags(
            Qt.WindowType.Dialog | Qt.WindowType.WindowTitleHint | Qt.WindowType.WindowCloseButtonHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")

        self.tabs = SegmentedWidget(self)
        self.tabs.addItem("text", _tr("Text"))
        self.tabs.addItem("file", _tr("Files"))
        self._stack = QStackedWidget(self)
        self._lists = {kind: ReorderableRowList(self) for kind in ("text", "file")}
        for row_list in self._lists.values():
            row_list.setStyleSheet(row_list.styleSheet() + scrollbar_qss(self))
            self._stack.addWidget(row_list)
        self.tabs.currentItemChanged.connect(
            lambda kind: self._stack.setCurrentWidget(self._lists[kind])
        )
        self.tabs.setCurrentItem("text")

        hint = CaptionLabel(
            _tr("Check the items to show in the menu. Drag the handle to reorder them. "
                "Uncheck all items to hide Special Paste for that type."), self
        )
        self.reset_button = TransparentPushButton(_tr("Restore defaults"), self)
        self.reset_button.clicked.connect(lambda: self._fill(special_paste.default_entries()))
        ok_button = PrimaryPushButton(_tr("OK"), self)
        ok_button.clicked.connect(self.accept)
        cancel_button = PushButton(_tr("Cancel"), self)
        cancel_button.clicked.connect(self.reject)

        buttons = QHBoxLayout()
        buttons.setSpacing(dialog_scaled(8))
        buttons.addWidget(self.reset_button)
        buttons.addStretch(1)
        buttons.addWidget(ok_button)
        buttons.addWidget(cancel_button)

        root = QVBoxLayout(self)
        root.setContentsMargins(dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14))
        root.setSpacing(dialog_scaled(10))
        root.addWidget(self.tabs)
        root.addWidget(self._stack, 1)
        root.addWidget(hint)
        root.addLayout(buttons)

        self._fill(entries)
        configure_dialog_controls(self)
        screen = self.parentWidget().screen() if self.parentWidget() else QApplication.primaryScreen()
        height = dialog_scaled(760)
        if screen is not None:
            height = min(height, screen.availableGeometry().height() - dialog_scaled(40))
        self.resize(dialog_scaled(560), max(dialog_scaled(360), height))

    def entries(self):
        return special_paste.normalize([
            (row.key, row.check.isChecked())
            for row_list in self._lists.values() for row in row_list.rows()
        ])

    def _fill(self, entries):
        for row_list in self._lists.values():
            row_list.clear()
        for key, enabled in special_paste.normalize(entries):
            option = special_paste.OPTIONS_BY_KEY[key]
            row_list = self._lists[option.content_type]
            row = _Row(option, enabled)
            row_list.add_row(row)
            configure_dialog_controls(row)
