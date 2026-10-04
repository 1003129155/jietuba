# -*- coding: utf-8 -*-
"""截图工具 OCR 封装：文件准备、图像转换、补边、释放。

前半部分用假的 oneocr 包；后半部分在本机装有截图工具时用真引擎跑，没有就跳过。
"""
import ctypes
import gc
import shutil
import sys
import types
import weakref
from ctypes import wintypes
from pathlib import Path

import pytest
from PIL import Image, ImageDraw, ImageFont
from PySide6.QtGui import QColor, QImage

from ocr import snipping_tool_ocr as sto


# ── 文件准备 ─────────────────────────────────────────────

@pytest.fixture
def source(tmp_path):
    src = tmp_path / "SnippingTool"
    src.mkdir()
    for name in sto.REQUIRED_FILES:
        (src / name).write_bytes(name.encode())
    return src


@pytest.fixture
def cache(tmp_path, monkeypatch):
    root = tmp_path / "cache"
    monkeypatch.setattr(sto, "cache_root", lambda: root)
    return root


def test_files_are_copied_into_a_folder_named_after_the_package(source, cache):
    target = sto.prepare_files(("Pkg_1.0.0.0_x64__p", source))
    assert target == cache / "Pkg_1.0.0.0_x64__p"
    assert sorted(p.name for p in target.iterdir()) == sorted(sto.REQUIRED_FILES)


def test_existing_copy_is_reused(source, cache, monkeypatch):
    sto.prepare_files(("Pkg_1.0.0.0_x64__p", source))
    copies = []
    monkeypatch.setattr(shutil, "copyfile", lambda *a: copies.append(a))
    sto.prepare_files(("Pkg_1.0.0.0_x64__p", source))
    assert copies == []


def test_new_version_replaces_the_old_copy(source, cache):
    sto.prepare_files(("Pkg_1.0.0.0_x64__p", source))
    sto.prepare_files(("Pkg_2.0.0.0_x64__p", source))
    assert [p.name for p in cache.iterdir()] == ["Pkg_2.0.0.0_x64__p"]


def test_failed_copy_leaves_no_partial_folder(source, cache, monkeypatch):
    real_copy = shutil.copyfile
    calls = []

    def flaky(src, dst):
        calls.append(src)
        if len(calls) == 2:
            raise OSError("disk full")
        return real_copy(src, dst)

    monkeypatch.setattr(shutil, "copyfile", flaky)
    with pytest.raises(OSError):
        sto.prepare_files(("Pkg_1.0.0.0_x64__p", source))
    assert list(cache.iterdir()) == []


def test_package_version_orders_numerically():
    assert sto._package_version("A_11.2607.23.0_x64__p") > sto._package_version("A_11.999.0.0_x64__p")
    assert sto._package_version("broken") == ()


# ── 图像转换与结果 ───────────────────────────────────────

class _FakeOcrEngine:
    result = {"text": "", "text_angle": None, "lines": []}
    fail = None

    def __init__(self):
        if self.fail is not None:
            raise self.fail
        self.config_dir = sys.modules["oneocr"].CONFIG_DIR
        self.seen = []

    def recognize_pil(self, image):
        self.seen.append(image.copy())
        return self.result


@pytest.fixture
def fake_oneocr(monkeypatch, tmp_path):
    module = types.ModuleType("oneocr")
    module.CONFIG_DIR = "unset"
    module.OcrEngine = _FakeOcrEngine
    monkeypatch.setitem(sys.modules, "oneocr", module)
    monkeypatch.setattr(_FakeOcrEngine, "fail", None)
    monkeypatch.setattr(_FakeOcrEngine, "result", {"text": "", "text_angle": None, "lines": []})
    restored = []
    monkeypatch.setattr(sto, "_restore_dll_directory", lambda: restored.append(True))
    monkeypatch.setattr(sto, "prepare_files", lambda package: tmp_path / "files")
    return restored


def _filled(width, height, color):
    image = QImage(width, height, QImage.Format.Format_RGB32)
    image.fill(QColor(*color))
    return image


def _rect(x, y, w, h):
    return {"x1": x, "y1": y, "x2": x + w, "y2": y, "x3": x + w, "y3": y + h, "x4": x, "y4": y + h}


def test_engine_reads_files_from_the_prepared_folder(fake_oneocr, tmp_path):
    ocr = sto.SnippingToolOcr(("Pkg", Path("src")))
    assert ocr._engine.config_dir == str(tmp_path / "files")
    assert fake_oneocr == [True]


def test_dll_search_path_is_restored_even_if_the_engine_fails(fake_oneocr, monkeypatch):
    monkeypatch.setattr(_FakeOcrEngine, "fail", RuntimeError("Pipeline creation failed"))
    with pytest.raises(RuntimeError):
        sto.SnippingToolOcr(("Pkg", Path("src")))
    assert fake_oneocr == [True]


def test_pixels_reach_the_engine_in_rgb_order(fake_oneocr, qapp):
    ocr = sto.SnippingToolOcr(("Pkg", Path("src")))
    ocr.recognize(_filled(80, 60, (10, 20, 30)))
    seen = ocr._engine.seen[0]
    assert seen.size == (80, 60)
    assert seen.getpixel((5, 5)) == (10, 20, 30, 255)


def test_small_image_is_padded_with_its_corner_colour(fake_oneocr, qapp):
    ocr = sto.SnippingToolOcr(("Pkg", Path("src")))
    image = _filled(30, 20, (40, 40, 40))
    image.setPixelColor(0, 0, QColor(200, 100, 50))
    ocr.recognize(image)
    seen = ocr._engine.seen[0]
    assert seen.size == (sto.MIN_SIDE, sto.MIN_SIDE)
    assert seen.getpixel((10, 10)) == (40, 40, 40, 255)
    assert seen.getpixel((sto.MIN_SIDE - 1, sto.MIN_SIDE - 1)) == (200, 100, 50, 255)


def test_lines_are_flattened_and_blank_ones_dropped(fake_oneocr, monkeypatch, qapp):
    monkeypatch.setattr(_FakeOcrEngine, "result", {"text": "", "text_angle": 0.0, "lines": [
        {"text": "你好", "bounding_rect": _rect(1, 2, 30, 10),
         "words": [{"confidence": 0.8}, {"confidence": 0.6}]},
        {"text": None, "bounding_rect": None, "words": []},
        {"text": "no box", "bounding_rect": None, "words": []},
        {"text": "no words", "bounding_rect": _rect(0, 20, 10, 10), "words": []},
    ]})
    ocr = sto.SnippingToolOcr(("Pkg", Path("src")))
    lines = ocr.recognize(_filled(80, 60, (255, 255, 255)))
    assert lines == [
        [[[1, 2], [31, 2], [31, 12], [1, 12]], "你好", pytest.approx(0.7)],
        [[[0, 20], [10, 20], [10, 30], [0, 30]], "no words", 1.0],
    ]


def test_engine_error_is_raised(fake_oneocr, monkeypatch, qapp):
    monkeypatch.setattr(_FakeOcrEngine, "result", {"text": "", "lines": [], "error": "Unsupported image size"})
    ocr = sto.SnippingToolOcr(("Pkg", Path("src")))
    with pytest.raises(RuntimeError, match="Unsupported image size"):
        ocr.recognize(_filled(80, 60, (255, 255, 255)))


def test_close_drops_the_last_reference(fake_oneocr, qapp):
    ocr = sto.SnippingToolOcr(("Pkg", Path("src")))
    ocr.recognize(_filled(80, 60, (255, 255, 255)))
    engine = weakref.ref(ocr._engine)
    ocr.close()
    assert engine() is None


# ── 真引擎（需要本机装有截图工具）────────────────────────

_PACKAGE = sto.find_snipping_tool()
real = pytest.mark.skipif(_PACKAGE is None, reason="本机没有带 OCR 的截图工具")


@pytest.fixture(scope="module")
def real_cache(tmp_path_factory):
    """真文件复制到临时目录，不动用户数据目录里的缓存。"""
    root = tmp_path_factory.mktemp("oneocr_cache")
    original = sto.cache_root
    sto.cache_root = lambda: root
    yield root
    sto.cache_root = original


def _text_image(text, size=28, padding=12):
    font = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", size)
    width = int(font.getlength(text)) + padding * 2
    height = size + padding * 2
    picture = Image.new("RGB", (width, height), "white")
    ImageDraw.Draw(picture).text((padding, padding // 2), text, fill="black", font=font)
    data = picture.tobytes()
    return QImage(data, width, height, width * 3, QImage.Format.Format_RGB888).copy()


def _handle_count():
    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.GetProcessHandleCount.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    count = wintypes.DWORD()
    kernel32.GetProcessHandleCount(kernel32.GetCurrentProcess(), ctypes.byref(count))
    return count.value


@real
def test_found_package_has_the_ocr_files():
    full_name, source = _PACKAGE
    assert full_name.startswith("Microsoft.ScreenSketch_")
    assert all((source / name).is_file() for name in sto.REQUIRED_FILES)


@real
def test_real_engine_reads_text(real_cache, qapp):
    ocr = sto.SnippingToolOcr(_PACKAGE)
    try:
        lines = ocr.recognize(_text_image("截图吧 OCR 12345"))
    finally:
        ocr.close()
    text = "".join(line[1] for line in lines).replace(" ", "")
    assert "截图吧" in text and "12345" in text


@real
def test_real_engine_reads_an_image_shorter_than_its_minimum(real_cache, qapp):
    image = _text_image("OK 确定", size=14, padding=6)
    assert image.height() < sto.MIN_SIDE
    ocr = sto.SnippingToolOcr(_PACKAGE)
    try:
        lines = ocr.recognize(image)
    finally:
        ocr.close()
    assert "确定" in "".join(line[1] for line in lines)


@real
def test_real_engine_releases_what_it_holds(real_cache, qapp):
    """两个判据：引擎对象确实被析构（DLL 侧资源随之释放）、句柄数不随轮数增长。
    反向对照：不调用 close 时两个判据都会报警，证明测得出泄漏。"""
    image = _text_image("释放测试 123")

    def cycle(keep):
        ocr = sto.SnippingToolOcr(_PACKAGE)
        ocr.recognize(image)
        ref = weakref.ref(ocr._engine)
        if keep is None:
            ocr.close()
        else:
            keep.append(ocr)
        del ocr
        gc.collect()
        return ref

    for _ in range(2):
        cycle(None)
    baseline = _handle_count()
    refs = [cycle(None) for _ in range(6)]
    assert all(ref() is None for ref in refs)
    assert _handle_count() - baseline <= 5

    kept = []
    leak_baseline = _handle_count()
    leaked = [cycle(kept) for _ in range(4)]
    try:
        assert all(ref() is not None for ref in leaked)
        # 每个未释放的引擎多占的句柄 ARM64 约 7 个、x64 约 21 个，都超过上面放行的 5 个
        assert _handle_count() - leak_baseline > 4 * 5
    finally:
        for ocr in kept:
            ocr.close()
        kept.clear()
        gc.collect()
