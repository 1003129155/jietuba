"""
遮罩浮层 —— 独立 QWidget，覆盖整个 ScreenshotWindow。

选区外半透明黑色遮罩。
作为 QWidget overlay 只需要在 selection_model.rectChanged 时局部 update()，
不参与 QGraphicsScene 的渲染管线，不影响 scene.render() 导出。

性能优化：
- 用 4 个 fillRect 代替 QPainterPath 相减（避免路径运算开销）
- 只标记新旧选区差异区域为脏区（避免全窗口重绘）
"""

from __future__ import annotations

from PySide6.QtCore import QRect, QRectF, Qt
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import QWidget
from core import safe_event


class MaskOverlayWidget(QWidget):
    """选区外半透明遮罩（QWidget 浮层）。

    职责单一：在选区外绘制半透明黑色遮罩。
    不绘制尺寸标注（由 SelectionItem 负责）。
    """

    def __init__(self, parent: QWidget, selection_model):
        super().__init__(parent)
        self._model = selection_model
        # 从主题管理器获取遮罩颜色
        from core.theme import get_theme
        self._mask_color = get_theme().mask_color
        self._last_local_sel = QRect()  # 上次绘制的选区（本地坐标，整数）

        # 透明穿透鼠标事件，不阻拦任何交互
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMouseTracking(False)

        # 选区变化时局部重绘
        self._model.rectChanged.connect(self._on_rect_changed)

        self.show()
        self.raise_()

    # ------------------------------------------------------------------
    # 外部接口
    # ------------------------------------------------------------------
    def rebind_model(self, selection_model):
        """切换到新的 SelectionModel（窗口复用时调用）。"""
        from core.qt_utils import safe_disconnect
        safe_disconnect(self._model.rectChanged, self._on_rect_changed)
        self._model = selection_model
        self._last_local_sel = QRect()
        self._model.rectChanged.connect(self._on_rect_changed)
        self.update()

    def set_mask_color(self, color: QColor):
        """允许外部调整遮罩颜色/透明度。"""
        self._mask_color = QColor(color)
        self.update()

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    def _scene_to_local(self, sel: QRectF) -> QRect:
        """场景坐标 → widget 本地坐标（整数像素）"""
        parent = self.parentWidget()
        if parent:
            return QRect(
                int(sel.x() - parent.x()),
                int(sel.y() - parent.y()),
                int(sel.width()),
                int(sel.height()),
            )
        return sel.toAlignedRect()

    def _on_rect_changed(self, rect: QRectF):
        # 只标记新旧选区的差异区域为脏区，避免全窗口重绘
        new_local = self._scene_to_local(rect) if not rect.isEmpty() else QRect()
        old_local = self._last_local_sel

        if old_local.isEmpty() and new_local.isEmpty():
            return

        # 两个矩形的并集 = 需要重绘的区域（多加几像素边距防止残影）
        margin = 5
        dirty = old_local.united(new_local).adjusted(-margin, -margin, margin, margin)
        self.update(dirty)

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)

        full = self.rect()

        if self._model.is_empty():
            # 没有选区，全屏遮罩
            painter.fillRect(full, self._mask_color)
            self._last_local_sel = QRect()
            painter.end()
            return

        # 有选区：用 4 个 fillRect 填充选区外的上/下/左/右四条带
        # 比 QPainterPath 相减快，因为不涉及路径运算，直接 4 次纯色矩形填充
        sel = self._model.rect()
        local_sel = self._scene_to_local(sel)
        self._last_local_sel = QRect(local_sel)

        color = self._mask_color
        fw = full.width()
        fh = full.height()
        sx = local_sel.x()
        sy = local_sel.y()
        sw = local_sel.width()
        sh = local_sel.height()

        # 上方条带：整行，从顶到选区上边
        if sy > 0:
            painter.fillRect(0, 0, fw, sy, color)
        # 下方条带：整行，从选区下边到底
        bottom = sy + sh
        if bottom < fh:
            painter.fillRect(0, bottom, fw, fh - bottom, color)
        # 左侧条带：只覆盖选区所在的行高
        if sx > 0:
            painter.fillRect(0, sy, sx, sh, color)
        # 右侧条带：只覆盖选区所在的行高
        right = sx + sw
        if right < fw:
            painter.fillRect(right, sy, fw - right, sh, color)

        painter.end()
 