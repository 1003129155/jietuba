"""专用于钉图窗口的 CanvasView 封装"""

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QFrame
from PyQt6.QtGui import QPainter

from canvas import CanvasView


class PinCanvasView(CanvasView):
    """在钉图窗口中复用 CanvasView，并兼顾窗口拖动/缩放"""

    def __init__(self, scene, pin_window, pin_canvas):
        super().__init__(scene)
        self.pin_window = pin_window
        self.pin_canvas = pin_canvas
        self._window_dragging = False

        # 细节调整：透明背景、无边框
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet("background: transparent;")
        
        # 🔥 启用高质量渲染 - 解决缩放模糊问题
        self.setRenderHints(
            QPainter.RenderHint.Antialiasing |
            QPainter.RenderHint.SmoothPixmapTransform |
            QPainter.RenderHint.TextAntialiasing
        )
        
        # 🔥 优化视口更新模式
        self.setViewportUpdateMode(CanvasView.ViewportUpdateMode.FullViewportUpdate)

    # ------------------------------------------------------------------
    # 鼠标事件：在非编辑状态下将拖动交给 PinWindow，编辑状态沿用原逻辑
    # ------------------------------------------------------------------

    def mousePressEvent(self, event):
        print(f"🖱️ [PinCanvasView] 鼠标按下: is_editing={self.pin_canvas.is_editing}, 按钮={event.button()}")
        
        if event.button() == Qt.MouseButton.LeftButton and not self.pin_canvas.is_editing:
            self._window_dragging = True
            self.pin_window.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            print(f"    → 开始拖动窗口")
            return
        
        print(f"    → 调用父类mousePressEvent（截图窗口逻辑）")
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
