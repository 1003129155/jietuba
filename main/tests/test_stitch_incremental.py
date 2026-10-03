"""长截图增量拼接：IncrementalStitcher 拼出的长图与页面真值一致，往下滚时也与原先逐对整图拼接一致。

参照实现 _pairwise_flow 照搬改造前 ScrollCaptureWindow._do_capture 的向下滚动：
每帧把已拼长图和新帧整张交给 stitch_images。
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
from stitch.jietuba_long_stitch_unified import stitch_images  # noqa: E402

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


def _sweep(start, stop, step):
    """从 start 每次挪 step 到 stop，最后一步停在 stop 上。"""
    tops = list(range(start, stop, step))
    return tops + [stop]


def _headings(tops):
    """相邻两帧的先后就是这一帧之前的滚动方向，第一帧没有方向。"""
    headings = [None]
    for prev, top in zip(tops, tops[1:]):
        headings.append("down" if top > prev else ("up" if top < prev else None))
    return headings


def _frames(page, tops, direction):
    if direction == "vertical":
        return [page.crop((0, t, W, t + H)) for t in tops]
    return [page.crop((t, 0, t + W, H)) for t in tops]


def _truth(page, tops, direction):
    lo, hi = min(tops), max(tops)
    if direction == "vertical":
        return page.crop((0, lo, W, hi + H))
    return page.crop((lo, 0, hi + W, H))


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


def _pairwise_flow(frames):
    """改造前的向下滚动：每帧把已拼长图和新帧整张交给 stitch_images。"""
    stitched = frames[0].convert("RGB")
    for frame in frames[1:]:
        result = stitch_images([stitched, frame.convert("RGB")], ignore_img1_top_ratio=0.15)
        if result is not None:
            stitched = result
    return stitched.convert("RGB")


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


def _submit(stitcher, frames, direction, headings):
    for i, (frame, heading) in enumerate(zip(frames, headings)):
        bgra = frame.convert("RGBA").tobytes("raw", "BGRA")
        stitcher.submit(Frame(bgra, frame.width, frame.height, i + 1, direction, heading, 0))


def _wait_for(qapp, collector, count, timeout=20.0):
    deadline = time.monotonic() + timeout
    while len(collector.results) < count and time.monotonic() < deadline:
        qapp.processEvents()
        time.sleep(0.001)
    assert len(collector.results) == count


def _run(qapp, frames, direction, headings):
    """推入全部帧，返回 (逐帧结论, 导出的长图)。"""
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit(stitcher, frames, direction, headings)
        _wait_for(qapp, collector, len(frames))
        exported = stitcher.export()
    finally:
        stitcher.close()
    return collector.results, exported


TALL = _page(2400)
WIDE = _page(2400, width=H, seed=5).rotate(90, expand=True)

# (页面, 每帧在页面上的位置, 横竖)
CASES = {
    "down": (TALL, _sweep(0, 2160, 90), "vertical"),
    "up": (TALL, _sweep(2160, 0, -90), "vertical"),
    "right": (WIDE, _sweep(0, 2080, 90), "horizontal"),
    "left": (WIDE, _sweep(2080, 0, -90), "horizontal"),
    "rollback": (TALL, [*range(0, 991, 90), 900, 810, *range(900, 2161, 90)], "vertical"),
    # 从页面中间开始，越过起点后往另一头长
    "past_start_up": (TALL, [1200, 1290, 1380, 1290, 1200, 1110, 1020, 930, 840], "vertical"),
    "past_start_down": (TALL, [1200, 1110, 1020, 1110, 1200, 1290, 1380, 1470, 1560], "vertical"),
}


@pytest.mark.parametrize("case", sorted(CASES))
def test_export_matches_the_page(qapp, case):
    page, tops, direction = CASES[case]
    _, exported = _run(qapp, _frames(page, tops, direction), direction, _headings(tops))
    expected = _truth(page, tops, direction)
    assert exported.size == expected.size
    assert exported.convert("RGB").tobytes() == expected.tobytes()


@pytest.mark.parametrize("with_heading", [True, False])
def test_scrolling_down_matches_pairwise_stitching(qapp, with_heading):
    """往下滚的结果与改造前一致；没有方向（手动截图）时也一样。"""
    page, tops, direction = CASES["down"]
    frames = _frames(page, tops, direction)
    headings = _headings(tops) if with_heading else [None] * len(tops)
    _, exported = _run(qapp, frames, direction, headings)
    assert exported.convert("RGB").tobytes() == _pairwise_flow(frames).tobytes()


def test_fixed_footer_matches_pairwise_stitching(qapp):
    tops = _sweep(0, 2160, 90)
    frames = _with_footer(_frames(TALL, tops, "vertical"))
    _, exported = _run(qapp, frames, "vertical", _headings(tops))
    expected = _pairwise_flow(frames)
    assert exported.height > H * 5
    assert exported.convert("RGB").tobytes() == expected.tobytes()


@pytest.mark.parametrize("lo, hi", [(0, 2160), (1200, 1560)])
def test_long_capture_keeps_every_row(qapp, lo, hi):
    """先往上再往下扫过整段：两头都接得上，与页面逐像素一致。"""
    page = _page(6000, seed=8)
    start = (lo + hi) // 2 // 90 * 90
    tops = [*range(start, lo - 1, -90), *range(lo, hi + 1, 90)]
    _, exported = _run(qapp, _frames(page, tops, "vertical"), "vertical", _headings(tops))
    assert exported.convert("RGB").tobytes() == page.crop((0, lo, W, hi + H)).tobytes()


def test_manual_capture_after_paging_up(qapp):
    """滚轮往上之后改用键盘翻页、手动截图，没有滚动方向；每次翻近九成屏，重叠只剩一成多也接得上。"""
    tops = [1800, 1710, 1620, 1410, 1200, 990]
    headings = [None, "up", "up", None, None, None]
    results, exported = _run(qapp, _frames(TALL, tops, "vertical"), "vertical", headings)
    assert all(r.ok for r in results)
    assert exported.convert("RGB").tobytes() == _truth(TALL, tops, "vertical").tobytes()


def test_single_frame_exports_unchanged(qapp):
    frame = TALL.crop((0, 0, W, H))
    _, exported = _run(qapp, [frame], "horizontal", [None])
    assert exported.mode == "RGB"
    assert exported.tobytes() == frame.tobytes()


def test_unmatched_frame_is_reported_and_skipped(qapp):
    tops = _sweep(0, 990, 90)
    frames = _frames(TALL, tops, "vertical")
    frames.insert(6, _noise())
    headings = ["down"] * len(frames)
    headings[0] = None
    results, exported = _run(qapp, frames, "vertical", headings)

    oks = [r.ok for r in results]
    assert oks == [True] * 6 + [False] + [True] * (len(frames) - 7)
    assert results[6].error is None
    assert exported.convert("RGB").tobytes() == _pairwise_flow(frames).tobytes()


def test_results_arrive_in_order_on_the_ui_thread(qapp):
    tops = _sweep(0, 1710, 90)
    collector = _Collector()
    stitcher = _stitcher(collector)
    try:
        _submit(stitcher, _frames(TALL, tops, "vertical"), "vertical", _headings(tops))
        _wait_for(qapp, collector, len(tops))
    finally:
        stitcher.close()
    assert [r.index for r in collector.results] == list(range(1, len(tops) + 1))
    assert set(collector.threads) == {threading.get_ident()}
    assert collector.results[0].first and not any(r.first for r in collector.results[1:])


@pytest.mark.parametrize("case", ["down", "up", "right", "rollback", "past_start_up", "footer"])
def test_preview_follows_the_result(qapp, case):
    """每一帧的缩略图长边都等于结果高度按比例换算：固定底栏截短时同步截短，回滚时不变。"""
    if case == "footer":
        tops, direction = _sweep(0, 1350, 90), "vertical"
        frames = _with_footer(_frames(TALL, tops, direction))
    else:
        page, tops, direction = CASES[case]
        tops = tops[:15]
        frames = _frames(page, tops, direction)
    results, _ = _run(qapp, frames, direction, _headings(tops))

    for result in results:
        preview = result.preview
        if direction == "vertical":
            short_side, long_side = preview.width(), preview.height()
        else:
            short_side, long_side = preview.height(), preview.width()
        assert short_side == THUMB
        if result.first:
            # 第一帧尚未转到拼接坐标，尺寸是原图的，由 Qt 缩放取整
            if direction == "vertical":
                frame_long, frame_short = result.height, result.width
            else:
                frame_long, frame_short = result.width, result.height
            assert abs(long_side - frame_long * THUMB / frame_short) <= 0.5
        else:
            assert long_side == round(result.height * THUMB / result.width)
    heights = [r.height for r in results]
    assert heights == sorted(heights)  # 回滚不截短


TRACKING_TOPS = {
    # 往前滚、回滚三步、超出原末尾、一下跳回开头（超过四屏）、再往前
    "down": [*range(0, 991, 90), 810, 630, 450, 540, 720, 900, 1080, 0, 90, 1170],
    "up": [*range(2160, 1259, -90), 1440, 1620, 1170, 1080, 2160, 2070, 990],
    "past_start": [1200, 1290, 1380, 1290, 1200, 1110, 1020, 1110, 1200, 1470, 1560, 930],
}


@pytest.mark.parametrize("case", sorted(TRACKING_TOPS))
def test_box_shows_where_the_frame_is(qapp, case):
    """框在预览里的位置对应这一帧在页面上的真实位置；长图往哪头长，框都跟得上。"""
    tops = TRACKING_TOPS[case]
    results, exported = _run(qapp, _frames(TALL, tops, "vertical"), "vertical", _headings(tops))

    scale = THUMB / W
    for i, (y, result) in enumerate(zip(tops, results)):
        assert result.ok, f"frame {i} at {y}"
        lo = min(tops[:i + 1])
        start, end = result.box
        assert abs(start - (y - lo) * scale) <= 1, f"frame {i} at {y}: {result.box}"
        assert abs(end - (y - lo + H) * scale) <= 1, f"frame {i} at {y}: {result.box}"
    assert exported.convert("RGB").tobytes() == _truth(TALL, tops, "vertical").tobytes()


@pytest.mark.parametrize("side", ["top", "bottom"])
def test_crop_cuts_at_the_latest_frame(qapp, side):
    """往下滚过之后回滚到 540 再裁：长图裁到这一帧为止，缩略图和框跟着变；裁完照常往两头长。"""
    tops = [*range(0, 991, 90), 540]
    after = [630, 720] if side == "bottom" else [450, 360]
    collector, crops = _Collector(), _Collector()
    stitcher = _stitcher(collector)
    stitcher.crop_done.connect(crops.collect, Qt.ConnectionType.QueuedConnection)
    try:
        _submit(stitcher, _frames(TALL, tops, "vertical"), "vertical", _headings(tops))
        stitcher.crop(side)
        headings = _headings([540, *after])[1:]
        for i, (frame, heading) in enumerate(zip(_frames(TALL, after, "vertical"), headings)):
            bgra = frame.convert("RGBA").tobytes("raw", "BGRA")
            stitcher.submit(Frame(bgra, W, H, len(tops) + i + 1, "vertical", heading, 0))
        _wait_for(qapp, collector, len(tops) + len(after))
        _wait_for(qapp, crops, 1)
        exported = stitcher.export()
    finally:
        stitcher.close()

    crop = crops.results[0]
    lo, hi = (0, 540) if side == "bottom" else (540, 990)
    scale = THUMB / W
    assert crop.ok and crop.height == hi + H - lo
    assert crop.preview.height() == round(crop.height * scale)
    assert abs(crop.box[0] - (540 - lo) * scale) <= 1
    assert crop.shift == -lo  # 裁掉顶部时已有内容整体上移
    final = (0, 720) if side == "bottom" else (360, 990)
    assert exported.convert("RGB").tobytes() == TALL.crop((0, final[0], W, final[1] + H)).tobytes()


def test_crop_before_the_second_frame_does_nothing(qapp):
    crops = _Collector()
    stitcher = _stitcher(_Collector())
    stitcher.crop_done.connect(crops.collect, Qt.ConnectionType.QueuedConnection)
    try:
        _submit(stitcher, _frames(TALL, [0], "vertical"), "vertical", [None])
        stitcher.crop("top")
        exported = stitcher.export()
    finally:
        stitcher.close()
    qapp.processEvents()
    assert crops.results == []
    assert exported.tobytes() == TALL.crop((0, 0, W, H)).tobytes()


def test_export_waits_for_frames_still_queued(qapp):
    tops = _sweep(0, 2160, 90)
    frames = _frames(TALL, tops, "vertical")
    stitcher = _stitcher(_Collector())
    try:
        _submit(stitcher, frames, "vertical", _headings(tops))
        exported = stitcher.export()  # 不处理事件，直接导出
    finally:
        stitcher.close()
    assert exported.convert("RGB").tobytes() == _truth(TALL, tops, "vertical").tobytes()


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
            stitcher.submit(Frame(bgra, big_w, big_h, i + 1, "vertical", "down" if i else None, 0))
        assert stitcher.export().height > big_h * 4
        stitcher.close()
        kept.append(stitcher)
        if round_ == 0:
            baseline = _private_bytes()

    leaked_threads = {t.ident for t in threading.enumerate()} - threads_before
    assert not leaked_threads
    # 每轮的拼接结果约 46MB；后三轮都不释放就会多出约 138MB
    assert _private_bytes() - baseline < 40 * 2**20
