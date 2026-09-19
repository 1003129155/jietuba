# -*- coding: utf-8 -*-
"""回放模式工具栏 — 范围滑块 / 播放 / 速度 / 保存 / 复制 / 重新录制"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QWidget, QPushButton, QLabel,
    QHBoxLayout, QVBoxLayout,
)
from PySide6.QtCore import Qt, QRect, Signal, QSize
from PySide6.QtGui import (
    QPainter, QColor, QPen, QCursor, QBrush,
)

from ._widgets import svg_icon as _svg_icon, ClickMenuButton as _ClickMenuButton
from core import safe_event
from core.i18n import make_tr
from core.ui_scale import get_ui_scale, scaled


_tr = make_tr("GifPlaybackToolbar")


# ──────────────────────────────────────────────
# RangeSlider : 带双手柄（trim）+ 播放指针的自定义滑块
# ──────────────────────────────────────────────
class RangeSlider(QWidget):
    """自定义范围滑块：trim_start / trim_end 手柄 + playhead 播放指针"""

    trim_changed   = Signal(int, int)   # (start_index, end_index)
    seek_requested = Signal(int)        # 点击空白处快速跳转

    # 基准尺寸（100% 下的实际像素）。手柄的点击判定也走这几个值，
    # 显示和可点区域才不会在缩放后错开。
    BASE_TRACK_H = 6
    BASE_HANDLE_W = 10
    BASE_HANDLE_H = 20
    BASE_PLAYHEAD_W = 4
    BASE_HEIGHT = 32
    BASE_MIN_WIDTH = 200
    BASE_GRAB_SLACK = 4

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self.apply_scale()

        self._total = 1          # 总帧数
        self._trim_start = 0
        self._trim_end = 0
        self._playhead = 0       # 当前播放帧索引

        self._dragging: str | None = None  # "start" | "end" | None

    # ── 外部 API ──

    def apply_scale(self):
        """按当前比例重算时间轴自身的尺寸"""
        self.setFixedHeight(scaled(self.BASE_HEIGHT))
        self.setMinimumWidth(scaled(self.BASE_MIN_WIDTH))
        self.update()

    def set_range(self, total: int):
        self._total = max(total, 1)
        self._trim_start = 0
        self._trim_end = self._total - 1
        self._playhead = 0
        self.update()

    def set_playhead(self, index: int):
        self._playhead = max(0, min(index, self._total - 1))
        self.update()

    def set_trim(self, start: int, end: int):
        self._trim_start = max(0, start)
        self._trim_end = min(end, self._total - 1)
        self.update()

    def trim_start(self) -> int:
        return self._trim_start

    def trim_end(self) -> int:
        return self._trim_end

    def get_trim_range(self) -> tuple:
        """返回当前裁剪范围 (start, end)"""
        return self._trim_start, self._trim_end

    # ── 坐标转换 ──

    def _track_rect(self) -> QRect:
        track_h = scaled(self.BASE_TRACK_H)
        margin = scaled(self.BASE_HANDLE_W)
        y = (self.height() - track_h) // 2
        return QRect(margin, y, self.width() - 2 * margin, track_h)

    def _index_to_x(self, index: int) -> int:
        tr = self._track_rect()
        if self._total <= 1:
            return tr.left()
        return tr.left() + int(index / (self._total - 1) * tr.width())

    def _x_to_index(self, x: int) -> int:
        tr = self._track_rect()
        ratio = max(0.0, min(1.0, (x - tr.left()) / max(tr.width(), 1)))
        return round(ratio * (self._total - 1))

    def _handle_rect(self, index: int) -> QRect:
        w, h = scaled(self.BASE_HANDLE_W), scaled(self.BASE_HANDLE_H)
        x = self._index_to_x(index)
        y = (self.height() - h) // 2
        return QRect(x - w // 2, y, w, h)

    def _start_handle_rect(self) -> QRect:
        return self._handle_rect(self._trim_start)

    def _end_handle_rect(self) -> QRect:
        return self._handle_rect(self._trim_end)

    # ── 绘制 ──

    @safe_event
    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        tr = self._track_rect()

        # 背景轨道
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(QColor(220, 220, 220)))
        p.drawRoundedRect(tr, 3, 3)

        # 选中区域（蓝色高亮）
        x1 = self._index_to_x(self._trim_start)
        x2 = self._index_to_x(self._trim_end)
        sel = QRect(x1, tr.top(), x2 - x1, tr.height())
        p.setBrush(QBrush(QColor(33, 150, 243, 120)))
        p.drawRoundedRect(sel, 3, 3)

        # trim 手柄
        for rect, color in [
            (self._start_handle_rect(), QColor(33, 150, 243)),
            (self._end_handle_rect(), QColor(33, 150, 243)),
        ]:
            p.setBrush(QBrush(color))
            p.setPen(QPen(color.darker(120), 1))
            p.drawRoundedRect(rect, 2, 2)

        # 播放指针
        playhead_w = scaled(self.BASE_PLAYHEAD_W)
        handle_h = scaled(self.BASE_HANDLE_H)
        px = self._index_to_x(self._playhead)
        py = (self.height() - handle_h) // 2
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(QColor(244, 67, 54)))
        p.drawRoundedRect(QRect(px - playhead_w // 2, py, playhead_w, handle_h), 2, 2)

        p.end()

    # ── 交互 ──

    @safe_event
    def mousePressEvent(self, ev):
        pos = ev.pos()
        slack = scaled(self.BASE_GRAB_SLACK)
        if self._start_handle_rect().adjusted(-slack, -slack, slack, slack).contains(pos):
            self._dragging = "start"
        elif self._end_handle_rect().adjusted(-slack, -slack, slack, slack).contains(pos):
            self._dragging = "end"
        else:
            # 点击空白处 → seek
            idx = self._x_to_index(pos.x())
            self.seek_requested.emit(idx)
        super().mousePressEvent(ev)

    @safe_event
    def mouseMoveEvent(self, ev):
        if self._dragging:
            idx = self._x_to_index(ev.pos().x())
            if self._dragging == "start":
                self._trim_start = max(0, min(idx, self._trim_end - 1))
            else:
                self._trim_end = min(self._total - 1, max(idx, self._trim_start + 1))
            self.trim_changed.emit(self._trim_start, self._trim_end)
            self.update()
        super().mouseMoveEvent(ev)

    @safe_event
    def mouseReleaseEvent(self, ev):
        self._dragging = None
        super().mouseReleaseEvent(ev)


# ──────────────────────────────────────────────
# PlaybackToolbar
# ──────────────────────────────────────────────
class PlaybackToolbar(QWidget):
    """回放阶段悬浮工具栏：范围裁剪 + 播放控制 + 导出"""

    play_pause      = Signal()
    speed_changed   = Signal(float)
    seek_requested  = Signal(int)
    trim_changed    = Signal(int, int)
    save_requested  = Signal()   # 保存 GIF
    copy_requested  = Signal()   # 复制 GIF
    rerecord        = Signal()
    close_requested = Signal()
    cursor_toggled  = Signal(bool)   # 鼠标光标显示开关

    # 基准尺寸（100% 下的实际像素）
    BASE_BTN_W = 34
    BASE_BTN_H = 30
    BASE_ICON = 22
    BASE_HEIGHT = 76
    BASE_MIN_WIDTH = 400

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        self._playing = False
        self._build_ui()
        # 改比例后自行重算尺寸（连接随本部件销毁自动断开）
        get_ui_scale().scale_changed.connect(self.apply_scale)

    def _container_qss(self) -> str:
        return f"""
            QWidget {{
                background-color: white;
                border: 2px solid #333;
                border-radius: {scaled(6)}px;
            }}
            QPushButton {{
                background: transparent;
                border: none;
                border-radius: {scaled(4)}px;
                font-size: {scaled(13)}px;
            }}
            QPushButton:hover {{ background: rgba(0,0,0,0.06); }}
            QPushButton:pressed {{ background: rgba(0,0,0,0.12); }}
            QPushButton:checked {{ background: rgba(64,224,208,0.22); border: 1px solid rgba(64,224,208,0.6); }}
            QLabel {{ border: none; }}
        """

    def _set_button_icon(self, button, svg: str):
        """设图标并记下用的是哪张 svg —— svg 按像素光栅化，改比例要按新尺寸重画"""
        self._button_svgs[button] = svg
        button.setIcon(_svg_icon(svg, scaled(self.BASE_ICON)))

    def apply_scale(self):
        """按当前比例重算回放工具栏和时间轴的尺寸，播放位置和裁剪范围不动。"""
        self._container.setStyleSheet(self._container_qss())
        self._vbox.setContentsMargins(scaled(10), scaled(6), scaled(10), scaled(6))
        self._vbox.setSpacing(scaled(4))
        self._row.setSpacing(scaled(6))
        self._slider.apply_scale()
        self._time_label.setStyleSheet(f"color: #666; font-size: {scaled(12)}px;")
        self._speed_label.apply_scale()
        btn_w, btn_h = scaled(self.BASE_BTN_W), scaled(self.BASE_BTN_H)
        icon = scaled(self.BASE_ICON)
        for button, svg in self._button_svgs.items():
            button.setFixedSize(btn_w, btn_h)
            button.setIconSize(QSize(icon, icon))
            button.setIcon(_svg_icon(svg, icon))
        self.setMinimumWidth(scaled(self.BASE_MIN_WIDTH))
        self.setFixedHeight(scaled(self.BASE_HEIGHT))

    def _build_ui(self):
        container = QWidget(self)
        self._container = container
        self._button_svgs = {}

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        outer.addWidget(container)

        vbox = QVBoxLayout(container)
        self._vbox = vbox

        # 第一行：RangeSlider
        self._slider = RangeSlider()
        self._slider.seek_requested.connect(self.seek_requested.emit)
        self._slider.trim_changed.connect(self._on_trim_changed)
        vbox.addWidget(self._slider)

        # 第二行：按钮
        row = QHBoxLayout()
        self._row = row

        # 播放/暂停  ← 回放窗口：停止时显示"重开录制"，播放时显示"暂停录制"
        self._play_btn = QPushButton()
        self._set_button_icon(self._play_btn, "重开录制.svg")   # 初始=停止状态
        self._play_btn.setToolTip(_tr("播放"))
        self._play_btn.clicked.connect(self._on_play_clicked)
        row.addWidget(self._play_btn)

        # 时间标签（替代帧号标签）
        self._time_label = QLabel("00:00 / 00:00")
        row.addWidget(self._time_label)

        # 速度选择（点击弹出菜单）
        self._speed_label = _ClickMenuButton(
            options=[
                ("0.5x",  0.5),
                ("0.75x", 0.75),
                ("1.0x",  1.0),
                ("1.25x", 1.25),
                ("1.5x",  1.5),
                ("2.0x",  2.0),
            ],
            default_index=2,
        )
        self._speed_label.option_selected.connect(self.speed_changed.emit)
        row.addWidget(self._speed_label)

        # 鼠标光标显示开关（已移至重新录制按钮右侧）

        row.addStretch()

        # 重新录制
        self._rerecord_btn = QPushButton()
        self._set_button_icon(self._rerecord_btn, "重新录制.svg")
        self._rerecord_btn.setToolTip(_tr("重新录制"))
        self._rerecord_btn.clicked.connect(self.rerecord.emit)
        row.addWidget(self._rerecord_btn)

        # 鼠标光标显示开关（重新录制右侧，默认开启并显示选中样式）
        self._cursor_btn = QPushButton()
        self._cursor_btn.setCheckable(True)
        self._cursor_btn.setChecked(True)
        self._cursor_btn.setToolTip(_tr("显示鼠标光标"))
        self._set_button_icon(self._cursor_btn, "鼠标.svg")
        self._cursor_btn.clicked.connect(self._on_cursor_toggled)
        row.addWidget(self._cursor_btn)

        # 复制到剪贴板
        self._copy_btn = QPushButton()
        self._set_button_icon(self._copy_btn, "复制.svg")
        self._copy_btn.setToolTip(_tr("复制到剪贴板"))
        self._copy_btn.clicked.connect(self._on_copy_clicked)
        row.addWidget(self._copy_btn)

        # 另存为
        self._save_btn = QPushButton()
        self._set_button_icon(self._save_btn, "保存.svg")
        self._save_btn.setToolTip(_tr("另存为"))
        self._save_btn.clicked.connect(self._on_save_clicked)
        row.addWidget(self._save_btn)

        # 关闭
        self._close_btn = QPushButton()
        self._set_button_icon(self._close_btn, "关闭.svg")
        self._close_btn.setToolTip(_tr("关闭"))
        self._close_btn.clicked.connect(self.close_requested.emit)
        row.addWidget(self._close_btn)

        vbox.addLayout(row)

        self.apply_scale()

    # ── 外部 API ──

    @staticmethod
    def _fmt(seconds: float) -> str:
        m = int(seconds) // 60
        s = int(seconds) % 60
        return f"{m:02d}:{s:02d}"

    def set_total_frames(self, total: int, fps: int = 16, total_ms: int = 0):
        self._fps = max(fps, 1)
        self._total_frames = max(total, 1)
        self._total_ms = total_ms  # 真实总时长（ms），0 则回退到帧索引推算
        self._slider.set_range(total)
        if total_ms > 0:
            total_s = total_ms / 1000.0
        else:
            total_s = (total - 1) / self._fps if total > 1 else 0
        self._time_label.setText(f"00:00 / {self._fmt(total_s)}")

    def set_playhead(self, index: int, total: int | None = None, fps: int | None = None,
                     current_ms: int = -1):
        """
        更新进度条和时间标签。
        current_ms >= 0 时直接用真实时间戳显示，否则回退到 index/fps 推算。
        """
        _fps = fps if fps is not None else getattr(self, "_fps", 16)
        _total = total if total is not None else getattr(self, "_total_frames", 1)
        _total_ms = getattr(self, "_total_ms", 0)
        self._slider.set_playhead(index)
        if current_ms >= 0:
            cur_s = current_ms / 1000.0
        else:
            cur_s = index / _fps if _fps > 0 else 0
        if _total_ms > 0:
            total_s = _total_ms / 1000.0
        else:
            total_s = (_total - 1) / _fps if _total > 1 and _fps > 0 else 0
        self._time_label.setText(f"{self._fmt(cur_s)} / {self._fmt(total_s)}")

    def set_playing(self, playing: bool):
        self._playing = playing
        if playing:
            # 播放中 → 显示暂停图标
            self._set_button_icon(self._play_btn, "暂停录制.svg")
            self._play_btn.setToolTip(_tr("暂停"))
        else:
            # 停止/暂停 → 显示播放图标
            self._set_button_icon(self._play_btn, "重开录制.svg")
            self._play_btn.setToolTip(_tr("播放"))

    # ── 回调 ──

    def _on_play_clicked(self):
        self._playing = not self._playing
        self.set_playing(self._playing)
        self.play_pause.emit()


    def _on_trim_changed(self, start: int, end: int):
        self.trim_changed.emit(start, end)

    def _on_save_clicked(self):
        self.save_requested.emit()

    def _on_copy_clicked(self):
        self.copy_requested.emit()

    def _on_cursor_toggled(self):
        checked = self._cursor_btn.isChecked()
        self.cursor_toggled.emit(checked)

    def get_trim_range(self) -> tuple:
        """获取裁剪范围（代理到内部 RangeSlider）"""
        return self._slider.get_trim_range()

 