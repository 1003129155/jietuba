"""长截图增量拼接：IncrementalStitcher 与原先逐对整图拼接的结果逐像素一致。

参照实现 _pairwise_flow 照搬改造前 ScrollCaptureWindow._do_capture 的帧处理：
每帧把已拼长图和新帧整张交给 stitch_images / stitch_images_auto。
"""

import ctypes
import random
import threading
import time
from ctypes import wintypes

import pytest
from PIL import Image, ImageDraw
from PySide6.QtCore import QObject, Qt, Slot

longstitch = pytest.importorskip("longstitch", reason="需要安装自制的 longstitch Rust 扩展包")

from stitch.incremental import Frame, IncrementalStitcher  # noqa: E402
from stitch.jietuba_long_stitch_unified import stitch_images, stitch_images_auto  # noqa: E402

W, H = 320, 240
THUMB = 190


def _page(height, width=W, seed=1):
    """白底上逐行长短不一的色块，近似一页文字，每行内容都不同。"""
    rnd = random.Random(seed)
    image = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(image)
    y = 8
    while y < height - 24:
        line_height = rnd.choice((8, 10, 12))
        color = tuple(rnd.randint(0, 120) for _ in range(3))
        for row in range(y, y + line_height):
            x = 12
            while x < width - 40:
                length = rnd.randint(2, 30)
                if rnd.random() < 0.6:
                    draw.line((x, row, x + length, row), fill=color)
                x += length + rnd.randint(1, 6)
        y += line_height + rnd.randint(4, 16)
    return image


def _vertical_frames(page, start, step, count):
    frames, y = [], start
    for _ in range(count):
        y = max(0, min(y, page.height - H))
        frames.append(page.crop((0, y, W, y + H)))
        if (step > 0 and y >= page.height - H) or (step < 0 and y <= 0):
            break
        y += step
    return frames


def _horizontal_frames(page, step, count):
    frames, x = [], 0
    for _ in range(count):
        x = min(x, page.width - W)
        frames.append(page.crop((x, 0, x + W, H)))
        if x >= page.width - W:
            break
        x += step
    return frames


def _with_footer(frames):
    """每帧底部同一条固定底栏：拼接时长图末尾的旧底栏会被截掉。"""
    out = []
    for frame in frames:
        frame = frame.copy()
        draw = ImageDraw.Draw(frame)
        draw.rectangle((0, H - 24, W, H), fill=(40, 70, 140))
        draw.rectangle((10, H - 18, 120, H - 8), fill=(230, 230, 230))
        out.append(frame)
    return out


def _noise():
    rnd = random.Random(99)
    image = Image.new("RGB", (W, H))
    image.putdata([tuple(rnd.randrange(256) for _ in range(3)) for _ in range(W * H)])
    return image


def _pairwise_flow(frames, direction, lock):
    """改造前的逐对拼接：lock 是第二帧到来前由滚轮锁定的方向，None 表示首对自动判向。"""
    stitched, count, locked = None, 0, None
    for i, frame in enumerate(frames):
        if i == 1:
            locked = lock
        count += 1
        image = frame.convert("RGB")
        if direction == "horizontal" and count > 1:
            image = image.rotate(-90, expand=True)
        if locked == "up":
            image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if stitched is None:
            stitched = image
            continue
        if direction == "horizontal" and count == 2:
            stitched = stitched.rotate(-90, expand=True)
        if locked == "up" and count == 2:
            stitched = stitched.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if locked is None and count == 2:
            result, found = stitch_images_auto(stitched, image)
            if result is not None:
                locked = "up" if found == "reverse" else "down"
        else:
            top = bottom = 0.0
            if direction != "horizontal":
                if locked == "up":
                    bottom = 0.05
                else:
                    top = 0.15
            result = stitch_images([stitched, image], ignore_img1_top_ratio=top, ignore_img1_bottom_ratio=bottom)
        if result is None:
            count -= 1
        else:
            stitched = result
    if count >= 2:
        if locked == "up":
            stitched = stitched.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        if direction == "horizontal":
            stitched = stitched.rotate(90, expand=True)
    return stitched


class _Collector(QObject):
    def __init__(self):
        super().__init__()
        self.results = []
        self.threads = []

    @Slot(object)
    def collect(self, result):
        self.results.append(result)
        self.threads.append(threading.get_ident())


def _stitcher(collector):
    stitcher = IncrementalStitcher(thumb_side=THUMB, ignore_right_pixels=20, ignore_top_pixels=0)
    stitcher.frame_done.connect(collector.collect, Qt.ConnectionType.QueuedConnection)
    return stitcher


def _submit_all(stitcher, frames, direction, lock):
    locked = None
    for i, frame in enumerate(frames):
        if i == 1:
            locked = lock
        bgra = frame.convert("RGBA").tobytes("raw", "BGRA")
        stitcher.submit(Frame(bgra, frame.width, frame.height, i + 1, direction, locked, 0))


def _wait_for(qapp, collector, count, timeout=20.0):
    deadline = time.monotonic() + timeout
    while len(collector.results) < count and time.monotonic() < deadline:
        qapp.processEvents()
        time.sleep(0.001)
    assert len(collector.results) == count


TALL = _page(2400)
WIDE = _page(2400, width=H, seed=5).rotate(90, expand=True)

CASES = {
    "down_locked": (_vertical_frames(TALL, 0, 90, 40), "vertical", "down"),
    "down_auto": (_vertical_frames(TALL, 0, 90, 40), "vertical", None),
    "up_locked": (_vertical_frames(TALL, 2160, -90, 40), "vertical", "up"),
    "up_auto": (_vertical_frames(TALL, 2160, -90, 40), "vertical", None),
    "right_locked": (_horizontal_frames(WIDE, 90, 40), "horizontal", "down"),
    "left_locked": (list(reversed(_horizontal_frames(WIDE, 90, 40))), "horizontal", "up"),
    "down_footer": (_with_footer(_vertical_frames(TALL, 0, 90, 40)), "vertical", "down"),
    # 滚到 990 后往回滚两步再继续：回滚那两帧会让结果截短
    "down_rollback": ([TALL.crop((0, y, W, y + H)) for y in [*range(0, 991, 90), 900, 810, *range(900, 2161, 90)]],
                      "vertical", "down"),
}


@pytest.mark.parametrize("case", sorted(CASES))
def test_export_matches_pairwise_stitching(qapp, case):
    frames, direction, lock = CASES[case]
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit_all(stitcher, frames, direction, lock)
        exported = stitcher.export()
    finally:
        stitcher.close()

    expected = _pairwise_flow(frames, direction, lock)
    assert exported.size == expected.size
    assert exported.convert("RGB").tobytes() == expected.convert("RGB").tobytes()


@pytest.mark.parametrize("lock", ["down", "up"])
def test_long_capture_keeps_every_row(qapp, lock):
    """与原页面逐像素比对。重叠 150 行，长图超过 3000 行后底部忽略若随长度增长就会盖住它。"""
    page = _page(6000, seed=8)
    start, step = (0, 90) if lock == "down" else (6000 - H, -90)
    frames = _vertical_frames(page, start, step, 80)
    stitcher = _stitcher(_Collector())
    try:
        _submit_all(stitcher, frames, "vertical", lock)
        exported = stitcher.export()
    finally:
        stitcher.close()
    assert exported.convert("RGB").tobytes() == page.tobytes()


def test_single_frame_exports_unchanged(qapp):
    frame = TALL.crop((0, 0, W, H))
    stitcher = _stitcher(_Collector())
    try:
        _submit_all(stitcher, [frame], "horizontal", None)
        exported = stitcher.export()
    finally:
        stitcher.close()
    assert exported.mode == "RGB"
    assert exported.tobytes() == frame.tobytes()


def test_unmatched_frame_is_reported_and_skipped(qapp):
    frames = _vertical_frames(TALL, 0, 90, 12)
    frames.insert(6, _noise())
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit_all(stitcher, frames, "vertical", "down")
        _wait_for(qapp, collector, len(frames))
        exported = stitcher.export()
    finally:
        stitcher.close()

    oks = [r.ok for r in collector.results]
    assert oks == [True] * 6 + [False] + [True] * (len(frames) - 7)
    assert collector.results[6].error is None
    assert exported.tobytes() == _pairwise_flow(frames, "vertical", "down").tobytes()


def test_results_arrive_in_order_on_the_ui_thread(qapp):
    frames = _vertical_frames(TALL, 0, 90, 20)
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit_all(stitcher, frames, "vertical", "down")
        _wait_for(qapp, collector, len(frames))
    finally:
        stitcher.close()
    assert [r.index for r in collector.results] == list(range(1, len(frames) + 1))
    assert set(collector.threads) == {threading.get_ident()}
    assert collector.results[0].first and not any(r.first for r in collector.results[1:])


def test_auto_detected_direction_is_reported(qapp):
    frames = _vertical_frames(TALL, 2160, -90, 6)
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit_all(stitcher, frames, "vertical", None)
        _wait_for(qapp, collector, len(frames))
    finally:
        stitcher.close()
    assert collector.results[0].locked is None
    assert all(r.locked == "up" for r in collector.results[1:])


@pytest.mark.parametrize("case", ["down_locked", "up_locked", "right_locked", "down_footer", "down_rollback"])
def test_preview_follows_the_result(qapp, case):
    """每一帧的缩略图长边都等于结果高度按比例换算，截短（固定底栏、回滚）时同步截短。"""
    frames, direction, lock = CASES[case]
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit_all(stitcher, frames, direction, lock)
        _wait_for(qapp, collector, len(frames))
    finally:
        stitcher.close()

    for result in collector.results:
        preview = result.preview
        short_side, long_side = (preview.width(), preview.height()) if direction == "vertical"             else (preview.height(), preview.width())
        assert short_side == THUMB
        if result.first:
            # 第一帧尚未转到拼接坐标，尺寸是原图的，由 Qt 缩放取整
            frame_long, frame_short = (result.height, result.width) if direction == "vertical"                 else (result.width, result.height)
            assert abs(long_side - frame_long * THUMB / frame_short) <= 0.5
        else:
            assert long_side == round(result.height * THUMB / result.width)
    heights = [r.height for r in collector.results]
    if case == "down_rollback":
        assert any(b < a for a, b in zip(heights, heights[1:]))


def test_export_waits_for_frames_still_queued(qapp):
    frames = _vertical_frames(TALL, 0, 90, 30)
    stitcher = _stitcher(_Collector())
    try:
        _submit_all(stitcher, frames, "vertical", "down")
        exported = stitcher.export()  # 不处理事件，直接导出
    finally:
        stitcher.close()
    assert exported.tobytes() == _pairwise_flow(frames, "vertical", "down").tobytes()


def _private_bytes():
    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                    ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                    ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                    ("PrivateUsage", ctypes.c_size_t)]
    counters = Counters()
    counters.cb = ctypes.sizeof(Counters)
    get_info = ctypes.windll.psapi.GetProcessMemoryInfo
    get_info.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    assert get_info(ctypes.windll.kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb)
    return counters.PrivateUsage


def test_close_releases_worker_and_result(qapp):
    """两个独立判据：后台线程退出；拼接结果的内存还给系统（关闭后仍持有 stitcher 对象）。"""
    big_w, big_h = 1600, 1200
    page = _page(big_h * 6, width=big_w, seed=3)
    frames = [page.crop((0, y, big_w, y + big_h)).convert("RGBA").tobytes("raw", "BGRA")
              for y in range(0, page.height - big_h + 1, 600)]
    threads_before = {t.ident for t in threading.enumerate()}

    kept = []
    baseline = None
    for round_ in range(4):
        stitcher = IncrementalStitcher(thumb_side=THUMB, ignore_right_pixels=20, ignore_top_pixels=0)
        for i, bgra in enumerate(frames):
            stitcher.submit(Frame(bgra, big_w, big_h, i + 1, "vertical", "down", 0))
        assert stitcher.export().height > big_h * 4
        stitcher.close()
        kept.append(stitcher)
        if round_ == 0:
            baseline = _private_bytes()

    leaked_threads = {t.ident for t in threading.enumerate()} - threads_before
    assert not leaked_threads
    # 每轮的拼接结果约 46MB；后三轮都不释放就会多出约 138MB
    assert _private_bytes() - baseline < 40 * 2**20
