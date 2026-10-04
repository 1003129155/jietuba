"""长截图窗口的模拟环境：合成页面、按格挪动的视口、假光标。

窗口截图时取视口里的那一段，注入的滚轮按每格固定像素挪动视口，并像钩子那样把滚轮交给窗口，
不碰真实的屏幕和鼠标。
"""

import random

from PIL import Image, ImageDraw
from PySide6.QtCore import QPoint, QRect
from PySide6.QtGui import QImage

PAGE_W, VIEW_H, PX_PER_NOTCH = 400, 300, 40


def synthetic_page(height, seed=4):
    """白底上逐行长短不一的色块，近似一页文字。"""
    rnd = random.Random(seed)
    image = Image.new("RGB", (PAGE_W, height), "white")
    draw = ImageDraw.Draw(image)
    y = 6
    while y < height - 20:
        line = rnd.choice((8, 10, 12))
        color = tuple(rnd.randint(0, 120) for _ in range(3))
        for row in range(y, y + line):
            x = 10
            while x < PAGE_W - 40:
                length = rnd.randint(2, 30)
                if rnd.random() < 0.6:
                    draw.line((x, row, x + length, row), fill=color)
                x += length + rnd.randint(1, 6)
        y += line + rnd.randint(4, 14)
    return image


class FakeCursor:
    def __init__(self):
        self.position = QPoint(0, 0)

    def pos(self):
        return QPoint(self.position)

    def setPos(self, point):
        self.position = QPoint(point)


class SimulatedPage:
    """视口顶边在页面上的位置为 top；滚轮每 120 挪 PX_PER_NOTCH 像素，正值往上。

    mode 是程序对不到一格的滚轮的处理："smooth" 按比例滚，"accumulate" 攒够一格再滚，"whole" 直接丢掉。
    unsettled 模拟动画中途没重绘完：每次滚动后的头几次截图上半截是新位置、下半截还是旧位置，且每次错开处都不同。
    hover 模拟光标停在截图区中间：开始注入滚轮后，每次截图中间那一行都带着悬停高亮。
    """

    def __init__(self, window, page, top, mode="smooth", unsettled=0, hover=False):
        self.window, self.page, self.mode, self.unsettled, self.hover = window, page, mode, unsettled, hover
        self._hovering = False
        self._exact = float(top)
        self._pending = 0
        self._previous_top = int(top)
        self._torn_left = 0
        self.injected = []
        self.torn = set()  # 截到过的错开画面（字节的哈希）

    @property
    def top(self):
        return int(self._exact)

    def inject(self, delta, horizontal=False):
        from stitch.scroll_window import _INPUT_WATCHER
        self.injected.append(delta)
        if self.mode == "smooth":
            notches = delta / 120
        elif self.mode == "accumulate":
            self._pending += delta
            notches = int(self._pending / 120)
            self._pending -= notches * 120
        else:
            notches = int(delta / 120)
        self._hovering = self.hover
        self._previous_top = self.top
        self._exact = max(0.0, min(self.page.height - VIEW_H, self._exact - notches * PX_PER_NOTCH))
        if self.top != self._previous_top:
            self._torn_left = self.unsettled
        self.window._on_wheel(_INPUT_WATCHER, 0, 0, delta, horizontal)

    def scroll_and_capture(self, notches):
        """手动滚几格再截图（不等冷却）。"""
        self.inject(notches * 120)
        self.window._do_capture()

    def grab(self):
        crop = self.page.crop((0, self.top, PAGE_W, self.top + VIEW_H))
        if self._torn_left:
            split = VIEW_H * self._torn_left // (self.unsettled + 1)
            stale = self.page.crop((0, self._previous_top, PAGE_W, self._previous_top + VIEW_H))
            crop.paste(stale.crop((0, split, PAGE_W, VIEW_H)), (0, split))
            self._torn_left -= 1
            self.torn.add(hash(crop.convert("RGBA").tobytes("raw", "BGRA")))
        if self._hovering:
            crop.paste((200, 225, 250), (0, VIEW_H // 2 - 6, PAGE_W, VIEW_H // 2 + 6))
        crop = crop.convert("RGBA")
        return QImage(crop.tobytes("raw", "BGRA"), PAGE_W, VIEW_H, PAGE_W * 4, QImage.Format.Format_RGB32).copy()


def open_window(monkeypatch):
    """开一个长截图窗口：不自动截第一帧，完成时把长图记在 window.captured 里而不保存、不复制。"""
    from stitch import scroll_window
    from stitch.scroll_window import ScrollCaptureWindow
    monkeypatch.setattr(ScrollCaptureWindow, "_capture_initial_screenshot", lambda self: None)
    monkeypatch.setattr(scroll_window, "STILL_CHECK_MS", 1)
    monkeypatch.setattr(scroll_window, "wheel_follows_focus", lambda: False)
    window = ScrollCaptureWindow(QRect(100, 100, PAGE_W, VIEW_H))
    captured = {}
    monkeypatch.setattr(window, "_save_result", lambda: captured.__setitem__("image", window.stitched_result))
    monkeypatch.setattr(window, "_copy_to_clipboard", lambda: None)
    window.captured = captured
    return window


def simulate(monkeypatch, window, page, top, mode="smooth", unsettled=0, hover=False):
    """让窗口截的是模拟页面，自动滚动的注入和光标也换成模拟的。"""
    sim = SimulatedPage(window, page, top, mode, unsettled, hover)
    monkeypatch.setattr(window, "_grab_capture_rect", sim.grab)
    monkeypatch.setattr(window._auto_scroller, "_inject", sim.inject)
    monkeypatch.setattr(window._auto_scroller, "_cursor", FakeCursor())
    return sim
