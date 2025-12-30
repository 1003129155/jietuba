"""
钉图翻译功能

负责处理钉图窗口的翻译相关功能
"""

from PyQt6.QtWidgets import QWidget, QMessageBox, QApplication
from PyQt6.QtCore import Qt, QPoint
from core import log_info, log_warning, log_error


class PinTranslationHelper:
    """
    钉图翻译助手
    
    管理翻译相关的逻辑：获取 OCR 文本、调用翻译服务、计算窗口位置等
    """
    
    def __init__(self, parent: QWidget, config_manager):
        """
        初始化翻译助手
        
        Args:
            parent: 父窗口（PinWindow）
            config_manager: 配置管理器
        """
        self.parent = parent
        self.config_manager = config_manager
    
    def translate(self, ocr_text_layer) -> bool:
        """
        执行翻译
        
        Args:
            ocr_text_layer: OCR 文字层对象
            
        Returns:
            是否成功启动翻译
        """
        try:
            # 1. 检查是否有 OCR 结果
            if not ocr_text_layer or not ocr_text_layer.has_text():
                log_warning("没有可翻译的文字", "Translate")
                return False
            
            # 2. 获取所有 OCR 文字
            all_text = ocr_text_layer.get_all_text(separator="\n")
            if not all_text.strip():
                log_warning("OCR 文字为空", "Translate")
                return False
            
            log_info(f"准备翻译 {len(all_text)} 个字符", "Translate")
            
            # 3. 获取 DeepL API 密钥
            api_key = self._get_deepl_api_key()
            if not api_key:
                log_error("DeepL API 密钥未配置", "Translate")
                self._show_error("请先在设置中配置 DeepL API 密钥")
                return False
            
            # 4. 获取目标语言
            target_lang = self._get_translation_target_lang()
            
            # 5. 获取翻译参数设置
            split_sentences, preserve_formatting = self._get_translation_params()
            
            # 6. 使用 TranslationManager 单例进行翻译
            from translation import TranslationManager
            
            # 智能计算窗口位置
            dialog_pos = self._calculate_window_position()
            
            # 获取单例并翻译
            manager = TranslationManager.instance()
            manager.translate(
                text=all_text,
                api_key=api_key,
                target_lang=target_lang,
                position=dialog_pos,
                use_pro=self._is_deepl_pro(),
                split_sentences=split_sentences,
                preserve_formatting=preserve_formatting
            )
            
            return True
            
        except Exception as e:
            log_error(f"翻译启动失败: {e}", "Translate")
            import traceback
            traceback.print_exc()
            return False
    
    def _get_deepl_api_key(self) -> str:
        """获取 DeepL API 密钥"""
        if self.config_manager:
            if hasattr(self.config_manager, 'get_deepl_api_key'):
                return self.config_manager.get_deepl_api_key() or ""
        return ""
    
    def _get_translation_target_lang(self) -> str:
        """获取翻译目标语言"""
        if self.config_manager:
            if hasattr(self.config_manager, 'get_translation_target_lang'):
                return self.config_manager.get_translation_target_lang() or "ZH"
        return "ZH"
    
    def _is_deepl_pro(self) -> bool:
        """是否使用 DeepL Pro API"""
        if self.config_manager:
            if hasattr(self.config_manager, 'get_deepl_use_pro'):
                return self.config_manager.get_deepl_use_pro()
        return False
    
    def _get_translation_params(self) -> tuple:
        """
        获取翻译参数设置
        
        Returns:
            (split_sentences, preserve_formatting) 元组
        """
        split_sentences_enabled = True
        preserve_formatting = True
        
        if self.config_manager:
            if hasattr(self.config_manager, 'get_translation_split_sentences'):
                split_sentences_enabled = self.config_manager.get_translation_split_sentences()
            if hasattr(self.config_manager, 'get_translation_preserve_formatting'):
                preserve_formatting = self.config_manager.get_translation_preserve_formatting()
        
        # 转换为 DeepL API 参数
        split_sentences = "nonewlines" if split_sentences_enabled else "0"
        
        return split_sentences, preserve_formatting
    
    def _calculate_window_position(self) -> QPoint:
        """
        智能计算翻译窗口位置
        
        优先放在钉图窗口右侧，如果右侧空间不足则放在左侧
        同时确保垂直方向不超出屏幕
        
        Returns:
            翻译窗口的全局坐标位置
        """
        # 翻译窗口的预估尺寸
        translation_window_width = 400
        translation_window_height = 300
        gap = 10
        
        # 获取钉图窗口的全局位置和尺寸
        pin_global_pos = self.parent.mapToGlobal(QPoint(0, 0))
        pin_x = pin_global_pos.x()
        pin_y = pin_global_pos.y()
        pin_width = self.parent.width()
        
        # 获取钉图窗口所在的屏幕
        screen = QApplication.screenAt(pin_global_pos)
        if screen is None:
            screen = QApplication.primaryScreen()
        
        screen_geometry = screen.availableGeometry()
        screen_left = screen_geometry.left()
        screen_right = screen_geometry.right()
        screen_top = screen_geometry.top()
        screen_bottom = screen_geometry.bottom()
        
        # 计算右侧位置
        right_x = pin_x + pin_width + gap
        
        # 检查右侧是否有足够空间
        if right_x + translation_window_width <= screen_right:
            final_x = right_x
        else:
            # 右侧空间不足，尝试放在左侧
            left_x = pin_x - translation_window_width - gap
            if left_x >= screen_left:
                final_x = left_x
            else:
                # 左右都不够，尽量靠右显示但不超出屏幕
                final_x = max(screen_left, screen_right - translation_window_width)
        
        # 计算垂直位置
        final_y = pin_y
        if final_y + translation_window_height > screen_bottom:
            final_y = max(screen_top, screen_bottom - translation_window_height)
        if final_y < screen_top:
            final_y = screen_top
        
        return QPoint(final_x, final_y)
    
    def _show_error(self, message: str):
        """显示错误提示"""
        msg_box = QMessageBox(self.parent)
        msg_box.setWindowTitle("翻译错误")
        msg_box.setText(message)
        msg_box.setIcon(QMessageBox.Icon.Warning)
        msg_box.setWindowFlags(msg_box.windowFlags() | Qt.WindowType.WindowStaysOnTopHint)
        msg_box.exec()
