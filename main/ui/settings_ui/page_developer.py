# -*- coding: utf-8 -*-
"""开发者选项页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QScrollArea,
)
from core.ui_scale import dialog_scaled
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, DoubleSpinBox, SpinBox,
    CaptionLabel, PrimaryPushButton,
)
from .components import SettingCardGroup


def create_developer_page(dialog) -> QWidget:
    """开发者选项 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    # ════ 长截图 ════
    grp_stitch = SettingCardGroup(dialog.tr("Long Screenshot"), view)

    # 引擎
    engine_card = FSettingCard(
        FluentIcon.SETTING,
        dialog.tr("Stitching Engine"),
        parent=grp_stitch,
    )
    dialog.engine_combo = ComboBox(engine_card)
    dialog.engine_combo.addItems(["Rust Hash"])
    dialog.engine_combo.setItemData(0, "hash_rust")
    dialog.engine_combo.setCurrentIndex(0)
    engine_card.addControl(dialog.engine_combo)
    grp_stitch.addSettingCard(engine_card)

    # 滚动冷却
    cooldown_card = FSettingCard(
        FluentIcon.STOP_WATCH,
        dialog.tr("Wait Time"),
        dialog.tr("Capture wait time after scroll (seconds)"),
        parent=grp_stitch,
    )
    dialog.cooldown_spinbox = DoubleSpinBox(cooldown_card)
    dialog.cooldown_spinbox.setRange(0.05, 1.0)
    dialog.cooldown_spinbox.setSingleStep(0.01)
    dialog.cooldown_spinbox.setDecimals(2)
    dialog.cooldown_spinbox.setValue(
        dialog.config_manager.get_scroll_cooldown()
    )
    cooldown_card.addControl(dialog.cooldown_spinbox)
    grp_stitch.addSettingCard(cooldown_card)

    # 后续截图顶部忽略像素
    ignore_top_card = FSettingCard(
        FluentIcon.FILTER,
        dialog.tr("Ignore Top Pixels"),
        dialog.tr("Skip this many top pixels on subsequent captures during matching"),
        parent=grp_stitch,
    )
    dialog.ignore_top_pixels_spinbox = SpinBox(ignore_top_card)
    dialog.ignore_top_pixels_spinbox.setRange(0, 300)
    dialog.ignore_top_pixels_spinbox.setSingleStep(1)
    dialog.ignore_top_pixels_spinbox.setValue(
        dialog.config_manager.get_long_stitch_ignore_top_pixels()
    )
    ignore_top_card.addControl(dialog.ignore_top_pixels_spinbox)
    grp_stitch.addSettingCard(ignore_top_card)

    layout.addWidget(grp_stitch)

    # ════ 工具 ════
    grp_tools = SettingCardGroup(dialog.tr("Tools"), view)

    wizard_card = FSettingCard(
        FluentIcon.SETTING,
        dialog.tr("Setup Wizard"),
        dialog.tr("Re-run the initial setup guide."),
        parent=grp_tools,
    )
    btn_wizard = PrimaryPushButton(dialog.tr("Open Wizard"), wizard_card)
    btn_wizard.clicked.connect(dialog._open_welcome_wizard)
    wizard_card.addControl(btn_wizard)
    grp_tools.addSettingCard(wizard_card)

    layout.addWidget(grp_tools)

    # ════ 启动预加载 ════
    grp_preload = SettingCardGroup(dialog.tr("Startup Preload"), view)

    preload_desc = CaptionLabel(
        dialog.tr("Control which modules are preloaded at startup. Disable to speed up launch. Changes take effect after restart."),
        view,
    )
    preload_desc.setWordWrap(True)
    preload_desc.setStyleSheet(f"padding: 0 0 {dialog_scaled(4)}px 0;")
    layout.addWidget(preload_desc)

    _preload_items = [
        ("preload_screenshot_toggle", FluentIcon.CAMERA,
         dialog.tr("Preload Screenshot Modules"),
         dialog.tr("Pre-load mss, canvas, tools in background thread"),
         "preload_screenshot"),
        ("preload_toolbar_toggle", FluentIcon.LAYOUT,
         dialog.tr("Preload Toolbar"),
         dialog.tr("Pre-create toolbar widgets to avoid first-capture lag"),
         "preload_toolbar"),
        ("preload_ocr_toggle", FluentIcon.SEARCH,
         dialog.tr("Preload OCR Engine"),
         dialog.tr("Pre-load OCR model in background thread"),
         "preload_ocr"),
        ("preload_settings_toggle", FluentIcon.SETTING,
         dialog.tr("Preload Settings Window"),
         dialog.tr("Pre-create the settings dialog on startup"),
         "preload_settings"),
        ("preload_clipboard_toggle", FluentIcon.PASTE,
         dialog.tr("Preload Clipboard Manager"),
         dialog.tr("Initialize clipboard monitoring on startup"),
         "preload_clipboard"),
    ]

    for attr_name, icon, title, desc, cfg_key in _preload_items:
        card = SwitchSettingCard(icon, title, desc, parent=grp_preload)
        card.setChecked(dialog.config_manager.get_app_setting(cfg_key))
        setattr(dialog, attr_name, card)
        grp_preload.addSettingCard(card)

    layout.addWidget(grp_preload)

    # ════ 截图信息面板 ════
    grp_panel = SettingCardGroup(dialog.tr("Selection Info Panel"), view)

    hide_card = SwitchSettingCard(
        FluentIcon.HIDE,
        dialog.tr("Hide Panel on Drag"),
        dialog.tr("Hide the info panel while dragging the selection area"),
        parent=grp_panel,
    )
    hide_card.setChecked(
        dialog.config_manager.get_app_setting("screenshot_info_hide_on_drag")
    )
    dialog.info_hide_on_drag_toggle = hide_card
    grp_panel.addSettingCard(hide_card)

    layout.addWidget(grp_panel)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
 
