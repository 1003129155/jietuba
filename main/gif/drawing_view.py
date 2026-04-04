# -*- coding: utf-8 -*-
"""GIF 录制绘制层 — 完整复用截图 CanvasView + SmartEditController

gdigrab 直接抓屏幕像素，因此本窗口画出的内容会被自动录进 GIF。
窗口是无边框 + 透明背景的顶层窗口，覆盖在录制区域上方。

架构设计（对齐钉图 PinCanvasView 方案）：
- GifDrawingScene 继承 QGraphicsScene，提供工具系统 + 撤销栈
  - 添加假 selection_model（永远 is_confirmed=True），让 CanvasView 直接进入编辑模式
- GifDrawingView 继承 CanvasView（非 QGraphicsView）
  - 复用 CanvasView 完整交互：SmartEditController、控制点拖拽、
    悬停高亮、文字编辑、二次编辑等全部功能
  - 只额外处理穿透 / 取消穿透（WS_EX_TRANSPARENT）
"""

from __future__ import annotations

import ctypes
from typing import Optional

from PySide6.QtWidgets import QGraphicsView, QGraphicsScene, QGraphicsRectItem
from PySide6.QtCore import Qt, QRectF, QPointF, QRect, QObject, Signal
from PySide6.QtGui import QPainter, QColor, QBrush, QPen

from canvas import CanvasView
from canvas.undo import CommandUndoStack
from tools import (
    ToolController, ToolContext,
    CursorTool, PenTool, RectTool, EllipseTool, ArrowTool,
    TextTool, NumberTool, EraserTool,
)
from tools.cursor_manager import CursorManager
from settings import get_tool_settings_manager

try:
    from core.logger import log_debug, log_info, log_exception
except ImportError:
    import logging
    _l = logging.getLogger("GIF")
    log_debug = log_info = _l.info

from core.shortcut_manager import ShortcutManager, ShortcutHandler
from core import safe_event


# ── Win32 穿透常量 ──
GWL_EXSTYLE       = -20
WS_EX_TRANSPARENT = 0x00000020
SWP_NOMOVE        = 0x0002
SWP_NOSIZE        = 0x0001
SWP_NOZORDER      = 0x0004
SWP_FRAMECHANGED  = 0x0020


def _set_click_through(hwnd: int, enable: bool):
    """设置/取消 WS_EX_TRANSPARENT"""
    try:
        user32 = ctypes.windll.user32
        style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if enable:
            style |= WS_EX_TRANSPARENT
        else:
            style &= ~WS_EX_TRANSPARENT
        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style)
        user32.SetWindowPos(
            hwnd, 0, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED,
        )
    except Exception as e:
        log_exception(e, "GIF 绘制层设置透明")


# ── 假 SelectionModel：永远 is_confirmed=True ──
class _AlwaysConfirmedSelection(QObject):
    """让 CanvasView 跳过「选区创建」阶段，直接进入编辑/绘图模式。"""
    rect_changed = Signal(QRectF)
    confirmed = Signal()

    def __init__(self):
        super().__init__()
        self._rect = QRectF()

    @property
    def is_confirmed(self) -> bool:
        return True

    @property
    def is_dragging(self) -> bool:
        return False

    def rect(self) -> QRectF:
        return self._rect

    def set_rect(self, r: QRectF):
        self._rect = r

    def activate(self):
        pass

    def start_dragging(self):
        pass

    def stop_dragging(self):
        pass

    def confirm(self):
        pass


class GifDrawingScene(QGraphicsScene):
    """
    GIF 绘制场景 — CanvasScene 的精简版。
    不需要 BackgroundItem / SelectionItem，但提供 CanvasView 需要的接口。
    """

    # CanvasView 需要的解耦信号（与 CanvasScene 保持一致）
    cursor_color_update_requested = Signal(object)    # 参数：QColor
    cursor_tool_update_requested = Signal(str, bool)  # 参数：tool_id, force
    item_auto_select_requested = Signal(object)       # 参数：绘制完成的 QGraphicsItem
    editing_cleanup_requested = Signal()              # 请求清除编辑状态

    def __init__(self, width: int, height: int):
        super().__init__()
        self.setSceneRect(QRectF(0, 0, width, height))

        # 假 selection_model — 让 CanvasView 直接进入编辑分支
        self.selection_model = _AlwaysConfirmedSelection()

        # 鼠标捕获背景 — alpha=1 肉眼不可见，让 Windows 探测到像素
        self._hit_rect = QGraphicsRectItem(QRectF(0, 0, width, height))
        self._hit_rect.setBrush(QBrush(QColor(0, 0, 0, 1)))
        self._hit_rect.setPen(QPen(Qt.PenStyle.NoPen))
        self._hit_rect.setZValue(-1000)
        self._hit_rect.setVisible(False)
        self.addItem(self._hit_rect)

        # 撤销栈
        self.undo_stack = CommandUndoStack(self)

        # 工具设置管理器（与截图共享同一份配置）
        self.tool_settings_manager = get_tool_settings_manager()

        # 初始颜色/线宽/透明度
        initial_color = self.tool_settings_manager.get_color("pen")
        initial_stroke_width = self.tool_settings_manager.get_stroke_width("pen")
        initial_opacity = self.tool_settings_manager.get_opacity("pen")

        # 工具上下文
        ctx = ToolContext(
            scene=self,
            selection=None,
            undo_stack=self.undo_stack,
            color=initial_color,
            stroke_width=initial_stroke_width,
            opacity=initial_opacity,
            settings_manager=self.tool_settings_manager,
        )
        self.tool_controller = ToolController(ctx)

        # 光标管理器（由 GifDrawingView 初始化）
        self.cursor_manager: Optional[CursorManager] = None
        self._layer_editor = None

        # 注册所有工具
        self.tool_controller.register(CursorTool())
        self.tool_controller.register(PenTool())
        self.tool_controller.register(RectTool())
        self.tool_controller.register(EllipseTool())
        self.tool_controller.register(ArrowTool())
        self.tool_controller.register(TextTool())
        self.tool_controller.register(NumberTool())
        self.tool_controller.register(EraserTool())

        # 默认 cursor 工具（穿透）
        self.tool_controller.activate("cursor")

        # 撤销栈变化 → 更新序号光标
        self.undo_stack.indexChanged.connect(self._on_undo_stack_changed)
        self.tool_controller.add_tool_changed_callback(self._on_tool_changed)

    # ── CanvasView 需要的属性 ──

    @property
    def scene_rect(self) -> QRectF:
        """CanvasView._get_smart_selection_rect 等访问的属性"""
        return self.sceneRect()

    def resize_scene(self, width: int, height: int):
        self.setSceneRect(QRectF(0, 0, width, height))
        self._hit_rect.setRect(QRectF(0, 0, width, height))

    def set_hit_test_visible(self, visible: bool):
        self._hit_rect.setVisible(visible)

    def _on_tool_changed(self, tool_id: str):
        if self.cursor_manager:
            self.cursor_manager.set_tool_cursor(tool_id)

    def _on_undo_stack_changed(self):
        if (self.tool_controller.current_tool
                and self.tool_controller.current_tool.id == "number"
                and self.cursor_manager):
            self.cursor_manager.set_tool_cursor("number", force=True)

    def drawForeground(self, painter, rect):
        """绘制前景层 — 渲染 LayerEditor 控制点（二次编辑手柄）"""
        super().drawForeground(painter, rect)
        if self._layer_editor and self._layer_editor.is_editing():
            self._layer_editor.render(painter)


class GifDrawingShortcutHandler(ShortcutHandler):
    """GIF 绘制层快捷键处理器 (priority=80)"""

    def __init__(self, view: 'GifDrawingView'):
        self._view = view

    @property
    def priority(self) -> int:
        return 80

    @property
    def handler_name(self) -> str:
        return "GifDrawing"

    def is_active(self) -> bool:
        v = self._view
        try:
            return v is not None and v.isVisible() and not v._passthrough
        except RuntimeError:
            return False

    def handle_key(self, event) -> bool:
        v = self._view
        if event.key() == Qt.Key.Key_Escape:
            v.activate_tool("cursor")
            return True
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            if event.key() == Qt.Key.Key_Z:
                v._gif_scene.undo_stack.undo()
                return True
            if event.key() == Qt.Key.Key_Y:
                v._gif_scene.undo_stack.redo()
                return True
        return False


class GifDrawingView(CanvasView):
    """
    GIF 录制绘制视图 — 继承 CanvasView 获得完整交互能力。

    额外功能：
    - 穿透模式（cursor 工具）: WS_EX_TRANSPARENT，鼠标完全穿透
    - 绘制模式（其他工具）: 取消穿透，由 CanvasView 接管鼠标
    - 独立顶层透明窗口
    """

    tool_changed = Signal(str)
    passthrough_changed = Signal(bool)

    def __init__(self, rect: QRect):
        self._gif_scene = GifDrawingScene(rect.width(), rect.height())
        super().__init__(self._gif_scene)
        # 注入 layer_editor 供 drawForeground 使用（单向引用，无循环）
        self._gif_scene._layer_editor = self.smart_edit_controller.layer_editor

        # ── 覆盖 CanvasView 的窗口属性 → 独立顶层透明窗口 ──
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setStyleSheet("background: transparent;")
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        # 渲染
        self.setRenderHint(QPainter.RenderHint.Antialiasing)

        # 重置视图变换 — CanvasView.__init__ 中会用 scene_rect 做偏移，
        # 但 GIF 场景坐标从 (0,0) 开始，所以重置为单位变换即可。
        self.resetTransform()

        # 工具切换回调 → 穿透管理
        self._gif_scene.tool_controller.add_tool_changed_callback(self._on_gif_tool_changed)

        # 穿透状态
        self._passthrough = True

        # 设置初始位置
        self.setGeometry(rect)

        # 默认穿透
        self._apply_passthrough(True)

        # 注册快捷键处理器
        self._shortcut_handler = GifDrawingShortcutHandler(self)
        ShortcutManager.instance().register(self._shortcut_handler)

    # ── 属性访问 ──

    @property
    def scene_obj(self) -> GifDrawingScene:
        return self._gif_scene

    @property
    def tool_controller(self) -> ToolController:
        return self._gif_scene.tool_controller

    @property
    def undo_stack(self) -> CommandUndoStack:
        return self._gif_scene.undo_stack

    # ── 外部 API ──

    def update_rect(self, rect: QRect):
        """同步位置和大小到录制区域"""
        self.setGeometry(rect)
        self._gif_scene.resize_scene(rect.width(), rect.height())

    def activate_tool(self, tool_id: str):
        self._gif_scene.tool_controller.activate(tool_id)

    def set_passthrough(self, enable: bool):
        if enable == self._passthrough:
            return
        self._apply_passthrough(enable)

    def clear_all(self):
        """清空所有绘制内容（保留 _hit_rect）"""
        hit = self._gif_scene._hit_rect
        for item in list(self._gif_scene.items()):
            if item is not hit:
                self._gif_scene.removeItem(item)
        self._gif_scene.undo_stack.clear()
        # 清除 SmartEditController 的选中状态
        if hasattr(self, 'smart_edit_controller'):
            self.smart_edit_controller.clear_selection()

    def has_drawings(self) -> bool:
        return len(self._gif_scene.items()) > 1

    # ── 穿透控制 ──

    def _apply_passthrough(self, enable: bool):
        self._passthrough = enable
        self._gif_scene.set_hit_test_visible(not enable)
        hwnd = int(self.winId())
        _set_click_through(hwnd, enable)
        if not enable:
            self.activateWindow()
            self.raise_()
        self.passthrough_changed.emit(enable)
        log_debug(f"绘制层穿透={'开' if enable else '关'}", "GIF")

    def _on_gif_tool_changed(self, tool_id: str):
        """工具切换 → 穿透管理 + 信号"""
        is_cursor = (tool_id == "cursor")
        self._apply_passthrough(is_cursor)
        self.tool_changed.emit(tool_id)

    # ── 鼠标事件重写 ──
    # 穿透模式下阻止所有鼠标事件到达 CanvasView（避免干扰下方窗口）
    # 非穿透模式完全交给 CanvasView 处理（包括 SmartEditController）

    @safe_event
    def mousePressEvent(self, ev):
        if self._passthrough:
            ev.ignore()
            return
        super().mousePressEvent(ev)

    @safe_event
    def mouseMoveEvent(self, ev):
        if self._passthrough:
            ev.ignore()
            return
        super().mouseMoveEvent(ev)

    @safe_event
    def mouseReleaseEvent(self, ev):
        if self._passthrough:
            ev.ignore()
            return
        super().mouseReleaseEvent(ev)

    @safe_event
    def keyPressEvent(self, ev):
        """快捷键已委托给 ShortcutManager，此处仅做穿透透传"""
        if self._passthrough:
            ev.ignore()
            return
        super().keyPressEvent(ev)

    # ── 覆盖 CanvasView 不需要的方法 ──

    def _get_magnifier_overlay(self):
        """GIF 绘制层不需要放大镜"""
        return None

    def _clear_magnifier_overlay(self):
        pass

    def _update_magnifier_overlay(self, scene_pos):
        pass
 