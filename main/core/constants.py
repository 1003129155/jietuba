# -*- coding: utf-8 -*-
"""
全局常量。
"""

# ── CSS font-family 值 ────────────────────────────────────
# 用于 QSS stylesheet 中的 font-family 属性（大部分 UI 控件）
CSS_FONT_FAMILY = '"Microsoft YaHei", "SimSun", Arial, sans-serif'

# 用于系统级 UI 元素（托盘菜单、上下文菜单等）
CSS_FONT_FAMILY_UI = '"Microsoft YaHei UI", "Segoe UI", sans-serif'

# QFont 构造时使用的默认字体族名
DEFAULT_FONT_FAMILY = "Microsoft YaHei"

# Fonts exposed by the text annotation tool. Keep this list small so the app
# never needs to scan the full system font database during startup.
COMMON_TEXT_FONTS = [
    "Microsoft YaHei UI",
    "SimSun",
    "Segoe UI",
    "Arial",
    "Yu Gothic UI",
    "Meiryo",
    "Microsoft JhengHei UI",
    "PMingLiU",
]

DEFAULT_TEXT_FONT_BY_LANGUAGE = {
    "zh": "Microsoft YaHei UI",
    "zh_CN": "Microsoft YaHei UI",
    "zh_TW": "Microsoft JhengHei UI",
    "en": "Segoe UI",
    "ja": "Yu Gothic UI",
}

_SYSTEM_DEFAULT_TEXT_FONT_LOGGED = False


def get_default_text_font_for_language(language_code: str | None = None) -> str:
    """Return the text-tool default font for the current UI language."""
    if language_code is None:
        try:
            from core.i18n import I18nManager
            language_code = I18nManager.get_current_language()
        except Exception:
            language_code = ""

    return DEFAULT_TEXT_FONT_BY_LANGUAGE.get(
        language_code or "",
        get_system_default_text_font_family(),
    )


def get_system_default_text_font_family() -> str:
    """Return Qt's system general font without enumerating all font families."""
    global _SYSTEM_DEFAULT_TEXT_FONT_LOGGED
    try:
        from PySide6.QtGui import QFontDatabase
        family = QFontDatabase.systemFont(
            QFontDatabase.SystemFont.GeneralFont
        ).family()
        family = family or COMMON_TEXT_FONTS[0]
    except Exception:
        family = COMMON_TEXT_FONTS[0]

    if not _SYSTEM_DEFAULT_TEXT_FONT_LOGGED:
        try:
            from core.logger import log_debug
            log_debug(f"系统默认文字字体: {family}", "Font")
        except Exception:
            pass
        _SYSTEM_DEFAULT_TEXT_FONT_LOGGED = True

    return family


def get_available_text_fonts() -> list[str]:
    """Return text-tool fonts plus the system default, deduplicated."""
    fonts = list(COMMON_TEXT_FONTS)
    system_font = get_system_default_text_font_family()
    if system_font and system_font not in fonts:
        fonts.insert(0, system_font)
    return fonts


def normalize_text_font_family(font_family: str | None, language_code: str | None = None) -> str:
    """Clamp text-tool fonts to the approved lightweight whitelist."""
    if font_family in get_available_text_fonts():
        return font_family
    return get_default_text_font_for_language(language_code)

# 项目主页（欢迎页与设置“关于”页共用）
PROJECT_GITHUB_URL = "https://github.com/1003129155/jietuba"
 
