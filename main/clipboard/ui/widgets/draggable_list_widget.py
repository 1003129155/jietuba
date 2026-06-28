# -*- coding: utf-8 -*-

"""可拖拽列表控件。

在 `QListWidget` 基础上补充拖拽视觉反馈和起始行记录，
用于管理窗口中的分组或内容排序交互。
"""

from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor, QCursor, QDrag, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QListWidget

from core import safe_event
from ui.fluent_lite.theme import ACCENT


class DraggableListWidget(QListWidget):
    """自定义列表控件，实现更好的拖拽视觉效果"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._drag_start_row = -1

    def startDrag(self, supportedActions):
        """重写拖拽开始事件，自定义拖拽样式"""
        item = self.currentItem()
        if not item:
            return

        self._drag_start_row = self.row(item)

        indexes = self.selectedIndexes()
        if not indexes:
            return

        mime_data = self.model().mimeData(indexes)
        drag = QDrag(self)
        drag.setMimeData(mime_data)

        rect = self.visualItemRect(item)
        pixmap = self.viewport().grab(rect)

        drag_pixmap = QPixmap(pixmap.size())
        drag_pixmap.fill(Qt.GlobalColor.transparent)

        painter = QPainter(drag_pixmap)
        painter.setOpacity(0.8)
        painter.drawPixmap(0, 0, pixmap)

        painter.setPen(QColor(ACCENT))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(0, 0, drag_pixmap.width() - 1, drag_pixmap.height() - 1)

        painter.end()

        drag.setPixmap(drag_pixmap)

        viewport_pos = self.viewport().mapFromGlobal(QCursor.pos())
        hot_spot = viewport_pos - rect.topLeft()
        drag.setHotSpot(hot_spot)

        drag.exec(supportedActions)
        self._drag_start_row = -1

    @safe_event
    def dropEvent(self, event):
        """重写放置事件，禁止拖到第一个位置"""
        drop_pos = event.position().toPoint() if hasattr(event, "position") else event.pos()
        drop_item = self.itemAt(drop_pos)

        if drop_item:
            drop_row = self.row(drop_item)
            item_rect = self.visualItemRect(drop_item)
            if drop_pos.y() < item_rect.center().y():
                target_row = drop_row
            else:
                target_row = drop_row + 1
        else:
            target_row = self.count()

        if target_row == 0 and self._drag_start_row > 0:
            event.ignore()
            return

        super().dropEvent(event)

    @safe_event
    def dragMoveEvent(self, event):
        """重写拖拽移动事件，控制视觉反馈"""
        super().dragMoveEvent(event)
        self.viewport().update()

    @safe_event
    def paintEvent(self, event):
        """重写绘制事件，绘制更明显的插入指示线"""
        super().paintEvent(event)

        if self.state() == QListWidget.State.DraggingState:
            painter = QPainter(self.viewport())

            pen = QPen(QColor(ACCENT))
            pen.setWidth(3)
            painter.setPen(pen)

            pos = self.viewport().mapFromGlobal(QCursor.pos())
            item = self.itemAt(pos)

            y = -1
            line_rect = None

            if item:
                item_index = self.row(item)
                rect = self.visualItemRect(item)

                if pos.y() < rect.center().y():
                    if item_index > 0:
                        y = rect.top()
                        line_rect = rect
                else:
                    y = rect.bottom()
                    line_rect = rect
            else:
                if self.count() > 0:
                    last_item = self.item(self.count() - 1)
                    rect = self.visualItemRect(last_item)
                    if pos.y() > rect.bottom():
                        y = rect.bottom()
                        line_rect = rect

            if y != -1 and line_rect:
                painter.drawLine(line_rect.left(), y, line_rect.right(), y)

                painter.setBrush(QColor(ACCENT))
                painter.setPen(Qt.PenStyle.NoPen)
                radius = 4
                painter.drawEllipse(QPoint(line_rect.left(), y), radius, radius)
                painter.drawEllipse(QPoint(line_rect.right(), y), radius, radius)

            painter.end()
