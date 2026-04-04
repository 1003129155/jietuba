# -*- coding: utf-8 -*-
"""
帧采集控制器 — 全 Rust 架构 (gifrecorder)

录制：Rust 线程 Win32 BitBlt 截屏 → JPEG 压缩 → FrameStore 存储
      Python 侧仅 QTimer tick 通知 UI 帧数，完全不碰像素

停止后：
  - _store: gifrecorder.FrameStore 实例（持有全部帧数据）
  - _frames: 每帧对应的 FrameData（含 elapsed_ms + 标注列表）

图像由 PlaybackEngine 通过 FrameStore.get_frame_rgb() / start_decoder() 解码。
"""

import time
import ctypes
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import List, Optional, Tuple

# Win32 POINT 结构体（复用）
class _POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

from PySide6.QtCore import QObject, QTimer, QRect, QThread, Signal

from core.logger import log_debug, log_error, log_info, log_exception

try:
    import gifrecorder
    _gifrecorder_available = True
except ImportError:
    gifrecorder = None
    _gifrecorder_available = False


# ── 数据结构 ──────────────────────────────────────────

class RecordState(Enum):
    IDLE      = auto()
    RECORDING = auto()
    PAUSED    = auto()
    STOPPED   = auto()


@dataclass
class CursorSnapshot:
    """单帧的鼠标状态快照（屏幕绝对坐标 → 相对录制区域坐标）"""
    x: int               # 相对于录制区域左上角的 X
    y: int               # 相对于录制区域左上角的 Y
    press: int           # 按键状态：0=无，1=左键，2=右键
    visible: bool = True # 光标是否在录制区域内
    scroll: int   = 0    # 滚轮方向：0=无，1=向上，-1=向下


@dataclass
class FrameData:
    """单帧元数据（无图像，图像由 PlaybackEngine 按需从 FrameStore 解码）"""
    elapsed_ms: int              # 距录制开始的毫秒数
    width: int    = 0            # 录制区域原始宽度（偶数对齐后）
    height: int   = 0            # 录制区域原始高度（偶数对齐后）
    annotations: list = field(default_factory=list)
    cursor: Optional[CursorSnapshot] = None  # 鼠标状态（None = 未采集）


# ── 采集器 ──────────────────────────────────────────

class FrameRecorder(QObject):
    """
    以指定帧率截屏，通过 gifrecorder.RecordSession (Rust) 驱动。

    架构：
      Rust 线程: Win32 BitBlt 截屏 → JPEG 压缩 → FrameStore
      Python 侧: QTimer tick 仅发信号通知 UI 帧数变化

    Python 完全不参与截屏和像素处理，零 GIL 争用。
    """

    frame_captured = Signal(int)   # 当前总帧数
    state_changed  = Signal(str)   # "IDLE" / "RECORDING" / ...
    limit_reached  = Signal()      # 达到最大录制时长，自动触发停止
    stop_finished  = Signal()      # stop_async 完成后发射

    def __init__(self, parent=None):
        super().__init__(parent)
        self._rect: QRect = QRect()
        self._fps: int = 16
        self._frames: List[FrameData] = []
        self._state = RecordState.IDLE

        # QTimer 仅用于通知 UI 帧数更新 + 检测超时
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)

        self._max_duration_s: int = 60   # 最大录制时长（秒），0 表示不限制
        self._start_time: float = 0.0
        self._pause_offset: float = 0.0
        self._pause_start: float = 0.0

        # gifrecorder 对象
        self._store = None            # gifrecorder.FrameStore
        self._session = None          # gifrecorder.RecordSession
        self._rec_width: int = 0
        self._rec_height: int = 0

        # 鼠标轨迹（录制期间每 tick 采集一次）
        self._cursor_track: List[CursorSnapshot] = []
        self._rec_left: int = 0       # 录制区域屏幕左上角
        self._rec_top: int = 0

        # pynput 滚轮监听（录制期间启动，读取后清零）
        self._scroll_value: int = 0   # 0=无, 1=向上, -1=向下
        self._mouse_listener = None   # pynput.mouse.Listener

    # ── 属性 ──

    @property
    def state(self) -> RecordState:
        return self._state

    @property
    def frames(self) -> List[FrameData]:
        return self._frames

    @property
    def fps(self) -> int:
        return self._fps

    @property
    def store(self):
        """gifrecorder.FrameStore 实例（录制/回放/导出共享）"""
        return self._store

    @property
    def total_duration_ms(self) -> int:
        if self._store is not None:
            return self._store.total_duration_ms
        if not self._frames:
            return 0
        return self._frames[-1].elapsed_ms

    @property
    def rec_size(self):
        """录制分辨率 (width, height)"""
        return self._rec_width, self._rec_height

    # ── 配置 ──

    def set_rect(self, rect: QRect):
        self._rect = QRect(rect)

    def set_fps(self, fps: int):
        self._fps = fps
        if self._state == RecordState.RECORDING:
            self._timer.setInterval(1000 // fps)

    # ── 生命周期 ──

    def start(self):
        """开始录制。Rust 线程 Win32 BitBlt 截屏 → FrameStore。"""
        if self._state not in (RecordState.IDLE, RecordState.STOPPED):
            return
        self._frames.clear()
        self._store = None
        self._session = None

        if not _gifrecorder_available:
            log_error("gifrecorder 不可用，无法录制", "GIF")
            return

        # 内缩 1px 四边，避免录入选区边框线
        _INSET = 1
        w = self._rect.width()  - _INSET * 2
        h = self._rect.height() - _INSET * 2
        left = self._rect.x() + _INSET
        top  = self._rect.y() + _INSET
        w = max(w, 2)
        h = max(h, 2)
        self._rec_width = w
        self._rec_height = h
        self._rec_left = left
        self._rec_top = top
        self._cursor_track.clear()

        # 创建 FrameStore
        try:
            self._store = gifrecorder.FrameStore(
                width=w,
                height=h,
                fps=self._fps,
                jpeg_quality=90,
            )
            self._store.set_state(gifrecorder.STATE_RECORDING)
        except Exception as e:
            log_error(f"FrameStore 创建失败: {e}", "GIF")
            self._store = None
            return

        # 启动 Rust 截屏会话（Win32 BitBlt，独立 Rust 线程）
        try:
            self._session = gifrecorder.RecordSession(
                self._store, left, top, w, h, self._fps,
            )
        except Exception as e:
            log_error(f"RecordSession 启动失败: {e}", "GIF")
            self._store = None
            self._session = None
            return

        self._start_time = time.perf_counter()
        self._pause_offset = 0.0
        self._pause_start = 0.0

        self._state = RecordState.RECORDING
        self._timer.start(1000 // self._fps)
        self._start_scroll_listener()
        self.state_changed.emit(self._state.name)
        log_info(f"录制开始: {w}x{h} @ {self._fps}fps (Rust Win32 BitBlt)", "GIF")

    def pause(self):
        """暂停录制"""
        if self._state != RecordState.RECORDING:
            return
        self._timer.stop()
        self._pause_start = time.perf_counter()
        if self._session:
            self._session.pause()
        if self._store is not None:
            self._store.set_state(gifrecorder.STATE_PAUSED)
        self._state = RecordState.PAUSED
        self.state_changed.emit(self._state.name)

    def resume(self):
        """恢复录制"""
        if self._state != RecordState.PAUSED:
            return
        self._pause_offset += time.perf_counter() - self._pause_start
        self._pause_start = 0.0
        if self._session:
            self._session.resume()
        if self._store is not None:
            self._store.set_state(gifrecorder.STATE_RECORDING)
        self._state = RecordState.RECORDING
        self._timer.start(1000 // self._fps)
        self.state_changed.emit(self._state.name)

    def stop(self) -> List[FrameData]:
        """同步停止录制"""
        self._timer.stop()
        self._stop_session()
        if self._store is not None:
            self._store.set_state(gifrecorder.STATE_STOPPED)
        self._sync_frames_from_store()
        self._state = RecordState.STOPPED
        self.state_changed.emit(self._state.name)
        return self._frames

    def stop_async(self):
        """非阻塞停止录制。

        立即停止计时器，在 QThread 后台线程等待 Rust 截屏线程退出，
        完成后发射 ``stop_finished`` 信号。
        """
        if self._state not in (RecordState.RECORDING, RecordState.PAUSED):
            self.stop_finished.emit()
            return

        self._timer.stop()
        session = self._session
        self._session = None

        class _StopWorker(QObject):
            finished = Signal()
            def __init__(self, recorder, session):
                super().__init__()
                self._rec = recorder
                self._session = session
            def run(self):
                try:
                    if self._session is not None:
                        try:
                            self._session.stop()
                        except Exception as e:
                            log_error(f"RecordSession.stop 异常: {e}", "GIF")
                    if self._rec._store is not None:
                        self._rec._store.set_state(gifrecorder.STATE_STOPPED)
                    self._rec._sync_frames_from_store()
                    self._rec._state = RecordState.STOPPED
                except Exception as e:
                    log_error(f"StopWorker 异常: {e}", "GIF")
                    self._rec._state = RecordState.STOPPED
                finally:
                    self.finished.emit()

        self._stop_thread = QThread()
        self._stop_worker = _StopWorker(self, session)
        self._stop_worker.moveToThread(self._stop_thread)
        self._stop_thread.started.connect(self._stop_worker.run)
        self._stop_worker.finished.connect(self._on_stop_worker_done)
        self._stop_thread.start()

    def _on_stop_worker_done(self):
        """后台停止完成，主线程回调。"""
        self.state_changed.emit(self._state.name)
        self.stop_finished.emit()
        if self._stop_thread:
            self._stop_thread.quit()
            self._stop_thread.wait()
            self._stop_thread.deleteLater()
            self._stop_thread = None
        if self._stop_worker:
            self._stop_worker.deleteLater()
            self._stop_worker = None

    def reset(self):
        """重置录制器（主线程调用）。"""
        self._timer.stop()
        self._stop_session()

        if self._store is not None:
            self._store.clear()
        self._store = None

        self._frames.clear()
        self._state = RecordState.IDLE
        self.state_changed.emit(self._state.name)

    def reset_data(self):
        """仅释放数据（FrameStore + 帧列表），不碰 QTimer。

        适合在非主线程调用（如 close_all 后台清理线程）。
        """
        self._stop_session()

        if self._store is not None:
            self._store.clear()
        self._store = None

        self._frames.clear()
        self._state = RecordState.IDLE

    # ── 内部 ──

    def _stop_session(self):
        """停止 Rust 截屏会话（阻塞等待线程退出）。"""
        self._stop_scroll_listener()
        if self._session is not None:
            try:
                self._session.stop()
            except Exception as e:
                log_exception(e, "停止 Rust 截屏会话")
            self._session = None

    def _sync_frames_from_store(self):
        """从 FrameStore 同步帧时间戳到 _frames 列表，并绑定鼠标轨迹"""
        if self._store is None:
            return
        self._frames.clear()
        timestamps = self._store.frame_timestamps
        n_frames = len(timestamps)
        n_cursor = len(self._cursor_track)

        for i, ts in enumerate(timestamps):
            # 鼠标采样数可能和帧数不完全一致（QTimer vs Rust 线程），
            # 按比例映射或直接按索引对齐
            cursor = None
            if n_cursor > 0:
                ci = min(i, n_cursor - 1) if n_frames <= n_cursor else int(i * n_cursor / n_frames)
                cursor = self._cursor_track[ci]
            self._frames.append(FrameData(
                elapsed_ms=ts,
                width=self._rec_width,
                height=self._rec_height,
                cursor=cursor,
            ))

    def _tick(self):
        """QTimer 回调：通知 UI 帧数更新 + 采集鼠标状态"""
        count = self._store.frame_count if self._store else len(self._frames)

        # ── 采集鼠标位置和按键状态 ──
        self._sample_cursor()

        self.frame_captured.emit(count)

        # 超过最大时长 → 自动停止
        elapsed = time.perf_counter() - self._start_time - self._pause_offset
        if self._max_duration_s > 0 and elapsed >= self._max_duration_s:
            self._timer.stop()
            self.limit_reached.emit()

    # ── 鼠标采集 ──

    @staticmethod
    def _get_cursor_pos() -> Tuple[int, int]:
        """获取鼠标屏幕坐标（Win32 GetCursorPos）"""
        pt = _POINT()
        ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
        return pt.x, pt.y

    @staticmethod
    def _is_left_pressed() -> bool:
        """鼠标左键是否按下（Win32 GetAsyncKeyState）"""
        # VK_LBUTTON = 0x01
        return bool(ctypes.windll.user32.GetAsyncKeyState(0x01) & 0x8000)

    @staticmethod
    def _is_right_pressed() -> bool:
        """鼠标右键是否按下（Win32 GetAsyncKeyState）"""
        # VK_RBUTTON = 0x02
        return bool(ctypes.windll.user32.GetAsyncKeyState(0x02) & 0x8000)

    def _start_scroll_listener(self):
        """启动 pynput 滚轮监听（后台线程）"""
        try:
            from pynput import mouse

            def _on_scroll(x, y, dx, dy):
                # dy>0=向上, dy<0=向下
                if dy > 0:
                    self._scroll_value = 1
                elif dy < 0:
                    self._scroll_value = -1

            self._mouse_listener = mouse.Listener(on_scroll=_on_scroll)
            self._mouse_listener.start()
        except Exception as e:
            log_error(f"pynput 滚轮监听启动失败: {e}", "GIF")
            self._mouse_listener = None

    def _stop_scroll_listener(self):
        """停止 pynput 滚轮监听"""
        if self._mouse_listener is not None:
            try:
                self._mouse_listener.stop()
            except Exception as e:
                log_exception(e, "停止滚轮监听")
            self._mouse_listener = None
        self._scroll_value = 0

    def _sample_cursor(self):
        """采集一次鼠标快照，追加到 _cursor_track"""
        try:
            sx, sy = self._get_cursor_pos()
            rx = sx - self._rec_left
            ry = sy - self._rec_top
            visible = (0 <= rx < self._rec_width and 0 <= ry < self._rec_height)
            # 左键优先，右键次之
            if self._is_left_pressed():
                press = 1
            elif self._is_right_pressed():
                press = 2
            else:
                press = 0
            # 读取滚轮状态并立即清零（每帧只保留一次）
            scroll = self._scroll_value
            self._scroll_value = 0
            self._cursor_track.append(CursorSnapshot(
                x=rx, y=ry, press=press, visible=visible, scroll=scroll,
            ))
        except Exception as e:
            log_exception(e, "采集鼠标快照")
 