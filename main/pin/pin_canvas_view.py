"""
专用于钉图窗口的 CanvasView 封装

架构说明：
- PinCanvasView 是钉图窗口的**唯一内容渲染者**
- 它直接使用 Qt 的 QGraphicsView 渲染机制（GPU 加速 + 增量更新）
- 圆角裁剪通过 viewport mask 实现
- 不再需要 scene.render() 手动渲染
"""

from PySide6.QtCore import Qt, QRectF
from PySide6.QtWidgets import QFrame
from PySide6.QtGui import QPainter, QRegion, QPainterPath

from canvas import CanvasView
from core import log_info, safe_event


class PinCanvasView(CanvasView):
    """
    钉图画布视图 - 唯一的内容渲染者
    
    特点：
    - 直接显示 CanvasScene 内容（GPU 加速）
    - 支持圆角裁剪（通过 viewport mask）
    - 透明背景
    - 处理窗口拖动和缩放
    """

    def __init__(self, scene, pin_window, pin_canvas):
        super().__init__(scene)
        self.pin_window = pin_window
        self.pin_canvas = pin_canvas
        self._window_dragging = False
        
        # 圆角半径（与 ShadowWindow 保持一致）
        self._corner_radius = 8

        # 透明背景、无边框
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet("background: transparent;")
        
        # 设置视口背景透明
        self.viewport().setAutoFillBackground(False)
        self.setBackgroundBrush(Qt.GlobalColor.transparent)
        
        # 只启用图片平滑缩放（避免放大后模糊）
        self.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        
        # 使用智能更新模式（只更新变化区域）
        self.setViewportUpdateMode(CanvasView.ViewportUpdateMode.SmartViewportUpdate)
        
    def set_corner_radius(self, radius: float):
        """设置圆角半径"""
        self._corner_radius = radius
        self._update_viewport_mask()
    
    def _update_viewport_mask(self):
        """更新视口的圆角遮罩"""
        if self._corner_radius <= 0:
            self.viewport().clearMask()
            return
        
        # 创建圆角矩形路径
        rect = QRectF(self.viewport().rect())
        path = QPainterPath()
        path.addRoundedRect(rect, self._corner_radius, self._corner_radius)
        
        # 转换为 QRegion 并设置为遮罩
        region = QRegion(path.toFillPolygon().toPolygon())
        self.viewport().setMask(region)
    
    @safe_event
    def resizeEvent(self, event):
        """重写 resize 事件，更新圆角遮罩"""
        super().resizeEvent(event)
        self._update_viewport_mask()

    @safe_event
    def enterEvent(self, event):
        """鼠标进入钉图：非编辑状态用箭头，编辑状态走父类（工具光标）"""
        if self.pin_canvas.is_editing:
            super().enterEvent(event)
        else:
            self.setCursor(Qt.CursorShape.ArrowCursor)

    # ------------------------------------------------------------------
    # 鼠标事件：在非编辑状态下将拖动交给 PinWindow，编辑状态沿用原逻辑
    # ------------------------------------------------------------------

    @safe_event
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and not self.pin_canvas.is_editing:
            self._window_dragging = True
            self.pin_window.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        
        super().mousePressEvent(event)

    @safe_event
    def mouseMoveEvent(self, event):
        if hasattr(self.pin_window, "_set_hover_state"):
            self.pin_window._set_hover_state(True)
        if self._window_dragging:
            self.pin_window.update_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mouseMoveEvent(event)

    @safe_event
    def mouseReleaseEvent(self, event):
        if self._window_dragging and event.button() == Qt.MouseButton.LeftButton:
            self._window_dragging = False
            self.pin_window.end_window_drag()
            event.accept()
            return
        if event.button() == Qt.MouseButton.RightButton:
            self.pin_window.show_context_menu(event.globalPosition().toPoint())
            event.accept()
            return
        super().mouseReleaseEvent(event)

    @safe_event
    def wheelEvent(self, event):
        """非编辑状态下让父窗口处理滚轮缩放"""
        if not self.pin_canvas.is_editing:
            event.ignore()  # 交给 PinWindow 处理
            return
        super().wheelEvent(event)

    @safe_event
    def keyPressEvent(self, event):
        """
        键盘事件

        钉图快捷键已由 ShortcutManager 统一管理：
        - PinEditShortcutHandler  (priority=50): 编辑模式 Ctrl+C / ESC
        - PinNormalShortcutHandler (priority=40): 普通模式 Ctrl+C / R / ESC
        
        以下为备用路径（当 ShortcutManager 未拦截时的兜底处理）。
        """
        if event.key() == Qt.Key.Key_Escape and self.pin_canvas.is_editing:
            # 编辑模式下 ESC：退出工具，不关闭窗口
            self.pin_canvas.deactivate_tool()
            if (self.pin_window.toolbar
                    and hasattr(self.pin_window.toolbar, 'current_tool')
                    and self.pin_window.toolbar.current_tool):
                for btn in self.pin_window.toolbar.tool_buttons.values():
                    btn.setChecked(False)
                self.pin_window.toolbar.current_tool = None
            event.accept()
            return

        super().keyPressEvent(event)
 