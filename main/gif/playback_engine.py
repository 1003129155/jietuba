# -*- coding: utf-8 -*-
"""回放引擎 — gifrecorder (Rust) 驱动预览，支持倍速 / 节选范围 / 循环

架构：
  播放时启动 FrameStore.start_decoder()（Rust 后台线程预解码 JPEG → RGB24）。
  QTimer 每 tick 取一帧 → 构造 QImage → 发送 frame_ready 信号。

  优势：
    - 解码在 Rust 侧完成，零 GIL 争用
    - 后台预解码缓冲，轻松达到 30fps+
    - seek / trim / speed 变化时重启解码器
"""

from enum import Enum, auto
from typing import List, Optional

from PySide6.QtCore import QObject, QTimer, Signal
from PySide6.QtGui import QImage

from .frame_recorder import FrameData
from core.logger import log_debug, log_error, log_info

try:
    import gifrecorder
except ImportError:
    gifrecorder = None


class PlayState(Enum):
    IDLE    = auto()
    PLAYING = auto()
    PAUSED  = auto()


class PlaybackEngine(QObject):
    """
    gifrecorder.FrameDecoder 驱动的回放引擎。

    信号:
        frame_ready(QImage, int)  — 新帧图像 + 对应帧索引
        frame_changed(int)        — 兼容旧连接：只发索引
        playback_finished()       — 播放到 trim_end
    """

    frame_ready       = Signal(QImage, int)
    frame_changed     = Signal(int)
    playback_finished = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._frames: List[FrameData] = []
        self._store = None             # gifrecorder.FrameStore
        self._fps: int = 16
        self._speed: float = 1.0
        self._current: int = 0
        self._trim_start: int = 0
        self._trim_end: int = 0
        self._display_w: int = 0
        self._display_h: int = 0
        self._state = PlayState.IDLE

        self._decoder = None           # gifrecorder.FrameDecoder

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._advance)

    # ══════════════════════════════════════════════
    # 公开方法
    # ══════════════════════════════════════════════

    def load(self, frames: List[FrameData], fps: int, store=None):
        """加载帧列表和 FrameStore。

        Args:
            frames: 帧元数据列表
            fps: 帧率
            store: gifrecorder.FrameStore 实例
        """
        self._stop_decoder()
        self._timer.stop()
        self._frames = frames
        self._fps = fps
        self._store = store
        self._current = 0
        self._trim_start = 0
        self._trim_end = max(0, len(frames) - 1)
        self._state = PlayState.IDLE

    def set_display_size(self, width: int, height: int):
        changed = (self._display_w != width or self._display_h != height)
        self._display_w = width
        self._display_h = height
        if changed and self._state == PlayState.PLAYING:
            self._restart_decoder_from(self._current)

    @property
    def state(self) -> PlayState:
        """当前播放状态"""
        return self._state

    @property
    def is_playing(self) -> bool:
        """是否正在播放"""
        return self._state == PlayState.PLAYING

    @property
    def frame_count(self) -> int:
        return len(self._frames)

    @property
    def current_index(self) -> int:
        return self._current

    @property
    def trim_start(self) -> int:
        """裁剪起始帧索引"""
        return self._trim_start

    @property
    def trim_end(self) -> int:
        """裁剪结束帧索引"""
        return self._trim_end

    def play(self):
        if not self._frames or self._store is None:
            return
        if self._state == PlayState.PLAYING:
            return
        self._state = PlayState.PLAYING
        if self._decoder is None:
            self._restart_decoder_from(self._current)
        self._timer.start(self._interval_ms())

    def pause(self):
        self._timer.stop()
        self._state = PlayState.PAUSED

    def stop(self):
        self._timer.stop()
        self._stop_decoder()
        self._state = PlayState.IDLE

    def stop_timer(self):
        """仅停止 QTimer（必须在主线程调用），供 close_all 在启动后台线程前调用。"""
        self._timer.stop()

    def cleanup(self, blocking: bool = False):
        """彻底释放 Rust 资源。QTimer 应已由主线程通过 stop_timer() 停止。"""
        # 注意：此方法可能从非 Qt 线程调用，不可再操作 QTimer
        self._stop_decoder()
        self._frames = []
        self._store = None
        self._state = PlayState.IDLE
        log_debug("PlaybackEngine cleanup 完成", "PlaybackEngine")

    def seek(self, index: int):
        index = max(self._trim_start, min(index, self._trim_end))
        self._current = index
        if self._state == PlayState.PLAYING:
            self._restart_decoder_from(index)
        else:
            self._stop_decoder()
        self.frame_changed.emit(index)

    def set_speed(self, speed: float):
        speed = max(0.1, speed)
        if self._speed == speed:
            return
        self._speed = speed
        self._timer.setInterval(self._interval_ms())
        if self._state == PlayState.PLAYING:
            self._restart_decoder_from(self._current)

    def set_trim(self, start: int, end: int):
        self._trim_start = max(0, start)
        self._trim_end = min(len(self._frames) - 1, end)
        if self._current < self._trim_start:
            self.seek(self._trim_start)
        elif self._current > self._trim_end:
            self.seek(self._trim_end)
        if self._state == PlayState.PLAYING:
            self._restart_decoder_from(self._current)

    def get_frame(self, index: int) -> FrameData:
        return self._frames[index]

    def get_frame_image(self, index: int, display_width: int, display_height: int) -> Optional[QImage]:
        """单帧按需解码，用于 seek 预览 / 首帧展示。"""
        if not self._frames or index >= len(self._frames):
            return None
        if self._store is None:
            return None
        try:
            rgb = self._store.get_frame_rgb(index, display_width, display_height)
        except Exception as e:
            log_error(f"get_frame_image 失败 idx={index}: {e}", "PlaybackEngine")
            return None
        # .copy() 深拷贝像素到 QImage 内部，rgb 可安全释放
        return QImage(
            rgb, display_width, display_height,
            display_width * 3,
            QImage.Format.Format_RGB888,
        ).copy()

    # ══════════════════════════════════════════════
    # 内部
    # ══════════════════════════════════════════════

    def _interval_ms(self) -> int:
        return max(10, int(1000 / self._fps / self._speed))

    def _stop_decoder(self):
        if self._decoder is not None:
            self._decoder.stop()
            self._decoder = None

    def _restart_decoder_from(self, index: int):
        self._stop_decoder()
        if self._store is None or not self._frames:
            return
        w = self._display_w or (self._frames[0].width if self._frames else 640)
        h = self._display_h or (self._frames[0].height if self._frames else 480)
        if w <= 0 or h <= 0:
            return

        # 创建 Rust 后台解码器（prefetch=6 帧缓冲）
        self._decoder = self._store.start_decoder(
            display_w=w, display_h=h, prefetch=6
        )
        self._current = index

        # 使用 Rust 侧 skip()：GIL 释放、无 PyBytes 分配，不阻塞主线程事件循环
        # （原先用 Python 层 next_frame() 循环：每帧 ~MB 数据拷贝 × N 帧，大帧号 seek 会显著卡顿）
        if index > 0:
            self._decoder.skip(index)

    def _advance(self):
        """QTimer tick：从解码器取一帧，构造 QImage，更新 UI。"""
        if self._decoder is None:
            return

        result = self._decoder.try_next_frame()
        if result is None:
            # 解码器可能暂时为空或已完成
            if self._decoder.is_finished:
                self._stop_decoder()
                self._timer.stop()
                self._state = PlayState.IDLE
                self.playback_finished.emit()
            return

        rgb_data, elapsed_ms = result
        w = self._display_w
        h = self._display_h
        # QImage 外部 buffer 构造不拷贝数据，必须确保 buffer 存活。
        # 调用 .copy() 让 QImage 深拷贝像素，之后 rgb_data 可安全释放。
        img = QImage(
            rgb_data,
            w, h,
            w * 3,
            QImage.Format.Format_RGB888,
        ).copy()

        self.frame_ready.emit(img, self._current)
        self.frame_changed.emit(self._current)

        nxt = self._current + 1
        if nxt > self._trim_end:
            self._stop_decoder()
            self._timer.stop()
            self._state = PlayState.IDLE
            self.playback_finished.emit()
            return
        self._current = nxt
 