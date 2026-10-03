# -*- coding: utf-8 -*-
"""Windows 截图工具自带的 OCR 引擎（oneocr.dll），经 PyPI 的 oneocr 包调用。

截图工具安装目录里的 DLL 不允许原地加载（管理员也一样），只能复制出来；
复制目录按截图工具的包全名区分，截图工具更新后不会混用新旧文件。
"""
import ctypes
import os
import shutil
from ctypes import wintypes
from pathlib import Path

from PySide6.QtGui import QImage

from core.constants import get_app_data_dir

PACKAGE_FAMILY = "Microsoft.ScreenSketch_8wekyb3d8bbwe"
REQUIRED_FILES = ("oneocr.dll", "oneocr.onemodel", "onnxruntime.dll")
# 引擎拒收任一边小于 50px 的图，补边到 50px 后能正常识别
MIN_SIDE = 50

_PACKAGE_FILTER_HEAD = 0x00000010
_ERROR_INSUFFICIENT_BUFFER = 122


def find_snipping_tool():
    """返回 (包全名, SnippingTool 目录)；没装截图工具或版本里没有 OCR 文件时返回 None。

    走包管理接口而不是列 WindowsApps 目录：后者普通用户没有权限。
    """
    try:
        kernel32 = ctypes.WinDLL("kernel32")
        find_packages = kernel32.FindPackagesByPackageFamily
        get_package_path = kernel32.GetPackagePathByFullName
    except (OSError, AttributeError):
        return None
    uint_p = ctypes.POINTER(ctypes.c_uint32)
    find_packages.argtypes = [
        wintypes.LPCWSTR, ctypes.c_uint32, uint_p,
        ctypes.POINTER(wintypes.LPWSTR), uint_p, wintypes.LPWSTR, uint_p,
    ]
    find_packages.restype = wintypes.LONG
    get_package_path.argtypes = [wintypes.LPCWSTR, uint_p, wintypes.LPWSTR]
    get_package_path.restype = wintypes.LONG

    count, buffer_len = ctypes.c_uint32(0), ctypes.c_uint32(0)
    rc = find_packages(PACKAGE_FAMILY, _PACKAGE_FILTER_HEAD, ctypes.byref(count),
                       None, ctypes.byref(buffer_len), None, None)
    if count.value == 0 or rc not in (0, _ERROR_INSUFFICIENT_BUFFER):
        return None
    full_names = (wintypes.LPWSTR * count.value)()
    buffer = ctypes.create_unicode_buffer(buffer_len.value)
    if find_packages(PACKAGE_FAMILY, _PACKAGE_FILTER_HEAD, ctypes.byref(count),
                     full_names, ctypes.byref(buffer_len), buffer, None) != 0:
        return None

    found = []
    for full_name in full_names[:count.value]:
        path_len = ctypes.c_uint32(0)
        get_package_path(full_name, ctypes.byref(path_len), None)
        path = ctypes.create_unicode_buffer(path_len.value)
        if get_package_path(full_name, ctypes.byref(path_len), path) != 0:
            continue
        source = Path(path.value) / "SnippingTool"
        if all((source / name).is_file() for name in REQUIRED_FILES):
            found.append((_package_version(full_name), full_name, source))
    if not found:
        return None
    _, full_name, source = max(found)
    return full_name, source


def _package_version(full_name: str) -> tuple:
    """包全名形如 Name_1.2.3.4_x64__PublisherId，取出可比较的版本号。"""
    try:
        return tuple(int(part) for part in full_name.split("_")[1].split("."))
    except (IndexError, ValueError):
        return ()


def cache_root() -> Path:
    return get_app_data_dir() / "oneocr"


def prepare_files(package) -> Path:
    """把截图工具的 OCR 文件复制到本应用的数据目录，返回该目录。

    先复制到临时目录再整体改名，中途失败不会留下缺文件的目录；
    其它版本的目录顺手删掉，删不掉（别的进程正在用）就留到下次。
    """
    full_name, source = package
    root = cache_root()
    target = root / full_name
    if all((target / name).is_file() for name in REQUIRED_FILES):
        return target

    staging = root / f"{full_name}.tmp{os.getpid()}"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        for name in REQUIRED_FILES:
            shutil.copyfile(source / name, staging / name)
        shutil.rmtree(target, ignore_errors=True)
        os.replace(staging, target)
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    for stale in root.iterdir():
        if stale.name != full_name:
            shutil.rmtree(stale, ignore_errors=True)
    return target


def _restore_dll_directory():
    """oneocr 建引擎时对整个进程调用 SetDllDirectoryW；依赖的 DLL 此时都已加载，恢复默认搜索顺序。"""
    set_dll_directory = ctypes.WinDLL("kernel32").SetDllDirectoryW
    set_dll_directory.argtypes = [wintypes.LPCWSTR]
    set_dll_directory.restype = wintypes.BOOL
    set_dll_directory(None)


class SnippingToolOcr:
    """持有一个 oneocr 引擎。不加锁，调用方保证同一时刻只有一个线程在用。"""

    def __init__(self, package):
        import oneocr

        files_dir = prepare_files(package)
        # oneocr 在建引擎时读模块级的 CONFIG_DIR
        oneocr.CONFIG_DIR = str(files_dir)
        try:
            self._engine = oneocr.OcrEngine()
        finally:
            _restore_dll_directory()

    def close(self):
        """释放引擎。oneocr 只在对象析构时释放 DLL 侧资源，所以这里必须是最后一个引用。"""
        self._engine = None

    def recognize(self, image: QImage) -> list:
        """返回 [[四个角点], 文字, 置信度] 列表。"""
        from PIL import Image

        if image.format() != QImage.Format.Format_ARGB32:
            image = image.convertToFormat(QImage.Format.Format_ARGB32)
        width, height, stride = image.width(), image.height(), image.bytesPerLine()
        # ARGB32 在小端机器上的内存顺序是 BGRA
        picture = Image.frombuffer("RGBA", (width, height), image.constBits(),
                                   "raw", "BGRA", stride, 1)
        if width < MIN_SIDE or height < MIN_SIDE:
            padded = Image.new("RGBA", (max(width, MIN_SIDE), max(height, MIN_SIDE)),
                               picture.getpixel((0, 0)))
            padded.paste(picture, (0, 0))
            picture = padded

        result = self._engine.recognize_pil(picture)
        if result.get("error"):
            raise RuntimeError(result["error"])

        lines = []
        for line in result["lines"]:
            text = line.get("text")
            rect = line.get("bounding_rect")
            if not text or not rect:
                continue
            box = [[rect["x1"], rect["y1"]], [rect["x2"], rect["y2"]],
                   [rect["x3"], rect["y3"]], [rect["x4"], rect["y4"]]]
            confidences = [word["confidence"] for word in line.get("words", [])
                           if word.get("confidence") is not None]
            score = sum(confidences) / len(confidences) if confidences else 1.0
            lines.append([box, text, score])
        return lines
