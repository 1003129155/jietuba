# -*- coding: utf-8 -*-
"""
手柄浮层的内存开销实测。

不是单元测试（不进 pytest 收集），单独跑：
    QT_QPA_PLATFORM=offscreen python tests/bench_handle_overlay_memory.py

关心两件事：
1. 浮层是不是子部件（子部件共用顶层窗口的 backing store，不会各自分配帧缓冲）。
   如果它拿到了原生窗口句柄，那才是真的每层一份 framebuffer。
2. 每个浮层实例本身占多少 RSS，以及每多一个 CanvasView 多花多少。
"""
import gc
import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import psutil
from PySide6.QtCore import QRectF
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

PROC = psutil.Process()
CANVAS_W, CANVAS_H = 2560, 1440


def rss_kb():
    gc.collect()
    return PROC.memory_info().rss / 1024.0


def make_view():
    from canvas.scene import CanvasScene
    from canvas.view import CanvasView

    bg = QImage(CANVAS_W, CANVAS_H, QImage.Format.Format_ARGB32)
    bg.fill(0xFFFFFFFF)
    view = CanvasView(CanvasScene(bg, QRectF(0, 0, CANVAS_W, CANVAS_H)))
    view.resize(CANVAS_W, CANVAS_H)
    view.show()
    return view


def main():
    app = QApplication.instance() or QApplication([])
    from canvas.handle_overlay import HandleOverlayWidget

    warmup = make_view()
    app.processEvents()
    overlay = warmup._handle_overlay

    print("== 浮层是不是独立窗口 ==")
    print(f"  internalWinId()      = {overlay.internalWinId()}   (0 = 无原生窗口，与父部件共用 backing store)")
    print(f"  windowHandle()       = {overlay.windowHandle()}")
    print(f"  isWindow()           = {overlay.isWindow()}")
    print(f"  顶层窗口             = {overlay.window() is warmup}")
    print(f"  几何                 = {overlay.geometry()}  (铺满 viewport)")

    # 单个浮层实例的成本
    n = 500
    base = rss_kb()
    extra = [HandleOverlayWidget(warmup) for _ in range(n)]
    app.processEvents()
    per_overlay = (rss_kb() - base) / n
    for w in extra:
        w.setParent(None)
        w.deleteLater()
    extra.clear()
    app.processEvents()

    # 整个 CanvasView 的成本（浮层只是其中一小块）
    views = []
    base = rss_kb()
    for _ in range(10):
        views.append(make_view())
    app.processEvents()
    per_view = (rss_kb() - base) / 10

    print("\n== 内存 ==")
    print(f"  每个 HandleOverlayWidget : {per_overlay:8.2f} KB")
    print(f"  每个 CanvasView (含浮层) : {per_view:8.1f} KB   画布 {CANVAS_W}x{CANVAS_H}")
    if per_view > 0:
        print(f"  浮层占一个 view 的       : {per_overlay / per_view * 100:8.2f} %")
    frame_kb = CANVAS_W * CANVAS_H * 4 / 1024
    print(f"  （对比：一张 {CANVAS_W}x{CANVAS_H} ARGB32 帧缓冲 = {frame_kb:,.0f} KB）")
    print(f"  浮层 / 一张帧缓冲        : {per_overlay / frame_kb * 100:8.4f} %")

    for v in views:
        v.cleanup()
        v.close()
    warmup.cleanup()
    warmup.close()


if __name__ == "__main__":
    main()
