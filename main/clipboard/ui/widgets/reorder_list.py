# -*- coding: utf-8 -*-

"""管理窗口里可排序的分组 / 内容列表。

每行的 UserRole 存条目 id。排序结果以「谁移动了、落在哪两个 id 之间」发出，
与存储层 move_*_between(before_id, after_id) 的参数一一对应。
另一个列表的条目也可以拖到某一行上（内容拖进分组），结果以 item_dropped_on 发出。
"""

from typing import Callable, Optional

from PySide6.QtCore import QPoint, QRect, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QCursor, QDrag, QFont, QFontMetrics, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QAbstractItemView, QListWidget

from core import safe_event
from core.ui_scale import dialog_scaled, dialog_scaled_f
from core.ui_theme import get_ui_theme
from ui.fluent_lite.theme import css_color

ID_ROLE = Qt.ItemDataRole.UserRole

# 拖到离边缘这么近时开始自动滚动，越靠边越快。
_AUTO_SCROLL_MARGIN = 56
_AUTO_SCROLL_MAX_STEP = 28
_AUTO_SCROLL_INTERVAL_MS = 16


class ReorderListWidget(QListWidget):
    """支持拖拽、Alt+↑/↓ 和程序调用三种方式排序的列表。"""

    # moved_id, before_id（上方邻居）, after_id（下方邻居）
    item_moved = Signal(int, object, object)
    # 别的列表拖进来、放在某一行上：dragged_id, target_id
    item_dropped_on = Signal(object, object)
    # 本列表的条目开始 / 结束被拖动（用来在别处显示拖放提示）
    drag_started = Signal()
    drag_finished = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setDragDropMode(QAbstractItemView.DragDropMode.InternalMove)
        self.setDefaultDropAction(Qt.DropAction.MoveAction)
        self.setDropIndicatorShown(False)
        # Qt 自带的自动滚动只在边缘 16px 内触发，且从每 50ms 一格起步，长列表要
        # 拖好几次才能到位，这里换成自己的计时器。
        self.setAutoScroll(False)
        self.setMouseTracking(True)

        self._reorder_enabled = True
        self._drop_row: Optional[int] = None
        self._drop_target_row: Optional[int] = None
        self._foreign_source: Optional[QListWidget] = None
        self._foreign_filter: Optional[Callable[[object, object], bool]] = None
        self._awaiting_foreign_drop = False
        self._foreign_drag_inside = False
        self._drop_hint = ""
        self._scroll_step = 0
        self._scroll_timer = QTimer(self)
        self._scroll_timer.setInterval(_AUTO_SCROLL_INTERVAL_MS)
        self._scroll_timer.timeout.connect(self._auto_scroll_tick)

    # ------------------------------------------------------------------
    # 公共接口
    # ------------------------------------------------------------------

    def set_reorder_enabled(self, enabled: bool):
        """搜索过滤时相邻关系不完整，要关掉本列表内的拖拽和快捷键排序；拖到别的列表不受影响。"""
        self._reorder_enabled = enabled

    def accept_items_from(self, source: QListWidget, can_drop: Callable[[object, object], bool]):
        """允许 source 的条目拖到本列表的某一行上；can_drop(dragged_id, target_id) 决定能不能放。"""
        self._foreign_source = source
        self._foreign_filter = can_drop

    def set_awaiting_foreign_drop(self, awaiting: bool, hint: str = ""):
        """别的列表正在拖条目时提示可以拖到这里。

        拖动还没进入本列表时整块盖一层写着 hint 的遮罩；一进入就撤掉遮罩，
        只留虚线框、放不下的行变淡和目标行高亮，方便看清要放到哪一行。
        """
        self._awaiting_foreign_drop = awaiting
        self._drop_hint = hint
        if not awaiting:
            self._drop_target_row = None
            self._foreign_drag_inside = False
        self.viewport().update()

    def is_row_droppable(self, row: int) -> bool:
        """当前拖动中的条目能不能放到这一行；没有在拖时每行都算可放。"""
        if not self._awaiting_foreign_drop or self._foreign_source is None:
            return True
        dragged = self._foreign_source.currentItem()
        target = self.item(row)
        if dragged is None or target is None:
            return False
        return self._foreign_filter is None or self._foreign_filter(
            dragged.data(ID_ROLE), target.data(ID_ROLE)
        )

    def reorder_enabled(self) -> bool:
        return self._reorder_enabled

    def item_id(self, row: int):
        item = self.item(row)
        return item.data(ID_ROLE) if item is not None else None

    def row_of_id(self, item_id) -> int:
        for row in range(self.count()):
            if self.item(row).data(ID_ROLE) == item_id:
                return row
        return -1

    def move_row(self, source: int, target: int) -> bool:
        """把 source 行移到 target 位置（按移除前的行号计，允许 == count）。"""
        count = self.count()
        if not (0 <= source < count) or not (0 <= target <= count):
            return False
        if target in (source, source + 1):
            return False
        insert_at = target - 1 if target > source else target
        # takeItem 会让当前行先落到相邻条目上，外部会以为用户选了那一条；
        # 移动前后当前项其实没变，中间这次切换不对外发。
        self.blockSignals(True)
        try:
            item = self.takeItem(source)
            self.insertItem(insert_at, item)
            self.setCurrentRow(insert_at)
        finally:
            self.blockSignals(False)
        self.scrollToItem(item)
        self.item_moved.emit(
            item.data(ID_ROLE),
            self.item_id(insert_at - 1) if insert_at > 0 else None,
            self.item_id(insert_at + 1) if insert_at + 1 < self.count() else None,
        )
        return True

    def move_current_to_top(self) -> bool:
        return self.move_row(self.currentRow(), 0)

    def move_current_to_bottom(self) -> bool:
        return self.move_row(self.currentRow(), self.count())

    # ------------------------------------------------------------------
    # 键盘
    # ------------------------------------------------------------------

    @safe_event
    def keyPressEvent(self, event):
        if (
            self._reorder_enabled
            and event.modifiers() == Qt.KeyboardModifier.AltModifier
            and event.key() in (Qt.Key.Key_Up, Qt.Key.Key_Down)
        ):
            row = self.currentRow()
            if row >= 0:
                if event.key() == Qt.Key.Key_Up:
                    self.move_row(row, row - 1)
                else:
                    self.move_row(row, row + 2)
            event.accept()
            return
        super().keyPressEvent(event)

    # ------------------------------------------------------------------
    # 拖拽
    # ------------------------------------------------------------------

    def startDrag(self, supportedActions):
        item = self.currentItem()
        if item is None:
            return
        rect = self.visualItemRect(item)
        drag = QDrag(self)
        drag.setMimeData(self.model().mimeData(self.selectedIndexes()))
        drag.setPixmap(self._drag_pixmap(rect))
        drag.setHotSpot(self.viewport().mapFromGlobal(QCursor.pos()) - rect.topLeft())
        # 放下时由 dropEvent 自己挪行；exec 的返回值不再交给 Qt 去删源行。
        self.drag_started.emit()
        try:
            drag.exec(Qt.DropAction.MoveAction)
        finally:
            self._end_drag_feedback()
            self.drag_finished.emit()

    def _drag_pixmap(self, rect: QRect) -> QPixmap:
        source = self.viewport().grab(rect)
        pixmap = QPixmap(source.size())
        pixmap.setDevicePixelRatio(source.devicePixelRatio())
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setOpacity(0.85)
        painter.drawPixmap(0, 0, source)
        painter.end()
        return pixmap

    def _is_foreign_drag(self, event) -> bool:
        return self._foreign_source is not None and event.source() is self._foreign_source

    def _foreign_target_row(self, event) -> Optional[int]:
        target = self.itemAt(event.position().toPoint())
        dragged = self._foreign_source.currentItem() if self._foreign_source is not None else None
        if target is None or dragged is None:
            return None
        if self._foreign_filter is not None and not self._foreign_filter(
            dragged.data(ID_ROLE), target.data(ID_ROLE)
        ):
            return None
        return self.row(target)

    @safe_event
    def dragEnterEvent(self, event):
        if self._is_foreign_drag(event):
            self._foreign_drag_inside = True
            self.viewport().update()
        if (event.source() is self and self._reorder_enabled) or self._is_foreign_drag(event):
            event.setDropAction(Qt.DropAction.MoveAction)
            event.accept()
            return
        event.ignore()

    @safe_event
    def dragMoveEvent(self, event):
        pos = event.position().toPoint()
        if self._is_foreign_drag(event):
            self._drop_target_row = self._foreign_target_row(event)
            self._update_auto_scroll(pos.y())
            self.viewport().update()
            if self._drop_target_row is None:
                event.ignore()
                return
            event.setDropAction(Qt.DropAction.MoveAction)
            event.accept()
            return
        if event.source() is not self or not self._reorder_enabled:
            event.ignore()
            return
        self._drop_row = self._drop_row_at(pos)
        self._update_auto_scroll(pos.y())
        event.setDropAction(Qt.DropAction.MoveAction)
        event.accept()
        self.viewport().update()

    @safe_event
    def dragLeaveEvent(self, event):
        self._end_drag_feedback()
        event.accept()

    @safe_event
    def dropEvent(self, event):
        if self._is_foreign_drag(event):
            row = self._foreign_target_row(event)
            dragged = self._foreign_source.currentItem()
            self._end_drag_feedback()
            if row is None or dragged is None:
                event.ignore()
                return
            event.setDropAction(Qt.DropAction.MoveAction)
            event.accept()
            self.item_dropped_on.emit(dragged.data(ID_ROLE), self.item_id(row))
            return
        if event.source() is not self or not self._reorder_enabled:
            event.ignore()
            return
        target = self._drop_row_at(event.position().toPoint())
        source = self.currentRow()
        self._end_drag_feedback()
        event.setDropAction(Qt.DropAction.MoveAction)
        event.accept()
        self.move_row(source, target)

    def _drop_row_at(self, pos: QPoint) -> int:
        """插入位置：落在某行上半截插到它前面，下半截插到它后面。"""
        item = self.itemAt(pos)
        if item is None:
            if self.count() and pos.y() < self.visualItemRect(self.item(0)).top():
                return 0
            return self.count()
        row = self.row(item)
        rect = self.visualItemRect(item)
        return row if pos.y() < rect.center().y() else row + 1

    def _update_auto_scroll(self, y: int):
        margin = dialog_scaled(_AUTO_SCROLL_MARGIN)
        height = self.viewport().height()
        if y < margin:
            depth = (margin - max(y, 0)) / margin
            self._scroll_step = -max(1, round(depth * dialog_scaled(_AUTO_SCROLL_MAX_STEP)))
        elif y > height - margin:
            depth = (y - (height - margin)) / margin
            self._scroll_step = max(1, round(min(depth, 1.0) * dialog_scaled(_AUTO_SCROLL_MAX_STEP)))
        else:
            self._scroll_step = 0
        if self._scroll_step and not self._scroll_timer.isActive():
            self._scroll_timer.start()
        elif not self._scroll_step:
            self._scroll_timer.stop()

    def _auto_scroll_tick(self):
        bar = self.verticalScrollBar()
        before = bar.value()
        bar.setValue(before + self._scroll_step)
        if bar.value() == before:
            self._scroll_timer.stop()
            return
        # 视图滚动了但鼠标没动，插入位置要按新内容重算。
        pos = self.viewport().mapFromGlobal(QCursor.pos())
        self._drop_row = self._drop_row_at(pos)
        self.viewport().update()

    def _end_drag_feedback(self):
        self._scroll_timer.stop()
        self._scroll_step = 0
        self._drop_row = None
        self._drop_target_row = None
        self._foreign_drag_inside = False
        self.viewport().update()

    @safe_event
    def paintEvent(self, event):
        super().paintEvent(event)
        if self._awaiting_foreign_drop:
            self._paint_drop_frame()
            if not self._foreign_drag_inside:
                self._paint_drop_overlay()
        if self._drop_target_row is not None:
            self._paint_drop_target()
            return
        if self._drop_row is None or self.count() == 0:
            return
        if self._drop_row < self.count():
            y = self.visualItemRect(self.item(self._drop_row)).top()
        else:
            y = self.visualItemRect(self.item(self.count() - 1)).bottom() + 1
        inset = dialog_scaled(8)
        radius = dialog_scaled(3)
        color = QColor(get_ui_theme().tokens.accent)
        painter = QPainter(self.viewport())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(color)
        painter.drawRoundedRect(
            inset, y - dialog_scaled(1), self.viewport().width() - inset * 2, dialog_scaled(3),
            radius, radius,
        )
        painter.drawEllipse(QPoint(inset, y), radius + 1, radius + 1)
        painter.end()

    def _paint_drop_target(self):
        rect = self.visualItemRect(self.item(self._drop_target_row)).adjusted(
            dialog_scaled(4), dialog_scaled(2), -dialog_scaled(4), -dialog_scaled(2)
        )
        tokens = get_ui_theme().tokens
        painter = QPainter(self.viewport())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        fill = QColor(tokens.accent)
        fill.setAlphaF(0.12)
        painter.setBrush(fill)
        painter.setPen(QPen(QColor(tokens.accent), dialog_scaled_f(1.5)))
        radius = dialog_scaled(8)
        painter.drawRoundedRect(rect, radius, radius)
        painter.end()

    def _paint_drop_frame(self):
        tokens = get_ui_theme().tokens
        painter = QPainter(self.viewport())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen = QPen(QColor(tokens.accent), 1)
        pen.setStyle(Qt.PenStyle.DashLine)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        radius = dialog_scaled(10)
        painter.drawRoundedRect(self.viewport().rect().adjusted(1, 1, -1, -1), radius, radius)
        painter.end()

    def _paint_drop_overlay(self):
        """一层很淡的底色，提示文字放在居中的小胶囊里，下面的分组仍然看得清。"""
        tokens = get_ui_theme().tokens
        painter = QPainter(self.viewport())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.viewport().rect().adjusted(2, 2, -2, -2))
        fill = css_color(tokens.accent_soft)
        fill.setAlphaF(0.4)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(fill)
        radius = dialog_scaled(10)
        painter.drawRoundedRect(rect, radius, radius)
        if self._drop_hint:
            font = QFont(self.font())
            font.setPixelSize(dialog_scaled(12))
            font.setWeight(QFont.Weight.DemiBold)
            metrics = QFontMetrics(font)
            pad_x, pad_y = dialog_scaled(14), dialog_scaled(7)
            text = metrics.elidedText(
                self._drop_hint, Qt.TextElideMode.ElideRight, int(rect.width()) - pad_x * 4
            )
            pill = QRectF(0, 0, metrics.horizontalAdvance(text) + pad_x * 2, metrics.height() + pad_y * 2)
            pill.moveCenter(rect.center())
            painter.setFont(font)
            painter.setBrush(css_color(tokens.popup_background))
            painter.setPen(QPen(css_color(tokens.border), 1))
            painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2)
            painter.setPen(css_color(tokens.accent_text))
            painter.drawText(pill, Qt.AlignmentFlag.AlignCenter, text)
        painter.end()
