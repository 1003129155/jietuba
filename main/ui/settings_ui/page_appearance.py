# -*- coding: utf-8 -*-
"""外观设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QScrollArea, QColorDialog, QWidgetAction,
)
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor

from qfluentwidgets import (
    SettingCard as FSettingCard, FluentIcon,
    ComboBox, BodyLabel, CaptionLabel, PushButton,
)
from .components import (
    SettingCardGroup, theme_menu_style, theme_color,
    theme_popup_background, theme_border_color,
)

# 统一控件宽度
_CTRL_W = 80
_CTRL_H = 26


def _update_color_btn(btn: QPushButton, color: QColor):
    """更新色块按钮的背景颜色"""
    r, g, b = color.red(), color.green(), color.blue()
    border = theme_color("#C9CDD4", "#4A4F57")
    hover_border = theme_color("#8A9099", "#B8BEC8")
    btn.setStyleSheet(f"""
        QPushButton {{
            background-color: rgb({r}, {g}, {b});
            border: 1px solid {border};
            border-radius: 3px;
        }}
        QPushButton:hover {{
            border: 1px solid {hover_border};
        }}
    """)


def _make_color_btn(dialog, size_w=_CTRL_W, size_h=_CTRL_H):
    """创建统一尺寸的色块按钮"""
    btn = PushButton(dialog)
    btn.setFixedSize(size_w, size_h)
    btn.setCursor(Qt.CursorShape.PointingHandCursor)
    return btn


# ================================================================
# 主题颜色表 — 与 clipboard/setting_panel.py 保持一致
# ================================================================
_THEME_COLORS = [
    ("light",  "#DDE3E9", "#FFFFFF"),
    ("dark",   "#1D1F20", "#1E1E1E"),
    ("blue",   "#2196F3", "#9AD1F8"),
    ("green",  "#4CAF50", "#B8F3BD"),
    ("pink",   "#E91E63", "#FA9BBB"),
    ("purple", "#9C27B0", "#D471E4"),
    ("orange", "#FF9800", "#F5C880"),
]


def create_appearance_page(dialog) -> QWidget:
    """创建外观设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ── 截图外观 ──────────────────────────────────────
    grp_ss = SettingCardGroup(dialog.tr("Screenshot"), view)
    _build_screenshot_section(dialog, grp_ss)
    layout.addWidget(grp_ss)

    # ── 剪贴板外观 ────────────────────────────────────
    grp_clip = SettingCardGroup(dialog.tr("Clipboard"), view)
    _build_clipboard_section(dialog, grp_clip)
    layout.addWidget(grp_clip)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Color changes take effect on the next screenshot."),
        view,
    )
    hint.setStyleSheet("padding: 5px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


# ================================================================
# 截图外观
# ================================================================

def _build_screenshot_section(dialog, grp: SettingCardGroup):
    """截图外观：主题色 + 遮罩色"""
    from core.theme import get_theme
    theme = get_theme()

    # 主题色
    theme_card = FSettingCard(
        FluentIcon.PALETTE,
        dialog.tr("Theme Color"),
        parent=grp,
    )
    dialog._theme_color_btn = _make_color_btn(dialog)
    dialog._appearance_theme_color = QColor(theme.theme_color)
    _update_color_btn(dialog._theme_color_btn, dialog._appearance_theme_color)

    def _pick_theme_color():
        _dlg = QColorDialog(dialog._appearance_theme_color, None)
        _dlg.setWindowTitle(dialog.tr("Select Theme Color"))
        _dlg.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
        if _dlg.exec():
            color = _dlg.selectedColor()
            dialog._appearance_theme_color = color
            _update_color_btn(dialog._theme_color_btn, color)

    dialog._theme_color_btn.clicked.connect(_pick_theme_color)
    theme_card.hBoxLayout.addWidget(
        dialog._theme_color_btn, 0, Qt.AlignmentFlag.AlignRight
    )
    theme_card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(theme_card)

    # 遮罩色
    mask_card = FSettingCard(
        FluentIcon.BRUSH,
        dialog.tr("Mask Color"),
        parent=grp,
    )
    dialog._mask_color_btn = _make_color_btn(dialog)
    mc = theme.mask_color
    dialog._appearance_mask_color = QColor(mc.red(), mc.green(), mc.blue())
    _update_color_btn(dialog._mask_color_btn, dialog._appearance_mask_color)

    def _pick_mask_color():
        _dlg = QColorDialog(dialog._appearance_mask_color, None)
        _dlg.setWindowTitle(dialog.tr("Select Mask Color"))
        _dlg.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
        if _dlg.exec():
            color = _dlg.selectedColor()
            dialog._appearance_mask_color = color
            _update_color_btn(dialog._mask_color_btn, color)

    dialog._mask_color_btn.clicked.connect(_pick_mask_color)
    mask_card.hBoxLayout.addWidget(
        dialog._mask_color_btn, 0, Qt.AlignmentFlag.AlignRight
    )
    mask_card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(mask_card)


# ================================================================
# 剪贴板外观
# ================================================================

def _build_clipboard_section(dialog, grp: SettingCardGroup):
    """剪贴板外观：主题 + 字体大小 + 透明度"""
    from clipboard.themes import get_theme_manager
    from settings import get_tool_settings_manager
    config = get_tool_settings_manager()
    theme_mgr = get_theme_manager()

    # ── 剪贴板主题（颜色块按钮） ──────────────────────
    current_theme_name = config.get_clipboard_theme()

    theme_card = FSettingCard(
        FluentIcon.BRUSH,
        dialog.tr("Theme"),
        parent=grp,
    )
    dialog._clip_theme_btn = PushButton(theme_card)
    dialog._clip_theme_btn.setFixedSize(_CTRL_W, _CTRL_H)
    dialog._clip_theme_btn.setCursor(Qt.CursorShape.PointingHandCursor)
    dialog._clip_theme_name = current_theme_name

    def _apply_theme_btn_style(name: str):
        for tname, accent, bg in _THEME_COLORS:
            if tname == name:
                dialog._clip_theme_btn.setStyleSheet(f"""
                    QPushButton {{
                        background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                            stop:0 {bg}, stop:0.5 {bg},
                            stop:0.5 {accent}, stop:1 {accent});
                        border: 2px solid {accent};
                        border-radius: 3px;
                    }}
                    QPushButton:hover {{ border: 2px solid {theme_color('#333333', '#F3F3F3')}; }}
                """)
                break

    _apply_theme_btn_style(current_theme_name)

    def _show_theme_popup():
        from PySide6.QtWidgets import QMenu
        menu = QMenu(dialog)
        menu.setStyleSheet(theme_menu_style() + " QMenu { padding: 4px; }")
        theme_buttons = []

        def _update_btns(selected: str):
            for n, btn, accent, bg in theme_buttons:
                is_sel = n == selected
                btn.setText("✓" if is_sel else "")
                text_color = "#FFFFFF" if n == "dark" else theme_color("#333333", "#F3F3F3")
                bw = 2 if is_sel else 1
                bc = accent if is_sel else theme_color("#CCCCCC", "#4A4F57")
                btn.setStyleSheet(f"""
                    QPushButton {{
                        background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                            stop:0 {bg}, stop:0.5 {bg},
                            stop:0.5 {accent}, stop:1 {accent});
                        border: {bw}px solid {bc};
                        border-radius: 3px;
                        color: {text_color};
                        font-size: 12px; font-weight: bold;
                        text-align: left; padding-left: 6px;
                    }}
                    QPushButton:hover {{ border: 2px solid {accent}; }}
                """)

        def _on_click(name: str):
            dialog._clip_theme_name = name
            _apply_theme_btn_style(name)
            theme_mgr.set_theme(name)
            _update_btns(name)
            menu.close()

        for tname, accent, bg in _THEME_COLORS:
            wa = QWidgetAction(menu)
            btn = PushButton(menu)
            btn.setFixedSize(_CTRL_W, _CTRL_H)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.clicked.connect(lambda _c, n=tname: _on_click(n))
            wa.setDefaultWidget(btn)
            menu.addAction(wa)
            theme_buttons.append((tname, btn, accent, bg))

        _update_btns(dialog._clip_theme_name)
        pos = dialog._clip_theme_btn.mapToGlobal(
            dialog._clip_theme_btn.rect().bottomLeft()
        )
        menu.popup(pos)

    dialog._clip_theme_btn.clicked.connect(_show_theme_popup)
    theme_card.hBoxLayout.addWidget(
        dialog._clip_theme_btn, 0, Qt.AlignmentFlag.AlignRight
    )
    theme_card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(theme_card)

    # ── 字体大小 ─────────────────────────────────────
    font_card = FSettingCard(
        FluentIcon.FONT_SIZE,
        dialog.tr("Font Size"),
        parent=grp,
    )
    dialog._clip_font_combo = ComboBox(font_card)
    dialog._clip_font_combo.setFixedWidth(_CTRL_W)

    font_options = config.get_clipboard_font_size_options()
    current_font = config.get_clipboard_font_size()
    for size in font_options:
        dialog._clip_font_combo.addItem(f"{size}px", userData=size)
    idx = dialog._clip_font_combo.findData(current_font)
    if idx >= 0:
        dialog._clip_font_combo.setCurrentIndex(idx)

    def _on_font_changed(index):
        size = dialog._clip_font_combo.itemData(index)
        if size is not None:
            config.set_clipboard_font_size(size)
            theme_mgr.notify_font_size_changed(size)

    dialog._clip_font_combo.currentIndexChanged.connect(_on_font_changed)
    font_card.hBoxLayout.addWidget(
        dialog._clip_font_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    font_card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(font_card)

    # ── 透明度 ───────────────────────────────────────
    opacity_card = FSettingCard(
        FluentIcon.TRANSPARENT,
        dialog.tr("Opacity"),
        parent=grp,
    )
    dialog._clip_opacity_combo = ComboBox(opacity_card)
    dialog._clip_opacity_combo.setFixedWidth(_CTRL_W)

    opacity_options = config.get_clipboard_window_opacity_options()
    current_opacity = config.get_clipboard_window_opacity()
    for percent in opacity_options:
        label = dialog.tr("Opaque") if percent == 0 else f"{percent}%"
        dialog._clip_opacity_combo.addItem(label, userData=percent)
    idx = dialog._clip_opacity_combo.findData(current_opacity)
    if idx >= 0:
        dialog._clip_opacity_combo.setCurrentIndex(idx)

    def _on_opacity_changed(index):
        percent = dialog._clip_opacity_combo.itemData(index)
        if percent is not None:
            config.set_clipboard_window_opacity(percent)
            theme_mgr.notify_opacity_changed(percent)

    dialog._clip_opacity_combo.currentIndexChanged.connect(_on_opacity_changed)
    opacity_card.hBoxLayout.addWidget(
        dialog._clip_opacity_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    opacity_card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(opacity_card)
 