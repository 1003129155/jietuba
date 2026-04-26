# -*- coding: utf-8 -*-
"""GIF 录制绘制工具栏 — 一级直接放工具按钮，点击弹出二级设置面板

设计:
- 工具按钮直接在一级工具栏上，无需二级入口
- 点击工具按钮后弹出对应的设置面板（复用截图 ui/ 面板）
- 工具 ID 与截图完全一致，确保设置互通
- 面板: paint→画笔/荧光笔, shape→矩形/圆形, arrow→箭头,
        number→序号, text→文字, eraser→无面板
"""

from __future__ import annotations

from PySide6.QtWidgets import QWidget, QPushButton, QHBoxLayout, QVBoxLayout
from PySide6.QtCore import Qt, QPoint, Signal, QSize
from PySide6.QtGui import QCursor, QColor, QIcon

from ._widgets import svg_icon as _svg_icon
from core.i18n import make_tr
from core.logger import log_exception


_tr = make_tr("GifDrawingToolbar")


# 工具按钮定义: (tool_id, svg_filename, tooltip_key)
_TOOL_BUTTONS = [
    ("pen",         "画笔.svg",     "画笔"),
    ("rect",        "方框.svg",     "矩形"),
    ("ellipse",     "圆框.svg",     "椭圆"),
    ("arrow",       "箭头.svg",     "箭头"),
    ("text",        "文字.svg",     "文字"),
    ("eraser",      "橡皮.svg",     "橡皮擦"),
]


class GifDrawingToolbar(QWidget):
    """GIF 录制绘制工具栏 — 一级按钮 + 二级设置面板"""

    tool_selected   = Signal(str)            # 工具 ID
    undo_requested  = Signal()
    redo_requested  = Signal()
    color_changed   = Signal(QColor)
    width_changed   = Signal(int)
    opacity_changed = Signal(int)            # 0-255
    deactivate_requested = Signal()          # 取消绘制（回到穿透）

    _BTN_SIZE = 30
    _ICON_SIZE = 20

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFixedHeight(40)

        self._current_tool: str | None = None
        self._tool_buttons: dict[str, QPushButton] = {}

        self._build_ui()
        self._init_settings_panels()

    # ── 一级工具栏 UI ──

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
            QPushButton:checked { background: rgba(0,120,215,0.15); border: 1px solid #0078D7; }
        """)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(container)

        layout = QHBoxLayout(container)
        layout.setContentsMargins(6, 3, 6, 3)
        layout.setSpacing(2)

        # 工具按钮
        for tool_id, svg_file, tip_key in _TOOL_BUTTONS:
            btn = QPushButton()
            btn.setFixedSize(self._BTN_SIZE, self._BTN_SIZE)
            btn.setIconSize(QSize(self._ICON_SIZE, self._ICON_SIZE))
            btn.setIcon(_svg_icon(svg_file))
            btn.setToolTip(_tr(tip_key))
            btn.setCheckable(True)
            btn.clicked.connect(lambda checked, tid=tool_id: self._on_tool_clicked(tid))
            layout.addWidget(btn)
            self._tool_buttons[tool_id] = btn

        # 分隔
        layout.addSpacing(6)

        # 撤销
        self._undo_btn = QPushButton()
        self._undo_btn.setFixedSize(self._BTN_SIZE, self._BTN_SIZE)
        self._undo_btn.setIconSize(QSize(self._ICON_SIZE, self._ICON_SIZE))
        self._undo_btn.setIcon(_svg_icon("撤回.svg"))
        self._undo_btn.setToolTip(_tr("撤销"))
        self._undo_btn.clicked.connect(self.undo_requested.emit)
        layout.addWidget(self._undo_btn)

        # 重做
        self._redo_btn = QPushButton()
        self._redo_btn.setFixedSize(self._BTN_SIZE, self._BTN_SIZE)
        self._redo_btn.setIconSize(QSize(self._ICON_SIZE, self._ICON_SIZE))
        self._redo_btn.setIcon(_svg_icon("复原.svg"))
        self._redo_btn.setToolTip(_tr("重做"))
        self._redo_btn.clicked.connect(self.redo_requested.emit)
        layout.addWidget(self._redo_btn)

        layout.addSpacing(4)

        # 退出绘制
        self._exit_btn = QPushButton()
        self._exit_btn.setFixedSize(self._BTN_SIZE, self._BTN_SIZE)
        self._exit_btn.setIconSize(QSize(self._ICON_SIZE, self._ICON_SIZE))
        self._exit_btn.setIcon(_svg_icon("关闭.svg"))
        self._exit_btn.setToolTip(_tr("退出绘制"))
        self._exit_btn.clicked.connect(self._on_exit)
        layout.addWidget(self._exit_btn)

    # ── 二级设置面板 ──

    def _init_settings_panels(self):
        """初始化所有工具的二级设置面板（复用截图 ui/ 面板）"""
        from ui.paint_settings_panel import PaintSettingsPanel
        from ui.shape_settings_panel import ShapeSettingsPanel
        from ui.arrow_settings_panel import ArrowSettingsPanel
        from ui.number_settings_panel import NumberSettingsPanel
        from ui.text_settings_panel import TextSettingsPanel

        flags = (
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )

        # 画笔 / 荧光笔面板
        self.paint_panel = PaintSettingsPanel()
        self.paint_panel.setWindowFlags(flags)
        self.paint_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.paint_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.paint_panel.color_changed.connect(self.color_changed.emit)
        self.paint_panel.size_changed.connect(self.width_changed.emit)
        self.paint_panel.opacity_changed.connect(self.opacity_changed.emit)
        self.paint_panel.hide()

        # 形状面板（矩形 / 圆形）
        self.shape_panel = ShapeSettingsPanel()
        self.shape_panel.setWindowFlags(flags)
        self.shape_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.shape_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.shape_panel.color_changed.connect(self.color_changed.emit)
        self.shape_panel.size_changed.connect(self.width_changed.emit)
        self.shape_panel.opacity_changed.connect(self.opacity_changed.emit)
        self.shape_panel.hide()

        # 箭头面板
        self.arrow_panel = ArrowSettingsPanel()
        self.arrow_panel.setWindowFlags(flags)
        self.arrow_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.arrow_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.arrow_panel.color_changed.connect(self.color_changed.emit)
        self.arrow_panel.size_changed.connect(self.width_changed.emit)
        self.arrow_panel.opacity_changed.connect(self.opacity_changed.emit)
        self.arrow_panel.hide()

        # 文字面板
        self.text_panel = TextSettingsPanel()
        self.text_panel.setWindowFlags(flags)
        self.text_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.text_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.text_panel.color_changed.connect(self.color_changed.emit)
        self.text_panel.hide()

        # 面板映射
        self._panel_map: dict[str, QWidget] = {
            "pen":         self.paint_panel,
            "rect":        self.shape_panel,
            "ellipse":     self.shape_panel,
            "arrow":       self.arrow_panel,
            "text":        self.text_panel,
            # eraser 没有面板
        }

        # 从设置加载初始值
        self._load_saved_settings()

    def _load_saved_settings(self):
        """从配置同步面板初始状态"""
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()

            # 画笔线条样式
            pen_settings = manager.get_tool_settings("pen")
            if pen_settings:
                self.paint_panel.line_style = pen_settings.get("line_style", "solid")

            # 箭头样式
            arrow_settings = manager.get_tool_settings("arrow")
            if arrow_settings and hasattr(self.arrow_panel, "arrow_style"):
                self.arrow_panel.arrow_style = arrow_settings.get("arrow_style", "single")
        except Exception as e:
            log_exception(e, "加载绘制工具样式")

    # ── 面板定位 ──

    def _position_panel(self, panel: QWidget):
        """将面板定位到工具栏正上方"""
        tb_pos = self.pos()
        tb_w = self.width()
        panel_w = panel.sizeHint().width()
        gap = 4

        x = tb_pos.x() + (tb_w - panel_w) // 2
        y = tb_pos.y() - panel.sizeHint().height() - gap
        if y < 0:
            # 上方放不下，放工具栏下方
            y = tb_pos.y() + self.height() + gap

        panel.move(x, y)

    def _show_panel_for_tool(self, tool_id: str):
        """显示指定工具的面板，隐藏其他"""
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
        if hasattr(self, 'text_panel'):
            self.text_panel.hide()

    # ── 外部 API ──

    def highlight_tool(self, tool_id: str | None):
        """高亮指定工具按钮，取消其他"""
        self._current_tool = tool_id
        for tid, btn in self._tool_buttons.items():
            btn.setChecked(tid == tool_id)

    def sync_from_controller(self, tool_controller):
        """从 ToolController 同步当前颜色/线宽到面板 UI"""
        ctx = tool_controller.ctx
        color = ctx.color
        width = int(ctx.stroke_width)
        opacity = int(ctx.opacity * 255) if ctx.opacity is not None else 255

        # 同步到当前可见面板
        for panel in {v for v in self._panel_map.values()}:
            if hasattr(panel, 'set_color'):
                panel.set_color(color)
            if hasattr(panel, 'set_size'):
                panel.set_size(width)
            if hasattr(panel, 'set_opacity'):
                panel.set_opacity(opacity)

    def reposition_panels(self):
        """重新定位所有可见面板（窗口移动后调用）"""
        for panel in {v for v in self._panel_map.values()}:
            if panel.isVisible():
                self._position_panel(panel)

    # ── 内部回调 ──

    def _on_tool_clicked(self, tool_id: str):
        if tool_id == self._current_tool:
            # 再次点击同一工具 → 取消
            self.deactivate_requested.emit()
            self.highlight_tool(None)
            self._hide_all_panels()
            return
        self.highlight_tool(tool_id)
        self._show_panel_for_tool(tool_id)
        self.tool_selected.emit(tool_id)

    def _on_exit(self):
        self.highlight_tool(None)
        self._hide_all_panels()
        self.deactivate_requested.emit()

    def hide(self):
        """隐藏工具栏 + 所有面板"""
        self._hide_all_panels()
        super().hide()

 