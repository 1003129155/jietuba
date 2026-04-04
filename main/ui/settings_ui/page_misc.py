# -*- coding: utf-8 -*-
"""杂项设置页 — Fluent Design"""
from PySide6.QtWidgets import QWidget, QVBoxLayout, QScrollArea
from PySide6.QtCore import Qt
from qfluentwidgets import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel,
)
from .components import SettingCardGroup


def create_misc_page(dialog) -> QWidget:
    """创建杂项设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ════ 启动行为 ════
    grp_startup = SettingCardGroup(dialog.tr("Startup"), view)

    # 开机自启
    from ..welcome.page6_finish import FinishPage as _FP
    autostart_card = SwitchSettingCard(
        FluentIcon.POWER_BUTTON,
        dialog.tr("Launch on Startup"),
        dialog.tr("Copy shortcut to Windows Startup folder."),
        parent=grp_startup,
    )
    autostart_card.setChecked(_FP._get_autostart())
    dialog.autostart_toggle = autostart_card
    grp_startup.addSettingCard(autostart_card)

    # 主界面显示
    show_card = SwitchSettingCard(
        FluentIcon.APPLICATION,
        dialog.tr("Show Main Window on Startup"),
        dialog.tr("If off, starts in background."),
        parent=grp_startup,
    )
    show_card.setChecked(dialog.config_manager.get_show_main_window())
    dialog.show_main_window_toggle = show_card
    grp_startup.addSettingCard(show_card)

    layout.addWidget(grp_startup)

    # ════ 钉图 ════
    grp_pin = SettingCardGroup(dialog.tr("Pin Window"), view)

    pin_card = SwitchSettingCard(
        FluentIcon.PIN,
        dialog.tr("Auto-show Drawing Tools on Pin"),
        dialog.tr("On: Shows toolbar when mouse enters pinned window.")
        + "\n"
        + dialog.tr("Off: Show via right-click toolbar button."),
        parent=grp_pin,
    )
    pin_card.setChecked(dialog.config_manager.get_pin_auto_toolbar())
    dialog.pin_auto_toolbar_toggle = pin_card
    grp_pin.addSettingCard(pin_card)

    layout.addWidget(grp_pin)

    # ════ 操作 ════
    grp_ops = SettingCardGroup(dialog.tr("Operation"), view)

    # 颜色复制格式
    fmt_card = FSettingCard(
        FluentIcon.PALETTE,
        dialog.tr("Color Copy Format"),
        dialog.tr("Used when copying color info in magnifier."),
        parent=grp_ops,
    )
    dialog.magnifier_color_format_combo = ComboBox(fmt_card)
    dialog.magnifier_color_format_combo.setFixedWidth(140)
    dialog.magnifier_color_format_combo.addItem(dialog.tr("RGB+HEX"), userData="rgb_hex")
    dialog.magnifier_color_format_combo.addItem(dialog.tr("RGB only"), userData="rgb")
    dialog.magnifier_color_format_combo.addItem(dialog.tr("HEX only"), userData="hex")

    current_format = dialog.config_manager.get_app_setting(
        "magnifier_color_copy_format", "rgb_hex"
    )
    idx = dialog.magnifier_color_format_combo.findData(current_format)
    if idx >= 0:
        dialog.magnifier_color_format_combo.setCurrentIndex(idx)
    fmt_card.hBoxLayout.addWidget(
        dialog.magnifier_color_format_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    fmt_card.hBoxLayout.addSpacing(16)
    grp_ops.addSettingCard(fmt_card)

    # 界面语言
    lang_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Language"),
        dialog.tr("Select display language. Restart required after change."),
        parent=grp_ops,
    )
    dialog.language_combo = ComboBox(lang_card)
    dialog.language_combo.setFixedWidth(140)

    from core.i18n import I18nManager
    for code, name in I18nManager.get_available_languages().items():
        dialog.language_combo.addItem(name, userData=code)

    current_lang = dialog.config_manager.get_app_setting("language", "ja")
    index = dialog.language_combo.findData(current_lang)
    if index >= 0:
        dialog.language_combo.setCurrentIndex(index)
    lang_card.hBoxLayout.addWidget(
        dialog.language_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    lang_card.hBoxLayout.addSpacing(16)
    grp_ops.addSettingCard(lang_card)

    layout.addWidget(grp_ops)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Even with background startup, you can operate from system tray."),
        view,
    )
    hint.setStyleSheet("padding: 5px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
 