# -*- coding: utf-8 -*-
"""
选区装饰浮层 —— 选区边框和控制点的独立合成层。
"""

from __future__ import annotations

import shiboken6
from PySide6.QtCore import QRect, Qt
from PySide6.QtGui import QPainter, QRegion
from PySide6.QtWidgets import QWidget

from core import safe_event


class SelectionOverlayWidget(QWidget):
    """绘制 SelectionItem 装饰的透明浮层，铺满窗口，按需局部失效。"""

    # 抗锯齿和描边会溢出 visual_bounds() 一点点
    EDGE_ALLOWANCE = 3

    def __init__(self, parent: QWidget, selection_item, selection_model):
        super().__init__(parent)
        self._item = selection_item
        self._model = selection_model
        # 上一次画过的区域（本地坐标），下次失效时要连它一起擦
        self._painted = QRect()

        # 鼠标事件必须穿透：选区的拖拽命中仍由场景里的 SelectionItem 负责
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMouseTracking(False)

        self._connect_model()
        self._item.repaint_requested = self.refresh
        self.show()
        self.raise_()

    # ------------------------------------------------------------------
    # 外部接口
    # ------------------------------------------------------------------
    def rebind(self, selection_item, selection_model):
        """切换到新的 scene（截图窗口跨会话复用时调用）。"""
        from core.qt_utils import safe_disconnect
        safe_disconnect(self._model.rectChanged, self._on_changed)
        safe_disconnect(self._model.draggingChanged, self._on_changed)
        safe_disconnect(self._model.confirmed, self._on_changed)
        if self._live_item() is not None:
            self._item.repaint_requested = None
        self._item = selection_item
        self._model = selection_model
        self._item.repaint_requested = self.refresh
        self._connect_model()
        # 不清 _painted、也不整层 update()：旧 scene 的装饰像素可能还在屏幕上，
        # 交给 refresh() 的「新 ∪ 旧」一起带走即可。
        self.refresh()

    def refresh(self):
        """选区状态一变就调它：失效"新位置 ∪ 旧位置"。"""
        dirty = self._chrome_rect().united(self._painted)
        if not dirty.isEmpty():
            self.update(dirty)

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------
    def _connect_model(self):
        # 手柄的显隐取决于拖拽/确认状态，不只是矩形变化
        self._model.rectChanged.connect(self._on_changed)
        self._model.draggingChanged.connect(self._on_changed)
        self._model.confirmed.connect(self._on_changed)

    def _on_changed(self, *_):
        self.refresh()

    def _origin(self):
        """场景坐标 → 本地坐标的平移量。

        场景用屏幕物理坐标，本层铺满窗口，所以差值就是窗口在桌面上的位置。
        """
        parent = self.parentWidget()
        return (-parent.x(), -parent.y()) if parent else (0, 0)

    def _live_item(self):
        """场景已销毁但浮层还没删时，item 的 C++ 对象会先没。

        本层的生命周期挂在截图窗口上，scene/view 却是每次会话重建的，两者的销毁
        顺序不由这里决定。render() 目前只读 model、不碰 item 的 C++ 侧，所以就算
        漏掉也不会崩——但那是巧合，不是保证。
        """
        item = self._item
        return item if item is not None and shiboken6.isValid(item) else None

    def _chrome_rect(self) -> QRect:
        """装饰在本地坐标下的包围盒；没东西可画时返回空矩形。"""
        item = self._live_item()
        if item is None:
            return QRect()
        bounds = item.visual_bounds()
        if bounds.isNull() or bounds.isEmpty():
            return QRect()
        dx, dy = self._origin()
        pad = self.EDGE_ALLOWANCE
        rect = bounds.translated(dx, dy).toAlignedRect()
        return rect.adjusted(-pad, -pad, pad, pad).intersected(self.rect())

    def _still_on_screen(self, repainted: QRegion, drawn: QRect) -> QRect:
        """这一帧过后，屏幕上还留着装饰像素的范围。

        不能直接记成"装饰现在在哪"：本层压在遮罩上，遮罩自己失效一小块就会顺带
        重绘这一块本层，而这种被动重绘完全可能发生在选区已经变了、refresh() 还
        没轮到的中间态。

        所以按"这次重绘到底盖住了多少旧区域"来算：没盖全就把旧区域继续记着，
        下一次失效连它一起带走，然后自然收敛回精确值。

        用相减判断"盖全了没有"，不能用 QRegion.contains(QRect)：它的语义是相交
        而不是包含，沾上一点就返回 True。
        """
        if QRegion(self._painted).subtracted(repainted).isEmpty():
            return drawn
        return drawn.united(self._painted)

    @safe_event
    def paintEvent(self, event):
        item = self._live_item()
        if item is None or self._model.is_empty():
            self._painted = self._still_on_screen(event.region(), QRect())
            return

        painter = QPainter(self)
        try:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.translate(*self._origin())
            item.render(painter)
        finally:
            painter.end()

        # 被动重绘也会走到这里，所以 _painted 始终反映屏幕上的真实情况，
        # 不依赖任何人来通知。
        self._painted = self._still_on_screen(event.region(), self._chrome_rect())
