# -*- coding: utf-8 -*-
"""Mock ConfigManager — 用于独立调试 SettingsDialog"""
import os
import sys

from settings.tool_settings import ANNOTATION_TOOL_SHORTCUTS

from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QFont

APP_DEFAULT_SETTINGS = {
    "hotkey": "ctrl+shift+a",
    "hotkey_2": "ctrl+shift+a",
    "clipboard_hotkey": "ctrl+shift+v",
    "clipboard_hotkey_2": "ctrl+shift+v",
    "translation_hotkey": "",
    "translation_hotkey_2": "",
    "smart_selection": True,
    "log_enabled": True,
    "log_level": "INFO",
    "log_retention_days": 7,
    "log_dir": os.path.expanduser("~"),
    "long_stitch_engine": "hash_rust",
    "scroll_cooldown": 0.15,
    "long_stitch_ignore_top_pixels": 0,
    "preload_screenshot": True,
    "preload_toolbar": True,
    "preload_ocr": True,
    "preload_settings": True,
    "preload_clipboard": True,
    "screenshot_info_hide_on_drag": False,
    "screenshot_save_enabled": True,
    "screenshot_save_path": os.path.join(os.path.expanduser("~"), "Desktop", "スクショ"),
    "screenshot_format": "PNG",
    "screenshot_quality": 85,
    "show_main_window": True,
    "ocr_enabled": True,
    "ocr_engine": "windos_ocr",
    "ocr_grayscale_enabled": False,
    "ocr_upscale_enabled": False,
    "ocr_upscale_factor": 2.0,
    "pin_auto_toolbar": True,
    "translation_provider": "google",
    "deepl_api_key": "",
    "deepl_use_pro": False,
    "amazon_translate_region": "us-west-2",
    "amazon_translate_access_key_id": "",
    "amazon_translate_secret_access_key": "",
    "amazon_translate_session_token": "",
    "google_translate_api_key": "",
    "azure_translate_api_key": "",
    "azure_translate_region": "",
    "azure_translate_endpoint": "",
    "translation_target_lang": "",
    "translation_split_sentences": True,
    "translation_preserve_formatting": True,
    "clipboard_enabled": True,
    "clipboard_auto_paste": False,
    "clipboard_history_limit": 100,
    "clipboard_auto_cleanup": False,
    "magnifier_color_copy_format": "rgb_hex",
    "ui_theme_mode": "system",
    "inapp_confirm": "ctrl+c",
    "inapp_pin": "ctrl+d",
    "inapp_undo": "ctrl+z",
    "inapp_redo": "ctrl+y",
    "inapp_delete": "delete",
    "inapp_copy_pin": "ctrl+c",
    "inapp_thumbnail": "r",
    "inapp_toggle_toolbar": "space",
    "inapp_zoom_in": "pageup",
    "inapp_zoom_out": "pagedown",
    "inapp_translate": "shift+c",
    "inapp_cursor_move_mode": "both",
    **{key: default for key, _tool, _label, default in ANNOTATION_TOOL_SHORTCUTS},
}


class MockConfig:
    APP_DEFAULT_SETTINGS = APP_DEFAULT_SETTINGS

    def __init__(self):
        self.settings = QSettings("TestApp", "Settings")
        self.qsettings = self.settings

    # --- getter / setter stubs ---
    def get_smart_selection(self): return False
    def set_smart_selection(self, v): pass
    def get_log_enabled(self): return True
    def set_log_enabled(self, v): pass
    def get_log_dir(self): return os.path.expanduser("~")
    def set_log_dir(self, v): pass
    def get_log_level(self): return "INFO"
    def set_log_level(self, v): pass
    def get_log_retention_days(self): return 7
    def set_log_retention_days(self, v): pass
    def get_long_stitch_engine(self): return "hash_rust"
    def set_long_stitch_engine(self, v): pass
    def get_long_stitch_debug(self): return False
    def set_long_stitch_debug(self, v): pass
    def get_scroll_cooldown(self): return 0.15
    def set_scroll_cooldown(self, v): pass
    def get_long_stitch_ignore_top_pixels(self): return 0
    def set_long_stitch_ignore_top_pixels(self, v): pass
    def get_screenshot_save_enabled(self): return True
    def set_screenshot_save_enabled(self, v): pass
    def get_screenshot_save_path(self): return os.path.join(os.path.expanduser("~"), "Desktop", "スクショ")
    def set_screenshot_save_path(self, v): pass
    def get_screenshot_format(self): return "PNG"
    def set_screenshot_format(self, v): pass
    def get_screenshot_quality(self): return 85
    def set_screenshot_quality(self, v): pass
    def get_show_main_window(self): return True
    def set_show_main_window(self, v): pass
    def get_ocr_enabled(self): return True
    def set_ocr_enabled(self, v): pass
    def get_ocr_engine(self): return "windos_ocr"
    def set_ocr_engine(self, v): pass
    def get_ocr_grayscale_enabled(self): return False
    def set_ocr_grayscale_enabled(self, v): pass
    def get_ocr_upscale_enabled(self): return False
    def set_ocr_upscale_enabled(self, v): pass
    def get_ocr_upscale_factor(self): return 2.0
    def set_ocr_upscale_factor(self, v): pass
    def get_pin_auto_toolbar(self): return True
    def set_pin_auto_toolbar(self, v): pass
    def get_deepl_api_key(self): return ""
    def set_deepl_api_key(self, v): pass
    def get_deepl_use_pro(self): return False
    def set_deepl_use_pro(self, v): pass
    def get_translation_provider(self): return "google"
    def set_translation_provider(self, v): pass
    def get_translation_provider_config(self, provider_id):
        if provider_id == "deepl":
            return {"api_key": "", "use_pro": False}
        if provider_id == "amazon":
            return {
                "region": "us-west-2",
                "access_key_id": "",
                "secret_access_key": "",
                "session_token": "",
            }
        if provider_id == "google":
            return {"api_key": ""}
        return {}
    def get_amazon_translate_region(self): return "us-west-2"
    def set_amazon_translate_region(self, v): pass
    def get_amazon_translate_access_key_id(self): return ""
    def set_amazon_translate_access_key_id(self, v): pass
    def get_amazon_translate_secret_access_key(self): return ""
    def set_amazon_translate_secret_access_key(self, v): pass
    def get_amazon_translate_session_token(self): return ""
    def set_amazon_translate_session_token(self, v): pass
    def get_google_translate_api_key(self): return ""
    def set_google_translate_api_key(self, v): pass
    def get_azure_translate_api_key(self): return ""
    def set_azure_translate_api_key(self, v): pass
    def get_azure_translate_region(self): return ""
    def set_azure_translate_region(self, v): pass
    def get_azure_translate_endpoint(self): return ""
    def set_azure_translate_endpoint(self, v): pass
    def get_app_setting(self, key, default=None):
        if default is None:
            default = self.APP_DEFAULT_SETTINGS.get(key)
        return default
    def set_app_setting(self, key, v): pass
    def get_translation_split_sentences(self): return True
    def set_translation_split_sentences(self, v): pass
    def get_translation_preserve_formatting(self): return True
    def set_translation_preserve_formatting(self, v): pass
    def set_translation_target_lang(self, v): pass
    def get_hotkey(self): return "ctrl+shift+a"
    def set_hotkey(self, v): pass
    def get_hotkey_2(self): return "ctrl+shift+a"
    def set_hotkey_2(self, v): pass
    def get_clipboard_hotkey(self): return "ctrl+shift+v"
    def set_clipboard_hotkey(self, v): pass
    def get_clipboard_hotkey_2(self): return "ctrl+shift+v"
    def set_clipboard_hotkey_2(self, v): pass
    def get_translation_hotkey(self): return ""
    def set_translation_hotkey(self, v): pass
    def get_translation_hotkey_2(self): return ""
    def set_translation_hotkey_2(self, v): pass
    def get_clipboard_enabled(self): return True
    def set_clipboard_enabled(self, v): pass
    def get_clipboard_auto_paste(self): return False
    def set_clipboard_auto_paste(self, v): pass
    def get_clipboard_history_limit(self): return 100
    def set_clipboard_history_limit(self, v): pass
    def get_clipboard_db_path(self): return ""
    def set_clipboard_db_path(self, v): pass
    def get_clipboard_auto_cleanup(self): return False
    def set_clipboard_auto_cleanup(self, v): pass
    def get_inapp_shortcut(self, key):
        return self.settings.value(
            f"inapp/{key}", self.APP_DEFAULT_SETTINGS.get(key, ""), type=str
        )
    def set_inapp_shortcut(self, key, value):
        self.settings.setValue(f"inapp/{key}", value)
    def get_inapp_cursor_move_mode(self): return "both"
    def set_inapp_cursor_move_mode(self, value): pass


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setFont(QFont("Microsoft YaHei", 9))

    from .dialog import SettingsDialog
    dlg = SettingsDialog(MockConfig())
    dlg.show()
    sys.exit(app.exec())
