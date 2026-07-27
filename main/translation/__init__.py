# -*- coding: utf-8 -*-
"""
翻译模块 - 提供文字翻译功能

主要组件:
- DeepLService: DeepL API 调用服务
- TranslationDialog: 翻译结果显示窗口
- TranslationManager: 翻译窗口单例管理器（推荐使用）
"""

from .deepl_service import DeepLService, TranslationThread
from .models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
)
from .provider import ProviderMetadata, TranslationProvider
from .registry import ProviderRegistry
from .service import TranslationService, create_default_translation_service
from .translation_dialog import TranslationDialog, TranslationLoadingDialog
from .translation_manager import TranslationManager
from .worker import TranslationWorker

__all__ = [
    'DeepLService', 
    'TranslationThread', 
    'TranslationDialog',
    'TranslationLoadingDialog',
    'TranslationManager',
    'TranslationErrorCode',
    'TranslationRequest',
    'TranslationResult',
    'ProviderMetadata',
    'TranslationProvider',
    'ProviderRegistry',
    'TranslationService',
    'create_default_translation_service',
    'TranslationWorker',
]
 
