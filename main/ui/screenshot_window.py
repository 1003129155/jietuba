import sys
import os
import gc
from datetime import datetime
from PyQt6.QtWidgets import QApplication, QWidget, QMessageBox, QGraphicsTextItem
from PyQt6.QtCore import Qt, QTimer, QRect
from PyQt6.QtGui import QPixmap, QImage

from canvas import CanvasScene, CanvasView
from capture.capture_service import CaptureService
from ui.toolbar import Toolbar
from ui.magnifier import MagnifierOverlay
from tools.action import ActionTools
from settings import get_tool_settings_manager
from stitch.scroll_window import ScrollCaptureWindow
from core.logger import log_debug, log_info, log_warning, log_error, log_exception

class ScreenshotWindow(QWidget):
    def __init__(self, config_manager=None):
        super().__init__()
        
        # 设置窗口属性：关闭时自动删除，避免内存泄漏
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        
        # Config manager for auto-save settings
        self.config_manager = config_manager if config_manager else get_tool_settings_manager()
        
        # 1. 使用 CaptureService 截取全屏
        capture_service = CaptureService()
        self.original_image, rect = capture_service.capture_all_screens()
        
        self.virtual_x = rect.x()
        self.virtual_y = rect.y()
        self.virtual_width = rect.width()
        self.virtual_height = rect.height()
        
        log_debug(f"虚拟桌面: {self.virtual_width}x{self.virtual_height} at ({self.virtual_x}, {self.virtual_y})", "ScreenshotWindow")
        log_debug(f"图像尺寸: {self.original_image.width()}x{self.original_image.height()}", "ScreenshotWindow")

        # 2. 初始化场景和视图
        self.scene = CanvasScene(self.original_image, rect)
        self.view = CanvasView(self.scene)
        
        # 启用智能选区（从配置读取）
        smart_selection_enabled = self.config_manager.get_smart_selection()
        self.view.enable_smart_selection(smart_selection_enabled)
        
        # 3. 初始化工具栏
        self.toolbar = Toolbar(self)
        self.toolbar.hide() # 初始隐藏，选区确认后显示
        
        # 4. 创建ActionTools来处理工具栏按钮逻辑
        self.action_handler = ActionTools(
            scene=self.scene,
            config_manager=self.config_manager,
            parent_window=self
        )
        
        # 5. 窗口设置
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        
        # 设置窗口几何形状以覆盖所有屏幕
        self.setGeometry(int(self.virtual_x), int(self.virtual_y), int(self.virtual_width), int(self.virtual_height))
        
        # View 填满整个窗口
        self.view.setParent(self)
        # View 的几何形状是相对于父窗口(self)的，所以应该是 (0, 0, w, h)
        self.view.setGeometry(0, 0, int(self.virtual_width), int(self.virtual_height))

        # 叠加鼠标放大镜，复刻老版 UI 的色彩信息视图
        self.magnifier_overlay = MagnifierOverlay(self, self.scene, self.view, self.config_manager)
        self.magnifier_overlay.setGeometry(self.rect())
        
        # 工具栏初始位置（底部居中）
        self.update_toolbar_position()
        
        # 6. 连接信号
        self.connect_signals()
        
        self.show()
        self.activateWindow()
        self.raise_()
        self.setFocus()
        
        # Ensure focus after a short delay (workaround for Windows focus stealing prevention)
        QTimer.singleShot(50, self.activateWindow)
        QTimer.singleShot(50, self.setFocus)
        
        # 初始状态：进入选区模式
        # CanvasView 默认处理鼠标按下进入选区
        
    def connect_signals(self):
        # 工具切换
        self.toolbar.tool_changed.connect(self.on_tool_changed)
        
        # 样式改变
        self.toolbar.color_changed.connect(self.on_color_changed)
        self.toolbar.stroke_width_changed.connect(self.on_stroke_width_changed)
        self.toolbar.opacity_changed.connect(self.on_opacity_changed)
        
        # 文字工具信号连接
        if hasattr(self.view, 'smart_edit_controller'):
            controller = self.view.smart_edit_controller
            self.toolbar.text_font_changed.connect(controller.on_text_font_changed)
            # 注意：text_outline_changed, text_shadow_changed, text_background_changed 已移除
            # 颜色改变也需要通知文字工具
            self.toolbar.color_changed.connect(controller.on_text_color_changed)
        
        # 操作按钮 - 连接到ActionHandler
        self.toolbar.undo_clicked.connect(self.on_undo)
        self.toolbar.redo_clicked.connect(self.on_redo)
        self.toolbar.confirm_clicked.connect(self.action_handler.handle_confirm)
        self.toolbar.copy_clicked.connect(self.action_handler.handle_copy)
        self.toolbar.save_clicked.connect(self.action_handler.handle_save)
        self.toolbar.pin_clicked.connect(self.action_handler.handle_pin)
        self.toolbar.long_screenshot_clicked.connect(self.start_long_screenshot_mode)
        self.toolbar.screenshot_translate_clicked.connect(self.action_handler.handle_screenshot_translate)
        
        # 场景信号
        self.scene.selectionConfirmed.connect(self.on_selection_confirmed)
        self.scene.selection_model.rectChanged.connect(self.update_toolbar_position)

    def on_selection_confirmed(self):
        # 选区确认后，显示工具栏
        self.toolbar.show()
        self.toolbar.raise_()  # 只提升到顶层，不激活窗口
        self.update_toolbar_position()
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.refresh()
        
        # 确保主窗口保持焦点
        self.activateWindow()
        self.setFocus()

    def resizeEvent(self, event):
        self.view.setGeometry(self.rect())
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.setGeometry(self.rect())
            self.magnifier_overlay.refresh()
        if self.scene.selection_model.is_confirmed:
            self.update_toolbar_position()
        super().resizeEvent(event)
        
    def update_toolbar_position(self):
        """更新工具栏位置 - 完全参考老代码的逻辑"""
        if not hasattr(self, 'toolbar') or not self.toolbar.isVisible():
            return
            
        rect = self.scene.selection_model.rect()
        if rect.isEmpty():
            return
            
        # 将场景坐标转换为视图坐标
        view_polygon = self.view.mapFromScene(rect)
        view_rect = view_polygon.boundingRect()
        
        # 使用 View 作为父窗口进行坐标转换
        self.toolbar.position_near_rect(view_rect, self.view)
        
        # 如果二级菜单可见，也更新其位置（但不重复调用 show_paint_menu）
        if hasattr(self.toolbar, 'paint_menu') and self.toolbar.paint_menu.isVisible():
            # 直接更新二级菜单位置，不重新显示
            toolbar_pos = self.toolbar.pos()
            menu_x = toolbar_pos.x()
            menu_y = toolbar_pos.y() + self.toolbar.height() + 5
            
            # 检查屏幕边界
            screen = QApplication.screenAt(toolbar_pos)
            if screen:
                screen_rect = screen.geometry()
                if menu_y + self.toolbar.paint_menu.height() > screen_rect.y() + screen_rect.height():
                    menu_y = toolbar_pos.y() - self.toolbar.paint_menu.height() - 5
                if menu_x + self.toolbar.paint_menu.width() > screen_rect.x() + screen_rect.width():
                    menu_x = screen_rect.x() + screen_rect.width() - self.toolbar.paint_menu.width() - 5
            
            if self.toolbar.paint_menu.pos().x() != menu_x or self.toolbar.paint_menu.pos().y() != menu_y:
                self.toolbar.paint_menu.move(menu_x, menu_y)
    
    def cleanup_and_close(self):
        """清理资源并关闭窗口 - 防止内存泄漏"""
        log_info("开始清理截图窗口资源", "ScreenshotWindow")
        
        # 停止定时器
        if hasattr(self, 'visibility_timer'):
            self.visibility_timer.stop()
            self.visibility_timer.deleteLater()
            self.visibility_timer = None
        
        # 释放大图片内存（最重要！必须在 scene.clear() 之前）
        if hasattr(self, 'original_image'):
            log_debug(f"释放原始截图内存: {self.original_image.width()}x{self.original_image.height()}", "ScreenshotWindow")
            self.original_image = None
        
        # 清理 Scene 中的所有图层和对象
        if hasattr(self, 'scene'):
            # 获取所有图层项并清理
            items = self.scene.items()
            log_debug(f"清理 {len(items)} 个场景对象", "ScreenshotWindow")
            
            # 手动删除所有 items（释放 pixmap 内存）
            for item in items:
                if hasattr(item, 'setPixmap'):
                    # 清空 pixmap（释放 GPU 纹理和内存）
                    item.setPixmap(QPixmap())
                self.scene.removeItem(item)
            
            # 清除所有图层
            self.scene.clear()
            
            # 断开信号连接
            try:
                self.scene.selectionConfirmed.disconnect()
                self.scene.selection_model.rectChanged.disconnect()
            except Exception as e:
                log_exception(e, "断开场景信号连接")
            
            # 删除 scene
            self.scene.deleteLater()
            self.scene = None
        
        # 清理 View
        if hasattr(self, 'view'):
            self.view.setScene(None)  # 断开与 scene 的连接
            self.view.deleteLater()
            self.view = None

        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.deleteLater()
            self.magnifier_overlay = None
        
        # 关闭工具栏和二级菜单
        if hasattr(self, 'toolbar'):
            # 断开信号连接
            try:
                self.toolbar.tool_changed.disconnect()
                self.toolbar.color_changed.disconnect()
                self.toolbar.stroke_width_changed.disconnect()
                self.toolbar.opacity_changed.disconnect()
                self.toolbar.undo_clicked.disconnect()
                self.toolbar.redo_clicked.disconnect()
                self.toolbar.confirm_clicked.disconnect()
                self.toolbar.copy_clicked.disconnect()
                self.toolbar.save_clicked.disconnect()
                self.toolbar.long_screenshot_clicked.disconnect()
            except Exception as e:
                log_exception(e, "断开工具栏信号连接")
            
            if hasattr(self.toolbar, 'paint_menu'):
                self.toolbar.paint_menu.close()
                self.toolbar.paint_menu.deleteLater()
                self.toolbar.paint_menu = None
            self.toolbar.close()
            self.toolbar.deleteLater()
            self.toolbar = None
        
        # 清理 ActionHandler
        if hasattr(self, 'action_handler'):
            self.action_handler = None
        
        # 强制垃圾回收
        gc.collect()
        
        # 关闭窗口（WA_DeleteOnClose 会自动删除窗口对象）
        self.close()
        
        log_info("截图窗口资源清理完成", "ScreenshotWindow")

    def keyPressEvent(self, event):
        """键盘事件处理 - 参考老代码逻辑"""
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if self._is_text_editing():
                super().keyPressEvent(event)
                return
        if event.key() == Qt.Key.Key_Escape:
            # ESC键随时退出截图
            self.cleanup_and_close()
        elif event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            # Enter键完成截图（如果选区已确认）- 调用确定操作
            if self.scene.selection_model.is_confirmed:
                self.action_handler.handle_confirm()
        elif event.key() == Qt.Key.Key_PageUp:
            # PageUp键增加放大镜倍数（仅在放大镜显示时）
            if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
                if self.magnifier_overlay.cursor_scene_pos is not None and self.magnifier_overlay._should_render():
                    self.magnifier_overlay.adjust_zoom(1)
                    event.accept()
                    return
        elif event.key() == Qt.Key.Key_PageDown:
            # PageDown键减少放大镜倍数（仅在放大镜显示时）
            if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
                if self.magnifier_overlay.cursor_scene_pos is not None and self.magnifier_overlay._should_render():
                    self.magnifier_overlay.adjust_zoom(-1)
                    event.accept()
                    return
        super().keyPressEvent(event)

    def _is_text_editing(self) -> bool:
        focus_item = self.scene.focusItem() if hasattr(self.scene, 'focusItem') else None
        if isinstance(focus_item, QGraphicsTextItem) and focus_item.hasFocus():
            flags = focus_item.textInteractionFlags()
            return bool(flags & Qt.TextInteractionFlag.TextEditorInteraction)
        return False
    # --- 槽函数 ---

    def on_tool_changed(self, tool_id):
        """工具切换 - 确保主窗口保持焦点"""
        self.scene.activate_tool(tool_id)
        
        # 同步 UI：工具激活后，其设置已加载到 ToolContext，现在同步到工具栏 UI
        ctx = self.scene.tool_controller.ctx
        
        # 临时断开信号，避免循环触发
        self.toolbar.color_changed.disconnect(self.on_color_changed)
        self.toolbar.stroke_width_changed.disconnect(self.on_stroke_width_changed)
        self.toolbar.opacity_changed.disconnect(self.on_opacity_changed)
        
        try:
            # 更新工具栏 UI 显示当前工具的设置
            self.toolbar.set_current_color(ctx.color)
            self.toolbar.set_stroke_width(ctx.stroke_width)
            self.toolbar.set_opacity(int(ctx.opacity * 255))
        finally:
            # 重新连接信号
            self.toolbar.color_changed.connect(self.on_color_changed)
            self.toolbar.stroke_width_changed.connect(self.on_stroke_width_changed)
            self.toolbar.opacity_changed.connect(self.on_opacity_changed)
        
        # 切换工具后，将焦点还给 View（确保快捷键可用）
        from PyQt6.QtCore import QTimer
        QTimer.singleShot(0, self.view.setFocus)
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.refresh()
        
    def on_color_changed(self, color):
        self.scene.update_style(color=color)
        
    def on_stroke_width_changed(self, width):
        ctx = getattr(self.scene.tool_controller, 'ctx', None)
        prev_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        log_debug(f"slider width change -> prev={prev_width}, target={width}", "ScreenshotWindow")
        self.scene.update_style(width=width)
        new_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)

        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_size_change_to_selection') and prev_width > 0:
            scale = new_width / prev_width
            if abs(scale - 1.0) > 1e-6:
                log_debug(f"apply selection scale via view: scale={scale:.3f}", "ScreenshotWindow")
                view._apply_size_change_to_selection(scale)

        if view and hasattr(view, 'cursor_manager'):
            view.cursor_manager.update_tool_cursor_size(int(width))
        
        log_debug(f"线宽: {width}", "ScreenshotWindow")
        
    def on_opacity_changed(self, opacity_int):
        # opacity_int 是 0-255，转换为 0.0-1.0
        opacity = opacity_int / 255.0
        log_debug(f"slider opacity change -> target={opacity:.3f}", "ScreenshotWindow")
        self.scene.update_style(opacity=opacity)
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_opacity_change_to_selection'):
            view._apply_opacity_change_to_selection(opacity)
        log_debug(f"透明度: {opacity:.2f}", "ScreenshotWindow")

    def on_undo(self):
        """撤销"""
        if self.scene.undo_stack.canUndo():
            self.scene.undo_stack.undo()
        
    def on_redo(self):
        """重做"""
        if self.scene.undo_stack.canRedo():
            self.scene.undo_stack.redo()
    
    def start_long_screenshot_mode(self):
        """启动长截图模式"""
        log_info("启动长截图模式", "ScreenshotWindow")
        
        # 获取当前选中的区域
        if self.scene.selection_model.is_confirmed:
            selection_rect = self.scene.selection_model.rect()
            
            log_debug(f"selection_rect（场景坐标）: x={selection_rect.x()}, y={selection_rect.y()}, w={selection_rect.width()}, h={selection_rect.height()}", "LongScreenshot")
            log_debug(f"virtual偏移: x={self.virtual_x}, y={self.virtual_y}", "LongScreenshot")
            
            # 场景坐标已经是屏幕的全局坐标，背景图层通过 setOffset 保留了系统提供的虚拟桌面偏移
            # 因此此处不需要再次叠加 virtual_x / virtual_y，否则会导致坐标被重复平移
            real_x = int(selection_rect.x())
            real_y = int(selection_rect.y())
            real_width = int(selection_rect.width())
            real_height = int(selection_rect.height())
            
            # 创建屏幕坐标的选区矩形
            capture_rect = QRect(real_x, real_y, real_width, real_height)
            
            log_debug(f"选中区域（屏幕坐标）: x={real_x}, y={real_y}, w={real_width}, h={real_height}", "LongScreenshot")
            
            # 保存配置，用于长截图窗口
            save_dir = self.config_manager.get_screenshot_save_path()
            
            # 创建独立的长截图窗口（不传递 parent，让它独立运行）
            scroll_window = ScrollCaptureWindow(capture_rect, parent=None)
            scroll_window.set_save_directory(save_dir)  # 设置保存目录
            
            # 显示长截图窗口
            log_debug("长截图窗口创建完成，准备显示", "LongScreenshot")
            scroll_window.show()
            scroll_window.raise_()
            scroll_window.activateWindow()
            
            log_info("滚动截图窗口已显示并激活", "LongScreenshot")
            
            # 立即关闭截图窗口，释放内存
            log_debug("释放截图窗口内存", "LongScreenshot")
            self.cleanup_and_close()
        else:
            # 如果没有确认选区，显示提示
            QMessageBox.warning(self, "警告", "请先选择一个有效的截图区域！")
    
    def closeEvent(self, event):
        """窗口关闭事件 - 确保工具栏和二级菜单也关闭"""
        # 长截图窗口现在是独立的，不需要在这里清理
        
        if hasattr(self, 'toolbar') and self.toolbar:
            if hasattr(self.toolbar, 'paint_menu') and self.toolbar.paint_menu:
                self.toolbar.paint_menu.close()
                self.toolbar.paint_menu.deleteLater()
            self.toolbar.close()
            self.toolbar.deleteLater()
        super().closeEvent(event)
