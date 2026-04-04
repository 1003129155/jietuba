# -*- coding: utf-8 -*-
"""GIF 录制/回放主窗口 — 状态机协调器

职责划分：
  - 本文件: 状态机、窗口层管理、录制控制、绘制工具转发
  - PlaybackController: 回放引擎、预览层、光标覆盖层、导出
"""

from __future__ import annotations

from enum import Enum, auto

from PySide6.QtCore import QObject, QRect, QPoint, Qt, QThread, QTimer, Signal
from PySide6.QtWidgets import QApplication, QWidget

from .overlay import CaptureOverlay, OverlayMode
from .drawing_view import GifDrawingView
from .frame_recorder import FrameRecorder, RecordState
from .record_toolbar import RecordToolbar
from .playback_controller import PlaybackController

try:
    from core.logger import log_debug, log_info, log_warning, log_error, log_exception
except ImportError:
    import logging
    _l = logging.getLogger("GIF")
    log_debug = log_info = _l.info
    log_warning = _l.warning
    log_error = _l.error

from core.platform_utils import request_trim_working_set as _request_trim


class AppState(Enum):
    IDLE     = auto()   # 录制空闲，所有层穿透
    RESIZING = auto()   # 调整选区，overlay 接管鼠标
    DRAWING  = auto()   # 绘制中，draw_overlay 接管鼠标
    PLAYBACK = auto()   # 回放模式


class GifRecordWindow(QObject):
    """GIF 录制的总协调者 — 管理3层窗口 + 工具栏 + 状态机

    回放/导出逻辑委托给 PlaybackController。
    """

    def __init__(self, capture_rect: QRect, parent=None):
        super().__init__(parent)
        self._rect = QRect(capture_rect)
        self._state = AppState.IDLE
        log_info(f"GifRecordWindow 初始化, 区域={capture_rect}", "GIF")

        # ── 录制器 ──
        self._recorder = FrameRecorder()

        # ── 三层窗口 ──
        self._overlay = CaptureOverlay(self._rect)
        self._overlay.rect_changed.connect(self._on_rect_changed)

        # 绘制层（QGraphicsView，复用截图 canvas/tools）
        self._drawing_view = GifDrawingView(self._rect)

        # ── 录制工具栏 ──
        self._record_toolbar = RecordToolbar()
        # 让 CanvasView.wheelEvent 能通过 self.window().toolbar 找到工具栏
        self._drawing_view.toolbar = self._record_toolbar

        self._connect_record_toolbar()

        self._playback = PlaybackController(self)
        self._playback.close_requested.connect(self.close_all)
        self._playback.rerecord_requested.connect(self._on_rerecord)

        # ── 合成线程 ──
        self._compose_thread: QThread | None = None

        # ── 显示 ──
        self._overlay.show()
        self._drawing_view.show()
        self._record_toolbar.show()
        self._record_toolbar.set_record_rect(self._rect)
        self._reposition_toolbar(self._record_toolbar)

        # 初始穿透
        self._enter_state(AppState.IDLE)
        log_debug("GifRecordWindow 初始化完成", "GIF")

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 信号连接
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _connect_record_toolbar(self):
        tb = self._record_toolbar
        tb.fps_changed.connect(self._on_fps_changed)
        tb.record_start.connect(self._on_record_start)
        tb.record_stop.connect(self._on_record_stop)
        tb.pause_toggled.connect(self._on_pause_toggled)
        tb.move_requested.connect(self._on_move_requested)
        tb.drag_ended.connect(self._on_drag_ended)
        tb.close_requested.connect(self.close_all)
        # 绘制工具信号
        tb.tool_selected.connect(self._on_drawing_tool_selected)
        tb.deactivate_requested.connect(self._on_drawing_deactivate)
        tb.undo_requested.connect(self._on_drawing_undo)
        tb.redo_requested.connect(self._on_drawing_redo)
        tb.color_changed.connect(self._on_drawing_color_changed)
        tb.width_changed.connect(self._on_drawing_width_changed)
        tb.opacity_changed.connect(self._on_drawing_opacity_changed)
        # 文字工具信号 → SmartEditController
        tb.font_changed.connect(self._on_drawing_font_changed)
        tb.background_changed.connect(self._on_drawing_background_changed)
        # 样式信号
        tb.arrow_style_changed.connect(self._on_drawing_arrow_style_changed)
        tb.line_style_changed.connect(self._on_drawing_line_style_changed)
        # 帧捕获信号 → 更新录制时间显示
        self._recorder.frame_captured.connect(self._on_record_frame_count)
        # 达到 60s 上限 → 直接触发停止流程（等同于用户手动点停止）
        self._recorder.limit_reached.connect(self._on_record_stop)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 状态机
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _enter_state(self, state: AppState):
        self._state = state

        if state == AppState.IDLE:
            self._overlay.set_recording(False)            # 边框恢复蓝色
            self._overlay.set_mode(OverlayMode.RESIZE)    # IDLE 时允许拖拽调整录制区域
            self._drawing_view.activate_tool("cursor")    # 穿透
            self._record_toolbar.highlight_tool(None)

        elif state == AppState.RESIZING:
            self._overlay.set_mode(OverlayMode.RESIZE)
            self._drawing_view.activate_tool("cursor")
            self._record_toolbar.highlight_tool(None)

        elif state == AppState.DRAWING:
            self._overlay.set_mode(OverlayMode.PASSTHROUGH)
            # drawing_view 工具由录制工具栏控制

        elif state == AppState.PLAYBACK:
            self._overlay.set_mode(OverlayMode.PASSTHROUGH)
            self._drawing_view.activate_tool("cursor")
            self._drawing_view.hide()
            self._record_toolbar.highlight_tool(None)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 位置同步
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _sync_geometry(self, rect: QRect, reposition: bool = True):
        self._rect = QRect(rect)
        self._overlay.update_rect(rect)
        if self._drawing_view:
            self._drawing_view.update_rect(rect)
        # 通知工具栏当前录制区域（用于面板定位方向判断）
        self._record_toolbar.set_record_rect(self._rect)
        self._playback.update_rect(self._rect)
        if reposition:
            pb_tb = self._playback.playback_toolbar
            tb = pb_tb if pb_tb and pb_tb.isVisible() else self._record_toolbar
            self._reposition_toolbar(tb)
        # 重新定位面板
        self._record_toolbar.reposition_panels()

    def _reposition_toolbar(self, toolbar: QWidget):
        """工具栏智能定位：优先选区下方 → 上方 → 左侧 → 右侧"""
        r = self._rect
        tw = toolbar.width()
        th = toolbar.height()
        gap = 4

        # 根据录制区域中心点获取所属屏幕，而非固定使用主屏
        center = r.center()
        screen = QApplication.screenAt(center)
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_rect = screen.geometry()

        # 获取二级设置面板的最大高度，与工具栏一起整体判断能否放下
        panel_extra = toolbar.get_max_panel_height() if hasattr(toolbar, 'get_max_panel_height') else 0

        # 水平居中 x（供上下方案使用）
        x_center = r.left() + (r.width() - tw) // 2
        x_center = max(screen_rect.left(), min(x_center, screen_rect.right() - tw))

        # 策略 1：录制区下方
        y_below = r.bottom() + gap
        if y_below + th + panel_extra <= screen_rect.bottom():
            x, y, toolbar_below = x_center, y_below, True
        # 策略 2：录制区上方
        elif r.top() - th - gap >= screen_rect.top():
            x, y, toolbar_below = x_center, r.top() - th - gap, False
        else:
            # 策略 3/4：左侧 / 右侧（录制区占满纵向时）
            # 竖方向居中对齐录制区
            y_mid = r.top() + (r.height() - th) // 2
            y_mid = max(screen_rect.top(), min(y_mid, screen_rect.bottom() - th))
            if r.left() - tw - gap >= screen_rect.left():
                # 左侧有空间
                x, y, toolbar_below = r.left() - tw - gap, y_mid, False
            else:
                # 右侧兜底
                x = min(r.right() + gap, screen_rect.right() - tw)
                x = max(screen_rect.left(), x)
                x, y, toolbar_below = x, y_mid, True

        if hasattr(toolbar, 'set_toolbar_side'):
            toolbar.set_toolbar_side(toolbar_below)
        toolbar.move(x, y)

    def _on_rect_changed(self, rect: QRect):
        self._sync_geometry(rect)
        # resize 完成后切回 IDLE
        self._enter_state(AppState.IDLE)

    def _on_move_requested(self, delta: QPoint):
        """拖动手柄移动整个选区 + 工具栏"""
        new_rect = self._rect.translated(delta)
        # 拖动期间跳过智能重定位，工具栏跟随平移；松手后由 _on_drag_ended 对齐
        self._sync_geometry(new_rect, reposition=False)
        pb_tb = self._playback.playback_toolbar
        tb = pb_tb if pb_tb and pb_tb.isVisible() else self._record_toolbar
        tb.move(tb.pos() + delta)

    def _on_drag_ended(self):
        pb_tb = self._playback.playback_toolbar
        tb = pb_tb if pb_tb and pb_tb.isVisible() else self._record_toolbar
        self._reposition_toolbar(tb)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 录制回调
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _on_fps_changed(self, fps: int):
        log_debug(f"FPS 切换: {fps}", "GIF")
        self._recorder.set_fps(fps)

    def _on_record_frame_count(self, count: int):
        """帧捕获回调 → 将帧数转为已录制时长显示"""
        fps = self._record_toolbar.get_current_fps()
        elapsed_s = count / fps if fps > 0 else 0
        self._record_toolbar.set_elapsed(elapsed_s)

    def _on_record_start(self):
        fps = self._record_toolbar.get_current_fps()
        log_info(f"开始录制, fps={fps}, 区域={self._rect}", "GIF")
        self._recorder.set_fps(fps)
        self._recorder.set_rect(self._rect)
        self._recorder.start()
        # 边框变红，禁止拖拽（PASSTHROUGH）
        self._overlay.set_recording(True)
        self._overlay.set_mode(OverlayMode.PASSTHROUGH)

    def _on_record_stop(self):
        """停止录制：调用 recorder.stop_async()，后台等待 gdigrab 退出，完成后切回放"""
        if self._recorder.state not in (RecordState.RECORDING, RecordState.PAUSED):
            return

        self._show_stop_progress()

        frames = self._recorder.frames
        store = self._recorder.store
        log_info(f"停止录制, 共 {len(frames)} 帧, store={'有' if store else '无'}", "GIF")

        if store is not None and store.frame_count == 0:
            log_warning("FrameStore 为空", "GIF")
            from ui.dialogs import show_warning_dialog
            show_warning_dialog(None, "录制异常", "录制数据为空，请重新录制。")
            # 恢复 overlay + toolbar 到可操作状态（避免卡在红色穿透模式）
            self._overlay.set_recording(False)
            self._overlay.set_mode(OverlayMode.RESIZE)
            self._record_toolbar.reset_state()
            return

        if not frames:
            log_warning("录制帧为空，不进入回放", "GIF")
            self._overlay.set_recording(False)
            self._overlay.set_mode(OverlayMode.RESIZE)
            self._record_toolbar.reset_state()
            return
        self._switch_to_playback(frames)

    def _show_stop_progress(self):
        """无边框进度条（延迟显示，避免被录进视频尾部）"""
        from PySide6.QtWidgets import QProgressBar, QFrame, QVBoxLayout
        from PySide6.QtCore import QEventLoop, QTimer as _QTimer
        from ._widgets import PROGRESS_BAR_STYLE

        # ── 无边框进度条 ──
        dlg = QFrame(None)
        dlg.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        dlg.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        dlg.setFixedSize(260, 14)

        layout = QVBoxLayout(dlg)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        bar = QProgressBar(dlg)
        bar.setRange(0, 0)
        bar.setTextVisible(False)
        bar.setFixedHeight(14)
        bar.setStyleSheet(PROGRESS_BAR_STYLE)
        layout.addWidget(bar)

        dlg.move(
            self._rect.center().x() - 130,
            self._rect.center().y() - 7,
        )

        # ── 通过 stop_async + stop_finished 信号等待 gdigrab 退出 ──
        loop = QEventLoop()
        self._recorder.stop_finished.connect(loop.quit)
        self._recorder.stop_async()

        # 延迟约 400ms 才显示进度条，确保 gdigrab 停止画面捕获
        _show_timer = _QTimer()
        _show_timer.setSingleShot(True)
        _show_timer.timeout.connect(dlg.show)
        _show_timer.start(400)

        loop.exec()

        # ── 清理进度条 ──
        _show_timer.stop()
        from core.qt_utils import safe_disconnect
        safe_disconnect(self._recorder.stop_finished, loop.quit)
        bar.setRange(0, 1)
        bar.setValue(1)
        dlg.hide()
        QApplication.processEvents()
        dlg.deleteLater()

    def _on_pause_toggled(self, paused: bool):
        log_debug(f"录制{'暂停' if paused else '恢复'}", "GIF")
        if paused:
            self._recorder.pause()
        else:
            self._recorder.resume()

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 绘制工具回调
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _on_drawing_tool_selected(self, tool_id: str):
        """录制工具栏选择了一个绘制工具"""
        log_debug(f"绘制工具: {tool_id}", "GIF")
        self._drawing_view.activate_tool(tool_id)
        # 同步当前颜色/线宽到工具栏面板 UI
        self._record_toolbar.sync_from_controller(self._drawing_view.tool_controller)

    def _on_drawing_deactivate(self):
        """退出绘制 → 回到穿透"""
        log_debug("退出绘制模式", "GIF")
        self._drawing_view.activate_tool("cursor")
        self._record_toolbar.highlight_tool(None)

    def _on_drawing_undo(self):
        self._drawing_view.undo_stack.undo()

    def _on_drawing_redo(self):
        self._drawing_view.undo_stack.redo()

    def _on_drawing_color_changed(self, color):
        self._drawing_view.tool_controller.update_style(color=color)
        # 通知光标管理器更新颜色
        if hasattr(self._drawing_view, 'cursor_manager') and self._drawing_view.cursor_manager:
            self._drawing_view.cursor_manager.update_tool_cursor_color(color)
        # 文字工具也需要通过 SmartEditController 更新（用于已选中的 TextItem）
        if hasattr(self._drawing_view, 'smart_edit_controller'):
            self._drawing_view.smart_edit_controller.on_text_color_changed(color)

    def _on_drawing_width_changed(self, width: int):
        self._drawing_view.tool_controller.update_style(width=width)

    def _on_drawing_opacity_changed(self, opacity: int):
        self._drawing_view.tool_controller.update_style(opacity=opacity / 255.0)

    def _on_drawing_font_changed(self, font):
        """文字字体/样式改变 → 通知 SmartEditController 更新选中文字"""
        if hasattr(self._drawing_view, 'smart_edit_controller'):
            self._drawing_view.smart_edit_controller.on_text_font_changed(font)

    def _on_drawing_background_changed(self, enabled: bool, color, opacity: int):
        """文字背景改变 → 通知 SmartEditController"""
        if hasattr(self._drawing_view, 'smart_edit_controller'):
            self._drawing_view.smart_edit_controller.on_text_background_changed(enabled, color, opacity)

    def _on_drawing_arrow_style_changed(self, style: str):
        """箭头样式改变 → 更新选中的箭头图元"""
        if not hasattr(self._drawing_view, 'smart_edit_controller'):
            return
        controller = self._drawing_view.smart_edit_controller
        item = controller.selected_item
        from canvas.items import ArrowItem
        if isinstance(item, ArrowItem) and hasattr(item, 'arrow_style'):
            item.arrow_style = style
            item.update()

    def _on_drawing_line_style_changed(self, style: str):
        """线条样式改变 → 更新选中的画笔/形状图元"""
        if hasattr(self._drawing_view, '_apply_line_style_change_to_selection'):
            self._drawing_view._apply_line_style_change_to_selection(style)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 录制 ↔ 回放 切换
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _switch_to_playback(self, frames):
        """停止录制 → 进入回放"""
        # 隐藏录制工具栏
        self._record_toolbar.hide()
        self._record_toolbar.reset_state()   # 复位图标状态（limit_reached 路径不经过按钮回调）
        # 隐藏绘制层（回放时不需要）
        self._drawing_view.activate_tool("cursor")
        self._drawing_view.hide()

        self._playback.set_context(
            self._rect, self._recorder, self._record_toolbar, self._reposition_toolbar
        )
        self._playback.start_playback(frames)
        self._enter_state(AppState.PLAYBACK)

    def _on_rerecord(self):
        """重新录制 — 完全重置，从零开始"""
        log_info("重新录制", "GIF")

        # 完全重置录制器（清空所有帧 + 释放 Rust 堆内存）
        self._recorder.reset()

        # 立即处理 deleteLater，让 Qt 真正销毁已排队的对象
        QApplication.processEvents()

        # 重置录制工具栏 UI 状态（按钮 / 时间清零）
        self._record_toolbar.reset_state()

        # 重建绘制层
        if self._drawing_view:
            self._drawing_view.clear_all()
            self._drawing_view.update_rect(self._rect)
            self._drawing_view.show()

        self._record_toolbar.show()
        self._reposition_toolbar(self._record_toolbar)
        self._enter_state(AppState.IDLE)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 关闭
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def close_all(self):
        log_info("关闭 GIF 窗口", "GIF")

        if self._recorder.state in (RecordState.RECORDING, RecordState.PAUSED):
            self._recorder.stop()

        # 断开 recorder 信号，防止 queued signal 访问已销毁的工具栏
        from core.qt_utils import safe_disconnect
        safe_disconnect(self._recorder.frame_captured)
        safe_disconnect(self._recorder.limit_reached)

        for w in [self._overlay, self._drawing_view, self._record_toolbar]:
            if w:
                w.hide()

        self._playback.hide_all()
        self._playback.disconnect_engine_signals()

        engine, playback_toolbar = self._playback.detach_for_cleanup()
        recorder = self._recorder
        overlay = self._overlay
        drawing_view = self._drawing_view
        record_toolbar = self._record_toolbar

        self._overlay = None
        self._drawing_view = None
        self._record_toolbar = None
        self._compose_thread = None
        self._recorder = None   # 防止延迟回调访问

        # ── 全部在主线程同步完成 ──
        # PySide6 不允许从 threading.Thread emit Signal（与 PyQt6 不同），
        # 而 recorder.stop() 已在上面同步完成了阻塞操作，
        # 剩余的 engine.cleanup() 和 recorder.reset_data() 都是轻量调用。
        if engine:
            engine.stop_timer()
            engine.cleanup(blocking=True)

        if recorder:
            recorder.reset_data()

        try:
            app = QApplication.instance()
            if app and getattr(app, "_gif_window", None) is self:
                app._gif_window = None
                log_debug("清除 app._gif_window", "GIF")
        except Exception as e:
            log_exception(e, "清除 app._gif_window")

        widgets_to_delete = [w for w in [engine, overlay, drawing_view,
                                          record_toolbar, playback_toolbar] if w]
        for w in widgets_to_delete:
            w.deleteLater()

        _request_trim(1000)
        self.deleteLater()
 