# -*- coding: utf-8 -*-
"""
翻译模块 - 提供文字翻译功能

主要组件:
- DeepLService: DeepL API 调用服务
- TranslationDialog: 翻译结果显示窗口
- TranslationManager: 翻译窗口单例管理器（推荐使用）
"""

from .deepl_service import DeepLService, TranslationThread
from .translation_dialog import TranslationDialog, TranslationLoadingDialog
from .translation_manager import TranslationManager

__all__ = [
    'DeepLService', 
    'TranslationThread', 
    'TranslationDialog',
    'TranslationLoadingDialog',
    'TranslationManager'
]
