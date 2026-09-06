# -*- coding: utf-8 -*-
"""
手柄浮层 vs 整场景重绘：拖动一帧的实测开销。

不是单元测试（不进 pytest 收集），单独跑：
    QT_QPA_PLATFORM=offscreen python tests/bench_handle_overlay.py

两种失效策略，跑同一段拖动：
- full_scene : 每帧 scene.update()   —— 改造前热路径的做法
- overlay    : 图元自身失效 + 浮层重绘 —— 改造后的做法

统计每帧的 processEvents() 墙钟时间，以及 viewport 实际被重绘的像素数
（由 Paint 事件的 region 累加得到）。
"""
import os
import sys
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PySide6.QtCore import QObject, QEvent, QPointF, QRectF
from PySide6.QtGui import QColor, QImage, QPen
from PySide6.QtWidgets import QApplication

CANVAS_W, CANVAS_H = 2560, 1440
ITEM_COUNT = 30
FRAMES = 200


class PaintProbe(QObject):
    """统计 viewport 每次 Paint 事件覆盖的像素数。"""

    def __init__(self):
        super().__init__()
        self.painted_px = 0
        self.paints = 0

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Type.Paint:
            self.paints += 1
            for r in event.region():
                self.painted_px += r.width() * r.height()
        return False


def build(app):
    from canvas.scene import CanvasScene
    from canvas.view import CanvasView
    from canvas.items import RectItem

    bg = QImage(CANVAS_W, CANVAS_H, QImage.Format.Format_ARGB32)
    bg.fill(0xFFFFFFFF)
    scene = CanvasScene(bg, QRectF(0, 0, CANVAS_W, CANVAS_H))
    view = CanvasView(scene)
    view.resize(CANVAS_W, CANVAS_H)
    view.show()

    pen = QPen(QColor(220, 40, 40), 3)
    items = []
    for i in range(ITEM_COUNT):
        it = RectItem(QRectF(40 + (i % 10) * 220, 60 + (i // 10) * 300, 180, 120), pen)
        scene.addItem(it)
        items.append(it)

    target = items[0]
    view.smart_edit_controller.select_item(target)
    app.processEvents()
    return view, target


def run(app, mode):
    view, target = build(app)
    probe = PaintProbe()
    view.viewport().installEventFilter(probe)
    app.processEvents()
    probe.painted_px = probe.paints = 0

    step = QPointF(1.0, 1.0)
    frames = []
    for i in range(FRAMES):
        target.moveBy(step.x(), step.y())
        if mode == "full_scene":
            view.canvas_scene.update()
        view._update_edit_handles()

        t0 = time.perf_counter()
        app.processEvents()
        frames.append((time.perf_counter() - t0) * 1000.0)

    view.viewport().removeEventFilter(probe)
    view.cleanup()
    view.close()
    frames.sort()
    return {
        "median_ms": frames[len(frames) // 2],
        "p95_ms": frames[int(len(frames) * 0.95)],
        "total_ms": sum(frames),
        "paints": probe.paints,
        "mpx": probe.painted_px / 1e6,
    }


def main():
    app = QApplication.instance() or QApplication([])
    print(f"canvas {CANVAS_W}x{CANVAS_H}, {ITEM_COUNT} items, {FRAMES} drag frames\n")
    results = {}
    for mode in ("full_scene", "overlay"):
        run(app, mode)  # 预热，排除首帧缓存/字体初始化
        results[mode] = run(app, mode)
        r = results[mode]
        print(
            f"{mode:11s} median {r['median_ms']:7.3f} ms/frame | "
            f"p95 {r['p95_ms']:7.3f} | total {r['total_ms']:8.1f} ms | "
            f"paints {r['paints']:4d} | repainted {r['mpx']:8.1f} Mpx"
        )

    a, b = results["full_scene"], results["overlay"]
    print(
        f"\noverlay 相对 full_scene: 时间 {a['total_ms'] / max(b['total_ms'], 1e-9):.1f}x 更快, "
        f"重绘像素 {a['mpx'] / max(b['mpx'], 1e-9):.1f}x 更少"
    )


if __name__ == "__main__":
    main()
