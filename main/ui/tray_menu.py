# -*- coding: utf-8 -*-
"""System tray context menu."""

from datetime import datetime
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QFileDialog, QMenu

from core import safe_event
from core.constants import CSS_FONT_FAMILY_UI
from core.i18n import make_tr
from core.logger import log_exception
from ui.dialogs import show_warning_dialog

_tr = make_tr("SystemTray")


class CustomTrayMenu(QMenu):
    @safe_event
    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            return
        super().mouseReleaseEvent(event)


def create_tray_menu(app) -> QMenu:
    """Create the system tray context menu for MainApp."""
    menu = CustomTrayMenu()
    menu.setStyleSheet(_menu_style())

    action_screenshot = QAction(_tr("Screenshot"), app)
    action_screenshot.triggered.connect(app.start_screenshot)
    menu.addAction(action_screenshot)

    action_clipboard = QAction(_tr("Clipboard"), app)
    action_clipboard.triggered.connect(app.open_clipboard_window)
    menu.addAction(action_clipboard)

    action_translate = QAction(_tr("Translation"), app)
    action_translate.triggered.connect(app.open_translator)
    menu.addAction(action_translate)

    menu.addSeparator()

    action_global_hotkeys = QAction(_tr("Disable Global Hotkeys"), app)
    action_global_hotkeys.setCheckable(True)
    action_global_hotkeys.setChecked(
        app.config_manager.get_app_setting("global_hotkeys_disabled", False)
    )
    action_global_hotkeys.triggered.connect(app.set_global_hotkeys_disabled)
    menu.addAction(action_global_hotkeys)

    menu.addSeparator()
    _add_pin_actions(menu, app)

    menu.addSeparator()

    action_settings = QAction(_tr("Settings"), app)
    action_settings.triggered.connect(app.open_settings)
    menu.addAction(action_settings)

    menu.addSeparator()

    action_quit = QAction(_tr("Exit"), app)
    action_quit.triggered.connect(app.quit_app)
    menu.addAction(action_quit)

    return menu


def _menu_style() -> str:
    from core.theme import get_theme

    tc = get_theme().theme_color_hex
    return f"""
        QMenu {{
            background-color: white;
            border: 1px solid #ccc;
            border-radius: 4px;
            padding: 4px;
            font-family: {CSS_FONT_FAMILY_UI};
            font-size: 9pt;
            color: #000000;
        }}
        QMenu::item {{
            padding: 6px 12px;
            border-radius: 3px;
            color: #000000;
            background-color: transparent;
        }}
        QMenu::item:selected {{
            background-color: {tc};
            color: #ffffff;
        }}
        QMenu::item:disabled {{
            color: #9e9e9e;
        }}
        QMenu::separator {{
            height: 1px;
            background: #ddd;
            margin: 4px 6px;
        }}
    """


def _add_pin_actions(menu: QMenu, app):
    from pin.pin_manager import PinManager

    pin_manager = PinManager.instance()
    pin_count = pin_manager.count()
    has_pins = pin_count > 0

    close_all_action = QAction(_tr("Close all pinned windows"), app)
    close_all_action.setEnabled(has_pins)
    close_all_action.triggered.connect(pin_manager.close_all)
    menu.addAction(close_all_action)

    pins_title = _tr("Current pinned windows: {count}").format(count=pin_count)
    if not has_pins:
        pins_action = QAction(pins_title, app)
        pins_action.setEnabled(False)
        menu.addAction(pins_action)
        return

    pins_menu = QMenu(pins_title, menu)
    pins_menu.setStyleSheet(_menu_style())

    move_center_action = QAction(_tr("Move all pins to center"), app)
    move_center_action.triggered.connect(pin_manager.move_all_to_screen_center)
    pins_menu.addAction(move_center_action)

    thumbnail_all_action = QAction(_tr("Thumbnail all pins"), app)
    thumbnail_all_action.triggered.connect(lambda: pin_manager.set_all_thumbnail_mode(True))
    pins_menu.addAction(thumbnail_all_action)

    restore_all_action = QAction(_tr("Restore all pin thumbnails"), app)
    restore_all_action.triggered.connect(lambda: pin_manager.set_all_thumbnail_mode(False))
    pins_menu.addAction(restore_all_action)

    pins_menu.addSeparator()

    submenu_close_all_action = QAction(_tr("Close all pins"), app)
    submenu_close_all_action.triggered.connect(pin_manager.close_all)
    pins_menu.addAction(submenu_close_all_action)

    save_all_action = QAction(_tr("Save all pins as..."), app)
    save_all_action.triggered.connect(lambda: _save_all_pins_as(app, pin_manager))
    pins_menu.addAction(save_all_action)

    menu.addMenu(pins_menu)


def _save_all_pins_as(app, pin_manager):
    directory = QFileDialog.getExistingDirectory(
        None,
        _tr("Choose folder to save pinned images"),
        str(Path.home()),
    )
    if not directory:
        return

    prefix = datetime.now().strftime("pins_%Y%m%d_%H%M%S")
    try:
        saved, failed = pin_manager.save_all_to_directory(directory, prefix=prefix)
    except Exception as e:
        log_exception(e, "Save all pins")
        show_warning_dialog(None, _tr("Save Failed"), str(e))
        return

    if failed:
        show_warning_dialog(
            None,
            _tr("Save Failed"),
            _tr("Saved {saved} pinned images, failed {failed}.").format(
                saved=saved,
                failed=failed,
            ),
        )
