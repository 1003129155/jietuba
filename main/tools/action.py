"""
操作工具 - 处理工具栏的操作按钮（确定、复制、保存等）
这些不是绘图工具，而是截图窗口的操作
"""

from PyQt6.QtWidgets import QApplication, QFileDialog
from PyQt6.QtGui import QPixmap
from PyQt6.QtCore import QPoint
from canvas.export import ExportService
from core.save import SaveService
from pin.pin_manager import PinManager


class ActionTools:
    """
    操作工具集 - 处理确定、复制、保存等操作
    """
    
    def __init__(self, scene, config_manager=None, parent_window=None):
        """
        初始化操作工具
        
        Args:
            scene: CanvasScene实例
            config_manager: ConfigManager实例，用于读取配置
            parent_window: 父窗口（ScreenshotWindow），用于对话框和关闭操作
        """
        self.scene = scene
        self.config_manager = config_manager
        self.parent_window = parent_window
        self.export_service = ExportService(scene)
        self.save_service = SaveService(config_manager=self.config_manager)
    
    def handle_confirm(self):
        """
        处理确定按钮点击
        确定的意思是：结束截图，将选区内容复制到剪贴板，根据配置自动保存
        """
        # 🔥 临时退出编辑模式，隐藏选择框和手柄
        self._temporarily_exit_editing()
        
        # 获取选区内的图像
        pixmap = self.export_service.get_result_pixmap()
        
        # 复制到剪贴板
        QApplication.clipboard().setPixmap(pixmap)
        print("✅ [确定] 已复制到剪贴板")
        
        # 自动保存逻辑
        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            self._auto_save(pixmap)
        
        # 关闭窗口
        if self.parent_window:
            self._cleanup_and_close()
    
    def handle_copy(self):
        """
        处理复制按钮点击
        将选区内容复制到剪贴板，根据配置自动保存
        """
        # 🔥 临时退出编辑模式，隐藏选择框和手柄
        self._temporarily_exit_editing()
        
        # 获取选区内的图像
        pixmap = self.export_service.get_result_pixmap()
        
        # 复制到剪贴板
        QApplication.clipboard().setPixmap(pixmap)
        print("✅ [复制] 已复制到剪贴板")
        
        # 自动保存逻辑
        if self.config_manager and self.config_manager.get_screenshot_save_enabled():
            self._auto_save(pixmap)
        
        # 关闭窗口
        if self.parent_window:
            self._cleanup_and_close()
    
    def handle_save(self):
        """
        处理保存按钮点击
        弹出保存对话框，让用户选择保存位置和文件名
        """
        # 🔥 临时退出编辑模式，隐藏选择框和手柄
        self._temporarily_exit_editing()
        
        pixmap = self.export_service.get_result_pixmap()
        
        # 弹出保存对话框
        file_path, _ = QFileDialog.getSaveFileName(
            self.parent_window, "保存截图", "screenshot.png", "Images (*.png *.jpg *.bmp)"
        )
        
        if file_path:
            pixmap.save(file_path)
            print(f"✅ [保存] 已保存到: {file_path}")
            
            # 保存后关闭窗口
            if self.parent_window:
                self._cleanup_and_close()
    
    def handle_pin(self):
        """
        处理钉图按钮点击
        创建钉图窗口，显示选区内容，然后关闭截图窗口
        
        钉图继承方式：
        1. 底图：只继承选区的纯净底图（不含绘制内容）
        2. 绘制层：通过向量数据（绘制项目列表）继承，可继续编辑
        """
        # 🔥 临时退出编辑模式，取消选择（避免把控制点手柄也截图进去）
        self._temporarily_exit_editing()
        
        # 获取选区矩形
        selection_rect = self.scene.selection_model.rect()
        
        # 🔥 获取纯净底图（不含绘制内容）
        base_image = self.export_service.export_base_image_only(selection_rect)
        
        # 🔥 获取选区内的所有绘制项目
        drawing_items = self.scene.get_drawing_items_in_rect(selection_rect)
        
        # 获取选区的屏幕位置（作为钉图窗口的初始位置）
        # 🔥 直接使用scene坐标 + 虚拟屏幕偏移，避免mapToGlobal的精度损失
        # scene坐标就是相对于虚拟屏幕的坐标
        position = QPoint(
            round(selection_rect.x()),
            round(selection_rect.y())
        )
        
        # 使用 PinManager 创建钉图窗口
        pin_manager = PinManager.get_instance()
        pin_window = pin_manager.create_pin(
            image=base_image,
            position=position,
            config_manager=self.config_manager,
            drawing_items=drawing_items,  # 🔥 传递绘制项目列表（向量数据）
            selection_offset=QPoint(int(selection_rect.x()), int(selection_rect.y()))  # 🔥 选区偏移量
        )
        
        # 显示钉图窗口
        pin_window.show()
        print(f"📌 [钉图] 已创建钉图窗口")
        print(f"    位置: ({position.x()}, {position.y()})")
        print(f"    底图: {base_image.width()}x{base_image.height()}")
        print(f"    继承绘制项目: {len(drawing_items)} 个")
        
        # 关闭截图窗口
        if self.parent_window:
            self._cleanup_and_close()
    
    def _auto_save(self, pixmap: QPixmap):
        """
        自动保存图片到配置的路径（异步执行，不阻塞UI）
        
        Args:
            pixmap: 要保存的QPixmap
        """
        if not self.config_manager:
            return
        
        self.save_service.save_pixmap_async(
            pixmap,
            directory=self.config_manager.get_screenshot_save_path(),
            prefix=""
        )
    
    def _temporarily_exit_editing(self):
        """
        临时退出编辑模式，隐藏选择框和手柄
        在保存/复制图像时调用，避免将编辑UI（虚线框、手柄）保存到图像中
        注意：不需要恢复，因为截图窗口会在保存/复制后立即关闭
        """
        if not self.scene or not hasattr(self.scene, 'tool_controller'):
            return
        
        tool_controller = self.scene.tool_controller
        current_tool = tool_controller.current_tool
        
        # 如果当前不是cursor工具（说明在编辑状态），则切换到cursor
        if current_tool and current_tool.id != "cursor":
            print(f"🔧 [临时退出编辑] 从 {current_tool.id} 切换到 cursor")
            tool_controller.activate("cursor")
        
        # 🔥 取消智能编辑的选择（清除8个控制点手柄）
        if hasattr(self.scene, 'view') and self.scene.view:
            if hasattr(self.scene.view, 'smart_edit_controller'):
                smart_edit = self.scene.view.smart_edit_controller
                if smart_edit.selected_item:
                    print(f"🔧 [临时退出编辑] 取消智能编辑选择")
                    smart_edit.deselect()
        
        # 🔥 隐藏画笔指示器
        if hasattr(self.scene, 'view') and self.scene.view:
            if hasattr(self.scene.view, 'cursor_manager'):
                cursor_mgr = self.scene.view.cursor_manager
                if hasattr(cursor_mgr, 'hide_brush_indicator'):
                    cursor_mgr.hide_brush_indicator()
    
    def _cleanup_and_close(self):
        """
        清理资源并关闭窗口
        """
        if self.parent_window:
            # 停止定时器
            if hasattr(self.parent_window, 'visibility_timer'):
                self.parent_window.visibility_timer.stop()
                self.parent_window.visibility_timer.deleteLater()
            
            # 关闭工具栏和二级菜单
            if hasattr(self.parent_window, 'toolbar'):
                if hasattr(self.parent_window.toolbar, 'paint_menu'):
                    self.parent_window.toolbar.paint_menu.close()
                self.parent_window.toolbar.close()
            
            # 关闭主窗口
            self.parent_window.close()
