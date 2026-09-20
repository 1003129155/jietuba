# -*- coding: utf-8 -*-
"""外观设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QPushButton,
    QScrollArea, QColorDialog, QWidgetAction,
)
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor

from core.ui_scale import configure_dialog_controls, dialog_scaled
from ui.fluent_lite import (
    SettingCard as FSettingCard, FluentIcon,
    ComboBox, CaptionLabel, PushButton,
)
from .components import (
    SettingCardGroup, theme_menu_style, theme_color,
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
            border-radius: {dialog_scaled(3)}px;
        }}
        QPushButton:hover {{
            border: 1px solid {hover_border};
        }}
    """)


def _make_color_btn(dialog, size_w=None, size_h=None):
    """创建统一尺寸的色块按钮"""
    btn = PushButton(dialog)
    btn.setFixedSize(
        dialog_scaled(size_w if size_w is not None else _CTRL_W),
        dialog_scaled(size_h if size_h is not None else _CTRL_H),
    )
    btn.setCursor(Qt.CursorShape.PointingHandCursor)
    return btn


# ================================================================
# 剪贴板主题色块 — 色板取自 clipboard.ui.theme.themes.PRESET_THEME_SWATCHES
# ================================================================
def _apply_clip_theme_btn_style(btn: QPushButton, name: str):
    """把主题按钮画成该主题的双色方块。建页与 refresh_settings 共用，不再各抄一份。"""
    from clipboard.ui.theme.themes import PRESET_THEME_SWATCHES
    swatch = PRESET_THEME_SWATCHES.get(name)
    if swatch is None:
        return
    accent, bg = swatch
    btn.setStyleSheet(f"""
        QPushButton {{
            background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                stop:0 {bg}, stop:0.5 {bg},
                stop:0.5 {accent}, stop:1 {accent});
            border: 2px solid {accent};
            border-radius: {dialog_scaled(3)}px;
        }}
        QPushButton:hover {{ border: 2px solid {theme_color('#333333', '#F3F3F3')}; }}
    """)


def create_appearance_page(dialog) -> QWidget:
    """创建外观设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    # ── 应用界面 ──────────────────────────────────────
    grp_app = SettingCardGroup(dialog.tr("Application"), view)
    _build_application_section(dialog, grp_app)
    layout.addWidget(grp_app)

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
    hint.setStyleSheet(f"padding: {dialog_scaled(5)}px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


# ================================================================
# 应用界面
# ================================================================

def _build_application_section(dialog, grp: SettingCardGroup):
    """Application appearance: follow OS, force light or force dark."""
    from core.ui_theme import get_ui_theme

    card = FSettingCard(
        FluentIcon.APPLICATION,
        dialog.tr("Interface Theme"),
        dialog.tr("Choose a display mode."),
        parent=grp,
    )
    dialog._ui_theme_combo = ComboBox(card)
    dialog._ui_theme_combo.setFixedWidth(dialog_scaled(150))
    dialog._ui_theme_combo.addItem(dialog.tr("System"), userData="system")
    dialog._ui_theme_combo.addItem(dialog.tr("Light"), userData="light")
    dialog._ui_theme_combo.addItem(dialog.tr("Dark"), userData="dark")
    index = dialog._ui_theme_combo.findData(get_ui_theme().mode.value)
    dialog._ui_theme_combo.setCurrentIndex(max(0, index))
    card.hBoxLayout.addWidget(
        dialog._ui_theme_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(card)

    _build_ui_scale_card(dialog, grp)
    _build_dialog_scale_card(dialog, grp)


def _build_ui_scale_card(dialog, grp: SettingCardGroup):
    """工具栏与面板缩放：只作用于操作界面，不影响截图像素尺寸和绘制内容。"""
    from core.ui_scale import get_ui_scale, UIScaleManager

    card = FSettingCard(
        FluentIcon.LAYOUT,
        dialog.tr("Toolbar & Panel Scale"),
        dialog.tr("Size of toolbars, tool panels and popups."),
        parent=grp,
    )
    dialog._ui_scale_combo = ComboBox(card)
    dialog._ui_scale_combo.setFixedWidth(dialog_scaled(150))
    for percent in UIScaleManager.PERCENT_OPTIONS:
        dialog._ui_scale_combo.addItem(f"{percent}%", userData=percent)
    index = dialog._ui_scale_combo.findData(get_ui_scale().percent)
    dialog._ui_scale_combo.setCurrentIndex(max(0, index))
    card.hBoxLayout.addWidget(
        dialog._ui_scale_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(card)


def _build_dialog_scale_card(dialog, grp: SettingCardGroup):
    """窗口界面缩放：独立业务窗口的字号与尺寸。"""
    from core.ui_scale import get_dialog_scale

    card = FSettingCard(
        FluentIcon.FONT_SIZE,
        dialog.tr("Window Scale"),
        dialog.tr("Font and size of application windows."),
        parent=grp,
    )
    dialog._dialog_scale_combo = ComboBox(card)
    dialog._dialog_scale_combo.setFixedWidth(dialog_scaled(150))
    for percent in get_dialog_scale().PERCENT_OPTIONS:
        dialog._dialog_scale_combo.addItem(f"{percent}%", userData=percent)
    index = dialog._dialog_scale_combo.findData(get_dialog_scale().percent)
    dialog._dialog_scale_combo.setCurrentIndex(max(0, index))
    card.hBoxLayout.addWidget(
        dialog._dialog_scale_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    card.hBoxLayout.addSpacing(16)
    grp.addSettingCard(card)


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
    from clipboard.ui.theme.themes import PRESET_THEME_SWATCHES, get_theme_manager
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
    dialog._clip_theme_btn.setFixedSize(dialog_scaled(_CTRL_W), dialog_scaled(_CTRL_H))
    dialog._clip_theme_btn.setCursor(Qt.CursorShape.PointingHandCursor)
    dialog._clip_theme_name = current_theme_name

    _apply_clip_theme_btn_style(dialog._clip_theme_btn, current_theme_name)

    def _show_theme_popup():
        from PySide6.QtWidgets import QMenu
        menu = QMenu(dialog)
        menu.setStyleSheet(theme_menu_style() + f" QMenu {{ padding: {dialog_scaled(4)}px; }}")
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
                        border-radius: {dialog_scaled(3)}px;
                        color: {text_color};
                        font-size: {dialog_scaled(13)}px; font-weight: bold;
                        text-align: left; padding-left: {dialog_scaled(6)}px;
                    }}
                    QPushButton:hover {{ border: 2px solid {accent}; }}
                """)

        def _on_click(name: str):
            dialog._clip_theme_name = name
            _apply_clip_theme_btn_style(dialog._clip_theme_btn, name)
            theme_mgr.set_theme(name)
            _update_btns(name)
            menu.close()

        for tname, (accent, bg) in PRESET_THEME_SWATCHES.items():
            wa = QWidgetAction(menu)
            btn = PushButton(menu)
            btn.setFixedSize(dialog_scaled(_CTRL_W), dialog_scaled(_CTRL_H))
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.clicked.connect(lambda _c, n=tname: _on_click(n))
            wa.setDefaultWidget(btn)
            menu.addAction(wa)
            theme_buttons.append((tname, btn, accent, bg))

        # 菜单和按钮是点开时才现建的，不在建页时那一轮 configure_dialog_controls
        # 扫描范围内，弹出前单独扫一遍。必须排在 _update_btns 之前：打标记会
        # 重跑 PushButton._apply_theme，把色板自己的渐变样式换成通用样式。
        configure_dialog_controls(menu)
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
    dialog._clip_font_combo.setFixedWidth(dialog_scaled(_CTRL_W))

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
    dialog._clip_opacity_combo.setFixedWidth(dialog_scaled(_CTRL_W))

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
 
