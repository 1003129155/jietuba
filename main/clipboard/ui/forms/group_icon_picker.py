# -*- coding: utf-8 -*-

"""分组图标选择器构造与交互辅助。

负责 emoji 图标输入、预览、分组切换和预设按钮区域搭建，
供分组表单复用，不直接参与分组保存逻辑。
"""

from PySide6.QtCore import QTimer, Qt, QPoint
from PySide6.QtWidgets import QGridLayout, QHBoxLayout, QLabel, QPushButton, QScrollArea, QWidget

from ui.fluent_lite import CaptionLabel, LineEdit
from ui.fluent_lite.theme import ACCENT, ACCENT_SOFT, ACCENT_SUBTLE
from ..layout_scale import scale_ui, scale_x, scale_y

try:
    from ..resources.emoji_data import get_emoji_groups, get_group_icon
except ImportError:
    from clipboard.ui.resources.emoji_data import get_emoji_groups, get_group_icon


def create_group_icon_picker(dialog, current_icon: str = "📁"):
    """创建分组图标选择器。"""
    form_token = getattr(dialog, "_detail_form_token", None)
    input_row = QHBoxLayout()
    input_row.setSpacing(scale_x(12))

    dialog.icon_input = LineEdit()
    dialog.icon_input.setPlaceholderText(dialog.tr("Enter or paste emoji..."))
    dialog.icon_input.setText(current_icon)
    dialog.icon_input.setMaxLength(4)
    dialog.icon_input.setStyleSheet(
        f"font-size: {scale_ui(18)}px; min-width: {scale_x(120)}px; max-width: {scale_x(150)}px;"
    )
    dialog.icon_input.textChanged.connect(lambda text, token=form_token: dialog._on_icon_input_changed(text, token))
    input_row.addWidget(dialog.icon_input)

    preview_label = CaptionLabel(dialog.tr("Preview:"))
    input_row.addWidget(preview_label)

    dialog.icon_preview = QLabel(current_icon)
    dialog.icon_preview.setFixedSize(scale_ui(48), scale_ui(48))
    dialog.icon_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
    dialog.icon_preview.setStyleSheet(
        f"""
            QLabel {{
                font-size: {scale_ui(28)}px;
                background: #F5F5F5;
                border: 2px solid #E0E0E0;
                border-radius: {scale_ui(8)}px;
            }}
        """
    )
    input_row.addWidget(dialog.icon_preview)
    input_row.addStretch()
    dialog.detail_layout.addLayout(input_row)

    group_order, emoji_groups = get_emoji_groups()

    tab_bar = QHBoxLayout()
    tab_bar.setSpacing(0)
    tab_bar.setContentsMargins(0, scale_y(4), 0, 0)
    dialog._emoji_tab_buttons = []
    dialog._emoji_group_order = group_order
    dialog._emoji_groups = emoji_groups

    for idx, group_name in enumerate(group_order):
        icon_char = get_group_icon(group_name)
        btn = QPushButton(icon_char)
        btn.setFixedSize(scale_x(36), scale_y(30))
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setCheckable(True)
        btn.setStyleSheet(emoji_tab_style(False))
        btn.clicked.connect(lambda checked, i=idx, token=form_token: dialog._switch_emoji_group(i, token))
        tab_bar.addWidget(btn)
        dialog._emoji_tab_buttons.append(btn)
    tab_bar.addStretch()
    dialog.detail_layout.addLayout(tab_bar)

    dialog._emoji_scroll = QScrollArea()
    dialog._emoji_scroll.setWidgetResizable(True)
    dialog._emoji_scroll.setMinimumHeight(scale_y(110))
    dialog._emoji_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    dialog._emoji_scroll.setStyleSheet(
        f"""
            QScrollArea {{ border: none; background: transparent; }}
            QScrollBar:vertical {{ width: {scale_x(6)}px; background: transparent; }}
            QScrollBar::handle:vertical {{ background: #CCC; border-radius: {scale_ui(3)}px; min-height: {scale_y(20)}px; }}
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0px; }}
        """
    )
    dialog.detail_layout.addWidget(dialog._emoji_scroll, 1)

    dialog._emoji_current_idx = 0
    QTimer.singleShot(0, lambda token=form_token: dialog._switch_emoji_group(0, token))


def emoji_tab_style(active: bool) -> str:
    """返回 emoji 分组 tab 按钮样式。"""
    if active:
        return f"""
            QPushButton {{
                background: {ACCENT_SOFT}; border: none;
                border-bottom: 2px solid {ACCENT};
                border-radius: 0px; font-size: {scale_ui(18)}px;
                padding: {scale_y(2)}px 0px;
            }}
        """
    return f"""
        QPushButton {{
            background: transparent; border: none;
            border-bottom: 2px solid transparent;
            border-radius: 0px; font-size: {scale_ui(18)}px;
            padding: {scale_y(2)}px 0px;
        }}
        QPushButton:hover {{ background: #F5F5F5; }}
    """


def emoji_btn_style() -> str:
    """返回 emoji 网格按钮样式。"""
    return f"""
        QPushButton {{
            background: transparent; border: none;
            border-radius: {scale_ui(4)}px; font-size: {scale_ui(22)}px; padding: 0px;
        }}
        QPushButton:hover {{
            background: {ACCENT_SOFT};
        }}
        QPushButton:pressed {{
            background: {ACCENT_SUBTLE};
        }}
    """


def switch_emoji_group(dialog, group_idx: int):
    """切换 emoji 分组。"""
    dialog._emoji_current_idx = group_idx
    for i, btn in enumerate(dialog._emoji_tab_buttons):
        btn.setChecked(i == group_idx)
        btn.setStyleSheet(emoji_tab_style(i == group_idx))

    group_name = dialog._emoji_group_order[group_idx]
    emojis = dialog._emoji_groups[group_name]

    btn_size = scale_ui(36)
    spacing = scale_ui(2)
    avail_w = dialog._emoji_scroll.viewport().width() - scale_x(4)
    if avail_w < btn_size * 2:
        avail_w = scale_x(400)
    cols = max(1, avail_w // (btn_size + spacing))

    container = QWidget()
    container.setMaximumWidth(avail_w + scale_x(4))
    grid = QGridLayout(container)
    grid.setSpacing(spacing)
    grid.setContentsMargins(scale_ui(2), scale_ui(2), scale_ui(2), scale_ui(2))

    btn_style = emoji_btn_style()
    form_token = getattr(dialog, "_detail_form_token", None)
    for i, em in enumerate(emojis):
        btn = QPushButton(em)
        btn.setFixedSize(btn_size, btn_size)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setStyleSheet(btn_style)
        btn.clicked.connect(lambda checked, ic=em, token=form_token: dialog._on_preset_icon_clicked(ic, token))
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
