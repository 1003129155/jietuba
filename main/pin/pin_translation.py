"""
钉图翻译功能

负责处理钉图窗口的翻译相关功能
"""

from PySide6.QtWidgets import QWidget, QApplication
from PySide6.QtCore import Qt, QPoint
from core import log_info, log_warning, log_error
from core.logger import T
from core.i18n import make_tr


_tr = make_tr("PinTranslationHelper")


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
                log_warning(T("没有可翻译的文字"), "Translate")
                return False

            # 2. 获取所有 OCR 文字
            all_text = ocr_text_layer.get_all_text(separator="\n")
            if not all_text.strip():
                log_warning(T("OCR 文字为空"), "Translate")
                return False

            log_info(T("准备翻译 {count} 个字符", count=len(all_text)), "Translate")
            
            # 3. 获取厂商无关的翻译参数
            params = (
                self.config_manager.get_translation_request_params()
                if self.config_manager
                else {"target_lang": "ZH"}
            )

            # 4. 使用 TranslationManager 单例进行翻译
            from translation import TranslationManager
            
            # 智能计算窗口位置
            dialog_pos = self._calculate_window_position()
            
            # 获取单例并翻译
            manager = TranslationManager.instance()
            manager.translate(
                text=all_text,
                position=dialog_pos,
                **params,
            )
            
            return True
            
        except Exception as e:
            log_error(T("翻译启动失败: {e}", e=e), "Translate")
            import traceback
            traceback.print_exc()
            return False
    
    def _calculate_window_position(self) -> QPoint:
        """
        智能计算翻译窗口位置
        
        优先放在钉图窗口右侧，如果右侧空间不足则放在左侧
        同时确保垂直方向不超出屏幕
        
        Returns:
            翻译窗口的全局坐标位置
        """
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
        from translation.translation_dialog import TranslationDialog
        translation_window_width, translation_window_height = (
            TranslationDialog.initial_size_for_available_geometry(screen_geometry)
        )
        screen_left = screen_geometry.left()
        screen_right = screen_left + screen_geometry.width()
        screen_top = screen_geometry.top()
        screen_bottom = screen_top + screen_geometry.height()
        
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
                # 左右都不够，限制在当前屏幕可用区域内
                max_x = max(screen_left, screen_right - translation_window_width)
                final_x = min(max(pin_x, screen_left), max_x)
        
        # 计算垂直位置
        max_y = max(screen_top, screen_bottom - translation_window_height)
        final_y = min(max(pin_y, screen_top), max_y)
        
        return QPoint(final_x, final_y)
    
    def _show_error(self, message: str):
        """显示错误提示"""
        from ui.dialogs import show_warning_dialog
        show_warning_dialog(self.parent, _tr("Translation Error"), message)
