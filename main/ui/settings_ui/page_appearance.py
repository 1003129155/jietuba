# -*- coding: utf-8 -*-
"""外观设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QScrollArea, QColorDialog, QWidgetAction,
)
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor

from core.ui_scale import configure_dialog_controls, dialog_scaled
from ui.fluent_lite import (
    SettingCard as FSettingCard, FluentIcon,
    ComboBox, CaptionLabel, ColorSwatchButton,
)
from .components import SettingCardGroup, theme_menu_style
from core.ui_theme import set_own_style


def _update_color_btn(btn, color: QColor):
    """把色块按钮填成这个颜色。"""
    btn.setFill(f"rgb({color.red()}, {color.green()}, {color.blue()})")


# ================================================================
# 剪贴板主题色块 — 色板取自 clipboard.ui.theme.themes.PRESET_THEME_SWATCHES
# ================================================================
def _apply_clip_theme_btn_style(btn, name: str):
    """把主题按钮填成该主题的双色块。建页与 refresh_settings 共用，不再各抄一份。"""
    from clipboard.ui.theme.themes import PRESET_THEME_SWATCHES
    swatch = PRESET_THEME_SWATCHES.get(name)
    if swatch is None:
        return
    accent, bg = swatch
    btn.setFill(
        "qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        f" stop:0 {bg}, stop:0.5 {bg}, stop:0.5 {accent}, stop:1 {accent})"
    )


def create_appearance_page(dialog) -> QWidget:
    """创建外观设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
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
    dialog._ui_theme_combo.addItem(dialog.tr("System"), userData="system")
    dialog._ui_theme_combo.addItem(dialog.tr("Light"), userData="light")
    dialog._ui_theme_combo.addItem(dialog.tr("Dark"), userData="dark")
    index = dialog._ui_theme_combo.findData(get_ui_theme().mode.value)
    dialog._ui_theme_combo.setCurrentIndex(max(0, index))
    card.addControl(dialog._ui_theme_combo)
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
    for percent in UIScaleManager.PERCENT_OPTIONS:
        dialog._ui_scale_combo.addItem(f"{percent}%", userData=percent)
    index = dialog._ui_scale_combo.findData(get_ui_scale().percent)
    dialog._ui_scale_combo.setCurrentIndex(max(0, index))
    card.addControl(dialog._ui_scale_combo)
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
    for percent in get_dialog_scale().PERCENT_OPTIONS:
        dialog._dialog_scale_combo.addItem(f"{percent}%", userData=percent)
    index = dialog._dialog_scale_combo.findData(get_dialog_scale().percent)
    dialog._dialog_scale_combo.setCurrentIndex(max(0, index))
    card.addControl(dialog._dialog_scale_combo)
    grp.addSettingCard(card)


# ================================================================
# 截图外观
# ================================================================

def _build_screenshot_section(dialog, grp: SettingCardGroup):
    """截图外观：主题色 + 遮罩色 + 选区边框与手柄"""
    from core.theme import get_theme
    theme = get_theme()

    # 主题色
    theme_card = FSettingCard(
        FluentIcon.PALETTE,
        dialog.tr("Theme Color"),
        parent=grp,
    )
    dialog._theme_color_btn = ColorSwatchButton(theme_card)
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
    theme_card.addControl(dialog._theme_color_btn)
    grp.addSettingCard(theme_card)

    # 遮罩色
    mask_card = FSettingCard(
        FluentIcon.BRUSH,
        dialog.tr("Mask Color"),
        parent=grp,
    )
    dialog._mask_color_btn = ColorSwatchButton(mask_card)
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
    mask_card.addControl(dialog._mask_color_btn)
    grp.addSettingCard(mask_card)

    _build_selection_border_card(dialog, grp)
    _build_selection_handle_card(dialog, grp)
    _build_selection_handle_size_card(dialog, grp)


def _build_selection_border_card(dialog, grp: SettingCardGroup):
    """选区边框笔宽。档位由主题管理器给出，设置页不自己列一遍。"""
    from core.theme import ThemeManager, get_theme

    card = FSettingCard(
        FluentIcon.EDIT,
        dialog.tr("Selection Border Width"),
        dialog.tr("Thickness of the selection outline."),
        parent=grp,
    )
    dialog._selection_border_combo = ComboBox(card)
    for width in ThemeManager.BORDER_WIDTH_OPTIONS:
        dialog._selection_border_combo.addItem(f"{width}px", userData=width)
    index = dialog._selection_border_combo.findData(get_theme().selection_border_width)
    dialog._selection_border_combo.setCurrentIndex(max(0, index))
    card.addControl(dialog._selection_border_combo)
    grp.addSettingCard(card)


def _build_selection_handle_card(dialog, grp: SettingCardGroup):
    """选区手柄显示档位。关掉只是不画，八个方向照样能拖。"""
    from core.theme import ThemeManager, get_theme

    card = FSettingCard(
        FluentIcon.ALIGNMENT,
        dialog.tr("Selection Handles"),
        dialog.tr("Hidden handles can still be dragged to resize."),
        parent=grp,
    )
    dialog._selection_handle_combo = ComboBox(card)
    for value, label in (
        (ThemeManager.HANDLES_ALL, dialog.tr("8 handles")),
        (ThemeManager.HANDLES_CORNERS, dialog.tr("4 corners")),
        (ThemeManager.HANDLES_NONE, dialog.tr("None")),
    ):
        dialog._selection_handle_combo.addItem(label, userData=value)
    index = dialog._selection_handle_combo.findData(get_theme().selection_handle_style)
    dialog._selection_handle_combo.setCurrentIndex(max(0, index))
    card.addControl(dialog._selection_handle_combo)
    grp.addSettingCard(card)


def _build_selection_handle_size_card(dialog, grp: SettingCardGroup):
    """选区手柄大小。圆点直径和外圈描边粗细是一体的观感，只给小/中/大三档，
    不拆成两个像素级设置项。"""
    from core.theme import ThemeManager, get_theme

    card = FSettingCard(
        FluentIcon.EDIT,
        dialog.tr("Selection Handle Size"),
        dialog.tr("Size of the selection handles."),
        parent=grp,
    )
    dialog._selection_handle_size_combo = ComboBox(card)
    for value, label in (
        (ThemeManager.HANDLE_SIZE_SMALL, dialog.tr("Small")),
        (ThemeManager.HANDLE_SIZE_MEDIUM, dialog.tr("Medium")),
        (ThemeManager.HANDLE_SIZE_LARGE, dialog.tr("Large")),
    ):
        dialog._selection_handle_size_combo.addItem(label, userData=value)
    index = dialog._selection_handle_size_combo.findData(get_theme().selection_handle_size)
    dialog._selection_handle_size_combo.setCurrentIndex(max(0, index))
    card.addControl(dialog._selection_handle_size_combo)
    grp.addSettingCard(card)


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
    dialog._clip_theme_btn = ColorSwatchButton(theme_card)
    dialog._clip_theme_name = current_theme_name

    _apply_clip_theme_btn_style(dialog._clip_theme_btn, current_theme_name)

    def _show_theme_popup():
        from PySide6.QtWidgets import QMenu
        menu = QMenu(dialog)
        menu.setStyleSheet(theme_menu_style() + f" QMenu {{ padding: {dialog_scaled(4)}px; }}")
        theme_buttons = []

        def _update_btns(selected: str):
            for name, btn, _accent, _bg in theme_buttons:
                # 勾画在色板自己的底色上，深浅由色板决定，跟界面主题无关。
                btn.setTextColor("#FFFFFF" if name == "dark" else "#333333")
                btn.setText("✓" if name == selected else "")
                btn.setSelected(name == selected)

        def _on_click(name: str):
            dialog._clip_theme_name = name
            _apply_clip_theme_btn_style(dialog._clip_theme_btn, name)
            theme_mgr.set_theme(name)
            _update_btns(name)
            menu.close()

        for tname, (accent, bg) in PRESET_THEME_SWATCHES.items():
            wa = QWidgetAction(menu)
            btn = ColorSwatchButton(menu)
            btn.setFixedWidth(dialog._clip_theme_btn.width())
            _apply_clip_theme_btn_style(btn, tname)
            btn.clicked.connect(lambda _c, n=tname: _on_click(n))
            wa.setDefaultWidget(btn)
            menu.addAction(wa)
            theme_buttons.append((tname, btn, accent, bg))

        # 菜单和按钮是点开时才现建的，不在建页时那一轮 configure_dialog_controls
        # 扫描范围内，弹出前单独扫一遍。
        configure_dialog_controls(menu)
        _update_btns(dialog._clip_theme_name)
        pos = dialog._clip_theme_btn.mapToGlobal(
            dialog._clip_theme_btn.rect().bottomLeft()
        )
        menu.popup(pos)

    dialog._clip_theme_btn.clicked.connect(_show_theme_popup)
    theme_card.addControl(dialog._clip_theme_btn)
    grp.addSettingCard(theme_card)

    # ── 字体大小 ─────────────────────────────────────
    font_card = FSettingCard(
        FluentIcon.FONT_SIZE,
        dialog.tr("Font Size"),
        parent=grp,
    )
    dialog._clip_font_combo = ComboBox(font_card)

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
    font_card.addControl(dialog._clip_font_combo)
    grp.addSettingCard(font_card)

    # ── 透明度 ───────────────────────────────────────
    opacity_card = FSettingCard(
        FluentIcon.TRANSPARENT,
        dialog.tr("Opacity"),
        parent=grp,
    )
    dialog._clip_opacity_combo = ComboBox(opacity_card)

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
    opacity_card.addControl(dialog._clip_opacity_combo)
    grp.addSettingCard(opacity_card)
 
