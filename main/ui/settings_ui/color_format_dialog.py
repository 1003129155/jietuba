# -*- coding: utf-8 -*-
"""放大镜颜色格式的管理窗口。

勾选决定哪些格式显示在放大镜上，顺序决定按取色键复制的是哪一个——复制的永远是
排在最前的那条，所以「拖到第一位」就是「设为主格式」。内置格式只能取消勾选不能
删除：删掉之后用户在这里就再也找不回来了。

对话框只负责编辑，读写配置由调用方做（和 ToolbarLayoutDialog 一样的分工）。
"""
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QDialog, QHBoxLayout, QVBoxLayout,
)

from core.i18n import make_tr
from core.ui_scale import configure_dialog_control, configure_dialog_controls, dialog_scaled, scale_dialog_font
from settings.color_formats import ColorFormat, PLACEHOLDERS, default_formats, normalize
from ui.dialogs import show_confirm_dialog
from ui.fluent_lite import (
    BodyLabel, CaptionLabel, FluentIcon, LineEdit, PrimaryPushButton, PushButton,
    TransparentToolButton, TransparentPushButton, ui_tokens,
)
from ui.reorderable_rows import DraggableRow, ReorderableRowList

_tr = make_tr("ColorFormatDialog")

# 列表里每条格式都拿它渲染一遍，让用户直接看到复制出去是什么样
SAMPLE_COLOR = QColor(230, 153, 60)


class _Row(DraggableRow):
    """一行：拖动手柄、启用勾选、格式名、渲染样例、编辑与删除。"""

    def __init__(self, fmt: ColorFormat, parent=None):
        super().__init__(parent)
        self.format = fmt

        self.check = QCheckBox(self)
        self.check.setChecked(fmt.enabled)

        self.name_label = BodyLabel(fmt.name, self)
        self.sample_label = CaptionLabel(self)
        self.sample_label.setWordWrap(False)

        self.edit_btn = TransparentToolButton(FluentIcon.EDIT, self)
        self.edit_btn.setToolTip(_tr("Edit"))
        self.delete_btn = TransparentToolButton(FluentIcon.DELETE, self)
        self.delete_btn.setToolTip(_tr("Delete"))
        # 内置格式是这张表的底座，删掉就再也加不回来了，只允许取消勾选
        self.delete_btn.setVisible(not fmt.builtin)

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
        layout.addWidget(self.edit_btn)
        layout.addWidget(self.delete_btn)

        self._refresh_texts()

    def _refresh_texts(self):
        self.name_label.setText(self.format.name)
        self.sample_label.setText(self.format.render(SAMPLE_COLOR))

    def set_format(self, fmt: ColorFormat):
        self.format = fmt
        self._refresh_texts()

    def entry(self) -> ColorFormat:
        return ColorFormat(
            name=self.format.name,
            template=self.format.template,
            enabled=self.check.isChecked(),
            builtin=self.format.builtin,
        )


class _EditDialog(QDialog):
    """新增或编辑一条格式：名称 + 模板，底下列出可用的占位符。"""

    def __init__(self, fmt: ColorFormat | None, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Edit Color Format") if fmt else _tr("Add Color Format"))
        self.setWindowFlags(
            Qt.WindowType.Dialog
            | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")

        self.name_edit = LineEdit(self)
        self.name_edit.setPlaceholderText(_tr("Name shown in the list"))
        self.template_edit = LineEdit(self)
        self.template_edit.setPlaceholderText("rgb({r}, {g}, {b})")
        if fmt is not None:
            self.name_edit.setText(fmt.name)
            self.template_edit.setText(fmt.template)

        self.preview = CaptionLabel(self)
        self.template_edit.textChanged.connect(self._update_preview)

        placeholders = CaptionLabel(
            _tr("Placeholders: ") + "  ".join("{%s}" % name for name in PLACEHOLDERS), self)
        placeholders.setWordWrap(True)

        ok_btn = PrimaryPushButton(_tr("OK"), self)
        ok_btn.clicked.connect(self.accept)
        cancel_btn = PushButton(_tr("Cancel"), self)
        cancel_btn.clicked.connect(self.reject)
        buttons = QHBoxLayout()
        buttons.addStretch(1)
        buttons.addWidget(ok_btn)
        buttons.addWidget(cancel_btn)

        root = QVBoxLayout(self)
        root.setContentsMargins(
            dialog_scaled(16), dialog_scaled(16), dialog_scaled(16), dialog_scaled(14)
        )
        root.setSpacing(dialog_scaled(8))
        root.addWidget(BodyLabel(_tr("Name"), self))
        root.addWidget(self.name_edit)
        root.addWidget(BodyLabel(_tr("Template"), self))
        root.addWidget(self.template_edit)
        root.addWidget(placeholders)
        root.addWidget(self.preview)
        root.addLayout(buttons)

        configure_dialog_controls(self)
        self._update_preview()
        self.resize(dialog_scaled(420), self.sizeHint().height())

    def _update_preview(self):
        sample = ColorFormat("", self.template_edit.text()).render(SAMPLE_COLOR)
        self.preview.setText(_tr("Preview: ") + sample)

    def entry(self, builtin: bool = False) -> ColorFormat:
        name = self.name_edit.text().strip() or self.template_edit.text().strip()
        return ColorFormat(name=name, template=self.template_edit.text().strip(),
                           enabled=True, builtin=builtin)

    def is_valid(self) -> bool:
        return bool(self.template_edit.text().strip())


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

        add_btn = PushButton(_tr("Add Color Format"), self)
        add_btn.clicked.connect(self._add_format)
        reset_btn = TransparentPushButton(_tr("Restore defaults"), self)
        reset_btn.clicked.connect(lambda: self._fill(default_formats()))
        ok_btn = PrimaryPushButton(_tr("OK"), self)
        ok_btn.clicked.connect(self.accept)
        cancel_btn = PushButton(_tr("Cancel"), self)
        cancel_btn.clicked.connect(self.reject)

        buttons = QHBoxLayout()
        buttons.setSpacing(dialog_scaled(8))
        buttons.addWidget(add_btn)
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
            self._add_row(fmt)

    def _add_row(self, fmt: ColorFormat, index: int = -1):
        row = _Row(fmt)
        row.edit_btn.clicked.connect(lambda _checked=False, r=row: self._edit_format(r))
        row.delete_btn.clicked.connect(lambda _checked=False, r=row: self._delete_format(r))
        self._list.add_row(row, index)
        configure_dialog_controls(row)
        return row

    def _add_format(self):
        dialog = _EditDialog(None, self)
        if dialog.exec() and dialog.is_valid():
            self._add_row(dialog.entry())
            self._fit_height_to_rows()

    def _edit_format(self, row):
        dialog = _EditDialog(row.format, self)
        if dialog.exec() and dialog.is_valid():
            # 内置格式改了模板还是内置的：它的位置不能丢，否则删不掉也找不回
            row.set_format(dialog.entry(builtin=row.format.builtin))

    def _delete_format(self, row):
        if not show_confirm_dialog(
            self, _tr("Delete Color Format"),
            _tr("Delete “%1”?").replace("%1", row.format.name),
        ):
            return
        self._list.remove_row(row)
        self._fit_height_to_rows()

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
