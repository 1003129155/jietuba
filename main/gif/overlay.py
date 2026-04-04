# -*- coding: utf-8 -*-
"""蓝色/红色选区边框覆盖层 — 动态穿透切换 + RESIZE 模式下 4 边拖拽"""

import ctypes
from enum import Enum, auto

from PySide6.QtWidgets import QWidget
from PySide6.QtCore import Qt, QRect, QPoint, Signal
from PySide6.QtGui import QPainter, QPen, QColor, QCursor
from core import safe_event

GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_LAYERED     = 0x00080000

# 边框外扩量（供鼠标检测，不画在内容上）
_BORDER_W    = 3    # 实际绘制边框宽度（px）
_HIT_MARGIN  = 8    # 边框可点击宽度（px，居中于边框线）


class OverlayMode(Enum):
    PASSTHROUGH = auto()   # 完全穿透，鼠标事件不响应
    RESIZE      = auto()   # 可拖动 4 条边调整尺寸


# 颜色常量
_COLOR_IDLE    = QColor("#2196F3")   # 蓝色（未录制）
_COLOR_RECORD  = QColor("#F44336")   # 红色（录制中）


class CaptureOverlay(QWidget):
    """选区边框，支持穿透 / RESIZE 两种模式，颜色可切换（蓝/红）"""

    rect_changed = Signal(QRect)

    def __init__(self, rect: QRect, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        self._rect   = QRect(rect)
        self._mode   = OverlayMode.PASSTHROUGH
        self._color  = _COLOR_IDLE

        self._drag_edge: str   = ""
        self._drag_origin      = QPoint()
        self._drag_rect_origin = QRect()

        self.setMouseTracking(True)
        self.setGeometry(self._outer_geom())
        self._set_passthrough(True)

    # ══════════════════════════════════════════════
    # 公开方法
    # ══════════════════════════════════════════════

    def set_mode(self, mode: OverlayMode):
        if self._mode == mode:
            return
        self._mode = mode
        self._set_passthrough(mode == OverlayMode.PASSTHROUGH)
        self.update()

    def set_recording(self, recording: bool):
        """录制中切红色，未录制切蓝色"""
        self._color = _COLOR_RECORD if recording else _COLOR_IDLE
        self.update()

    def update_rect(self, rect: QRect):
        self._rect = QRect(rect)
        self.setGeometry(self._outer_geom())
        self.update()

    def capture_rect(self) -> QRect:
        return QRect(self._rect)

    # ══════════════════════════════════════════════
    # 穿透切换
    # ══════════════════════════════════════════════

    def _set_passthrough(self, enable: bool):
        hwnd = int(self.winId())
        style = ctypes.windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if enable:
            style |= WS_EX_TRANSPARENT | WS_EX_LAYERED
        else:
            style &= ~WS_EX_TRANSPARENT
            style |= WS_EX_LAYERED
        ctypes.windll.user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style)

    # ══════════════════════════════════════════════
    # 绘制
    # ══════════════════════════════════════════════

    @safe_event
    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        m = _HIT_MARGIN // 2
        inner = QRect(m, m, self.width() - m * 2, self.height() - m * 2)

        pen = QPen(self._color, _BORDER_W)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRect(inner)
        p.end()

    # ══════════════════════════════════════════════
    # 鼠标事件（仅 RESIZE 模式响应）
    # ══════════════════════════════════════════════

    @safe_event
    def mousePressEvent(self, ev):
        if self._mode != OverlayMode.RESIZE:
            return super().mousePressEvent(ev)
        edge = self._hit_edge(ev.pos())
        if edge:
            self._drag_edge = edge
            self._drag_origin = ev.globalPosition().toPoint()
            self._drag_rect_origin = QRect(self._rect)

    @safe_event
    def mouseMoveEvent(self, ev):
        if self._mode != OverlayMode.RESIZE:
            return super().mouseMoveEvent(ev)
        if self._drag_edge:
            delta = ev.globalPosition().toPoint() - self._drag_origin
            self._apply_resize(delta)
            return
        # hover：更新光标
        edge = self._hit_edge(ev.pos())
        cursor_map = {
            "t": Qt.CursorShape.SizeVerCursor,
            "b": Qt.CursorShape.SizeVerCursor,
            "l": Qt.CursorShape.SizeHorCursor,
            "r": Qt.CursorShape.SizeHorCursor,
            "tl": Qt.CursorShape.SizeFDiagCursor,
            "br": Qt.CursorShape.SizeFDiagCursor,
            "tr": Qt.CursorShape.SizeBDiagCursor,
            "bl": Qt.CursorShape.SizeBDiagCursor,
        }
        self.setCursor(QCursor(cursor_map.get(edge, Qt.CursorShape.ArrowCursor)))

    @safe_event
    def mouseReleaseEvent(self, ev):
        if self._drag_edge:
            self._drag_edge = ""
            self.rect_changed.emit(QRect(self._rect))
        super().mouseReleaseEvent(ev)

    # ══════════════════════════════════════════════
    # 边命中检测
    # ══════════════════════════════════════════════

    def _hit_edge(self, pos) -> str:
        x, y = pos.x(), pos.y()
        w, h = self.width(), self.height()
        half = _HIT_MARGIN // 2

        on_left   = abs(x - half) <= half
        on_right  = abs(x - (w - half)) <= half
        on_top    = abs(y - half) <= half
        on_bottom = abs(y - (h - half)) <= half

        # 角落优先检测
        if on_top    and on_left:  return "tl"
        if on_top    and on_right: return "tr"
        if on_bottom and on_left:  return "bl"
        if on_bottom and on_right: return "br"

        in_lr_range = half <= y <= h - half
        in_tb_range = half <= x <= w - half

        if on_left   and in_lr_range: return "l"
        if on_right  and in_lr_range: return "r"
        if on_top    and in_tb_range: return "t"
        if on_bottom and in_tb_range: return "b"
        return ""

    def _apply_resize(self, delta: QPoint):
        r = QRect(self._drag_rect_origin)
        dx, dy = delta.x(), delta.y()
        e = self._drag_edge
        if "l" in e: r.setLeft(r.left() + dx)
        if "r" in e: r.setRight(r.right() + dx)
        if "t" in e: r.setTop(r.top() + dy)
        if "b" in e: r.setBottom(r.bottom() + dy)
        if r.width()  < 64: r.setWidth(64)
        if r.height() < 64: r.setHeight(64)
        self._rect = r.normalized()
        self.setGeometry(self._outer_geom())
        self.update()

    def _outer_geom(self) -> QRect:
        m = _HIT_MARGIN // 2
        return QRect(
            self._rect.x() - m,
            self._rect.y() - m,
            self._rect.width()  + m * 2,
            self._rect.height() + m * 2,
        )
 