# -*- coding: utf-8 -*-
"""
languages.py - 翻译语言配置

统一管理所有翻译相关的语言列表，避免代码重复。
"""

# DeepL 支持的目标语言列表（使用原生语言名称）
TRANSLATION_LANGUAGES = {
    "ZH": "中文",
    "ZH-HANT": "繁体中文",
    "EN": "English",
    "JA": "日本語",
    "KO": "한국어",
    "VI": "Tiếng Việt",
    "DE": "Deutsch",
    "FR": "Français",
    "ES": "Español",
    "IT": "Italiano",
    "PT": "Português",
    "RU": "Русский",
    "PL": "Polski",
    "NL": "Nederlands",
    "TR": "Türkçe",
    "TH": "ไทย",
    "AR": "العربية",
    "ID": "Bahasa Indonesia",
    "UK": "Українська",
}

# 源语言列表（包含自动检测）
SOURCE_LANGUAGES = {
    "auto": "Auto Detect",  # 这个会被翻译
    **TRANSLATION_LANGUAGES
}
 