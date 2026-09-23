# -*- coding: utf-8 -*-
"""放大镜颜色格式的管理窗口。

格式只有固定的几种预设。勾选决定哪些格式显示在放大镜上，顺序决定按取色键复制的
是哪一个——复制的永远是排在最前的那条，所以「拖到第一位」就是「设为主格式」。

对话框只负责编辑，读写配置由调用方做（和 ToolbarLayoutDialog 一样的分工）。
"""
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QDialog, QHBoxLayout, QVBoxLayout,
)

from core.i18n import make_tr
from core.ui_scale import configure_dialog_control, configure_dialog_controls, dialog_scaled, scale_dialog_font
from settings.color_formats import ColorFormat, default_formats, normalize
from ui.fluent_lite import (
    BodyLabel, CaptionLabel, PrimaryPushButton, PushButton, TransparentPushButton, ui_tokens,
)
from ui.reorderable_rows import DraggableRow, ReorderableRowList

_tr = make_tr("ColorFormatDialog")

# 列表里每条格式都拿它渲染一遍，让用户直接看到复制出去是什么样
SAMPLE_COLOR = QColor(230, 153, 60)


class _Row(DraggableRow):
    """一行：拖动手柄、启用勾选、格式名、渲染样例。"""

    def __init__(self, fmt: ColorFormat, parent=None):
        super().__init__(parent)
        self.format = fmt

        self.check = QCheckBox(self)
        self.check.setChecked(fmt.enabled)

        self.name_label = BodyLabel(fmt.name, self)
        self.sample_label = CaptionLabel(self)
        self.sample_label.setWordWrap(False)

        text_column = QVBoxLayout()
        text_column.setContentsMargins(0, 0, 0, 0)
        text_column.setSpacing(dialog_scaled(2))
        text_column.addWidget(self.name_label)
        text_column.addWidget(self.sample_label)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(
            dialog_scaled(8), dialog_scaled(6), dialog_scaled(8), dialog_scaled(6)
        )
        layout.setSpacing(dialog_scaled(10))
        layout.addWidget(self.grip)
        layout.addWidget(self.check)
        layout.addLayout(text_column, 1)

        self.sample_label.setText(fmt.render(SAMPLE_COLOR))

    def entry(self) -> ColorFormat:
        return ColorFormat(
            name=self.format.name,
            template=self.format.template,
            enabled=self.check.isChecked(),
        )


class ColorFormatDialog(QDialog):
    """编辑放大镜的颜色格式列表。formats 为 [ColorFormat]。"""

    def __init__(self, formats, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Manage Color Formats"))
        self.setWindowFlags(
            Qt.WindowType.Dialog
            | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")

        self._list = ReorderableRowList(self)

        hint = CaptionLabel(
            _tr("Checked formats show on the magnifier. The first one is what the "
                "copy shortcut puts on the clipboard — drag it to the top to change that."),
            self)
        hint.setWordWrap(True)
        configure_dialog_control(hint)

        reset_btn = TransparentPushButton(_tr("Restore defaults"), self)
        reset_btn.clicked.connect(lambda: self._fill(default_formats()))
        ok_btn = PrimaryPushButton(_tr("OK"), self)
        ok_btn.clicked.connect(self.accept)
        cancel_btn = PushButton(_tr("Cancel"), self)
        cancel_btn.clicked.connect(self.reject)

        buttons = QHBoxLayout()
        buttons.setSpacing(dialog_scaled(8))
        buttons.addWidget(reset_btn)
        buttons.addStretch(1)
        buttons.addWidget(ok_btn)
        buttons.addWidget(cancel_btn)

        root = QVBoxLayout(self)
        root.setContentsMargins(
            dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14)
        )
        root.setSpacing(dialog_scaled(10))
        root.addWidget(self._list, 1)
        root.addWidget(hint)
        root.addLayout(buttons)

        configure_dialog_controls(self)
        self._fill(formats)
        self._fit_height_to_rows()

    def entries(self):
        """编辑结果，顺序就是列表里的顺序。"""
        return normalize([row.entry() for row in self._list.rows()])

    def _fill(self, formats):
        self._list.clear()
        for fmt in normalize(list(formats)):
            row = _Row(fmt)
            self._list.add_row(row)
            configure_dialog_controls(row)

    def _fit_height_to_rows(self):
        """优先完整展示所有格式；只有超过屏幕可用高度时才让列表滚动。"""
        content_height = self._list.content_height()
        self._list.setFixedHeight(content_height)

        desired_height = self.sizeHint().height()
        screen = self.parentWidget().screen() if self.parentWidget() else QApplication.primaryScreen()
        if screen is not None:
            max_height = max(dialog_scaled(360), screen.availableGeometry().height() - dialog_scaled(40))
            if desired_height > max_height:
                non_list_height = desired_height - content_height
                content_height = max(dialog_scaled(180), max_height - non_list_height)
                self._list.setFixedHeight(content_height)
                desired_height = non_list_height + content_height

        self.resize(dialog_scaled(520), desired_height)
