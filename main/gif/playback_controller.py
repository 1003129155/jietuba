# -*- coding: utf-8 -*-
"""回放控制器 — 管理回放引擎、预览层、光标覆盖层、导出
"""

from __future__ import annotations

import os
from typing import Optional

from PySide6.QtCore import QObject, QRect, Qt, Signal
from PySide6.QtWidgets import QApplication, QFileDialog, QLabel
from PySide6.QtGui import QPixmap, QImage

from .playback_engine import PlaybackEngine
from .playback_toolbar import PlaybackToolbar
from .composer import ComposerProgressDialog
from .cursor_overlay import CursorOverlay

try:
    from core.logger import log_debug, log_info, log_warning, log_error, log_exception
except ImportError:
    import logging
    _l = logging.getLogger("GIF")
    log_debug = log_info = _l.info
    log_warning = _l.warning
    log_error = _l.error
    log_exception = lambda e, ctx="": _l.error(f"{ctx}: {e}")


class PlaybackController(QObject):
    """回放阶段的控制器 — 管理引擎、预览层、光标、工具栏、导出。

    信号:
        close_requested()        — 导出完成或用户关闭时请求关闭整个窗口
        rerecord_requested()     — 用户请求重新录制
    """

    close_requested    = Signal()
    rerecord_requested = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._engine: Optional[PlaybackEngine] = None
        self._playback_toolbar: Optional[PlaybackToolbar] = None
        self._preview_label: Optional[QLabel] = None
        self._cursor_overlay: Optional[CursorOverlay] = None
        self._cursor_export_enabled: bool = True
        self._export_speed: float = 1.0   # 导出速度倍率，与播放速度同步

        # 由 GifRecordWindow 注入的上下文
        self._rect = QRect()
        self._recorder = None          # FrameRecorder 引用
        self._record_toolbar = None    # RecordToolbar 引用（取 fps）
        self._reposition_fn = None     # 工具栏定位回调

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 初始化 / 注入
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def set_context(self, rect: QRect, recorder, record_toolbar, reposition_fn):
        """注入 GifRecordWindow 的共享上下文。"""
        self._rect = QRect(rect)
        self._recorder = recorder
        self._record_toolbar = record_toolbar
        self._reposition_fn = reposition_fn

    def update_rect(self, rect: QRect):
        self._rect = QRect(rect)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 属性
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    @property
    def engine(self) -> Optional[PlaybackEngine]:
        return self._engine

    @property
    def playback_toolbar(self) -> Optional[PlaybackToolbar]:
        return self._playback_toolbar

    @property
    def preview_label(self) -> Optional[QLabel]:
        return self._preview_label

    @property
    def cursor_overlay(self) -> Optional[CursorOverlay]:
        return self._cursor_overlay

    @property
    def is_active(self) -> bool:
        """回放控制器是否处于活跃状态（有引擎且有工具栏）"""
        return self._engine is not None

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 进入 / 退出回放
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def start_playback(self, frames):
        """创建回放引擎、预览层、工具栏，进入回放模式。"""
        log_info(f"切换到回放模式, {len(frames)} 帧", "GIF")

        # ── 创建预览层（在 overlay 之上显示帧图像）──
        self._preview_label = QLabel()
        self._preview_label.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self._preview_label.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self._preview_label.setStyleSheet("background: black;")
        self._preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._preview_label.setGeometry(self._rect)
        self._preview_label.show()
        log_debug(f"预览层创建, geometry={self._rect}", "GIF")

        # ── 创建鼠标光标覆盖层（在预览层之上）──
        self._cursor_overlay = CursorOverlay()
        self._cursor_overlay.update_rect(self._rect)
        self._cursor_overlay.show()
        self._cursor_overlay.raise_()

        # ── 创建回放工具栏 ──
        fps = self._record_toolbar.get_current_fps()
        total_ms = self._recorder.total_duration_ms
        self._playback_toolbar = PlaybackToolbar()
        self._connect_playback_toolbar()
        self._playback_toolbar.set_total_frames(len(frames), fps, total_ms=total_ms)
        self._playback_toolbar.show()
        if self._reposition_fn:
            self._reposition_fn(self._playback_toolbar)

        # ── 创建回放引擎 ──
        self._engine = PlaybackEngine()
        self._engine.load(frames, fps, store=self._recorder.store)
        self._engine.set_display_size(self._preview_label.width(), self._preview_label.height())
        self._engine.frame_ready.connect(self._on_frame_ready)
        self._engine.frame_changed.connect(self._on_playback_frame_index)
        self._engine.playback_finished.connect(self._on_playback_finished)

        # 同步解码并显示第一帧
        QApplication.processEvents()
        self._render_frame(0)

    def teardown(self):
        """完全销毁回放相关资源（重新录制 / 关闭时调用）。"""
        # 停止并销毁引擎
        if self._engine:
            from core.qt_utils import safe_disconnect
            safe_disconnect(self._engine.frame_ready, self._on_frame_ready)
            safe_disconnect(self._engine.frame_changed, self._on_playback_frame_index)
            safe_disconnect(self._engine.playback_finished, self._on_playback_finished)
            self._engine.cleanup()
            self._engine.deleteLater()
            self._engine = None

        # 销毁回放工具栏
        self._disconnect_and_destroy_playback_toolbar()

        # 销毁预览层和光标层
        self._destroy_preview_label()

    def hide_all(self):
        """隐藏所有回放相关 UI（close_all 先隐藏后异步释放用）"""
        if self._playback_toolbar:
            self._playback_toolbar.hide()
        if self._preview_label:
            self._preview_label.hide()
        if self._cursor_overlay:
            self._cursor_overlay.hide()

    def disconnect_engine_signals(self):
        """断开引擎信号（close_all 避免延迟回调用）"""
        if self._engine:
            from core.qt_utils import safe_disconnect
            safe_disconnect(self._engine.frame_ready)
            safe_disconnect(self._engine.frame_changed)
            safe_disconnect(self._engine.playback_finished)

    def detach_for_cleanup(self):
        """分离引擎和工具栏引用，供后台清理线程使用。

        Returns:
            (engine, playback_toolbar) 元组，由调用方负责 deleteLater。
        """
        engine = self._engine
        toolbar = self._playback_toolbar
        self._engine = None
        self._playback_toolbar = None
        self._destroy_preview_label()
        return engine, toolbar

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 信号连接
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _connect_playback_toolbar(self):
        tb = self._playback_toolbar
        tb.play_pause.connect(self._on_play_pause)
        tb.speed_changed.connect(self._on_speed_changed)
        tb.seek_requested.connect(self._on_seek)
        tb.trim_changed.connect(self._on_trim_changed)
        tb.save_requested.connect(self._on_save)
        tb.copy_requested.connect(self._on_copy)
        tb.rerecord.connect(self._on_rerecord)
        tb.close_requested.connect(self.close_requested.emit)
        tb.cursor_toggled.connect(self._on_cursor_toggled)

    def _disconnect_and_destroy_playback_toolbar(self):
        """安全断开回放工具栏所有信号并销毁。"""
        if not self._playback_toolbar:
            return
        from core.qt_utils import safe_disconnect
        tb = self._playback_toolbar
        safe_disconnect(tb.play_pause)
        safe_disconnect(tb.speed_changed)
        safe_disconnect(tb.seek_requested)
        safe_disconnect(tb.trim_changed)
        safe_disconnect(tb.save_requested)
        safe_disconnect(tb.copy_requested)
        safe_disconnect(tb.rerecord)
        safe_disconnect(tb.close_requested)
        safe_disconnect(tb.cursor_toggled)
        self._playback_toolbar.hide()
        self._playback_toolbar.deleteLater()
        self._playback_toolbar = None

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 回放回调
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _on_play_pause(self):
        if not self._engine:
            return
        if self._engine.is_playing:
            log_debug("回放暂停", "GIF")
            self._engine.pause()
        else:
            log_debug("回放开始", "GIF")
            self._engine.play()

    def _on_speed_changed(self, speed: float):
        log_debug(f"回放速度: {speed}", "GIF")
        self._export_speed = speed
        if self._engine:
            self._engine.set_speed(speed)

    def _on_seek(self, index: int):
        log_debug(f"跳转帧: {index}", "GIF")
        if self._engine:
            self._engine.seek(index)
            if not self._engine.is_playing:
                self._render_frame(index)

    def _on_trim_changed(self, start: int, end: int):
        if self._engine:
            self._engine.set_trim(start, end)

    def _on_rerecord(self):
        """重新录制 — 由 GifRecordWindow 处理具体重置逻辑"""
        log_info("重新录制（回放控制器）", "GIF")
        self.teardown()
        QApplication.processEvents()
        self.rerecord_requested.emit()

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 帧渲染
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _render_frame(self, index: int):
        """静止预览单帧（seek / 首帧 / 暂停后）— 从 FrameStore 解码"""
        if not self._engine or not self._preview_label:
            return
        try:
            w = self._preview_label.width()
            h = self._preview_label.height()
            img = self._engine.get_frame_image(index, w, h)
            if img is None:
                return
            self._preview_label.setPixmap(QPixmap.fromImage(img))
            self._update_cursor_for_frame(index)
        except Exception as e:
            log_error(f"帧渲染失败 index={index}: {e}", "GIF")

    def _on_frame_ready(self, img: QImage, index: int):
        """常驻解码器吐帧回调 — 直接上屏"""
        if self._preview_label:
            self._preview_label.setPixmap(QPixmap.fromImage(img))
        self._update_cursor_for_frame(index)
        self._update_playhead(index)

    def _on_playback_frame_index(self, index: int):
        """frame_changed 信号回调 — 仅更新进度条（seek / 暂停时使用）"""
        if self._engine and not self._engine.is_playing:
            self._update_playhead(index)

    def _update_playhead(self, index: int):
        """更新进度条时间显示"""
        if not self._playback_toolbar:
            return
        frames = self._recorder.frames
        total = len(frames)
        fps = self._record_toolbar.get_current_fps()
        current_ms = frames[index].elapsed_ms if index < total else -1
        self._playback_toolbar.set_playhead(index, total, fps, current_ms=current_ms)

    def _on_playback_finished(self):
        log_debug("回放结束", "GIF")
        start = self._engine.trim_start if self._engine else 0
        if self._engine:
            self._engine.seek(start)
            self._render_frame(start)
        if self._playback_toolbar:
            self._playback_toolbar.set_playing(False)
            frames = self._recorder.frames
            total = len(frames)
            fps = self._record_toolbar.get_current_fps()
            start_ms = frames[start].elapsed_ms if start < total else -1
            self._playback_toolbar.set_playhead(start, total, fps, current_ms=start_ms)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 鼠标光标覆盖
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _update_cursor_for_frame(self, index: int):
        """根据帧索引更新鼠标光标覆盖层"""
        if self._cursor_overlay is None:
            return
        frames = self._recorder.frames
        if 0 <= index < len(frames):
            self._cursor_overlay.set_frame_cursor(frames[index].cursor)
        else:
            self._cursor_overlay.set_frame_cursor(None)

    def _on_cursor_toggled(self, visible: bool):
        """用户切换鼠标光标显示"""
        self._cursor_export_enabled = visible
        if self._cursor_overlay:
            self._cursor_overlay.set_cursor_visible(visible)

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 预览层管理
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _destroy_preview_label(self):
        """销毁帧预览层和鼠标光标覆盖层"""
        if self._cursor_overlay:
            self._cursor_overlay.hide()
            self._cursor_overlay.deleteLater()
            self._cursor_overlay = None
        if self._preview_label:
            self._preview_label.setPixmap(QPixmap())
            self._preview_label.hide()
            self._preview_label.deleteLater()
            self._preview_label = None
            log_debug("预览层销毁", "GIF")

    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    # 导出：保存 / 复制
    # ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    def _get_trim_range(self):
        """获取当前裁剪范围"""
        if self._playback_toolbar:
            return self._playback_toolbar.get_trim_range()
        return 0, len(self._recorder.frames) - 1

    def _on_save(self):
        path, _ = QFileDialog.getSaveFileName(
            None, "保存 GIF", "", "GIF 动图 (*.gif)"
        )
        if not path:
            log_debug("保存取消", "GIF")
            return
        log_info(f"保存 GIF: {path}", "GIF")
        self._compose_and_export(path, copy_to_clipboard=False)

    def _on_copy(self):
        out = self._make_save_path()
        log_info(f"复制 GIF → {out}", "GIF")
        self._compose_and_export(out, copy_to_clipboard=True)

    def _compose_and_export(self, output_path: str, copy_to_clipboard: bool):
        """弹出进度框导出 GIF，完成后写文件或复制到剪贴板"""
        frames = self._recorder.frames
        store = self._recorder.store
        log_info(
            f"开始导出 GIF: frames={len(frames)}, "
            f"copy={copy_to_clipboard}, store={'有' if store else '无'}",
            "GIF",
        )

        if not frames:
            log_warning("帧列表为空，中止导出", "GIF")
            return

        # ── 获取修剪范围 ──
        trim_start, trim_end = self._get_trim_range()
        trim_start = max(0, min(trim_start, len(frames) - 1))
        trim_end   = max(trim_start, min(trim_end, len(frames) - 1))
        log_info(f"导出裁剪范围: [{trim_start}, {trim_end}]，共 {trim_end - trim_start + 1} 帧", "GIF")

        # ── 准备鼠标光标导出参数 ──
        cursor_sprites = None
        cursor_infos = None
        if self._cursor_export_enabled and store is not None:
            from .cursor_overlay import rasterize_cursor_sprites, compute_burst_states
            from .composer import DEFAULT_GIF_WIDTH

            src_w = store.width
            gif_width = DEFAULT_GIF_WIDTH
            scale = gif_width / src_w if src_w > 0 and gif_width > 0 else 1.0

            cursor_sprites = rasterize_cursor_sprites(scale)

            cursors = [f.cursor for f in frames]
            burst_states = compute_burst_states(cursors)
            cursor_infos = []
            for i, cur in enumerate(cursors):
                if cur is None or not cur.visible:
                    cursor_infos.append(None)
                else:
                    bf, bs = burst_states[i]
                    cursor_infos.append((
                        int(cur.x * scale),
                        int(cur.y * scale),
                        cur.press, cur.scroll, bf, bs,
                    ))

            n_active = sum(1 for c in cursor_infos[trim_start:trim_end + 1] if c is not None)
            log_info(f"鼠标导出: {n_active}/{trim_end - trim_start + 1} 帧有光标", "GIF")

        result = ComposerProgressDialog.run_compose(
            path=output_path,
            store=store,
            center_pos=self._rect.center(),
            frame_start=trim_start,
            frame_end=trim_end,
            cursor_sprites=cursor_sprites,
            cursor_infos=cursor_infos,
            speed_multiplier=self._export_speed,
        )

        cursor_sprites = None
        cursor_infos = None

        if result is None:
            log_warning("GIF 导出取消或失败", "GIF")
            return

        log_info(f"GIF 导出完成: {result}", "GIF")

        if copy_to_clipboard:
            self._copy_file_to_clipboard(output_path, mime_type="image/gif")

        self.close_requested.emit()

    @staticmethod
    def _copy_file_to_clipboard(path: str, mime_type: str = "image/gif"):
        """将文件复制到剪贴板（写 URL + 原始 bytes 双格式）"""
        try:
            from PySide6.QtCore import QMimeData, QUrl
            with open(path, "rb") as f:
                data_bytes = f.read()
            clipboard = QApplication.clipboard()
            mime = QMimeData()
            mime.setUrls([QUrl.fromLocalFile(path)])
            mime.setData(mime_type, data_bytes)
            clipboard.setMimeData(mime)
            log_info(f"已复制到剪贴板 [{mime_type}], 大小={len(data_bytes)} bytes", "GIF")
        except Exception as e:
            log_error(f"复制到剪贴板失败: {e}", "GIF")

    @staticmethod
    def _make_save_path() -> str:
        """生成截图保存目录下的 GIF 路径（确保目录存在）"""
        from datetime import datetime
        try:
            from settings.tool_settings import ToolSettingsManager
            save_dir = ToolSettingsManager().get_screenshot_save_path()
        except Exception as e:
            log_exception(e, "获取截图保存路径")
            save_dir = os.path.join(os.path.expanduser("~"), "Desktop")

        os.makedirs(save_dir, exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        return os.path.join(save_dir, f"jietuba_gif_{ts}.gif")
 