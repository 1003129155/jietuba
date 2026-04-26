# -*- coding: utf-8 -*-
"""录制模式工具栏 — 帧率 / 录制 / 暂停 / 绘制工具 / 撤回重做 / 拖动 / 关闭

绘制工具按钮直接放在一级工具栏上，点击弹出对应的二级设置面板。
"""

from __future__ import annotations

from PySide6.QtWidgets import (
    QWidget, QPushButton, QLabel, QHBoxLayout, QVBoxLayout,
)
from PySide6.QtCore import Qt, QPoint, Signal, QSize
from PySide6.QtGui import QCursor, QColor

from ._widgets import svg_icon as _svg_icon, ClickMenuButton as _ClickMenuButton
from core.constants import DEFAULT_FONT_FAMILY
from core.i18n import make_tr
from core import safe_event
from core.logger import log_exception


_tr = make_tr("GifRecordToolbar")


# 绘制工具按钮: (tool_id, svg_file, tooltip)
_DRAW_TOOLS = [
    ("pen",     "画笔.svg",   "画笔"),
    ("rect",    "方框.svg",   "矩形"),
    ("ellipse", "圆框.svg",   "椭圆"),
    ("arrow",   "箭头.svg",   "箭头"),
    ("text",    "文字.svg",   "文字"),
    ("eraser",  "橡皮.svg",   "橡皮擦"),
]


# ── 主工具栏 ────────────────────────────────────────────────
class RecordToolbar(QWidget):
    """录制阶段悬浮工具栏 — 录制控制 + 绘制工具一体化"""

    fps_changed     = Signal(int)
    record_start    = Signal()
    record_stop     = Signal()
    pause_toggled   = Signal(bool)
    move_requested  = Signal(QPoint)
    drag_ended      = Signal()
    close_requested = Signal()

    # 绘制工具信号
    tool_selected   = Signal(str)            # 工具 ID
    undo_requested  = Signal()
    redo_requested  = Signal()
    color_changed   = Signal(QColor)
    width_changed   = Signal(int)
    opacity_changed = Signal(int)            # 0-255
    deactivate_requested = Signal()          # 退出绘制（回到穿透）
    # 样式信号
    arrow_style_changed = Signal(str)        # single/double/bar
    line_style_changed  = Signal(str)        # solid/dashed/dashed_dense
    # 文字工具专用信号
    font_changed    = Signal(object)         # QFont
    background_changed = Signal(bool, QColor, int)  # enabled, color, opacity

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFixedHeight(44)

        self._recording = False
        self._paused = False
        self._drag_pos: QPoint | None = None
        self._current_tool: str | None = None
        self._tool_buttons: dict[str, QPushButton] = {}
        self._record_rect = None  # 录制区域矩形，用于面板定位
        self._toolbar_below_record_rect = True

        self._build_ui()
        self._init_settings_panels()

    # ── UI 构建 ──────────────────────────────────────────────

    def _build_ui(self):
        container = QWidget(self)
        container.setStyleSheet("""
            QWidget {
                background-color: white;
                border: 2px solid #333;
                border-radius: 6px;
            }
            QPushButton {
                background: transparent;
                border: none;
                border-radius: 4px;
            }
            QPushButton:hover { background: rgba(0,0,0,0.06); }
            QPushButton:pressed { background: rgba(0,0,0,0.12); }
            QPushButton:disabled { opacity: 0.4; }
            QPushButton:checked { background: rgba(0,120,215,0.15); border: 1px solid #0078D7; }
        """)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(container)

        layout = QHBoxLayout(container)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(4)

        BS = 32   # 按钮大小
        IS = 20   # 图标大小

        # ── 左侧：录制控制 ──

        # 0. 帧率选择
        try:
            from settings.tool_settings import get_tool_settings_manager
            _cfg = get_tool_settings_manager()
            _fps_options = _cfg.get_gif_fps_options()
            _fps_default = _cfg.get_gif_fps()
        except Exception as e:
            log_exception(e, "加载 GIF 帧率设置")
            _fps_options = [5, 10, 16, 24, 48, 60]
            _fps_default = 16

        _fps_pairs = [(f"{v} fps", v) for v in _fps_options]
        _default_index = next(
            (i for i, (_, v) in enumerate(_fps_pairs) if v == _fps_default), 0
        )
        self._fps_btn = _ClickMenuButton(
            options=_fps_pairs,
            default_index=_default_index,
        )
        self._fps_btn.option_selected.connect(self.fps_changed.emit)
        self._fps_btn.option_selected.connect(self._on_fps_selected)
        layout.addWidget(self._fps_btn)

        # 1. 录制 / 停止
        self._record_btn = QPushButton()
        self._record_btn.setFixedSize(BS, BS)
        self._record_btn.setIconSize(QSize(IS, IS))
        self._record_btn.setIcon(_svg_icon("开始录制.svg"))
        self._record_btn.setToolTip(_tr("开始录制"))
        self._record_btn.clicked.connect(self._on_record_clicked)
        layout.addWidget(self._record_btn)

        # 2. 暂停 / 恢复
        self._pause_btn = QPushButton()
        self._pause_btn.setFixedSize(BS, BS)
        self._pause_btn.setIconSize(QSize(IS, IS))
        self._pause_btn.setIcon(_svg_icon("暂停不可.svg"))
        self._pause_btn.setToolTip(_tr("暂停录制"))
        self._pause_btn.setEnabled(False)
        self._pause_btn.clicked.connect(self._on_pause_clicked)
        layout.addWidget(self._pause_btn)

        # 录制时间
        self._time_label = QLabel("00:00")
        self._time_label.setStyleSheet(
            "color: #666; font-size: 12px; border: none; min-width: 38px;"
        )
        layout.addWidget(self._time_label)

        # ── 分隔线 ──
        sep = QWidget()
        sep.setFixedSize(1, 24)
        sep.setStyleSheet("background: #ccc; border: none;")
        layout.addWidget(sep)

        # ── 中间：绘制工具 ──
        for tool_id, svg_file, tip in _DRAW_TOOLS:
            btn = QPushButton()
            btn.setFixedSize(BS, BS)
            btn.setIconSize(QSize(IS, IS))
            btn.setIcon(_svg_icon(svg_file))
            btn.setToolTip(_tr(tip))
            btn.setCheckable(True)
            btn.clicked.connect(lambda checked, tid=tool_id: self._on_tool_clicked(tid))
            layout.addWidget(btn)
            self._tool_buttons[tool_id] = btn

        # ── 分隔线 ──
        sep2 = QWidget()
        sep2.setFixedSize(1, 24)
        sep2.setStyleSheet("background: #ccc; border: none;")
        layout.addWidget(sep2)

        # ── 撤销 / 重做 ──
        self._undo_btn = QPushButton()
        self._undo_btn.setFixedSize(BS, BS)
        self._undo_btn.setIconSize(QSize(IS, IS))
        self._undo_btn.setIcon(_svg_icon("撤回.svg"))
        self._undo_btn.setToolTip(_tr("撤销"))
        self._undo_btn.clicked.connect(self.undo_requested.emit)
        layout.addWidget(self._undo_btn)

        self._redo_btn = QPushButton()
        self._redo_btn.setFixedSize(BS, BS)
        self._redo_btn.setIconSize(QSize(IS, IS))
        self._redo_btn.setIcon(_svg_icon("复原.svg"))
        self._redo_btn.setToolTip(_tr("重做"))
        self._redo_btn.clicked.connect(self.redo_requested.emit)
        layout.addWidget(self._redo_btn)

        # ── 右侧：拖动 + 关闭 ──
        layout.addStretch()

        self._move_handle = QLabel()
        self._move_handle.setFixedSize(28, BS)
        pm = _svg_icon("移动窗口.svg", 18).pixmap(QSize(18, 18))
        self._move_handle.setPixmap(pm)
        self._move_handle.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._move_handle.setStyleSheet("QLabel { background: transparent; border: none; }")
        self._move_handle.setCursor(QCursor(Qt.CursorShape.SizeAllCursor))
        self._move_handle.mousePressEvent   = self._handle_press
        self._move_handle.mouseMoveEvent    = self._handle_move
        self._move_handle.mouseReleaseEvent = self._handle_release
        layout.addWidget(self._move_handle)

        self._close_btn = QPushButton()
        self._close_btn.setFixedSize(BS, BS)
        self._close_btn.setIconSize(QSize(IS, IS))
        self._close_btn.setIcon(_svg_icon("关闭.svg"))
        self._close_btn.setToolTip(_tr("关闭"))
        self._close_btn.clicked.connect(self.close_requested.emit)
        layout.addWidget(self._close_btn)

    # ── 二级设置面板 ──────────────────────────────────────────

    def _init_settings_panels(self):
        """初始化绘制工具的二级设置面板（复用截图 ui/ 面板）"""
        from ui.paint_settings_panel import PaintSettingsPanel
        from ui.shape_settings_panel import ShapeSettingsPanel
        from ui.arrow_settings_panel import ArrowSettingsPanel
        from ui.text_settings_panel import TextSettingsPanel

        flags = (
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )

        # 画笔面板
        self.paint_panel = PaintSettingsPanel()
        self.paint_panel.setWindowFlags(flags)
        self.paint_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.paint_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.paint_panel.color_changed.connect(self.color_changed.emit)
        self.paint_panel.size_changed.connect(self.width_changed.emit)
        self.paint_panel.opacity_changed.connect(self.opacity_changed.emit)
        self.paint_panel.line_style_changed.connect(self._on_line_style_changed)
        self.paint_panel.hide()

        # 形状面板（矩形 / 圆形）
        self.shape_panel = ShapeSettingsPanel()
        self.shape_panel.setWindowFlags(flags)
        self.shape_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.shape_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.shape_panel.color_changed.connect(self.color_changed.emit)
        self.shape_panel.size_changed.connect(self.width_changed.emit)
        self.shape_panel.opacity_changed.connect(self.opacity_changed.emit)
        self.shape_panel.line_style_changed.connect(self._on_line_style_changed)
        self.shape_panel.hide()

        # 箭头面板
        self.arrow_panel = ArrowSettingsPanel()
        self.arrow_panel.setWindowFlags(flags)
        self.arrow_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.arrow_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.arrow_panel.color_changed.connect(self.color_changed.emit)
        self.arrow_panel.size_changed.connect(self.width_changed.emit)
        self.arrow_panel.opacity_changed.connect(self.opacity_changed.emit)
        self.arrow_panel.arrow_style_changed.connect(self._on_arrow_style_changed)
        self.arrow_panel.hide()

        # 文字面板
        self.text_panel = TextSettingsPanel()
        self.text_panel.setWindowFlags(flags)
        self.text_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.text_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.text_panel.color_changed.connect(self.color_changed.emit)
        self.text_panel.font_changed.connect(self._on_text_font_changed)
        if hasattr(self.text_panel, 'background_changed'):
            self.text_panel.background_changed.connect(self._on_text_background_changed)
        self.text_panel.hide()

        # 面板映射
        self._panel_map: dict[str, QWidget] = {
            "pen":     self.paint_panel,
            "rect":    self.shape_panel,
            "ellipse": self.shape_panel,
            "arrow":   self.arrow_panel,
            "text":    self.text_panel,
        }

        # 兼容 CanvasView.wheelEvent 对 toolbar.text_menu 的访问
        self.text_menu = self.text_panel

        # 从设置加载初始值
        self._load_saved_settings()

    def _load_saved_settings(self):
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()

            # ── 画笔 ──
            pen_settings = manager.get_tool_settings("pen")
            if pen_settings:
                self.paint_panel.line_style = pen_settings.get("line_style", "solid")

            # ── 箭头 ──
            arrow_settings = manager.get_tool_settings("arrow")
            if arrow_settings and hasattr(self.arrow_panel, "arrow_style"):
                self.arrow_panel.arrow_style = arrow_settings.get("arrow_style", "single")

            # ── 文字（委托给面板）──
            self.text_panel.load_from_config()
        except Exception as e:
            log_exception(e, "加载绘制工具设置")

    def _on_arrow_style_changed(self, style: str):
        """箭头样式变化 — 保存设置并转发信号"""
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            manager.update_settings("arrow", arrow_style=style)
        except Exception as e:
            log_exception(e, "保存箭头样式")
        self.arrow_style_changed.emit(style)

    def _on_line_style_changed(self, style: str):
        """线条样式变化 — 保存设置并转发信号"""
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            tool_id = self._current_tool or "pen"
            if tool_id in ("rect", "ellipse"):
                manager.update_settings(tool_id, line_style=style)
            elif tool_id == "pen":
                manager.update_settings("pen", line_style=style)
        except Exception as e:
            log_exception(e, "保存线条样式")
        self.line_style_changed.emit(style)

    def _on_text_font_changed(self, font):
        """文字字体变化 — 保存设置并转发信号"""
        from ui.text_settings_panel import TextSettingsPanel
        TextSettingsPanel.save_font_to_config(font)
        self.font_changed.emit(font)

    def _on_text_background_changed(self, enabled: bool, color, opacity: int):
        """文字背景变化 — 保存设置并转发信号"""
        from ui.text_settings_panel import TextSettingsPanel
        TextSettingsPanel.save_background_to_config(enabled, color, opacity)
        self.background_changed.emit(enabled, color, opacity)

    # ── 面板定位 ──────────────────────────────────────────────

    def set_record_rect(self, rect):
        """记录当前录制区域矩形，用于面板定位判断"""
        self._record_rect = rect

    def set_toolbar_side(self, below: bool):
        """记录工具栏位于录制区域上方/下方，供二级面板复用同一判断逻辑"""
        self._toolbar_below_record_rect = below

    def _position_panel(self, panel: QWidget):
        """将面板定位到不遮挡绘制区域的位置。

        策略：
        - 工具栏在录制区域下方 → 面板优先在工具栏**下方**（远离绘制区）
        - 工具栏在录制区域上方 → 面板优先在工具栏**上方**（远离绘制区）
        - 如果优先方向超出屏幕边界，则翻转到另一侧
        """
        from PySide6.QtWidgets import QApplication

        # 使用 mapToGlobal 获取工具栏在全局坐标系中的准确位置
        tb_pos = self.mapToGlobal(QPoint(0, 0))
        tb_w = self.width()
        tb_h = self.height()
        panel_w = panel.sizeHint().width()
        panel_h = panel.sizeHint().height()
        gap = 4

        # 根据工具栏所在位置获取对应屏幕，而非固定主屏
        screen = QApplication.screenAt(tb_pos)
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_rect = screen.geometry() if screen else None
        if screen_rect is None:
            screen_rect = QApplication.primaryScreen().geometry()

        # 与截图工具栏保持一致：二级面板复用一级工具栏已决策好的上下方向
        toolbar_below_rect = self._toolbar_below_record_rect

        if toolbar_below_rect:
            # 工具栏在录制区下方 → 面板优先在工具栏下方（远离录制区）
            y_below = tb_pos.y() + tb_h + gap
            y_above = tb_pos.y() - panel_h - gap
            if y_below + panel_h <= screen_rect.bottom():
                y = y_below
            elif y_above >= screen_rect.top():
                y = y_above
            else:
                y = screen_rect.bottom() - panel_h  # 贴底
        else:
            # 工具栏在录制区上方 → 面板优先在工具栏上方（远离录制区）
            y_above = tb_pos.y() - panel_h - gap
            y_below = tb_pos.y() + tb_h + gap
            if y_above >= screen_rect.top():
                y = y_above
            elif y_below + panel_h <= screen_rect.bottom():
                y = y_below
            else:
                y = screen_rect.top()  # 贴顶

        # 纵轴夹紧
        y = max(screen_rect.top(), min(y, screen_rect.bottom() - panel_h))

        # 水平居中对齐到工具栏，并确保不超出当前屏幕边界
        x = tb_pos.x() + (tb_w - panel_w) // 2
        x = max(screen_rect.left(), min(x, screen_rect.right() - panel_w))
        panel.move(x, y)

    def _show_panel_for_tool(self, tool_id: str):
        self._hide_all_panels()
        panel = self._panel_map.get(tool_id)
        if not panel:
            return
        panel.show()
        panel.raise_()
        self._position_panel(panel)

    def _hide_all_panels(self):
        seen = set()
        for p in self._panel_map.values():
            pid = id(p)
            if pid not in seen:
                p.hide()
                seen.add(pid)

    def reposition_panels(self):
        """重新定位所有可见面板（窗口移动后调用）"""
        for panel in {v for v in self._panel_map.values()}:
            if panel.isVisible():
                self._position_panel(panel)

    def get_max_panel_height(self) -> int:
        """获取所有二级设置面板的最大高度（含间距），供工具栏定位时预留空间"""
        gap = 4
        max_h = 0
        for attr in ('paint_panel', 'shape_panel', 'arrow_panel', 'text_panel'):
            panel = getattr(self, attr, None)
            if panel:
                max_h = max(max_h, panel.sizeHint().height())
        return (max_h + gap) if max_h else 0

    # ── 绘制工具回调 ─────────────────────────────────────────

    def _on_tool_clicked(self, tool_id: str):
        if tool_id == self._current_tool:
            # 再次点击同一工具 → 取消选中，回到穿透
            self._current_tool = None
            self._update_tool_checked()
            self._hide_all_panels()
            self.deactivate_requested.emit()
            return
        self._current_tool = tool_id
        self._update_tool_checked()
        self._show_panel_for_tool(tool_id)
        self.tool_selected.emit(tool_id)

    def _update_tool_checked(self):
        for tid, btn in self._tool_buttons.items():
            btn.setChecked(tid == self._current_tool)

    def highlight_tool(self, tool_id: str | None):
        """外部设置高亮（如 Esc 退出绘制时调用）"""
        self._current_tool = tool_id
        self._update_tool_checked()
        if not tool_id:
            self._hide_all_panels()

    def sync_from_controller(self, tool_controller):
        """从 ToolController 同步当前颜色/线宽到面板 UI"""
        ctx = tool_controller.ctx
        color = ctx.color
        width = int(ctx.stroke_width)
        opacity = int(ctx.opacity * 255) if ctx.opacity is not None else 255
        for panel in {v for v in self._panel_map.values()}:
            if hasattr(panel, 'set_color'):
                panel.set_color(color)
            if hasattr(panel, 'set_size'):
                panel.set_size(width)
            if hasattr(panel, 'set_opacity'):
                panel.set_opacity(opacity)

    # ── 外部 API ─────────────────────────────────────────────

    def set_stroke_width(self, width: int):
        """设置笔触宽度（更新所有面板的大小显示，不触发信号）"""
        width = int(width)
        for panel in {v for v in self._panel_map.values()}:
            if hasattr(panel, 'set_size'):
                panel.set_size(width)

    def set_opacity(self, opacity_255: int):
        """设置透明度（更新所有面板的透明度显示，不触发信号）"""
        for panel in {v for v in self._panel_map.values()}:
            if hasattr(panel, 'set_opacity'):
                panel.set_opacity(opacity_255)

    def set_elapsed(self, seconds: float):
        m = int(seconds) // 60
        s = int(seconds) % 60
        self._time_label.setText(f"{m:02d}:{s:02d}")

    def reset_state(self):
        """重新录制时将工具栏恢复到初始状态"""
        self._recording = False
        self._paused = False
        self._current_tool = None
        self._update_tool_checked()
        self._hide_all_panels()
        self._record_btn.setIcon(_svg_icon("开始录制.svg"))
        self._record_btn.setToolTip(_tr("开始录制"))
        self._pause_btn.setEnabled(False)
        self._pause_btn.setIcon(_svg_icon("暂停不可.svg"))
        self._pause_btn.setToolTip(_tr("暂停录制"))
        self._fps_btn.set_enabled(True)
        self._move_handle.setEnabled(True)
        self._move_handle.setCursor(QCursor(Qt.CursorShape.SizeAllCursor))
        self._move_handle.setToolTip("")
        self._time_label.setText("00:00")
        self._time_label.setStyleSheet(
            "color: #666; font-size: 12px; border: none; min-width: 38px;"
        )

    def get_current_fps(self) -> int:
        return self._fps_btn.current_value()

    # ── 录制回调 ─────────────────────────────────────────────

    def _on_record_clicked(self):
        if not self._recording:
            self._recording = True
            self._record_btn.setIcon(_svg_icon("结束录制.svg"))
            self._record_btn.setToolTip(_tr("停止录制"))
            self._pause_btn.setEnabled(True)
            self._pause_btn.setIcon(_svg_icon("暂停录制.svg"))
            self._pause_btn.setToolTip(_tr("暂停录制"))
            self._fps_btn.set_enabled(False)
            self._move_handle.setEnabled(False)
            self._move_handle.setCursor(QCursor(Qt.CursorShape.ArrowCursor))
            self._move_handle.setToolTip(_tr("录制中无法移动"))
            self.record_start.emit()
        else:
            self._recording = False
            self._paused = False
            self._record_btn.setIcon(_svg_icon("开始录制.svg"))
            self._record_btn.setToolTip(_tr("开始录制"))
            self._pause_btn.setEnabled(False)
            self._pause_btn.setIcon(_svg_icon("暂停不可.svg"))
            self._pause_btn.setToolTip(_tr("暂停录制"))
            self._fps_btn.set_enabled(True)
            self._move_handle.setEnabled(True)
            self._move_handle.setCursor(QCursor(Qt.CursorShape.SizeAllCursor))
            self._move_handle.setToolTip("")
            self.record_stop.emit()

    # 暂停功能
    def _on_pause_clicked(self):
        self._paused = not self._paused
        if self._paused:
            self._pause_btn.setIcon(_svg_icon("重开录制.svg"))
            self._pause_btn.setToolTip(_tr("恢复录制"))
        else:
            self._pause_btn.setIcon(_svg_icon("暂停录制.svg"))
            self._pause_btn.setToolTip(_tr("暂停录制"))
        self.pause_toggled.emit(self._paused)

    # ── 拖动 ─────────────────────────────────────────────────

    def _handle_press(self, ev):
        if ev.button() == Qt.MouseButton.LeftButton:
            self._drag_pos = ev.globalPosition().toPoint()

    def _handle_move(self, ev):
        if self._drag_pos is not None:
            new_pos = ev.globalPosition().toPoint()
            delta = new_pos - self._drag_pos
            self._drag_pos = new_pos
            self.move_requested.emit(delta)

    def _handle_release(self, ev):
        self._drag_pos = None
        self.drag_ended.emit()

    def _on_fps_selected(self, fps: int):
        try:
            from settings.tool_settings import get_tool_settings_manager
            get_tool_settings_manager().set_gif_fps(fps)
        except Exception as e:
            log_exception(e, "保存 GIF 帧率")

    def hide(self):
        """隐藏工具栏 + 所有面板"""
        self._hide_all_panels()
        super().hide()

    @safe_event
    def mousePressEvent(self, ev):
        super().mousePressEvent(ev)

    @safe_event
    def mouseMoveEvent(self, ev):
        super().mouseMoveEvent(ev)

    @safe_event
    def mouseReleaseEvent(self, ev):
        super().mouseReleaseEvent(ev) 