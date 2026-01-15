"""
操作工具 - 处理工具栏的操作按钮（确定、复制、保存等）
"""

from PyQt6.QtWidgets import QApplication, QFileDialog
from PyQt6.QtGui import QPixmap
from PyQt6.QtCore import QPoint
from core.export import ExportService
from core.save import SaveService
from pin.pin_manager import PinManager
from core import log_debug, log_info


class ActionTools:
    """操作工具集 - 处理确定、复制、保存等操作"""
    
    def __init__(self, scene, config_manager=None, parent_window=None):
        self.scene = scene
        self.config_manager = config_manager
        self.parent_window = parent_window
        self.export_service = ExportService(scene)
        self.save_service = SaveService(config_manager=self.config_manager)
    
    def handle_confirm(self):
        """确定：复制到剪贴板，根据配置自动保存，关闭窗口"""
        self._temporarily_exit_editing()
        
        pixmap = self.export_service.get_result_pixmap()
        QApplication.clipboard().setPixmap(pixmap)
        log_info("已复制到剪贴板", "Action")
        
        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            self._auto_save(pixmap)
        
        if self.parent_window:
            self._cleanup_and_close()
    
    def handle_copy(self):
        """复制：将选区内容复制到剪贴板"""
        self._temporarily_exit_editing()
        
        pixmap = self.export_service.get_result_pixmap()
        QApplication.clipboard().setPixmap(pixmap)
        log_info("已复制到剪贴板", "Action")
        
        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            self._auto_save(pixmap)
        
        if self.parent_window:
            self._cleanup_and_close()
    
    def handle_save(self):
        """保存：弹出对话框让用户选择保存位置"""
        self._temporarily_exit_editing()
        
        pixmap = self.export_service.get_result_pixmap()
        
        file_path, _ = QFileDialog.getSaveFileName(
            self.parent_window, "保存截图", "screenshot.png", "Images (*.png *.jpg *.bmp)"
        )
        
        if file_path:
            pixmap.save(file_path)
            log_info(f"已保存到: {file_path}", "Action")
            
            if self.parent_window:
                self._cleanup_and_close()
    
    def handle_pin(self):
        """
        钉图：创建钉图窗口，显示选区内容
        
        钉图继承方式：
        1. 底图：只继承选区的纯净底图（不含绘制内容）
        2. 绘制层：通过向量数据继承，可继续编辑
        """
        self._temporarily_exit_editing()
        
        selection_rect = self.scene.selection_model.rect()
        
        # 获取纯净底图（不含绘制内容）
        base_image = self.export_service.export_base_image_only(selection_rect)
        
        # 获取选区内的所有绘制项目
        drawing_items = self.scene.get_drawing_items_in_rect(selection_rect)
        
        # 获取选区的屏幕位置
        position = QPoint(
            round(selection_rect.x()),
            round(selection_rect.y())
        )
        
        # 创建钉图窗口
        pin_manager = PinManager.instance()
        pin_window = pin_manager.create_pin(
            image=base_image,
            position=position,
            config_manager=self.config_manager,
            drawing_items=drawing_items,
            selection_offset=QPoint(int(selection_rect.x()), int(selection_rect.y()))
        )
        
        pin_window.show()
        log_info("已创建钉图窗口", "PinAction")
        log_debug(f"位置: ({position.x()}, {position.y()})", "PinAction")
        log_debug(f"底图: {base_image.width()}x{base_image.height()}", "PinAction")
        log_debug(f"继承绘制项目: {len(drawing_items)} 个", "PinAction")
        
        # 复制到剪贴板
        result_pixmap = self.export_service.get_result_pixmap()
        QApplication.clipboard().setPixmap(result_pixmap)
        log_info("已复制到剪贴板", "PinAction")
        
        # 自动保存
        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            self._auto_save(result_pixmap)
            log_debug("钉图已触发自动保存", "PinAction")
        
        if self.parent_window:
            self._cleanup_and_close()
    
    def _auto_save(self, pixmap: QPixmap):
        """异步自动保存图片到配置的路径"""
        if not self.config_manager:
            return
        
        self.save_service.save_pixmap_async(
            pixmap,
            directory=self.config_manager.get_screenshot_save_path(),
            prefix=""
        )
    
    def handle_screenshot_translate(self):
        """
        截图翻译 - 选区OCR识别后打开翻译窗口
        
        流程：
        1. 复制选区底图（独立副本）
        2. 关闭截图窗口（释放内存）
        3. 打开翻译窗口（显示"识别中..."）
        4. 后台OCR识别
        5. 识别完成后填入原文并自动翻译
        """
        from PyQt6.QtWidgets import QMessageBox
        from translation import TranslationManager
        from core.i18n import I18nManager
        
        log_info("启动截图翻译模式", "ScreenshotTranslate")
        
        if not self.scene.selection_model.is_confirmed:
            QMessageBox.warning(self.parent_window, "警告", "请先选择一个有效的截图区域！")
            return
        
        # 1. 获取选区的纯净底图（不含绘制内容）
        selection_rect = self.scene.selection_model.rect()
        base_image = self.export_service.export_base_image_only(selection_rect)
        if base_image is None or base_image.isNull():
            QMessageBox.warning(self.parent_window, "错误", "无法获取截图区域！")
            return
        
        # 转换为 QPixmap（OCR 线程需要使用）
        from PyQt6.QtGui import QPixmap
        pixmap_copy = QPixmap.fromImage(base_image)
        log_debug(f"已复制底图用于OCR: {pixmap_copy.width()}x{pixmap_copy.height()}", "ScreenshotTranslate")
        
        # 2. 获取翻译参数（在关闭窗口前获取）
        api_key = ""
        if self.config_manager and hasattr(self.config_manager, 'get_deepl_api_key'):
            api_key = self.config_manager.get_deepl_api_key() or ""
        
        if not api_key:
            QMessageBox.warning(self.parent_window, "提示", "请先在设置中配置 DeepL API 密钥")
            return
        
        # 根据当前应用语言设置目标语言
        app_lang = I18nManager.get_current_language()
        lang_map = {"zh": "ZH", "en": "EN", "ja": "JA"}
        target_lang = lang_map.get(app_lang, "ZH")
        
        use_pro = False
        if self.config_manager and hasattr(self.config_manager, 'get_deepl_use_pro'):
            use_pro = self.config_manager.get_deepl_use_pro()
        
        split_sentences_enabled = True
        preserve_formatting = True
        if self.config_manager and hasattr(self.config_manager, 'get_translation_split_sentences'):
            split_sentences_enabled = self.config_manager.get_translation_split_sentences()
        if self.config_manager and hasattr(self.config_manager, 'get_translation_preserve_formatting'):
            preserve_formatting = self.config_manager.get_translation_preserve_formatting()
        
        split_sentences = "nonewlines" if split_sentences_enabled else "0"
        
        # 3. 关闭截图窗口（释放内存）
        log_debug("关闭截图窗口，释放内存", "ScreenshotTranslate")
        self._cleanup_and_close()
        
        # 4. 启动OCR并打开翻译窗口
        manager = TranslationManager.instance()
        manager.translate_from_image(
            pixmap=pixmap_copy,
            api_key=api_key,
            target_lang=target_lang,
            use_pro=use_pro,
            split_sentences=split_sentences,
            preserve_formatting=preserve_formatting
        )
    
    def _temporarily_exit_editing(self):
        """
        临时退出编辑模式，隐藏选择框和手柄
        避免将编辑UI（虚线框、手柄）保存到图像中
        """
        if not self.scene or not hasattr(self.scene, 'tool_controller'):
            return
        
        tool_controller = self.scene.tool_controller
        current_tool = tool_controller.current_tool
        
        # 如果当前不是cursor工具，则切换到cursor
        if current_tool and current_tool.id != "cursor":
            log_debug(f"从 {current_tool.id} 切换到 cursor", "Action")
            tool_controller.activate("cursor")
        
        # 取消智能编辑的选择（清除控制点手柄）
        if hasattr(self.scene, 'view') and self.scene.view:
            if hasattr(self.scene.view, 'smart_edit_controller'):
                smart_edit = self.scene.view.smart_edit_controller
                if smart_edit.selected_item:
                    log_debug("取消智能编辑选择", "Action")
                    smart_edit.deselect()
        
        # 隐藏画笔指示器
        if hasattr(self.scene, 'view') and self.scene.view:
            if hasattr(self.scene.view, 'cursor_manager'):
                cursor_mgr = self.scene.view.cursor_manager
                if hasattr(cursor_mgr, 'hide_brush_indicator'):
                    cursor_mgr.hide_brush_indicator()
    
    def _cleanup_and_close(self):
        """清理资源并关闭窗口"""
        if self.parent_window:
            if hasattr(self.parent_window, 'cleanup_and_close'):
                self.parent_window.cleanup_and_close()
            else:
                self.parent_window.close()
