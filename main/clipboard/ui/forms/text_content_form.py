# -*- coding: utf-8 -*-

"""文本内容表单构造器。

负责在 ManageDialog 详情区构建普通文本条目的新增和编辑表单，
只创建控件并挂到 dialog 上，不直接处理保存逻辑。
"""

from core.ui_scale import dialog_scaled
from ui.fluent_lite import LineEdit, TextEdit
from .form_widgets import field_label


def build_text_content_form(dialog):
    """构建普通文本内容输入表单。"""
    _build_form(dialog, dialog.tr("Enter title (e.g., Restart Command)..."), "", "")
    dialog.content_edit.setPlaceholderText(dialog.tr("Enter text content to save..."))


def build_edit_text_content_form(dialog, item):
    """构建文本条目的编辑表单。"""
    _build_form(dialog, dialog.tr("Enter title..."), item.title or "", item.content)


def _build_form(dialog, title_placeholder: str, title: str, content: str):
    dialog.detail_layout.addWidget(field_label(dialog.tr("Title (Optional)"), top_gap=False))
    dialog.title_input = LineEdit()
    dialog.title_input.setPlaceholderText(title_placeholder)
    dialog.title_input.setText(title)
    dialog.detail_layout.addWidget(dialog.title_input)

    dialog.detail_layout.addWidget(field_label(dialog.tr("Content")))
    dialog.content_edit = TextEdit()
    dialog.content_edit.setPlainText(content)
    dialog.content_edit.setLineWrapMode(TextEdit.LineWrapMode.NoWrap)
    dialog.content_edit.setMinimumHeight(dialog_scaled(220))
    dialog.detail_layout.addWidget(dialog.content_edit, 1)
