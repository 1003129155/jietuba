# -*- coding: utf-8 -*-
"""可拖动重排的一列行。

行没有做成 QListWidget 的条目再用内置拖放：InternalMove 会把被拖的条目删掉再
插入，setItemWidget 挂上去的下拉框、复选框会跟着丢失。这里每一行就是布局里的
普通部件，拖动时直接在布局里换位置。

工具栏排布和放大镜颜色格式两个对话框都要这套东西，所以抽在这里，行里放什么
由各自决定——它们只需给出一个带 grip 的行部件。
"""
from functools import partial

from PySide6.QtCore import QPoint, QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import QScrollArea, QVBoxLayout, QWidget

from core import safe_event
from core.ui_scale import dialog_scaled, dialog_scaled_f
from ui.fluent_lite import SimpleCardWidget, ui_tokens


class DragGrip(QWidget):
    """行首的拖动手柄（2×3 圆点）。只把按下、拖动、松开报出去，换位由列表做。"""

    pressed = Signal()
    dragged = Signal(int)   # 鼠标的全局 y
    released = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(dialog_scaled(16), dialog_scaled(28))
        self.setCursor(Qt.CursorShape.OpenHandCursor)

    @safe_event
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            self.pressed.emit()

    @safe_event
    def mouseMoveEvent(self, event):
        # 按下后 Qt 会隐式抓住鼠标，拖出手柄、拖出对话框也照样收得到移动事件
        if event.buttons() == Qt.MouseButton.LeftButton:
            self.dragged.emit(event.globalPosition().toPoint().y())

    @safe_event
    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setCursor(Qt.CursorShape.OpenHandCursor)
            self.released.emit()

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(ui_tokens(self).text_muted))
        center = QPointF(self.rect().center())
        for dx in (-3, 3):
            for dy in (-6, 0, 6):
                painter.drawEllipse(
                    center + QPointF(dialog_scaled(dx), dialog_scaled(dy)),
                    dialog_scaled_f(1.5),
                    dialog_scaled_f(1.5),
                )
        painter.end()


class DraggableRow(QWidget):
    """带拖动手柄的一行。子类往 content_layout 里放自己的内容。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._dragging = False
        self.grip = DragGrip(self)

    def set_dragging(self, dragging):
        """拖动中的行垫一层强调色，让用户看清自己拖的是哪一行"""
        self._dragging = dragging
        self.update()

    @safe_event
    def paintEvent(self, event):
        if not self._dragging:
            return
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(ui_tokens(self).accent_soft))
        painter.drawRoundedRect(self.rect(), dialog_scaled(8), dialog_scaled(8))
        painter.end()


class ReorderableRowList(QScrollArea):
    """一张卡片，里面竖着排若干可拖动换位的行。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._card = SimpleCardWidget()
        self._layout = QVBoxLayout(self._card)
        self._layout.setContentsMargins(
            dialog_scaled(6), dialog_scaled(6), dialog_scaled(6), dialog_scaled(6)
        )
        self._layout.setSpacing(0)
        self.setWidgetResizable(True)
        self.setFrameShape(QScrollArea.Shape.NoFrame)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setStyleSheet("QScrollArea { border: none; background: transparent; }")
        self.setWidget(self._card)

    def add_row(self, row, index: int = -1):
        row.setParent(self._card)
        row.grip.pressed.connect(partial(row.set_dragging, True))
        row.grip.dragged.connect(partial(self._drag_row, row))
        row.grip.released.connect(partial(row.set_dragging, False))
        if index < 0:
            self._layout.addWidget(row)
        else:
            self._layout.insertWidget(index, row)
        return row

    def remove_row(self, row):
        self._layout.removeWidget(row)
        row.setParent(None)
        row.deleteLater()

    def rows(self):
        """按当前显示顺序交出所有行。"""
        items = (self._layout.itemAt(i) for i in range(self._layout.count()))
        return [item.widget() for item in items if item.widget() is not None]

    def clear(self):
        for row in self.rows():
            self.remove_row(row)

    def content_height(self) -> int:
        self._layout.activate()
        return self._layout.sizeHint().height() + 2

    def _place(self, row, index):
        self._layout.removeWidget(row)
        self._layout.insertWidget(index, row)

    def _drag_row(self, row, global_y):
        """被拖的行跟着鼠标换位：它该排第几，就看其余行里有几行的中线在鼠标上方"""
        y = self._card.mapFromGlobal(QPoint(0, global_y)).y()
        index = sum(
            1 for other in self.rows()
            if other is not row and other.geometry().center().y() < y
        )
        if self._layout.indexOf(row) != index:
            self._place(row, index)
            # 立刻按新顺序算出各行几何，下一次鼠标移动才能基于新位置判断
            self._layout.activate()
        # 拖到可见区域外时让滚动跟上
        self.ensureWidgetVisible(row, 0, 0)
