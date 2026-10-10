# -*- coding: utf-8 -*-
"""钉图设置页 — Fluent Design"""
from PySide6.QtWidgets import QWidget, QVBoxLayout

from core.resource_manager import ResourceManager
from core.ui_scale import dialog_scaled
from core.ui_theme import set_own_style
from ui.fluent_lite import (
    ComboBox, FluentIcon, PushButton, SettingCard as FSettingCard, SwitchSettingCard,
)
from .components import SettingCardGroup, page_scroll_area

# 下拉框里的档位（百分比）
DEFAULT_OPACITY_STEPS = (100, 90, 80, 70, 60, 50, 40, 30)
CLICK_THROUGH_OPACITY_STEPS = (90, 80, 70, 60, 50, 40, 30, 20)
ZOOM_STEPS = (5, 10, 15, 20, 25)
FINE_ZOOM_STEPS = (1, 2, 3, 5)
OPACITY_STEPS = (1, 2, 5, 10)
ZOOM_ANCHOR_LABELS = (
    ("top_left", "Top-left"), ("top_right", "Top-right"),
    ("bottom_left", "Bottom-left"), ("bottom_right", "Bottom-right"),
    ("center", "Center"), ("mouse", "Mouse Position"),
)

# 本页通过 _behavior_controls 保存的配置项，重置本页时按它恢复默认值
PIN_BEHAVIOR_KEYS = frozenset({
    "pin_default_opacity", "pin_rounded_corners", "pin_auto_border",
    "pin_hover_buttons", "pin_click_through_opacity",
    "pin_zoom_step", "pin_fine_zoom_step", "pin_opacity_step", "pin_zoom_anchor",
    "pin_border_resize", "pin_auto_shrink",
    "pin_lock_block_move", "pin_lock_block_click_close", "pin_lock_block_key_close", "pin_lock_hint",
})


def create_pin_page(dialog) -> QWidget:
    """钉图设置：行为、缩放与大小、外观、鼠标穿透、锁定，快捷键跳到快捷键页的钉图一栏编辑。"""
    scroll = page_scroll_area(dialog)
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    if not hasattr(dialog, '_behavior_controls'):
        dialog._behavior_controls = {}

    for title, build in (
        ("Behavior", _build_behavior_section),
        ("Zoom and Size", _build_zoom_section),
        ("Appearance", _build_appearance_section),
        ("Mouse Click-Through", _build_click_through_section),
        ("Lock", _build_lock_section),
        ("Shortcuts", _build_shortcuts_section),
    ):
        group = SettingCardGroup(dialog.tr(title), view, tone="pin")
        build(dialog, group)
        layout.addWidget(group)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


def _choice_combo(dialog, card, key, items, current=None):
    """下拉框，items 是 (显示文字, 存的值)；存的值不在选项里时显示默认值。"""
    config = dialog.config_manager
    combo = ComboBox(card)
    for label, value in items:
        combo.addItem(label, userData=value)
    index = combo.findData(config.get_app_setting(key) if current is None else current)
    if index < 0:
        index = combo.findData(config.APP_DEFAULT_SETTINGS[key])
    combo.setCurrentIndex(max(0, index))
    dialog._behavior_controls[key] = combo
    card.addControl(combo)
    return combo


def _opacity_combo(dialog, card, key, steps):
    items = [(f"{percent}%", round(percent / 100, 2)) for percent in steps]
    return _choice_combo(dialog, card, key, items,
                         current=round(float(dialog.config_manager.get_app_setting(key)), 2))


def _percent_card(dialog, grp, icon, title, desc, key, steps):
    card = FSettingCard(icon, dialog.tr(title), dialog.tr(desc), parent=grp)
    _choice_combo(dialog, card, key, [(f"{percent}%", percent) for percent in steps])
    grp.addSettingCard(card)


def _switch_card(dialog, grp, icon, title, desc, key):
    card = SwitchSettingCard(icon, dialog.tr(title), dialog.tr(desc), parent=grp)
    card.setChecked(bool(dialog.config_manager.get_app_setting(key)))
    dialog._behavior_controls[key] = card
    grp.addSettingCard(card)


# ================================================================
# 行为
# ================================================================

def _build_behavior_section(dialog, grp: SettingCardGroup):
    toolbar_card = SwitchSettingCard(
        FluentIcon.PIN,
        dialog.tr("Auto-show Drawing Tools on Pin"),
        dialog.tr("On: Shows toolbar when mouse enters pinned window.")
        + "\n"
        + dialog.tr("Off: Show via right-click toolbar button."),
        parent=grp,
    )
    toolbar_card.setChecked(dialog.config_manager.get_pin_auto_toolbar())
    dialog.pin_auto_toolbar_toggle = toolbar_card
    grp.addSettingCard(toolbar_card)

    try:
        from ocr import get_available_engines
        ocr_available = bool(get_available_engines())
    except Exception:
        ocr_available = False
    ocr_card = SwitchSettingCard(
        FluentIcon.FONT,
        dialog.tr("Automatically recognize text after pinning"),
        dialog.tr(
            "On very low-end computers, OCR after pinning may cause a brief stutter. "
            "Turn off automatic recognition if needed."
        ),
        parent=grp,
    )
    ocr_card.setChecked(dialog.config_manager.get_ocr_enabled() if ocr_available else False)
    ocr_card.setEnabled(ocr_available)
    dialog.ocr_enable_toggle = ocr_card
    grp.addSettingCard(ocr_card)

    opacity_card = FSettingCard(
        FluentIcon.TRANSPARENT,
        dialog.tr("Default Opacity"),
        dialog.tr("Opacity of newly pinned images. Each image can still be adjusted with the mouse."),
        parent=grp,
    )
    _opacity_combo(dialog, opacity_card, "pin_default_opacity", DEFAULT_OPACITY_STEPS)
    grp.addSettingCard(opacity_card)


# ================================================================
# 缩放与大小
# ================================================================

def _build_zoom_section(dialog, grp: SettingCardGroup):
    """滚轮每格按原图尺寸加减固定的百分点，步长、基准点和边缘拖动都在这里。"""
    _percent_card(dialog, grp, FluentIcon.SEARCH, "Zoom Step",
                  "How much one wheel notch zooms, relative to the original size.",
                  "pin_zoom_step", ZOOM_STEPS)
    _percent_card(dialog, grp, FluentIcon.SEARCH, "Fine Zoom Step",
                  "Zoom step for the fine zoom mouse shortcut (Alt + wheel by default).",
                  "pin_fine_zoom_step", FINE_ZOOM_STEPS)
    _percent_card(dialog, grp, FluentIcon.TRANSPARENT, "Opacity Step",
                  "Opacity change per notch of the opacity mouse shortcut (Ctrl + wheel by default).",
                  "pin_opacity_step", OPACITY_STEPS)

    anchor_card = FSettingCard(
        FluentIcon.ALIGNMENT,
        dialog.tr("Zoom Anchor"),
        dialog.tr("The point of the image that stays in place while zooming with the wheel."),
        parent=grp,
    )
    _choice_combo(dialog, anchor_card, "pin_zoom_anchor",
                  [(dialog.tr(label), value) for value, label in ZOOM_ANCHOR_LABELS])
    grp.addSettingCard(anchor_card)

    _switch_card(dialog, grp, FluentIcon.LAYOUT, "Resize by Dragging Edges",
                 "Drag near the edge or corner of a pinned image to resize it. The aspect ratio is kept.",
                 "pin_border_resize")
    _switch_card(dialog, grp, FluentIcon.FONT_SIZE, "Shrink Oversized Images",
                 "Images larger than the screen are scaled down to fit and centered when pinned.",
                 "pin_auto_shrink")


# ================================================================
# 外观
# ================================================================

def _build_appearance_section(dialog, grp: SettingCardGroup):
    """系统圆角阴影、新钉图描边、悬停按钮。

    圆角和按钮保存后已打开的钉图立即跟上；描边只决定之后新建的钉图，已有钉图在右键菜单里单独开关。
    """
    from core.platform_utils import supports_window_corner_preference
    from settings import get_tool_settings_manager
    config = get_tool_settings_manager()

    # 圆角和阴影是 Windows 11 加的，更早的系统上没有，也就不给这个开关
    if supports_window_corner_preference():
        corners_card = SwitchSettingCard(
            FluentIcon.LAYOUT,
            dialog.tr("Rounded Corners and Shadow"),
            dialog.tr(
                "Windows 11 rounds the corners of pinned images and adds a shadow. "
                "Turn off to show the whole image with square corners and no shadow."
            ),
            parent=grp,
        )
        corners_card.setChecked(config.get_app_setting("pin_rounded_corners"))
        dialog._behavior_controls["pin_rounded_corners"] = corners_card
        grp.addSettingCard(corners_card)

    border_card = SwitchSettingCard(
        FluentIcon.PALETTE,
        dialog.tr("Auto Border"),
        dialog.tr("New pinned images get a theme-colored border. Each image can still toggle it from the right-click menu."),
        parent=grp,
    )
    border_card.setChecked(config.get_app_setting("pin_auto_border"))
    dialog._behavior_controls["pin_auto_border"] = border_card
    grp.addSettingCard(border_card)

    buttons_card = SwitchSettingCard(
        FluentIcon.CLOSE,
        dialog.tr("Hover Buttons"),
        dialog.tr(
            "Show the close and toolbar buttons in the top-right corner when the mouse "
            "is over a pinned image. When off, use the right-click menu or shortcuts."
        ),
        parent=grp,
    )
    buttons_card.setChecked(config.get_app_setting("pin_hover_buttons"))
    dialog._behavior_controls["pin_hover_buttons"] = buttons_card
    grp.addSettingCard(buttons_card)


# ================================================================
# 鼠标穿透
# ================================================================

def _build_click_through_section(dialog, grp: SettingCardGroup):
    card = FSettingCard(
        FluentIcon.TRANSPARENT,
        dialog.tr("Click-Through Opacity"),
        dialog.tr(
            "Opacity of a pinned image when click-through starts from its right-click menu. "
            "The slider on the bar above it still adjusts each image."
        ),
        parent=grp,
    )
    _opacity_combo(dialog, card, "pin_click_through_opacity", CLICK_THROUGH_OPACITY_STEPS)
    grp.addSettingCard(card)


# ================================================================
# 锁定
# ================================================================

def _build_lock_section(dialog, grp: SettingCardGroup):
    """锁定后禁止哪几类操作。右键菜单不受影响，用来解锁和做明确的操作。"""
    _switch_card(dialog, grp, ResourceManager.get_icon_path("移动窗口.svg"), "Block Moving and Resizing",
                 "Locked images cannot be dragged, zoomed or resized.",
                 "pin_lock_block_move")
    _switch_card(dialog, grp, FluentIcon.CLOSE, "Block Closing by Mouse",
                 "The close button and the close mouse shortcut do nothing on locked images.",
                 "pin_lock_block_click_close")
    _switch_card(dialog, grp, FluentIcon.COMMAND_PROMPT, "Block Closing by Keyboard",
                 "Esc does nothing on locked images.",
                 "pin_lock_block_key_close")
    _switch_card(dialog, grp, FluentIcon.INFO, "Show Lock Hints",
                 "Show a short hint when locking, unlocking or trying a blocked action. "
                 "The right-click menu always works on locked images.",
                 "pin_lock_hint")


# ================================================================
# 快捷键
# ================================================================

def _build_shortcuts_section(dialog, grp: SettingCardGroup):
    """快捷键的录入、判重都在快捷键页，这里只放入口，免得两处各有一份编辑框。"""
    for icon, title, desc, mouse in (
        (FluentIcon.COMMAND_PROMPT, "Keyboard Shortcuts",
         "Copy, lock, thumbnail, toolbar and other keys for pinned images.", False),
        (ResourceManager.get_icon_path("鼠标线框.svg"), "Mouse Shortcuts",
         "Zoom, opacity, close and other mouse actions for pinned images.", True),
    ):
        card = FSettingCard(icon, dialog.tr(title), dialog.tr(desc), parent=grp)
        button = PushButton(dialog.tr("Edit"), card)
        button.clicked.connect(lambda _checked=False, m=mouse: dialog._show_pin_shortcuts(mouse=m))
        card.addControl(button)
        grp.addSettingCard(card)
