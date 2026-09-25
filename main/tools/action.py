"""
操作工具 - 处理工具栏的操作按钮（确定、复制、保存等）
"""

import os
import re
from datetime import datetime

from PySide6.QtWidgets import QFileDialog
from PySide6.QtCore import QPoint
from core.i18n import make_tr
from core.export import ExportService
from core.save import SaveService
from pin.pin_manager import PinManager
from core.logger import log_debug, log_info, T


_tr = make_tr("ActionTools")


class ActionTools:
    """操作工具集 - 处理确定、复制、保存等操作"""
    
    def __init__(self, scene, config_manager=None, parent_window=None):
        self.scene = scene
        self.config_manager = config_manager
        self.parent_window = parent_window
        self.export_service = ExportService(scene)
        self.save_service = SaveService(config_manager=self.config_manager)

    def _copy_save_and_close(self, *, close_after=True):
        """核心操作：导出选区 → 复制到剪贴板 → 自动保存 → 关闭窗口"""
        from core.clipboard_utils import deliver_image_async
        self._temporarily_exit_editing()

        selection_rect = self.export_service.scene.selection_model.rect()
        self._remember_last_region(selection_rect)

        # 导出选区图像（包含背景和绘制内容）
        image = self.export_service.export(selection_rect)

        # 导出完成后立即隐藏窗口（数据已拿到，后续操作不需要窗口可见）
        if self.parent_window and close_after:
            self.parent_window.hide()

        save_service = None
        save_kwargs = None
        write_file_reference = True

        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            fmt = self.config_manager.get_screenshot_format()
            save_service = self.save_service
            save_kwargs = dict(
                directory=self.config_manager.get_screenshot_save_path(),
                prefix="",
                image_format=fmt,
            )
            write_file_reference = self.config_manager.get_clipboard_file_reference_enabled()

        deliver_image_async(
            image,
            save_service=save_service,
            save_kwargs=save_kwargs,
            write_file_reference=write_file_reference,
        )
        if save_service is not None:
            log_debug(T("已完成复制到剪贴板，已提交异步保存任务"), "Action")
        else:
            log_debug(T("已完成复制到剪贴板"), "Action")

        if self.parent_window and close_after:
            self._cleanup_and_close()
    
    def handle_confirm(self):
        """确定：复制到剪贴板，根据配置自动保存，关闭窗口"""
        self._copy_save_and_close()
    
    def handle_copy(self):
        """复制：将选区内容复制到剪贴板（行为与确定相同）"""
        self._copy_save_and_close()
    
    def handle_save(self, *, close_after=True):
        """保存：弹出对话框让用户选择保存位置"""
        self._temporarily_exit_editing()

        fmt = (self.config_manager.get_screenshot_format() if self.config_manager else "PNG").lower()
        default_name = f"screenshot_{datetime.now().strftime('%Y%m%d_%H%M%S')}.{fmt}"
        file_path, selected_filter = QFileDialog.getSaveFileName(
            self.parent_window,
            _tr("Save Screenshot"),
            default_name,
            "PNG (*.png);;JPG (*.jpg);;BMP (*.bmp);;WebP (*.webp);;PDF (*.pdf)",
        )

        if file_path:
            image_format = self._format_from_save_path(file_path, selected_filter)
            if not os.path.splitext(file_path)[1]:
                file_path = f"{file_path}.{image_format.lower()}"

            selection_rect = self.export_service.scene.selection_model.rect()
            self._remember_last_region(selection_rect)
            if selection_rect.isEmpty():
                selection_rect = self.export_service.scene.sceneRect()
            image = self.export_service.export(selection_rect)
            if self.save_service.save_qimage_to_path(image, file_path, image_format=image_format):
                log_info(T("已保存到: {file_path}", file_path=file_path), "Action")
                if self.parent_window and close_after:
                    self._cleanup_and_close()
            else:
                from ui.dialogs import show_warning_dialog
                show_warning_dialog(self.parent_window, _tr("Save Failed"), file_path)
    
    def handle_pin(self, *, close_after=True):
        """
        钉图：创建钉图窗口，显示选区内容
        
        钉图继承方式：
        1. 底图：只继承选区的纯净底图（不含绘制内容）
        2. 绘制层：通过向量数据继承，可继续编辑
        """
        self._temporarily_exit_editing()

        selection_rect = self.scene.selection_model.rect()
        self._remember_last_region(selection_rect)

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
        from tools.number import NumberTool
        pin_manager = PinManager.instance()
        pin_window = pin_manager.create_pin(
            image=base_image,
            position=position,
            config_manager=self.config_manager,
            drawing_items=drawing_items,
            selection_offset=QPoint(int(selection_rect.x()), int(selection_rect.y())),
            number_next=NumberTool.get_next_number(self.scene),
        )
        
        pin_window.show()
        log_debug(T("已创建钉图窗口"), "PinAction")
        log_debug(T("位置: ({pos_x}, {pos_y})", pos_x=position.x(), pos_y=position.y()), "PinAction")
        log_debug(T("底图: {width}x{height}", width=base_image.width(), height=base_image.height()), "PinAction")
        log_debug(T("继承绘制项目: {count} 个", count=len(drawing_items)), "PinAction")
        
        # 复制到剪贴板
        from core.clipboard_utils import deliver_image_async
        result_image = self.export_service.export(selection_rect)

        # 数据已拿到，提前隐藏截图窗口
        if self.parent_window and close_after:
            self.parent_window.hide()

        deliver_image_async(result_image)
        log_debug(T("已完成复制到剪贴板"), "PinAction")
        
        if self.parent_window and close_after:
            self._cleanup_and_close()

    def handle_capture_action(self, trigger):
        """Execute a configured gesture without changing toolbar shortcuts."""
        from settings.tool_settings import get_capture_action

        if not self.scene.selection_model.is_confirmed or self.scene.selection_model.rect().isEmpty():
            return False
        action = get_capture_action(self.config_manager, trigger)
        close_after = self.config_manager.get_app_setting(f"capture_{trigger}_exit", True)
        if action == "none":
            return False
        if action == "copy":
            self._copy_save_and_close(close_after=close_after)
        elif action == "pin":
            self.handle_pin(close_after=close_after)
            if not close_after and self.parent_window:
                self.parent_window.raise_()
                self.parent_window.activateWindow()
                self.parent_window.setFocus()
        elif action == "save":
            self.handle_save(close_after=close_after)
        elif action == "quick_save":
            self._temporarily_exit_editing()
            rect = self.scene.selection_model.rect()
            self._remember_last_region(rect)
            image = self.export_service.export(rect)
            success, path = self.save_service.save_qimage(
                image, directory=self.config_manager.get_screenshot_save_path(),
                prefix="", image_format=self.config_manager.get_screenshot_format(),
            )
            if success and close_after:
                self._cleanup_and_close()
            elif not success:
                from ui.dialogs import show_warning_dialog
                show_warning_dialog(self.parent_window, _tr("Save Failed"), path or "")
        return True

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
        from translation import TranslationManager

        log_info(T("启动截图翻译模式"), "ScreenshotTranslate")

        # 1. 获取选区的纯净底图（不含绘制内容）
        base_image = self._selection_base_image()
        if base_image is None:
            return

        # 转换为 QPixmap（OCR 线程需要使用）
        from PySide6.QtGui import QPixmap
        pixmap_copy = QPixmap.fromImage(base_image)
        log_debug(T("已复制底图用于OCR: {width}x{height}", width=pixmap_copy.width(), height=pixmap_copy.height()), "ScreenshotTranslate")
        
        # 2. 获取翻译参数（在关闭窗口前获取）
        params = (
            self.config_manager.get_translation_request_params()
            if self.config_manager
            else {}
        )
        
        # 3. 关闭截图窗口（释放内存）
        log_debug(T("关闭截图窗口，释放内存"), "ScreenshotTranslate")
        self._cleanup_and_close()
        
        # 4. 启动OCR并打开翻译窗口
        manager = TranslationManager.instance()
        manager.translate_from_image(
            pixmap=pixmap_copy,
            **params
        )
    
    def handle_text_recognize(self):
        """文字识别：识别选区里的文字，关掉截图界面后在结果窗口里显示

        识别比扫码慢得多（本地引擎几百毫秒起），所以窗口先出来等，识别在它的后台线程里跑。
        """
        from text_recognition import copy_text_recognition, show_text_recognition

        image = self._selection_base_image()
        if image is None:
            return
        self._cleanup_and_close()
        if self.config_manager and self.config_manager.get_ocr_copy_directly_enabled():
            copy_text_recognition(image)
        else:
            show_text_recognition(image)

    def handle_scan_code(self):
        """扫码：识别选区里的二维码 / 条形码，关掉截图界面后在结果窗口里列出"""
        from barcode import show_barcode_result

        image = self._selection_base_image()
        if image is None:
            return
        self._cleanup_and_close()
        copy_single = bool(self.config_manager and self.config_manager.get_barcode_copy_single_enabled())
        show_barcode_result(image, copy_single=copy_single)

    def _selection_base_image(self):
        """选区的纯净底图——不含标注，画上去的框和马赛克会挡住文字和码。

        没有确认的选区或导出失败时，提示用户并返回 None。
        """
        from ui.dialogs import show_modeless_warning_dialog

        if not self.scene.selection_model.is_confirmed:
            show_modeless_warning_dialog(
                self.parent_window, _tr("Warning"), _tr("Please select a valid capture area first."))
            return None
        selection_rect = self.scene.selection_model.rect()
        self._remember_last_region(selection_rect)
        image = self.export_service.export_base_image_only(selection_rect)
        if image is None or image.isNull():
            show_modeless_warning_dialog(
                self.parent_window, _tr("Error"), _tr("Failed to capture the selected area."))
            return None
        return image

    def _remember_last_region(self, rect):
        """记住这次截图用的区域，供下次截图时用快捷键还原。

        场景坐标就是虚拟桌面绝对坐标，直接存，不再叠一层窗口偏移。
        """
        if rect.isEmpty():
            return
        from core.last_capture_region import set_last_region
        from PySide6.QtCore import QRect
        set_last_region(QRect(
            round(rect.x()), round(rect.y()),
            round(rect.width()), round(rect.height()),
        ))

    def _temporarily_exit_editing(self):
        """
        临时退出编辑模式，隐藏选择框和手柄
        避免将编辑UI（虚线框、手柄）保存到图像中
        """
        if not self.scene or not hasattr(self.scene, 'tool_controller'):
            return
        
        tool_controller = self.scene.tool_controller
        current_tool_id = tool_controller.current_tool_id

        # 如果当前不是cursor工具，则切换到cursor
        if current_tool_id != "cursor":
            log_debug(T("从 {tool_id} 切换到 cursor", tool_id=current_tool_id), "Action")
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

    def _format_from_save_path(self, file_path: str, selected_filter: str) -> str:
        ext = os.path.splitext(file_path)[1].lstrip(".")
        if ext:
            return ext.upper()
        # 从过滤器字符串中提取第一个扩展名，如 "JPEG (*.jpg *.jpeg)" → "jpg"
        match = re.search(r'\*\.(\w+)', selected_filter)
        if match:
            return match.group(1).upper()
        return "PNG"
 
