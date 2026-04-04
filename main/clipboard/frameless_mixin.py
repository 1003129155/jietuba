# -*- coding: utf-8 -*-
"""
无边框窗口拖拽与调整大小混入

提供 FramelessMixin，任何无边框 QWidget 只需多继承此类即可
获得 8 方向边缘 resize 和指定区域拖拽移动能力。
"""

from PySide6.QtCore import Qt, QPoint, QEvent
from PySide6.QtWidgets import QWidget


class FramelessMixin:
    """
    无边框窗口拖拽 + 8 方向 resize 混入。

    子类必须在 __init__ 中调用 ``self._init_frameless()``。
    """

    # ─────── 初始化 ───────

    def _init_frameless(self, edge_margin: int = 8):
        """初始化无边框拖拽/resize 状态。"""
        self._fl_edge_margin = edge_margin

        # 拖拽
        self._fl_is_dragging = False
        self._fl_drag_pos: QPoint | None = None

        # resize
        self._fl_resize_edge: str | None = None
        self._fl_resize_start_pos: QPoint | None = None
        self._fl_resize_start_geometry = None

        # 让自身感知鼠标移动
        self.setMouseTracking(True)
        self.installEventFilter(self)

    # ─────── 辅助方法 ───────

    def _fl_get_edge(self, local_pos: QPoint) -> str:
        """返回鼠标所在的边缘名称，空字符串表示不在边缘。"""
        x, y = local_pos.x(), local_pos.y()
        w, h = self.width(), self.height()
        m = self._fl_edge_margin

        on_left = x < m
        on_right = x > w - m
        on_top = y < m
        on_bottom = y > h - m

        if on_top and on_left:
            return "topleft"
        if on_top and on_right:
            return "topright"
        if on_bottom and on_left:
            return "bottomleft"
        if on_bottom and on_right:
            return "bottomright"
        if on_left:
            return "left"
        if on_right:
            return "right"
        if on_top:
            return "top"
        if on_bottom:
            return "bottom"
        return ""

    _EDGE_CURSORS = {
        "left":        Qt.CursorShape.SizeHorCursor,
        "right":       Qt.CursorShape.SizeHorCursor,
        "top":         Qt.CursorShape.SizeVerCursor,
        "bottom":      Qt.CursorShape.SizeVerCursor,
        "topleft":     Qt.CursorShape.SizeFDiagCursor,
        "bottomright": Qt.CursorShape.SizeFDiagCursor,
        "topright":    Qt.CursorShape.SizeBDiagCursor,
        "bottomleft":  Qt.CursorShape.SizeBDiagCursor,
    }

    def _fl_update_cursor(self, edge: str):
        cursor = self._EDGE_CURSORS.get(edge)
        if cursor:
            self.setCursor(cursor)
        else:
            self.unsetCursor()

    def _fl_do_resize(self, global_pos: QPoint):
        if not self._fl_resize_start_geometry or not self._fl_resize_start_pos:
            return

        dx = global_pos.x() - self._fl_resize_start_pos.x()
        dy = global_pos.y() - self._fl_resize_start_pos.y()

        geo = self._fl_resize_start_geometry
        new_x, new_y = geo.x(), geo.y()
        new_w, new_h = geo.width(), geo.height()
        min_w, min_h = self.minimumWidth(), self.minimumHeight()

        edge = self._fl_resize_edge

        if "left" in edge:
            new_w = max(min_w, geo.width() - dx)
            if new_w > min_w:
                new_x = geo.x() + dx
        if "right" in edge:
            new_w = max(min_w, geo.width() + dx)
        if "top" in edge:
            new_h = max(min_h, geo.height() - dy)
            if new_h > min_h:
                new_y = geo.y() + dy
        if "bottom" in edge:
            new_h = max(min_h, geo.height() + dy)

        self.setGeometry(new_x, new_y, new_w, new_h)

    def _fl_reset(self):
        """重置拖拽/resize 状态。"""
        self._fl_is_dragging = False
        self._fl_drag_pos = None
        self._fl_resize_edge = None
        self._fl_resize_start_pos = None
        self._fl_resize_start_geometry = None
        self.unsetCursor()

    # ─────── 可重写的钩子 ───────

    def _is_draggable_area(self, widget, local_pos: QPoint) -> bool:
        """
        子类重写此方法，返回 True 的区域允许拖拽移动窗口。
        默认返回 False（不可拖拽）。
        """
        return False

    # ─────── 递归启用追踪 ───────

    def _setup_mouse_tracking_recursive(self, widget):
        """递归为所有子控件启用鼠标追踪和安装事件过滤器。"""
        widget.setMouseTracking(True)
        widget.installEventFilter(self)
        for child in widget.findChildren(QWidget):
            child.setMouseTracking(True)
            child.installEventFilter(self)

    # ─────── 事件处理（供 eventFilter 调用） ───────

    def _fl_handle_event(self, obj, event) -> bool:
        """
        处理拖拽/resize 相关事件。返回 True 表示事件已消费。

        调用方应在 eventFilter 中调用此方法：
            if self._fl_handle_event(obj, event):
                return True
            return super().eventFilter(obj, event)
        """
        event_type = event.type()

        # 获取鼠标位置
        global_pos = None
        local_pos = None
        if hasattr(event, "globalPosition"):
            global_pos = event.globalPosition().toPoint()
            local_pos = self.mapFromGlobal(global_pos)

        # ── 鼠标移动 ──
        if event_type == QEvent.Type.MouseMove and local_pos:
            if self._fl_resize_edge:
                self._fl_do_resize(global_pos)
                return True
            if self._fl_is_dragging and self._fl_drag_pos:
                self.move(global_pos - self._fl_drag_pos)
                return True
            # 更新光标
            edge = self._fl_get_edge(local_pos)
            self._fl_update_cursor(edge)
            return False  # 不消费，允许后续处理

        # ── 鼠标按下 ──
        if event_type == QEvent.Type.MouseButtonPress and local_pos:
            if event.button() == Qt.MouseButton.LeftButton:
                edge = self._fl_get_edge(local_pos)
                if edge:
                    self._fl_resize_edge = edge
                    self._fl_resize_start_pos = global_pos
                    self._fl_resize_start_geometry = self.geometry()
                    return True
                if self._is_draggable_area(obj, local_pos):
                    self._fl_is_dragging = True
                    self._fl_drag_pos = global_pos - self.pos()
                    return True

        # ── 鼠标释放 ──
        if event_type == QEvent.Type.MouseButtonRelease:
            if event.button() == Qt.MouseButton.LeftButton:
                if self._fl_resize_edge or self._fl_is_dragging:
                    self._fl_reset()
                    return True

        return False
 