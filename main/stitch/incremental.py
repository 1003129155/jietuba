"""长截图的增量拼接。

UI 线程只截屏、提交帧；拼接在一个后台线程里按提交顺序进行，结论通过信号送回。
拼接结果留在 longstitch.StitchSession 里，每帧只回传拼接参数，预览缩略图在这里按参数增量维护。

帧的朝向处理与原先逐对拼接时一致：横向模式把帧顺时针转 90° 后按竖向拼接，
向上滚动把帧上下翻转后按向下拼接，导出时再还原。第一帧要等第二帧到来、方向定下后才推入会话。
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Optional

from PIL import Image
from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtGui import QImage, QTransform

# 原先逐对拼接时的匹配参数：向下滚动忽略长图顶部 15%（固定标题栏），
# 向上滚动（帧已翻转，标题栏到了底部）忽略长图底部 5% 帧高；横向模式标题栏已转到侧边，都不忽略。
_TOP_RATIO_DOWN = 0.15
_BOTTOM_RATIO_UP = 0.05


@dataclass(frozen=True)
class Frame:
    """一次截屏。bgra 为 width x height 的 BGRA 像素。"""
    bgra: bytes
    width: int
    height: int
    index: int
    scroll_direction: str
    locked: Optional[str]
    distance: int


@dataclass(frozen=True)
class FrameResult:
    """一帧的拼接结论。width/height 是拼接坐标下（尚未还原朝向）的结果尺寸。"""
    ok: bool
    first: bool
    index: int
    width: int
    height: int
    locked: Optional[str]
    distance: int
    preview: Optional[QImage]
    error: Optional[str] = None


def _match_ratios(direction: str, locked: Optional[str]) -> dict:
    if direction == "horizontal":
        return {}
    if locked == "up":
        return {"ignore_img1_bottom_ratio": _BOTTOM_RATIO_UP}
    return {"ignore_img1_top_ratio": _TOP_RATIO_DOWN}


def _as_rgba_image(bgra: bytes, width: int, height: int) -> Image.Image:
    return Image.frombuffer("RGBA", (width, height), bgra, "raw", "BGRA", 0, 1)


def _oriented(frame: Frame, direction: str, locked: Optional[str]) -> tuple[bytes, int, int]:
    """把帧转到拼接坐标下，返回 (bgra, 宽, 高)。"""
    if direction != "horizontal" and locked != "up":
        return frame.bgra, frame.width, frame.height
    image = _as_rgba_image(frame.bgra, frame.width, frame.height)
    if direction == "horizontal":
        image = image.rotate(-90, expand=True)
    if locked == "up":
        image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    return image.tobytes("raw", "BGRA"), image.width, image.height


def _flip_rows(data: bytes, row_bytes: int) -> bytes:
    rows = [data[i:i + row_bytes] for i in range(0, len(data), row_bytes)]
    return b"".join(reversed(rows))


class IncrementalStitcher(QObject):
    """后台按顺序拼接长截图帧。

    frame_done 在后台线程发出，连接时须用 QueuedConnection，槽在接收者所在线程执行。
    """

    frame_done = Signal(object)

    def __init__(self, thumb_side: int, ignore_right_pixels: int, ignore_top_pixels: int, parent=None):
        super().__init__(parent)
        self._thumb_side = thumb_side
        self._ignore_right_pixels = ignore_right_pixels
        self._ignore_top_pixels = ignore_top_pixels
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="long-stitch")
        # 以下状态只在后台线程里读写（close 在线程池停下后才碰）
        self._first: Optional[Frame] = None
        self._session = None
        self._direction = "vertical"
        self._locked: Optional[str] = None
        self._scale = 1.0
        self._thumb = bytearray()

    def submit(self, frame: Frame) -> None:
        self._executor.submit(self._process, frame)

    def export(self) -> Optional[Image.Image]:
        """等已提交的帧处理完，返回还原朝向后的完整长图；还没有帧时返回 None。"""
        return self._executor.submit(self._export).result()

    def close(self) -> None:
        """丢弃尚未处理的帧，释放拼接结果。"""
        self._executor.shutdown(wait=True, cancel_futures=True)
        if self._session is not None:
            self._session.close()
            self._session = None
        self._first = None
        self._thumb = bytearray()

    # ---- 以下在后台线程执行 ----

    def _process(self, frame: Frame) -> None:
        try:
            result = self._push(frame)
        except Exception as e:
            result = self._failure(frame, str(e) or type(e).__name__)
        self.frame_done.emit(result)

    def _push(self, frame: Frame) -> FrameResult:
        if self._first is None:
            self._first = frame
            return FrameResult(True, True, frame.index, frame.width, frame.height,
                               self._locked, frame.distance, self._first_preview(frame))
        if self._session is None:
            return self._start(frame)
        return self._append(frame)

    def _start(self, frame: Frame) -> FrameResult:
        import longstitch

        direction, locked = frame.scroll_direction, frame.locked
        first, width, height = _oriented(self._first, direction, locked)
        second, width2, height2 = _oriented(frame, direction, locked)
        if (width2, height2) != (width, height):
            return self._failure(frame, f"frame size {width2}x{height2} differs from {width}x{height}")

        session = longstitch.StitchSession(
            width, height,
            ignore_right_pixels=self._ignore_right_pixels,
            ignore_top_pixels=self._ignore_top_pixels,
        )
        session.push(first)
        if locked is None:
            step = session.push(second, detect_direction=True)
        else:
            step = session.push(second, **_match_ratios(direction, locked))
        if step is None:
            session.close()
            return self._failure(frame)

        reversed_ = step.direction == "reverse"
        if locked is None:
            locked = "up" if reversed_ else "down"
        self._session, self._direction, self._locked = session, direction, locked
        self._scale = self._thumb_side / width

        thumb_first = self._thumbnail(first, width, height)
        thumb_second = self._thumbnail(second, width, height)
        if reversed_:
            row_bytes = self._thumb_side * 4
            thumb_first = _flip_rows(thumb_first, row_bytes)
            thumb_second = _flip_rows(thumb_second, row_bytes)
        self._thumb = bytearray(thumb_first)
        self._apply_thumbnail(step.keep, step.height, thumb_second)
        return self._success(frame)

    def _append(self, frame: Frame) -> FrameResult:
        data, width, height = _oriented(frame, self._direction, self._locked)
        step = self._session.push(data, **_match_ratios(self._direction, self._locked))
        if step is None:
            return self._failure(frame)
        self._apply_thumbnail(step.keep, step.height, self._thumbnail(data, width, height))
        return self._success(frame)

    def _export(self) -> Optional[Image.Image]:
        if self._session is None:
            if self._first is None:
                return None
            first = self._first
            return _as_rgba_image(first.bgra, first.width, first.height).convert("RGB")
        image = _as_rgba_image(self._session.export(), self._session.width, self._session.height)
        if self._locked == "up":
            image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if self._direction == "horizontal":
            image = image.rotate(90, expand=True)
        return image

    def _thumbnail(self, bgra: bytes, width: int, height: int) -> bytes:
        side = self._thumb_side
        thumb_height = max(1, round(height * side / width))
        source = QImage(bgra, width, height, width * 4, QImage.Format.Format_RGB32)
        thumb = source.scaled(side, thumb_height, Qt.AspectRatioMode.IgnoreAspectRatio,
                              Qt.TransformationMode.SmoothTransformation)
        thumb = thumb.convertToFormat(QImage.Format.Format_RGB32)
        return bytes(thumb.constBits())

    def _apply_thumbnail(self, keep: int, height: int, frame_thumb: bytes) -> None:
        """缩略图跟着结果截到 keep 行，再补上新帧底部；height 为推入后结果的总行数。

        补多少行由总高换算，而不是由各帧分别换算：分别四舍五入的误差会逐帧累积。
        """
        row_bytes = self._thumb_side * 4
        kept = min(round(keep * self._scale), len(self._thumb) // row_bytes)
        added = round(height * self._scale) - kept
        del self._thumb[kept * row_bytes:]
        if added > 0:
            self._thumb += frame_thumb[-added * row_bytes:]

    def _first_preview(self, frame: Frame) -> QImage:
        """第一帧还没定朝向，直接缩原图；横向模式的面板按高度对齐，按高度缩。"""
        source = QImage(frame.bgra, frame.width, frame.height, frame.width * 4, QImage.Format.Format_RGB32)
        mode = Qt.TransformationMode.SmoothTransformation
        if frame.scroll_direction == "horizontal":
            thumb = source.scaledToHeight(self._thumb_side, mode)
        else:
            thumb = source.scaledToWidth(self._thumb_side, mode)
        return thumb.convertToFormat(QImage.Format.Format_RGB32)

    def _render(self, thumb: bytes) -> QImage:
        side = self._thumb_side
        rows = len(thumb) // (side * 4)
        image = QImage(bytes(thumb), side, rows, side * 4, QImage.Format.Format_RGB32).copy()
        if self._locked == "up":
            image = image.flipped(Qt.Orientation.Vertical)
        if self._direction == "horizontal":
            image = image.transformed(QTransform().rotate(-90))
        return image

    def _success(self, frame: Frame) -> FrameResult:
        return FrameResult(True, False, frame.index, self._session.width, self._session.height,
                           self._locked, frame.distance, self._render(self._thumb))

    def _failure(self, frame: Frame, error: Optional[str] = None) -> FrameResult:
        return FrameResult(False, False, frame.index, 0, 0, self._locked, frame.distance, None, error)
