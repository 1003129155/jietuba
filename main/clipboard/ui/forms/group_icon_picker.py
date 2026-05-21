# -*- coding: utf-8 -*-

"""分组图标选择器构造与交互辅助。

负责 emoji 图标输入、预览、分组切换和预设按钮区域搭建，
供分组表单复用，不直接参与分组保存逻辑。
"""

from PySide6.QtCore import QTimer, Qt, QPoint
from PySide6.QtWidgets import QGridLayout, QHBoxLayout, QLabel, QPushButton, QScrollArea, QWidget

from qfluentwidgets import CaptionLabel, LineEdit

try:
    from ..resources.emoji_data import get_emoji_groups, get_group_icon
except ImportError:
    from clipboard.ui.resources.emoji_data import get_emoji_groups, get_group_icon


def create_group_icon_picker(dialog, current_icon: str = "📁"):
    """创建分组图标选择器。"""
    input_row = QHBoxLayout()
    input_row.setSpacing(12)

    dialog.icon_input = LineEdit()
    dialog.icon_input.setPlaceholderText(dialog.tr("Enter or paste emoji..."))
    dialog.icon_input.setText(current_icon)
    dialog.icon_input.setMaxLength(4)
    dialog.icon_input.setStyleSheet("font-size: 18px; min-width: 120px; max-width: 150px;")
    dialog.icon_input.textChanged.connect(dialog._on_icon_input_changed)
    input_row.addWidget(dialog.icon_input)

    preview_label = CaptionLabel(dialog.tr("Preview:"))
    input_row.addWidget(preview_label)

    dialog.icon_preview = QLabel(current_icon)
    dialog.icon_preview.setFixedSize(48, 48)
    dialog.icon_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
    dialog.icon_preview.setStyleSheet(
        """
            QLabel {
                font-size: 28px;
                background: #F5F5F5;
                border: 2px solid #E0E0E0;
                border-radius: 8px;
            }
        """
    )
    input_row.addWidget(dialog.icon_preview)
    input_row.addStretch()
    dialog.detail_layout.addLayout(input_row)

    group_order, emoji_groups = get_emoji_groups()

    tab_bar = QHBoxLayout()
    tab_bar.setSpacing(0)
    tab_bar.setContentsMargins(0, 4, 0, 0)
    dialog._emoji_tab_buttons = []
    dialog._emoji_group_order = group_order
    dialog._emoji_groups = emoji_groups

    for idx, group_name in enumerate(group_order):
        icon_char = get_group_icon(group_name)
        btn = QPushButton(icon_char)
        btn.setFixedSize(36, 30)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setToolTip(group_name)
        btn.setCheckable(True)
        btn.setStyleSheet(emoji_tab_style(False))
        btn.clicked.connect(lambda checked, i=idx: dialog._switch_emoji_group(i))
        tab_bar.addWidget(btn)
        dialog._emoji_tab_buttons.append(btn)
    tab_bar.addStretch()
    dialog.detail_layout.addLayout(tab_bar)

    dialog._emoji_scroll = QScrollArea()
    dialog._emoji_scroll.setWidgetResizable(True)
    dialog._emoji_scroll.setMinimumHeight(110)
    dialog._emoji_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    dialog._emoji_scroll.setStyleSheet(
        """
            QScrollArea { border: none; background: transparent; }
            QScrollBar:vertical { width: 6px; background: transparent; }
            QScrollBar::handle:vertical { background: #CCC; border-radius: 3px; min-height: 20px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        """
    )
    dialog.detail_layout.addWidget(dialog._emoji_scroll, 1)

    dialog._emoji_current_idx = 0
    QTimer.singleShot(0, lambda: dialog._switch_emoji_group(0))


def emoji_tab_style(active: bool) -> str:
    """返回 emoji 分组 tab 按钮样式。"""
    if active:
        return """
            QPushButton {
                background: #E3F2FD; border: none;
                border-bottom: 2px solid #1E88E5;
                border-radius: 0px; font-size: 18px;
                padding: 2px 0px;
            }
        """
    return """
        QPushButton {
            background: transparent; border: none;
            border-bottom: 2px solid transparent;
            border-radius: 0px; font-size: 18px;
            padding: 2px 0px;
        }
        QPushButton:hover { background: #F5F5F5; }
    """


def emoji_btn_style() -> str:
    """返回 emoji 网格按钮样式。"""
    return """
        QPushButton {
            background: transparent; border: none;
            border-radius: 4px; font-size: 22px; padding: 0px;
        }
        QPushButton:hover {
            background: #E3F2FD;
        }
        QPushButton:pressed {
            background: #BBDEFB;
        }
    """


def switch_emoji_group(dialog, group_idx: int):
    """切换 emoji 分组。"""
    dialog._emoji_current_idx = group_idx
    for i, btn in enumerate(dialog._emoji_tab_buttons):
        btn.setChecked(i == group_idx)
        btn.setStyleSheet(emoji_tab_style(i == group_idx))

    group_name = dialog._emoji_group_order[group_idx]
    emojis = dialog._emoji_groups[group_name]

    btn_size = 36
    spacing = 2
    avail_w = dialog._emoji_scroll.viewport().width() - 4
    if avail_w < btn_size * 2:
        avail_w = 400
    cols = max(1, avail_w // (btn_size + spacing))

    container = QWidget()
    container.setMaximumWidth(avail_w + 4)
    grid = QGridLayout(container)
    grid.setSpacing(spacing)
    grid.setContentsMargins(2, 2, 2, 2)

    btn_style = emoji_btn_style()
    for i, em in enumerate(emojis):
        btn = QPushButton(em)
        btn.setFixedSize(btn_size, btn_size)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setStyleSheet(btn_style)
        btn.clicked.connect(lambda checked, ic=em: dialog._on_preset_icon_clicked(ic))
        grid.addWidget(btn, i // cols, i % cols)

    grid.setColumnStretch(cols, 1)
    grid.setRowStretch(len(emojis) // cols + 1, 1)

    dialog._emoji_scroll.setWidget(container)


def on_icon_input_changed(dialog, text: str):
    """输入框内容变化时更新预览。"""
    if not text.strip():
        dialog.icon_preview.setText("📁")
        return

    first_char = ""
    for char in text:
        if char.isspace():
            continue
        first_char = char
        break

    if first_char:
        if len(text.strip()) > len(first_char):
            dialog.icon_input.blockSignals(True)
            dialog.icon_input.setText(first_char)
            dialog.icon_input.blockSignals(False)
        dialog.icon_preview.setText(first_char)
    else:
        dialog.icon_preview.setText("📁")


def on_preset_icon_clicked(dialog, icon: str):
    """点击预设图标。"""
    dialog.icon_input.setText(icon)
    dialog.icon_preview.setText(icon)
