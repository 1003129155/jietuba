# -*- coding: utf-8 -*-
"""
translation_manager.py - 翻译窗口单例管理器

负责管理全局唯一的翻译窗口，实现与钉图窗口的解耦。

设计特点：
1. 单例模式 - 全局最多存在一个翻译窗口
2. 复用窗口 - 多个钉图点击翻译时复用同一窗口
3. 关闭即清理 - 窗口关闭时释放内存
4. 解耦设计 - 钉图窗口无需直接管理翻译窗口

使用方式：
    from translation import TranslationManager
    
    # 获取单例（不会创建窗口）
    manager = TranslationManager.instance()
    
    # 翻译文本（创建或复用窗口）
    manager.translate(
        text="Hello World",
        api_key="your-deepl-key",
        target_lang="ZH",
        position=QPoint(100, 100)  # 可选，窗口位置
    )
"""

from typing import Optional
from PyQt6.QtCore import QObject, QPoint, pyqtSignal
from PyQt6.QtWidgets import QApplication

from core import log_info, log_debug, log_error


class TranslationManager(QObject):
    """翻译窗口单例管理器"""
    
    _instance: Optional['TranslationManager'] = None
    
    # 信号
    translation_started = pyqtSignal(str)  # 开始翻译，参数为原文
    translation_finished = pyqtSignal(bool, str, str)  # 翻译完成(成功, 译文, 错误信息)
    
    def __init__(self):
        super().__init__()
        self._dialog = None  # TranslationLoadingDialog 实例
        self._thread = None  # TranslationThread 实例
        self._api_key = ""
        self._use_pro = False
        self._split_sentences = "nonewlines"  # 分句模式: "0"=不分句, "1"=自动分句, "nonewlines"=忽略换行
        self._preserve_formatting = True  # 保留格式
        
        log_debug("TranslationManager 已初始化", "Translation")
    
    @classmethod
    def instance(cls) -> 'TranslationManager':
        """获取单例实例"""
        if cls._instance is None:
            cls._instance = TranslationManager()
        return cls._instance
    
    @classmethod
    def has_instance(cls) -> bool:
        """检查单例是否已创建"""
        return cls._instance is not None
    
    def configure(self, api_key: str, use_pro: bool = False, 
                  split_sentences: str = "nonewlines", preserve_formatting: bool = True):
        """
        配置翻译服务
        
        Args:
            api_key: DeepL API 密钥
            use_pro: 是否使用 Pro 版 API
            split_sentences: 分句模式 ("0"=不分句, "1"=自动分句, "nonewlines"=忽略换行)
            preserve_formatting: 保留格式
        """
        self._api_key = api_key
        self._use_pro = use_pro
        self._split_sentences = split_sentences
        self._preserve_formatting = preserve_formatting
        log_debug(f"翻译服务已配置 (Pro: {use_pro}, split_sentences: {split_sentences}, preserve_formatting: {preserve_formatting})", "Translation")
    
    def translate(
        self,
        text: str,
        api_key: str = None,
        target_lang: str = "ZH",
        source_lang: str = None,
        position: QPoint = None,
        use_pro: bool = None,
        split_sentences: str = None,
        preserve_formatting: bool = None
    ):
        """
        翻译文本
        
        如果翻译窗口不存在，创建新窗口；
        如果已存在，复用并更新内容。
        
        Args:
            text: 要翻译的文本（可为空，此时只显示窗口不翻译）
            api_key: DeepL API 密钥（可选，不传则使用 configure 配置的）
            target_lang: 目标语言代码
            source_lang: 源语言代码（可选，不传则自动检测）
            position: 窗口位置（可选）
            use_pro: 是否使用 Pro 版 API（可选）
            split_sentences: 分句模式 ("0"/"1"/"nonewlines")（可选）
            preserve_formatting: 保留格式（可选）
        """
        # 使用传入的参数或已配置的参数
        api_key = api_key or self._api_key
        if use_pro is None:
            use_pro = self._use_pro
        if split_sentences is None:
            split_sentences = self._split_sentences
        if preserve_formatting is None:
            preserve_formatting = self._preserve_formatting
        
        # 保存配置供后续翻译使用
        if api_key:
            self._api_key = api_key
        if use_pro is not None:
            self._use_pro = use_pro
        self._split_sentences = split_sentences
        self._preserve_formatting = preserve_formatting
        
        if not api_key:
            log_error("未配置 DeepL API 密钥", "Translation")
            self._show_error_dialog(self.tr("Please configure DeepL API key in settings first"))
            return
        
        # 停止之前的翻译线程
        self._stop_current_thread()
        
        # 创建或复用窗口
        self._ensure_dialog(text, position, source_lang, target_lang)
        
        # 如果有文本，启动翻译；否则只显示空窗口
        if text and text.strip():
            log_info(f"开始翻译: {text[:50]}...", "Translation")
            self.translation_started.emit(text)
            
            # 使用窗口中用户选择的目标语言，而不是传入的默认值
            actual_target_lang = target_lang
            if self._is_dialog_valid():
                actual_target_lang = self._dialog.get_target_lang() or target_lang
            
            self._start_translation(text, api_key, actual_target_lang, source_lang, use_pro)
        else:
            log_info("打开翻译窗口（待用户输入）", "Translation")
            # 清空译文区域，等待用户输入
            if self._is_dialog_valid():
                self._dialog.target_edit.clear()
    
    def _ensure_dialog(
        self,
        text: str,
        position: QPoint,
        source_lang: str,
        target_lang: str
    ):
        """确保翻译窗口存在（创建或复用）"""
        from .translation_dialog import TranslationLoadingDialog
        
        if self._dialog is None or not self._is_dialog_valid():
            # 创建新窗口
            log_debug("创建新翻译窗口", "Translation")
            self._dialog = TranslationLoadingDialog(
                original_text=text,
                position=position,
                source_lang=source_lang or "auto",
                target_lang=target_lang
            )
            # 连接关闭信号
            self._dialog.destroyed.connect(self._on_dialog_destroyed)
            # 连接翻译信号 (text, source_lang, target_lang)
            self._dialog.translate_requested.connect(self._on_translate_requested)
            self._dialog.show()
        else:
            # 复用现有窗口 - 保留用户选择的目标语言
            log_debug("复用现有翻译窗口", "Translation")
            self._dialog.original_text = text
            self._dialog.source_lang = source_lang or "auto"
            # 不覆盖 target_lang，保留用户在 ComboBox 中选择的语言
            self._dialog.source_edit.setPlainText(text)
            
            # 只有有文本时才显示加载状态
            if text and text.strip():
                self._dialog.set_loading()
            
            # 更新位置（如果提供）
            if position:
                self._dialog.move(position)
            
            # 激活窗口
            self._dialog.raise_()
            self._dialog.activateWindow()
    
    def _is_dialog_valid(self) -> bool:
        """检查对话框是否有效（未被删除）"""
        if self._dialog is None:
            return False
        try:
            # 尝试访问对话框属性，如果已删除会抛出 RuntimeError
            _ = self._dialog.isVisible()
            return True
        except RuntimeError:
            return False
    
    def _start_translation(
        self,
        text: str,
        api_key: str,
        target_lang: str,
        source_lang: str,
        use_pro: bool
    ):
        """启动翻译线程"""
        from .deepl_service import TranslationThread
        
        log_info(f"调用 DeepL API: target={target_lang}, use_pro={use_pro}, split_sentences={self._split_sentences}, preserve_formatting={self._preserve_formatting}", "DeepL")
        
        self._thread = TranslationThread(
            text=text,
            api_key=api_key,
            target_lang=target_lang,
            use_pro=use_pro,
            split_sentences=self._split_sentences,
            preserve_formatting=self._preserve_formatting
        )
        self._thread.finished_signal.connect(self._on_translation_finished)
        self._thread.start()
    
    def _stop_current_thread(self):
        """停止当前翻译线程"""
        if self._thread is not None:
            if self._thread.isRunning():
                self._thread.quit()
                self._thread.wait(1000)  # 等待最多 1 秒
            self._thread.deleteLater()
            self._thread = None
    
    def _on_translation_finished(self, success: bool, translated_text: str, error: str, detected_lang: str):
        """翻译完成回调"""
        log_debug(f"翻译完成: success={success}, detected_lang={detected_lang}", "Translation")
        
        if self._is_dialog_valid():
            self._dialog.on_translation_finished(success, translated_text, error, detected_lang)
            # 恢复翻译按钮状态
            if hasattr(self._dialog, 'translate_btn'):
                self._dialog.translate_btn.setEnabled(True)
                self._dialog.translate_btn.setText(self._dialog.tr("Translate") + " →")
            if success and detected_lang:
                self._dialog._detected_source_lang = detected_lang
        
        self.translation_finished.emit(success, translated_text, error)
        
        # 清理线程
        if self._thread:
            self._thread.deleteLater()
            self._thread = None
    
    def _on_translate_requested(self, text: str, source_lang: str, target_lang: str):
        """处理翻译请求（来自翻译窗口的翻译按钮）"""
        if not self._api_key:
            if self._is_dialog_valid():
                self._dialog.set_translation_error(self.tr("API key not configured"))
            return
        
        if not text or not text.strip():
            if self._is_dialog_valid():
                self._dialog.set_translation_error(self.tr("Please enter text to translate"))
            return
        
        log_debug(f"翻译请求: -> {target_lang}", "Translation")
        
        # 停止当前线程
        self._stop_current_thread()
        
        # 启动翻译
        from .deepl_service import TranslationThread
        
        log_info(f"调用 DeepL API: target={target_lang}, use_pro={self._use_pro}, split_sentences={self._split_sentences}, preserve_formatting={self._preserve_formatting}", "DeepL")
        
        self._thread = TranslationThread(
            text=text,
            api_key=self._api_key,
            target_lang=target_lang,
            use_pro=self._use_pro,
            split_sentences=self._split_sentences,
            preserve_formatting=self._preserve_formatting
        )
        self._thread.finished_signal.connect(self._on_translation_finished)
        self._thread.start()
    
    def _on_dialog_destroyed(self):
        """窗口被销毁时的清理"""
        log_debug("翻译窗口已关闭，清理资源", "Translation")
        self._dialog = None
        self._stop_current_thread()
    
    def _show_error_dialog(self, message: str):
        """显示错误对话框"""
        from PyQt6.QtWidgets import QMessageBox
        QMessageBox.warning(None, self.tr("Translation Error"), message)
    
    def close_dialog(self):
        """主动关闭翻译窗口"""
        if self._is_dialog_valid():
            self._dialog.close()
        self._dialog = None
        self._stop_current_thread()
    
    def is_dialog_open(self) -> bool:
        """检查翻译窗口是否打开"""
        return self._is_dialog_valid() and self._dialog.isVisible()
    
    @classmethod
    def cleanup(cls):
        """清理单例（程序退出时调用）"""
        if cls._instance is not None:
            cls._instance.close_dialog()
            cls._instance = None
            log_debug("TranslationManager 已清理", "Translation")
    
    def translate_from_image(
        self,
        pixmap,
        api_key: str = None,
        target_lang: str = "ZH",
        use_pro: bool = None,
        split_sentences: str = None,
        preserve_formatting: bool = None
    ):
        """
        从图片进行OCR识别后翻译
        
        流程：
        1. 立即显示翻译窗口（原文区显示"识别中..."）
        2. 后台线程进行OCR识别
        3. 识别完成后填入原文
        4. 自动调用翻译API
        
        Args:
            pixmap: QPixmap 图片（已是独立副本）
            api_key: DeepL API 密钥
            target_lang: 目标语言代码
            use_pro: 是否使用 Pro 版 API
            split_sentences: 分句模式
            preserve_formatting: 保留格式
        """
        from PyQt6.QtGui import QPixmap
        
        # 使用传入的参数或已配置的参数
        api_key = api_key or self._api_key
        if use_pro is None:
            use_pro = self._use_pro
        if split_sentences is None:
            split_sentences = self._split_sentences
        if preserve_formatting is None:
            preserve_formatting = self._preserve_formatting
        
        # 保存配置
        if api_key:
            self._api_key = api_key
        if use_pro is not None:
            self._use_pro = use_pro
        self._split_sentences = split_sentences
        self._preserve_formatting = preserve_formatting
        
        # 保存目标语言和pixmap供OCR完成后使用
        self._pending_target_lang = target_lang
        self._pending_pixmap = pixmap
        
        log_info("截图翻译模式：显示窗口并启动OCR", "Translation")
        
        # 1. 显示翻译窗口（原文区显示"识别中..."）
        self._ensure_dialog(
            text="",  # 初始为空
            position=None,
            source_lang="auto",
            target_lang=target_lang
        )
        
        # 设置原文区为"识别中..."状态
        if self._is_dialog_valid():
            self._dialog.source_edit.setPlainText(self._dialog.tr("Recognizing..."))
            self._dialog.source_edit.setEnabled(False)  # 暂时禁用编辑
        
        # 2. 启动OCR线程
        self._start_ocr_thread(pixmap)
    
    def _start_ocr_thread(self, pixmap):
        """启动OCR识别线程"""
        from PyQt6.QtCore import QThread, pyqtSignal
        
        class OCRThread(QThread):
            """OCR识别线程"""
            finished_signal = pyqtSignal(bool, str)  # (成功, 识别文本或错误信息)
            
            def __init__(self, pixmap):
                super().__init__()
                self._pixmap = pixmap
            
            def run(self):
                try:
                    from ocr import is_ocr_available, recognize_text, format_ocr_result_text
                    
                    if not is_ocr_available():
                        self.finished_signal.emit(False, "OCR功能不可用")
                        return
                    
                    # 执行OCR识别，使用dict格式获取完整信息（含坐标）
                    result = recognize_text(self._pixmap, return_format="dict")
                    
                    if result and isinstance(result, dict) and result.get('code') == 100:
                        # 使用公共函数处理：按阅读顺序，同行合并
                        text = format_ocr_result_text(result)
                        if text and text.strip():
                            self.finished_signal.emit(True, text)
                        else:
                            self.finished_signal.emit(False, "未识别到文字")
                    else:
                        self.finished_signal.emit(False, "未识别到文字")
                        
                except Exception as e:
                    self.finished_signal.emit(False, f"OCR识别失败: {str(e)}")
        
        # 停止之前的OCR线程
        if hasattr(self, '_ocr_thread') and self._ocr_thread and self._ocr_thread.isRunning():
            self._ocr_thread.terminate()
            self._ocr_thread.wait()
        
        # 创建并启动新线程
        self._ocr_thread = OCRThread(pixmap)
        # 🚀 降低线程优先级，避免与UI线程竞争CPU导致卡顿
        self._ocr_thread.setPriority(QThread.Priority.LowPriority)
        self._ocr_thread.finished_signal.connect(self._on_ocr_finished)
        self._ocr_thread.start()
        
        log_debug("OCR线程已启动 (LowPriority)", "Translation")
    
    def _on_ocr_finished(self, success: bool, result: str):
        """OCR识别完成回调"""
        log_debug(f"OCR完成: success={success}, result_len={len(result) if result else 0}", "Translation")
        
        # 清理pixmap引用
        self._pending_pixmap = None
        
        if not self._is_dialog_valid():
            log_debug("翻译窗口已关闭，忽略OCR结果", "Translation")
            return
        
        # 恢复编辑状态
        self._dialog.source_edit.setEnabled(True)
        
        if success and result:
            # 填入识别的文本
            self._dialog.source_edit.setPlainText(result)
            log_info(f"OCR识别成功: {result[:50]}...", "Translation")
            
            # 自动开始翻译
            target_lang = getattr(self, '_pending_target_lang', "ZH")
            self._start_translation(
                text=result,
                api_key=self._api_key,
                target_lang=self._dialog.get_target_lang() or target_lang,
                source_lang="auto",
                use_pro=self._use_pro
            )
        else:
            # 显示错误信息
            self._dialog.source_edit.setPlainText("")
            self._dialog.source_edit.setPlaceholderText(result or "识别失败")
            log_error(f"OCR识别失败: {result}", "Translation")
