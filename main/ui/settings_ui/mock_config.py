# -*- coding: utf-8 -*-
"""Mock ConfigManager — 用于独立调试 SettingsDialog"""
import os
import sys

from PySide6.QtCore import QSettings
from PySide6.QtWidgets import QApplication
from PySide6.QtGui import QFont

APP_DEFAULT_SETTINGS = {
    "hotkey": "ctrl+shift+a",
    "hotkey_2": "ctrl+shift+a",
    "clipboard_hotkey": "ctrl+shift+v",
    "clipboard_hotkey_2": "ctrl+shift+v",
    "smart_selection": True,
    "log_enabled": True,
    "log_level": "INFO",
    "log_retention_days": 7,
    "log_dir": os.path.expanduser("~"),
    "long_stitch_engine": "hash_rust",
    "scroll_cooldown": 0.15,
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
    "deepl_api_key": "",
    "deepl_use_pro": False,
    "translation_target_lang": "",
    "translation_split_sentences": True,
    "translation_preserve_formatting": True,
    "clipboard_enabled": True,
    "clipboard_auto_paste": False,
    "clipboard_history_limit": 100,
    "clipboard_auto_cleanup": False,
    "magnifier_color_copy_format": "rgb_hex",
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
    def get_app_setting(self, key, default=None): return default or ""
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
    def get_clipboard_enabled(self): return True
    def set_clipboard_enabled(self, v): pass
    def get_clipboard_auto_paste(self): return False
    def set_clipboard_auto_paste(self, v): pass
    def get_clipboard_history_limit(self): return 100
    def set_clipboard_history_limit(self, v): pass
    def get_clipboard_auto_cleanup(self): return False
    def set_clipboard_auto_cleanup(self, v): pass


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setFont(QFont("Microsoft YaHei", 9))

    from .dialog import SettingsDialog
    dlg = SettingsDialog(MockConfig())
    dlg.show()
    sys.exit(app.exec())
 