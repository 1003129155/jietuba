# -*- coding: utf-8 -*-
"""杂项设置页 — Fluent Design"""
import os
from datetime import date

from PySide6.QtCore import QStandardPaths
from PySide6.QtWidgets import QWidget, QVBoxLayout, QFileDialog
from core.ui_scale import dialog_scaled
from ui.dialogs import show_error_dialog, show_info_dialog
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel, PushButton,
)
from .components import SettingCardGroup, page_scroll_area
from core.ui_theme import set_own_style


def create_misc_page(dialog) -> QWidget:
    """创建杂项设置页面 — Fluent Design"""
    scroll = page_scroll_area(dialog)
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    # ════ 启动行为 ════
    grp_startup = SettingCardGroup(dialog.tr("Startup"), view)

    # 开机自启
    autostart_card = SwitchSettingCard(
        FluentIcon.POWER_BUTTON,
        dialog.tr("Launch on Startup"),
        dialog.tr("Register in Windows startup via registry."),
        parent=grp_startup,
    )
    autostart_card.setChecked(dialog.config_manager.get_app_setting("autostart_enabled"))
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

    # ════ 操作 ════
    grp_ops = SettingCardGroup(dialog.tr("Operation"), view)

    # 界面语言
    lang_card = FSettingCard(
        FluentIcon.LANGUAGE,
        dialog.tr("Language"),
        dialog.tr("Select display language. Restart required after change."),
        parent=grp_ops,
    )
    dialog.language_combo = ComboBox(lang_card)

    from core.i18n import I18nManager
    for code, name in I18nManager.get_available_languages().items():
        dialog.language_combo.addItem(name, userData=code)

    current_lang = dialog.config_manager.get_app_setting("language", "ja")
    index = dialog.language_combo.findData(current_lang)
    if index >= 0:
        dialog.language_combo.setCurrentIndex(index)
    lang_card.addControl(dialog.language_combo)
    grp_ops.addSettingCard(lang_card)

    layout.addWidget(grp_ops)

    # ════ 设置备份 ════
    grp_backup = SettingCardGroup(dialog.tr("Settings Backup"), view)

    export_card = FSettingCard(
        FluentIcon.SAVE,
        dialog.tr("Export Settings"),
        dialog.tr("Save settings to a file, including translation API keys. Folder locations are not included."),
        parent=grp_backup,
    )
    dialog.export_settings_button = PushButton(dialog.tr("Export"), export_card)
    dialog.export_settings_button.clicked.connect(lambda: export_settings_to_file(dialog))
    export_card.addControl(dialog.export_settings_button)
    grp_backup.addSettingCard(export_card)

    import_card = FSettingCard(
        FluentIcon.DOWNLOAD,
        dialog.tr("Import Settings"),
        dialog.tr("Load settings from an exported file. Folder locations stay unchanged."),
        parent=grp_backup,
    )
    dialog.import_settings_button = PushButton(dialog.tr("Import"), import_card)
    dialog.import_settings_button.clicked.connect(lambda: import_settings_from_file(dialog))
    import_card.addControl(dialog.import_settings_button)
    grp_backup.addSettingCard(import_card)

    layout.addWidget(grp_backup)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Even with background startup, you can operate from system tray."),
        view,
    )
    hint.setStyleSheet(f"padding: {dialog_scaled(5)}px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
 


def _documents_dir() -> str:
    return QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DocumentsLocation)


def export_settings_to_file(dialog):
    from main_app import APP_VERSION
    from settings.settings_transfer import export_settings

    # 导出的是已保存的设置，先把界面上还没应用的修改存进去
    if dialog._has_unsaved_changes() and not dialog.apply_settings():
        return

    title = dialog.tr("Export Settings")
    suggested = os.path.join(_documents_dir(), f"jietuba-settings-{date.today():%Y%m%d}.json")
    path, _ = QFileDialog.getSaveFileName(dialog, title, suggested, dialog.tr("Settings File (*.json)"))
    if not path:
        return
    try:
        export_settings(dialog.config_manager, dialog.settings_keys(), path, APP_VERSION)
    except OSError as e:
        show_error_dialog(dialog, title, dialog.tr("Could not write the file: %1").replace("%1", str(e)))
        return
    show_info_dialog(dialog, title, dialog.tr("Settings exported to %1").replace("%1", path))


def import_settings_from_file(dialog):
    from settings.settings_transfer import SettingsFileError, read_settings_file

    title = dialog.tr("Import Settings")
    path, _ = QFileDialog.getOpenFileName(dialog, title, _documents_dir(), dialog.tr("Settings File (*.json)"))
    if not path:
        return
    try:
        values = read_settings_file(path)
    except SettingsFileError:
        show_error_dialog(dialog, title, dialog.tr("This file is not a Jietuba settings file, or it is damaged."))
        return
    dialog.load_imported_settings(values)
    show_info_dialog(dialog, title, dialog.tr("Settings loaded. Click Apply to use them."))
