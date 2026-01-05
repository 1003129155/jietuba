# -*- coding: utf-8 -*-
"""
i18n.py - 国际化翻译管理器

使用自定义的 XML 翻译系统实现多语言支持。

使用方法:
1. 在 UI 组件中使用 self.tr("text") 包装所有需要翻译的文本
2. 编辑 translations/ 目录下的 XML 文件进行翻译
3. XML 文件使用 Qt Linguist 的 .ts 格式
"""
from PyQt6.QtCore import QTranslator, QLocale, QCoreApplication, QObject, pyqtSignal
from PyQt6.QtWidgets import QApplication
from pathlib import Path
import sys
import os
import xml.etree.ElementTree as ET


class I18nSignals(QObject):
    """用于发出语言切换信号的 QObject"""
    language_changed = pyqtSignal(str)


class XmlTranslator(QTranslator):
    """
    自定义翻译器，从 XML (.ts) 文件加载翻译
    """
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self._translations = {}  # {context: {source: translation}}
    
    def load_from_xml(self, xml_path: str) -> bool:
        """
        从 XML 文件加载翻译
        
        Args:
            xml_path: XML 文件路径
            
        Returns:
            是否加载成功
        """
        try:
            tree = ET.parse(xml_path)
            root = tree.getroot()
            
            self._translations.clear()
            
            for context_elem in root.findall('context'):
                context_name = context_elem.find('name')
                if context_name is None:
                    continue
                context = context_name.text or ""
                
                if context not in self._translations:
                    self._translations[context] = {}
                
                for message in context_elem.findall('message'):
                    source_elem = message.find('source')
                    translation_elem = message.find('translation')
                    
                    if source_elem is not None and translation_elem is not None:
                        source = source_elem.text or ""
                        translation = translation_elem.text or source  # 如果没有翻译，使用原文
                        
                        # 检查翻译是否被标记为 unfinished
                        if translation_elem.get('type') == 'unfinished':
                            translation = source  # 未完成的翻译使用原文
                        
                        self._translations[context][source] = translation
            
            return True
            
        except Exception as e:
            print(f"[ERROR] [I18n] 加载 XML 翻译文件失败: {e}")
            return False
    
    def translate(self, context: str, sourceText: str, disambiguation: str = None, n: int = -1) -> str:
        """
        重写 QTranslator.translate() 方法
        
        Args:
            context: 翻译上下文（通常是类名）
            sourceText: 原始文本
            disambiguation: 消歧义字符串
            n: 用于复数形式
            
        Returns:
            翻译后的文本，如果没有找到则返回空字符串
        """
        if context in self._translations:
            if sourceText in self._translations[context]:
                return self._translations[context][sourceText]
        
        # 尝试不带上下文的查找（有些翻译可能共享）
        for ctx_translations in self._translations.values():
            if sourceText in ctx_translations:
                return ctx_translations[sourceText]
        
        return ""  # 返回空字符串表示没有翻译，Qt 将使用原文


class I18nManager:
    """
    翻译管理器 - 单例模式
    
    管理应用程序的多语言翻译，支持动态切换语言。
    """
    
    _instance = None
    _translator: XmlTranslator = None
    _current_lang: str = "ja"  # 默认日文
    _signals: I18nSignals = None
    
    # 支持的语言列表
    LANGUAGES = {
        "ja": "日本語",
        "en": "English",
        "zh": "简体中文",
    }
    
    def __init__(self):
        # 不直接调用，使用 instance() 方法
        self._translator = XmlTranslator()
        self._signals = I18nSignals()
    
    @classmethod
    def instance(cls) -> 'I18nManager':
        """获取单例实例"""
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance
    
    @property
    def language_changed(self):
        """语言切换信号，用于通知所有窗口刷新翻译"""
        return self._signals.language_changed
    
    @classmethod
    def get_translations_dir(cls) -> Path:
        """
        获取翻译文件目录
        
        支持开发环境和 PyInstaller 打包后的环境
        """
        if getattr(sys, 'frozen', False):
            # PyInstaller 打包后的环境
            base = Path(sys._MEIPASS)
            # 尝试多个可能的路径
            paths = [
                base / "main" / "translations",  # 标准打包路径
                base / "translations",            # 备用路径
            ]
            for path in paths:
                if path.exists():
                    return path
            return base / "main" / "translations"  # 默认返回标准路径
        else:
            # 开发环境
            base = Path(__file__).parent.parent
            return base / "translations"
    
    @classmethod
    def load_language(cls, lang_code: str) -> bool:
        """
        加载指定语言的翻译
        
        优先加载编译后的 .qm 文件（加载更快），如果不存在则回退到 .xml 文件
        
        Args:
            lang_code: 语言代码 (ja/en/zh)
            
        Returns:
            是否加载成功
        """
        if lang_code not in cls.LANGUAGES:
            print(f"[WARN] [I18n] 不支持的语言: {lang_code}")
            return False
        
        app = QApplication.instance()
        if not app:
            print("[WARN] [I18n] QApplication 实例不存在")
            return False
        
        instance = cls.instance()
        
        # 移除旧的翻译器
        if instance._translator:
            app.removeTranslator(instance._translator)
        
        # 构建文件路径
        translations_dir = cls.get_translations_dir()
        qm_file = translations_dir / f"app_{lang_code}.qm"
        xml_file = translations_dir / f"app_{lang_code}.xml"
        
        # 优先尝试加载 .qm 文件（编译后的二进制格式，加载更快）
        if qm_file.exists():
            qt_translator = QTranslator()
            if qt_translator.load(str(qm_file)):
                instance._translator = qt_translator
                app.installTranslator(instance._translator)
                cls._current_lang = lang_code
                print(f"[OK] [I18n] 已加载语言 (QM): {cls.LANGUAGES[lang_code]} ({lang_code})")
                instance.language_changed.emit(lang_code)
                return True
            else:
                print(f"[WARN] [I18n] QM 文件加载失败，尝试 XML: {qm_file}")
        
        # 回退到 XML 文件
        instance._translator = XmlTranslator()
        if xml_file.exists():
            if instance._translator.load_from_xml(str(xml_file)):
                app.installTranslator(instance._translator)
                cls._current_lang = lang_code
                print(f"[OK] [I18n] 已加载语言 (XML): {cls.LANGUAGES[lang_code]} ({lang_code})")
                instance.language_changed.emit(lang_code)
                return True
            else:
                print(f"[ERROR] [I18n] 加载翻译文件失败: {xml_file}")
        else:
            # 翻译文件不存在时，使用默认语言（代码中的原始文本）
            cls._current_lang = lang_code
            print(f"ℹ️ [I18n] 翻译文件不存在: {xml_file}，使用默认文本")
            instance.language_changed.emit(lang_code)
            return True
        
        return False
    
    @classmethod
    def get_current_language(cls) -> str:
        """获取当前语言代码"""
        return cls._current_lang
    
    @classmethod
    def get_current_language_name(cls) -> str:
        """获取当前语言的显示名称"""
        return cls.LANGUAGES.get(cls._current_lang, cls._current_lang)
    
    @classmethod
    def get_available_languages(cls) -> dict:
        """获取所有支持的语言"""
        return cls.LANGUAGES.copy()
    
    @classmethod
    def get_system_language(cls) -> str:
        """
        获取系统语言，返回对应的语言代码
        
        如果系统语言不在支持列表中，返回英语 'en' (国际通用语言)
        """
        locale = QLocale.system()
        lang = locale.language()
        
        # 映射 Qt 语言枚举到语言代码
        lang_map = {
            QLocale.Language.Japanese: "ja",
            QLocale.Language.English: "en",
            QLocale.Language.Chinese: "zh",
        }
        
        # 不支持的语言默认使用英文
        return lang_map.get(lang, "en")


# 便捷函数，用于在非 QWidget 类中进行翻译
def tr(text: str, context: str = "I18nManager") -> str:
    """
    翻译文本的便捷函数
    
    注意：在 QWidget 子类中，应优先使用 self.tr() 方法
    
    Args:
        text: 要翻译的原始文本
        context: 翻译上下文（用于区分相同文本的不同含义）
        
    Returns:
        翻译后的文本
    """
    return QCoreApplication.translate(context, text)


# 注意：不要在模块级别初始化单例，因为此时 QApplication 可能还不存在
# 使用 I18nManager.instance() 来获取实例
