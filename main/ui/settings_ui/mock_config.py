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
    "pin_clipboard_hotkey": "",
    "pin_clipboard_hotkey_2": "",
    "translation_hotkey": "",
    "translation_hotkey_2": "",
    "double_click_copy_close": True,
    "cross_tool_selection": True,
    "text_always_on_top": True,
    "smart_selection": False,
    "smart_selection_mode": "element",
    "smart_selection_animation": False,
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
    "baidu_translate_appid": "",
    "baidu_translate_secret_key": "",
    "translation_target_lang": "",
    "translation_split_sentences": True,
    "translation_preserve_formatting": True,
    "clipboard_enabled": True,
    "clipboard_auto_paste": False,
    "clipboard_close_after_paste": True,
    "clipboard_history_limit": 100,
    "clipboard_auto_cleanup": False,
    "magnifier_color_copy_format": "rgb_hex",
    "ui_theme_mode": "system",
    "ui_scale_percent": 100,
    "dialog_scale_percent": 100,
    "inapp_confirm": "ctrl+c",
    "inapp_pin": "ctrl+d",
    "inapp_undo": "ctrl+z",
    "inapp_redo": "ctrl+y",
    "inapp_delete": "delete",
    "inapp_copy_pin": "ctrl+c",
    "inapp_copy_pin_text": "ctrl+shift+c",
    "inapp_pin_reset_size": "mousemiddle",
    "inapp_thumbnail": "r",
    "inapp_toggle_toolbar": "space",
    "inapp_zoom_in": "pageup",
    "inapp_zoom_out": "pagedown",
    "inapp_translate": "shift+c",
    "inapp_text_recognize": "shift+t",
    "inapp_cursor_move_mode": "both",
    **{key: default for key, _tool, _label, default in ANNOTATION_TOOL_SHORTCUTS},
}


class _NullConfig:
    """给 create_default_translation_service 用的空壳。

    不能把 MockConfig 自己传进去：字段表是在 MockConfig.__getattr__ 里查的，
    而建服务又要读 config，一旦读到缺的属性就绕回 __getattr__，直接无限递归。
    """

    def get_translation_provider(self):
        return ""

    def get_translation_provider_config(self, provider_id):
        return {}


_FIELDS_CACHE = None


def _translation_fields():
    """{config_key: Field}，来自注册表里的全部翻译服务商。

    走注册表而不是在这里再列一遍 provider 类——那样就只是把手写清单从一个
    文件挪到另一个文件，新增一家照样要记得回来补。
    """
    global _FIELDS_CACHE
    if _FIELDS_CACHE is not None:
        return _FIELDS_CACHE
    fields = {}
    try:
        from translation.service import create_default_translation_service

        registry = create_default_translation_service(_NullConfig()).registry
        for meta in registry.available_providers():
            for f in tuple(meta.credentials) + tuple(meta.options):
                fields[f.config_key] = f
    except Exception:
        fields = {}
    _FIELDS_CACHE = fields
    return _FIELDS_CACHE


class MockConfig:
    APP_DEFAULT_SETTINGS = APP_DEFAULT_SETTINGS

    def __init__(self):
        self.settings = QSettings("TestApp", "Settings")
        self.qsettings = self.settings

    # --- getter / setter stubs ---
    def get_double_click_copy_close_enabled(self): return True
    def set_double_click_copy_close_enabled(self, v): pass
    def get_cross_tool_selection_enabled(self): return True
    def set_cross_tool_selection_enabled(self, v): pass
    def get_text_always_on_top_enabled(self): return True
    def set_text_always_on_top_enabled(self, v): pass
    def get_smart_selection(self): return False
    def set_smart_selection(self, v): pass
    def get_smart_selection_mode(self): return "window"
    def set_smart_selection_mode(self, v): pass
    def get_smart_selection_animation(self): return False
    def set_smart_selection_animation(self, v): pass
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
    def get_translation_provider(self): return "google"
    def set_translation_provider(self, v): pass
    def get_translation_provider_config(self, provider_id):
        # 预览用，值都为空即可；键名沿用 provider 自己的 config 约定
        try:
            from translation.service import create_default_translation_service
            meta = create_default_translation_service(
                _NullConfig()
            ).registry.metadata(provider_id)
        except Exception:
            return {}
        return {f.config_key: "" for f in meta.credentials}

    def __getattr__(self, name):
        """翻译服务商的 get_/set_ 一律按注册表的字段声明兜底。

        以前这里是一家家手写的 getter/setter。新增服务商时漏补两行不会有任何
        提示，直到设置页调到缺的方法——这个类没有别的兜底，那就是一个
        AttributeError，设置页的独立预览入口直接崩在构造阶段。baidu 就这么崩过。

        只认已注册字段，不认的名字照常抛 AttributeError：把所有 get_* 都变成
        合法调用会让真正的拼写错误变得无声无息。
        """
        if name.startswith(("get_", "set_")):
            key = name[4:]
            field = _translation_fields().get(key)
            if field is not None:
                if name.startswith("set_"):
                    return lambda _value: None
                # 默认值按字段类型给：开关给 False，文本给空串。
                # 统一给 "" 的话，新加的开关会拿到字符串，setChecked("")
                # 虽然不报错但语义已经错了。
                from translation.provider import ToggleField
                fallback = False if isinstance(field, ToggleField) else ""
                return lambda: APP_DEFAULT_SETTINGS.get(key, fallback)
        raise AttributeError(name)

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
    def get_pin_clipboard_hotkey(self): return ""
    def set_pin_clipboard_hotkey(self, v): pass
    def get_pin_clipboard_hotkey_2(self): return ""
    def set_pin_clipboard_hotkey_2(self, v): pass
    def get_translation_hotkey(self): return ""
    def set_translation_hotkey(self, v): pass
    def get_translation_hotkey_2(self): return ""
    def set_translation_hotkey_2(self, v): pass
    def get_clipboard_enabled(self): return True
    def set_clipboard_enabled(self, v): pass
    def get_clipboard_auto_paste(self): return False
    def set_clipboard_auto_paste(self, v): pass
    def get_clipboard_close_after_paste(self): return True
    def set_clipboard_close_after_paste(self, v): pass
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
    app.setFont(QFont("Microsoft YaHei", 10))

    from .dialog import SettingsDialog
    dlg = SettingsDialog(MockConfig())
    dlg.show()
    sys.exit(app.exec())
