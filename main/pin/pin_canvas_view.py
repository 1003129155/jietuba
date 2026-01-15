"""
专用于钉图窗口的 CanvasView 封装

架构说明：
- PinCanvasView 是钉图窗口的**唯一内容渲染者**
- 它直接使用 Qt 的 QGraphicsView 渲染机制（GPU 加速 + 增量更新）
- 圆角裁剪通过 viewport mask 实现
- 不再需要 scene.render() 手动渲染
"""

from PyQt6.QtCore import Qt, QRectF
from PyQt6.QtWidgets import QFrame
from PyQt6.QtGui import QPainter, QRegion, QPainterPath

from canvas import CanvasView
from core import log_debug, log_info


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
        
        # 🎨 只启用图片平滑缩放（避免放大后模糊）
        self.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        
        # 🔥 使用智能更新模式（只更新变化区域）
        self.setViewportUpdateMode(CanvasView.ViewportUpdateMode.SmartViewportUpdate)
        
        log_info("PinCanvasView 创建成功（唯一内容渲染者）", "PinCanvasView")
    
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
    
    def resizeEvent(self, event):
        """重写 resize 事件，更新圆角遮罩"""
        super().resizeEvent(event)
        self._update_viewport_mask()

    # ------------------------------------------------------------------
    # 鼠标事件：在非编辑状态下将拖动交给 PinWindow，编辑状态沿用原逻辑
    # ------------------------------------------------------------------

    def mousePressEvent(self, event):
        log_debug(f"鼠标按下: is_editing={self.pin_canvas.is_editing}, 按钮={event.button()}", "PinCanvasView")
        
        if event.button() == Qt.MouseButton.LeftButton and not self.pin_canvas.is_editing:
            self._window_dragging = True
            self.pin_window.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            log_debug("开始拖动窗口", "PinCanvasView")
            return
        
        log_debug("调用父类mousePressEvent（截图窗口逻辑）", "PinCanvasView")
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if hasattr(self.pin_window, "_set_hover_state"):
            self.pin_window._set_hover_state(True)
        if self._window_dragging:
            self.pin_window.update_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if self._window_dragging and event.button() == Qt.MouseButton.LeftButton:
            self._window_dragging = False
            self.pin_window.end_window_drag()
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def wheelEvent(self, event):
        """非编辑状态下让父窗口处理滚轮缩放"""
        if not self.pin_canvas.is_editing:
            event.ignore()  # 交给 PinWindow 处理
            return
        super().wheelEvent(event)
