# -*- coding: utf-8 -*-

"""文本内容表单构造器。

负责在 ManageDialog 详情区构建普通文本条目的新增和编辑表单，
只创建控件并挂到 dialog 上，不直接处理保存逻辑。
"""

from PySide6.QtWidgets import QTextEdit

from qfluentwidgets import BodyLabel, LineEdit


def build_text_content_form(dialog):
    """构建普通文本内容输入表单。"""
    title_label = BodyLabel(dialog.tr("Title"))
    dialog.detail_layout.addWidget(title_label)

    dialog.title_input = LineEdit()
    dialog.title_input.setPlaceholderText(dialog.tr("Enter title (e.g., Restart Command)..."))
    dialog.detail_layout.addWidget(dialog.title_input)

    content_label = BodyLabel(dialog.tr("Content"))
    dialog.detail_layout.addWidget(content_label)

    dialog.content_edit = QTextEdit()
    dialog.content_edit.setPlaceholderText(dialog.tr("Enter text content to save..."))
    dialog.content_edit.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
    dialog.content_edit.setMinimumHeight(180)
    dialog.detail_layout.addWidget(dialog.content_edit, 1)


def build_edit_text_content_form(dialog, item):
    """构建文本条目的编辑表单。"""
    title_label = BodyLabel(dialog.tr("Title"))
    dialog.detail_layout.addWidget(title_label)

    dialog.title_input = LineEdit()
    dialog.title_input.setText(item.title or "")
    dialog.title_input.setPlaceholderText(dialog.tr("Enter title..."))
    dialog.detail_layout.addWidget(dialog.title_input)

    content_label = BodyLabel(dialog.tr("Content"))
    dialog.detail_layout.addWidget(content_label)

    dialog.content_edit = QTextEdit()
    dialog.content_edit.setText(item.content)
    dialog.content_edit.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)
    dialog.content_edit.setMinimumHeight(180)
    dialog.detail_layout.addWidget(dialog.content_edit, 1)
