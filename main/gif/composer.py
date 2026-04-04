# -*- coding: utf-8 -*-
"""
GIF 合成器 + 进度对话框

技术路线：
  gifrecorder.FrameStore.export_gif()
  Rust 侧并行解码 → 缩放 → 光标叠加 → GIF 编码，进度通过回调报告。
  全部由 Rust gifrecorder 完成，无外部依赖。
"""

from __future__ import annotations

import os
import tempfile
from typing import Optional

from PySide6.QtCore import QObject, QThread, Signal, Qt, QEventLoop
from PySide6.QtWidgets import (QApplication, QFrame,
                              QProgressBar)
from ui.dialogs import show_warning_dialog

from ._widgets import PROGRESS_BAR_STYLE
from core.logger import log_info, log_error, log_exception

try:
    import gifrecorder
    _gifrecorder_available = True
except ImportError:
    gifrecorder = None
    _gifrecorder_available = False

# == 默认导出参数 ==
DEFAULT_GIF_WIDTH  = 0


# ── 合成线程 Worker ──────────────────────────────────

class _ComposeWorker(QObject):
    progress = Signal(int, int)      # (done, total)
    finished = Signal(bool, object)  # (ok, result: str|bytes|None)

    def __init__(self,
                 path: Optional[str],
                 store=None,
                 gif_width: int = DEFAULT_GIF_WIDTH,
                 frame_start: int = 0,
                 frame_end: int = 0,
                 cursor_sprites: Optional[dict] = None,
                 cursor_infos: Optional[list] = None,
                 speed_multiplier: float = 1.0):
        super().__init__()
        self._path = path
        self._store = store                     # gifrecorder.FrameStore 实例
        self._gif_width = gif_width
        self._frame_start = frame_start
        self._frame_end = frame_end
        self._cursor_sprites = cursor_sprites   # sprite 集合 dict
        self._cursor_infos = cursor_infos       # list of (x,y,press,scroll,burst_frame,burst_side)
        self._speed_multiplier = max(0.1, speed_multiplier)
        self._cancel = False

    def cancel(self):
        self._cancel = True
        if self._store is not None:
            self._store.cancel_export()

    def run(self):
        try:
            if not _gifrecorder_available or self._store is None:
                self.finished.emit(False, "gifrecorder_not_found")
                return
            result = self._compose()
            self.finished.emit(not self._cancel, result)
        except Exception as e:
            log_error(f"GIF 合成失败: {e}", "GIF")
            self.finished.emit(False, None)

    def _compose(self):
        need_bytes = self._path is None
        if need_bytes:
            tmp_fd, out_path = tempfile.mkstemp(suffix=".gif", prefix="jietuba_")
            os.close(tmp_fd)
        else:
            out_path = self._path

        ok = self._do_compose(out_path)

        if not ok or self._cancel:
            if need_bytes:
                try:
                    os.unlink(out_path)
                except Exception as e:
                    log_exception(e, "GIF 取消后删除临时文件")
            return None

        if need_bytes:
            try:
                with open(out_path, "rb") as f:
                    return f.read()
            finally:
                try:
                    os.unlink(out_path)
                except Exception as e:
                    log_exception(e, "GIF 读取后删除临时文件")
        else:
            return out_path

    def _do_compose(self, out_path: str) -> bool:
        """使用 gifrecorder.FrameStore.export_gif() 导出"""
        # 计算输出尺寸
        gif_width = self._gif_width
        gif_height = 0
        if gif_width > 0 and self._store.width > 0:
            ratio = gif_width / self._store.width
            gif_height = max(2, int(self._store.height * ratio))
            gif_height = gif_height if gif_height % 2 == 0 else gif_height - 1
        else:
            gif_width = self._store.width
            gif_height = self._store.height

        log_info(
            f"使用 gifrecorder export_gif: {gif_width}x{gif_height}",
            "GIF",
        )

        def _progress(done, total):
            self.progress.emit(done, total)
            return not self._cancel   # 返回 False 取消

        try:
            # 构建 cursor 导出参数
            export_kwargs = dict(
                path=out_path,
                width=gif_width,
                height=gif_height,
                repeat=0,
                frame_start=self._frame_start,
                frame_end=self._frame_end,
                progress_callback=_progress,
                speed=self._speed_multiplier,
            )
            if self._cursor_sprites and self._cursor_infos:
                export_kwargs["cursor_sprites"] = self._cursor_sprites
                export_kwargs["cursor_infos"] = self._cursor_infos

            self._store.export_gif(**export_kwargs)
        except Exception as e:
            err_str = str(e)
            if "cancelled" in err_str:
                return False
            log_error(f"export_gif 失败: {e}", "GIF")
            return False

        file_size = os.path.getsize(out_path) if os.path.isfile(out_path) else 0
        log_info(
            f"GIF 导出完成: {out_path} ({file_size / 1024:.1f} KB)",
            "GIF",
        )
        return True


# ── 进度浮层（无边框纯进度条） ──────────────────────────

class ComposerProgressDialog(QFrame):
    """
    无边框纯进度条浮层，样式与录制结束等待进度条保持一致。
    居中于 parent 窗口；合成完成/取消后 loop.quit() 解除阻塞。
    """

    def __init__(self, parent=None):
        super().__init__(None)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFixedSize(260, 14)

        self._result = None
        self._ok = False
        self._loop = QEventLoop()

        self._bar = QProgressBar(self)
        self._bar.setRange(0, 100)
        self._bar.setTextVisible(False)
        self._bar.setGeometry(0, 0, 260, 14)
        self._bar.setStyleSheet(PROGRESS_BAR_STYLE)

        self._thread: Optional[QThread] = None
        self._worker: Optional[_ComposeWorker] = None

    def _center_on(self, parent, center_pos=None):
        if center_pos is not None:
            self.move(center_pos.x() - self.width() // 2,
                      center_pos.y() - self.height() // 2)
            return
        if parent is not None:
            try:
                c = parent.geometry().center()
                self.move(c.x() - self.width() // 2, c.y() - self.height() // 2)
                return
            except Exception as e:
                log_exception(e, "居中进度对话框")
        screen = QApplication.primaryScreen().geometry()
        self.move(screen.center().x() - self.width() // 2,
                  screen.center().y() - self.height() // 2)

    def start(self,
              path: Optional[str] = None,
              store=None,
              gif_width: int = DEFAULT_GIF_WIDTH,
              frame_start: int = 0,
              frame_end: int = 0,
              cursor_sprites: Optional[dict] = None,
              cursor_infos: Optional[list] = None,
              speed_multiplier: float = 1.0):
        self._thread = QThread()
        self._worker = _ComposeWorker(
            path,
            store=store,
            gif_width=gif_width,
            frame_start=frame_start,
            frame_end=frame_end,
            cursor_sprites=cursor_sprites,
            cursor_infos=cursor_infos,
            speed_multiplier=speed_multiplier,
        )
        self._worker.moveToThread(self._thread)
        self._thread.started.connect(self._worker.run)
        self._worker.progress.connect(self._on_progress)
        self._worker.finished.connect(self._on_finished)
        self._thread.start()

    def _on_progress(self, done: int, total: int):
        if total <= 0:
            if self._bar.maximum() != 0:
                self._bar.setRange(0, 0)
        else:
            if self._bar.maximum() == 0:
                self._bar.setRange(0, 100)
            self._bar.setValue(int(done / total * 100))

    def _on_finished(self, ok: bool, result):
        self._ok = ok
        self._result = result
        # 先释放 worker 对 store 的引用，避免多余的 Arc 引用延迟释放
        if self._worker:
            self._worker._store = None
            self._worker._cursor_sprites = None
            self._worker._cursor_infos = None
        if self._thread:
            self._thread.quit()
            self._thread.wait()
            self._thread.deleteLater()
            self._thread = None
        if self._worker:
            self._worker.deleteLater()
            self._worker = None
        self.hide()
        self._loop.quit()

        if not ok and result == "gifrecorder_not_found":
            show_warning_dialog(
                None,
                "gifrecorder 不可用",
                "未找到 gifrecorder 模块，无法导出 GIF。\n\n"
                "请确认 gifrecorder 已正确安装。",
            )

    @staticmethod
    def run_compose(path: Optional[str] = None,
                    store=None,
                    gif_width: int = DEFAULT_GIF_WIDTH,
                    parent=None,
                    center_pos=None,
                    frame_start: int = 0,
                    frame_end: int = 0,
                    cursor_sprites: Optional[dict] = None,
                    cursor_infos: Optional[list] = None,
                    speed_multiplier: float = 1.0):
        """
        显示无边框进度条并阻塞直到合成完成/失败。
        返回 str(路径) / bytes / None(取消或失败)。
        center_pos: QPoint，直接指定进度条居中坐标（优先于 parent）。
        frame_start/frame_end: 修剪范围（含），0 表示不限制（导出全部帧）。
        speed_multiplier: 导出速度倍率（1.0=原速）。
        """
        dlg = ComposerProgressDialog(parent)
        dlg._center_on(parent, center_pos=center_pos)
        dlg.start(path,
                  store=store,
                  gif_width=gif_width,
                  frame_start=frame_start,
                  frame_end=frame_end,
                  cursor_sprites=cursor_sprites,
                  cursor_infos=cursor_infos,
                  speed_multiplier=speed_multiplier)
        dlg.show()
        QApplication.processEvents()
        dlg._loop.exec()
        dlg.deleteLater()

        return dlg._result if dlg._ok else None
 