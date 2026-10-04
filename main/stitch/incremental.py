"""长截图的增量拼接。

UI 线程只截屏、提交帧；拼接在一个后台线程里按提交顺序进行，结论通过信号送回。
拼接结果留在 longstitch.StitchSession 里，每帧只回传拼接参数和这一帧所在的位置，
预览缩略图在这里按参数增量维护。帧落在已拼内容之内时结果不变；伸出开头或末尾就在那一头接上，
所以从页面中间开始截，往上往下都能长。

竖向按页面本来的方向拼接；横向模式把帧顺时针转 90° 后按竖向拼接，导出时再转回。
第一帧要等第二帧到来才推入会话：两帧之间可能切换过横竖模式。
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Optional

from PIL import Image
from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtGui import QImage, QTransform

# 竖向时匹配避开长图顶部的固定标题栏：往下滚时它只在最初那一帧，避开帧高的 15%；
# 往上滚时顶部正是往上长的那一头，重叠本来就不长，只避开 5%。横向模式标题栏已转到侧边，不避开。
_TOP_RATIO_DOWN = 0.15
_TOP_RATIO_UP = 0.05


@dataclass(frozen=True)
class Frame:
    """一次截屏。bgra 为 width x height 的 BGRA 像素。

    heading 是截这一帧之前的滚动方向（拼接坐标下："down" 往下或往右，"up" 往上或往左），
    不知道时为 None，比如点了手动截图。
    """
    bgra: bytes
    width: int
    height: int
    index: int
    scroll_direction: str
    heading: Optional[str]
    distance: int


@dataclass(frozen=True)
class FrameResult:
    """一帧的拼接结论。width/height/top 是拼接坐标下（尚未还原朝向）的结果尺寸和这一帧的起始行；
    shift 是这次推入让已有内容整体下移的行数（伸出开头时为正），上一帧的起始行加上它才能和 top 比；
    box 是这一帧在预览图长边上的 [起, 止) 像素范围，size 是长图的 (宽, 高)，这两项都已还原朝向。"""
    ok: bool
    first: bool
    index: int
    width: int
    height: int
    distance: int
    preview: Optional[QImage]
    error: Optional[str] = None
    top: int = 0
    box: Optional[tuple[int, int]] = None
    shift: int = 0
    size: Optional[tuple[int, int]] = None


def _match_ratios(direction: str, heading: Optional[str]) -> dict:
    if direction == "horizontal":
        return {}
    return {"ignore_img1_top_ratio": _TOP_RATIO_UP if heading == "up" else _TOP_RATIO_DOWN}


def _push_frame(session, data: bytes, direction: str, heading: Optional[str]):
    """推入一帧。不知道滚动方向时（手动截图）按会话最近一次移动的方向，忽略比例也跟着它选。"""
    heading = heading or session.last_move
    return session.push(data, heading=heading, **_match_ratios(direction, heading))


def _as_rgba_image(bgra: bytes, width: int, height: int) -> Image.Image:
    return Image.frombuffer("RGBA", (width, height), bgra, "raw", "BGRA", 0, 1)


def _oriented(frame: Frame, direction: str) -> tuple[bytes, int, int]:
    """把帧转到拼接坐标下，返回 (bgra, 宽, 高)。"""
    if direction != "horizontal":
        return frame.bgra, frame.width, frame.height
    image = _as_rgba_image(frame.bgra, frame.width, frame.height).rotate(-90, expand=True)
    return image.tobytes("raw", "BGRA"), image.width, image.height


class IncrementalStitcher(QObject):
    """后台按顺序拼接长截图帧。

    frame_done、crop_done 在后台线程发出，连接时须用 QueuedConnection，槽在接收者所在线程执行。
    """

    frame_done = Signal(object)
    crop_done = Signal(object)  # 裁剪后的 FrameResult，index 为 0

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
        self._scale = 1.0
        self._frame_rows = 0
        self._top = 0
        self._thumb = bytearray()

    def submit(self, frame: Frame) -> None:
        self._executor.submit(self._process, frame)

    def crop(self, side: str) -> None:
        """把长图裁到最新一帧：side 为 "top" 去掉它上方的内容，"bottom" 去掉它下方的内容。

        排在已提交的帧之后执行，结论经 crop_done 送回；还只有第一帧时没有可裁的，什么也不做。
        """
        self._executor.submit(self._crop, side)

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

    def _crop(self, side: str) -> None:
        if self._session is None:
            return
        try:
            result = self._apply_crop(side)
        except Exception as e:
            result = FrameResult(False, False, 0, 0, 0, 0, None, str(e) or type(e).__name__)
        self.crop_done.emit(result)

    def _apply_crop(self, side: str) -> FrameResult:
        """缩略图跟着裁：行数仍由裁剪后的总高换算，与 _apply_step 一致。"""
        session = self._session
        row_bytes = self._thumb_side * 4
        rows = len(self._thumb) // row_bytes
        if side == "top":
            shift = -session.crop_top()
            self._top = 0
        else:
            session.crop_bottom()
            shift = 0
        target = round(session.height * self._scale)
        if side == "top":
            del self._thumb[:max(0, rows - target) * row_bytes]
        else:
            del self._thumb[target * row_bytes:]
        return FrameResult(True, False, 0, session.width, session.height, 0, self._render(self._thumb),
                           top=self._top, box=self._box(self._top), shift=shift, size=self._size())

    def _push(self, frame: Frame) -> FrameResult:
        if self._first is None:
            self._first = frame
            preview = self._first_preview(frame)
            long_side = preview.width() if frame.scroll_direction == "horizontal" else preview.height()
            return FrameResult(True, True, frame.index, frame.width, frame.height,
                               frame.distance, preview, box=(0, long_side), size=(frame.width, frame.height))
        if self._session is None:
            return self._start(frame)
        return self._append(frame)

    def _start(self, frame: Frame) -> FrameResult:
        import longstitch

        direction = frame.scroll_direction
        first, width, height = _oriented(self._first, direction)
        second, width2, height2 = _oriented(frame, direction)
        if (width2, height2) != (width, height):
            return self._failure(frame, f"frame size {width2}x{height2} differs from {width}x{height}")

        session = longstitch.StitchSession(
            width, height,
            ignore_right_pixels=self._ignore_right_pixels,
            ignore_top_pixels=self._ignore_top_pixels,
        )
        session.push(first)
        step = _push_frame(session, second, direction, frame.heading)
        if step is None:
            session.close()
            return self._failure(frame)

        self._session, self._direction = session, direction
        self._scale = self._thumb_side / width
        self._frame_rows = height
        self._thumb = bytearray(self._thumbnail(first, width, height))
        self._apply_step(step, self._thumbnail(second, width, height))
        return self._success(frame, step)

    def _append(self, frame: Frame) -> FrameResult:
        data, width, height = _oriented(frame, self._direction)
        step = _push_frame(self._session, data, self._direction, frame.heading)
        if step is None:
            return self._failure(frame)
        self._apply_step(step, self._thumbnail(data, width, height))
        return self._success(frame, step)

    def _export(self) -> Optional[Image.Image]:
        if self._session is None:
            if self._first is None:
                return None
            first = self._first
            return _as_rgba_image(first.bgra, first.width, first.height).convert("RGB")
        image = _as_rgba_image(self._session.export(), self._session.width, self._session.height)
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

    def _apply_step(self, step, frame_thumb: bytes) -> None:
        """缩略图跟着结果变：伸出开头时去掉顶部再补上新帧的上部，伸出末尾时截掉末尾再补上新帧的下部。

        补多少行由推入后的总高换算，而不是由各帧分别换算：分别四舍五入的误差会逐帧累积。
        """
        row_bytes = self._thumb_side * 4
        rows = len(self._thumb) // row_bytes
        target = round(step.height * self._scale)
        if step.head_cut or step.head_rows:
            cut = min(round(step.head_cut * self._scale), rows)
            added = target - (rows - cut)
            self._thumb = bytearray(frame_thumb[:max(0, added) * row_bytes]) + self._thumb[cut * row_bytes:]
            return
        kept = min(round(step.keep * self._scale), rows)
        added = target - kept
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
        if self._direction == "horizontal":
            image = image.transformed(QTransform().rotate(-90))
        return image

    def _box(self, top: int) -> tuple[int, int]:
        """这一帧在预览图长边上的范围。横向预览由拼接坐标逆时针转回，行号直接变成横坐标。"""
        rows = len(self._thumb) // (self._thumb_side * 4)
        start = min(rows, round(top * self._scale))
        end = min(rows, round((top + self._frame_rows) * self._scale))
        return start, end

    def _success(self, frame: Frame, step) -> FrameResult:
        self._top = step.top
        return FrameResult(True, False, frame.index, self._session.width, self._session.height,
                           frame.distance, self._render(self._thumb), top=step.top, box=self._box(step.top),
                           shift=step.head_rows - step.head_cut, size=self._size())

    def _size(self) -> tuple[int, int]:
        width, height = self._session.width, self._session.height
        return (height, width) if self._direction == "horizontal" else (width, height)

    def _failure(self, frame: Frame, error: Optional[str] = None) -> FrameResult:
        return FrameResult(False, False, frame.index, 0, 0, frame.distance, None, error)
