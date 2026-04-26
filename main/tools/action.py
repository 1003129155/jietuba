"""
操作工具 - 处理工具栏的操作按钮（确定、复制、保存等）
"""

from PySide6.QtWidgets import QFileDialog
from PySide6.QtGui import QPixmap
from PySide6.QtCore import QPoint
from core.i18n import make_tr
from core.export import ExportService
from core.save import SaveService
from pin.pin_manager import PinManager
from core import log_debug, log_info


_tr = make_tr("ActionTools")


class ActionTools:
    """操作工具集 - 处理确定、复制、保存等操作"""
    
    def __init__(self, scene, config_manager=None, parent_window=None):
        self.scene = scene
        self.config_manager = config_manager
        self.parent_window = parent_window
        self.export_service = ExportService(scene)
        self.save_service = SaveService(config_manager=self.config_manager)

    def _copy_save_and_close(self):
        """核心操作：导出选区 → 复制到剪贴板 → 自动保存 → 关闭窗口"""
        from core.clipboard_utils import deliver_image_async
        self._temporarily_exit_editing()

        # 导出选区图像（包含背景和绘制内容）
        image = self.export_service.export(self.export_service.scene.selection_model.rect())

        # 导出完成后立即隐藏窗口（数据已拿到，后续操作不需要窗口可见）
        if self.parent_window:
            self.parent_window.hide()

        save_service = None
        save_kwargs = None

        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            fmt = self.config_manager.get_screenshot_format()
            save_service = self.save_service
            save_kwargs = dict(
                directory=self.config_manager.get_screenshot_save_path(),
                prefix="",
                image_format=fmt,
            )

        deliver_image_async(
            image,
            save_service=save_service,
            save_kwargs=save_kwargs,
        )
        log_debug("已提交异步复制/保存任务", "Action")

        if self.parent_window:
            self._cleanup_and_close()
    
    def handle_confirm(self):
        """确定：复制到剪贴板，根据配置自动保存，关闭窗口"""
        self._copy_save_and_close()
    
    def handle_copy(self):
        """复制：将选区内容复制到剪贴板（行为与确定相同）"""
        self._copy_save_and_close()
    
    def handle_save(self):
        """保存：弹出对话框让用户选择保存位置"""
        self._temporarily_exit_editing()
        
        pixmap = self.export_service.get_result_pixmap()
        
        file_path, _ = QFileDialog.getSaveFileName(
            self.parent_window, _tr("Save Screenshot"), "screenshot.png", "Images (*.png *.jpg *.bmp)"
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
        log_debug("已创建钉图窗口", "PinAction")
        log_debug(f"位置: ({position.x()}, {position.y()})", "PinAction")
        log_debug(f"底图: {base_image.width()}x{base_image.height()}", "PinAction")
        log_debug(f"继承绘制项目: {len(drawing_items)} 个", "PinAction")
        
        # 复制到剪贴板
        from core.clipboard_utils import deliver_image_async
        result_image = self.export_service.export(selection_rect)

        # 数据已拿到，提前隐藏截图窗口
        if self.parent_window:
            self.parent_window.hide()

        deliver_image_async(result_image)
        log_debug("已提交异步复制任务", "PinAction")
        
        if self.parent_window:
            self._cleanup_and_close()

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
        from ui.dialogs import show_modeless_warning_dialog
        from translation import TranslationManager

        log_info("启动截图翻译模式", "ScreenshotTranslate")

        if not self.scene.selection_model.is_confirmed:
            show_modeless_warning_dialog(
                self.parent_window,
                _tr("Warning"),
                _tr("Please select a valid capture area first.")
            )
            return
        
        # 1. 获取选区的纯净底图（不含绘制内容）
        selection_rect = self.scene.selection_model.rect()
        base_image = self.export_service.export_base_image_only(selection_rect)
        if base_image is None or base_image.isNull():
            show_modeless_warning_dialog(
                self.parent_window,
                _tr("Error"),
                _tr("Failed to capture the selected area.")
            )
            return
        
        # 转换为 QPixmap（OCR 线程需要使用）
        from PySide6.QtGui import QPixmap
        pixmap_copy = QPixmap.fromImage(base_image)
        log_debug(f"已复制底图用于OCR: {pixmap_copy.width()}x{pixmap_copy.height()}", "ScreenshotTranslate")
        
        # 2. 获取翻译参数（在关闭窗口前获取）
        params = self.config_manager.get_translation_params() if self.config_manager else {}

        if not params.get("api_key"):
            show_modeless_warning_dialog(
                self.parent_window,
                _tr("Notice"),
                _tr("Please configure DeepL API key in Settings.")
            )
            return
        
        # 3. 关闭截图窗口（释放内存）
        log_debug("关闭截图窗口，释放内存", "ScreenshotTranslate")
        self._cleanup_and_close()
        
        # 4. 启动OCR并打开翻译窗口
        manager = TranslationManager.instance()
        manager.translate_from_image(
            pixmap=pixmap_copy,
            **params
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
        
        # 取消智能编辑的选择（清除控制点手柄）和隐藏画笔指示器
        # 通过信号通知，避免 action 直接访问 scene.view
        self.scene.editing_cleanup_requested.emit()
    
    def _cleanup_and_close(self):
        """清理资源并关闭窗口"""
        if self.parent_window:
            if hasattr(self.parent_window, 'cleanup_and_close'):
                self.parent_window.cleanup_and_close()
            else:
                self.parent_window.close()
 