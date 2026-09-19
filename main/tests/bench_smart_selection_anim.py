# -*- coding: utf-8 -*-
"""
智能选区换窗口：补间 vs 瞬间跳变的实测开销。

不是单元测试（不进 pytest 收集），单独跑：
    QT_QPA_PLATFORM=offscreen python tests/bench_smart_selection_anim.py

补间本身只改矩形，真实开销来自 selection_model.rectChanged 驱动的重绘：
遮罩脏区是新旧选区的并集，跨屏幕的大跳意味着每帧接近全屏的半透明填充。
跳一次的总量因此被放大到"帧数倍"，这里量的就是这个倍数。

不走事件循环：Windows 上 process_time() 只有 15.6ms 粒度，补间帧之间的 16ms
等待又会混进墙钟。改成截下每帧的真实脏区，同步 render() 到 QImage 再用
perf_counter 计时——量的就是 paintEvent 里那几个 fillRect 的光栅化成本。

渲染目标在多个缓冲之间轮转，让工作集超出 L3：单张 4K 画布才 33MB，在大缓存
CPU 上反复画同一张会全程命中缓存，量出来比真机快一倍。

不含 DWM 把分层窗口合成到屏幕那一段，真机上每帧还要再加一次全屏拷贝。
"""
import os
import sys
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from PySide6.QtCore import QEasingCurve, QPoint, QRectF
from PySide6.QtGui import QImage, QRegion
from PySide6.QtWidgets import QApplication, QWidget

from canvas.smart_selection_anim import SmartSelectionAnimator

# 屏幕分辨率 → 一次"从左上角小窗口跳到右下角大窗口"的最坏情况
SCREENS = [(1920, 1080), (2560, 1440), (3840, 2160)]
REPEATS = 20
FRAME_BUDGET_MS = 16.67
WORKING_SET_MB = 220  # 需超过 L3；9800X3D 有 96MB


def build_mask(w, h):
    from canvas.selection_model import SelectionModel
    from ui.mask_overlay import MaskOverlayWidget

    host = QWidget()
    host.resize(w, h)
    model = SelectionModel()
    model.activate()
    mask = MaskOverlayWidget(host, model)
    mask.setGeometry(0, 0, w, h)
    return host, mask, model


def frame_rects(a: QRectF, b: QRectF):
    """补间实际会经过的矩形序列，和 _tick 用同一条曲线。"""
    curve = QEasingCurve(QEasingCurve.Type.OutCubic)
    rects = []
    elapsed = 0
    while elapsed < SmartSelectionAnimator.DURATION_MS:
        t = curve.valueForProgress(elapsed / float(SmartSelectionAnimator.DURATION_MS))
        rects.append(SmartSelectionAnimator._lerp(a, b, t))
        elapsed += SmartSelectionAnimator.FRAME_MS
    rects.append(QRectF(b))
    return rects


def measure(w, h, rects):
    """把 rects 依次落到模型上，量每一帧遮罩重绘的耗时和脏区像素。"""
    host, mask, model = build_mask(w, h)
    pool_size = max(2, int(WORKING_SET_MB * 1e6 / (w * h * 4)) + 1)
    pool = [QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
            for _ in range(pool_size)]
    for buf in pool:
        buf.fill(0)

    # 截下 update(dirty) 的脏区；paintEvent 里 4 条 fillRect 会被裁到这块
    dirty = []
    mask.update = lambda *a: dirty.append(a[0] if a else mask.rect())

    model.set_rect(QRectF(rects[0]))
    for buf in pool:
        mask.render(buf, QPoint(), QRegion(mask.rect()))  # 预热
    dirty.clear()

    per_frame, px = [], 0
    for rect in rects[1:]:
        dirty.clear()
        model.set_rect(QRectF(rect))
        if not dirty:
            continue
        region = QRegion(dirty[-1])
        px += sum(r.width() * r.height() for r in region)
        samples = []
        for i in range(REPEATS):
            t0 = time.perf_counter_ns()
            mask.render(pool[i % len(pool)], QPoint(), region)
            samples.append((time.perf_counter_ns() - t0) / 1e6)
        samples.sort()
        per_frame.append(samples[len(samples) // 2])

    host.close()
    return per_frame, px / 1e6


def main():
    QApplication.instance() or QApplication([])
    print("一次跨屏换窗口的遮罩重绘开销（补间 %dms / %dms 一帧，工作集 >%dMB 绕开缓存）"
          % (SmartSelectionAnimator.DURATION_MS, SmartSelectionAnimator.FRAME_MS,
             WORKING_SET_MB))
    print()
    print("%11s %6s %8s %11s %11s %11s %9s"
          % ("分辨率", "模式", "帧数", "最慢帧", "总耗时", "重绘", "占16.7ms"))
    print("-" * 74)

    for w, h in SCREENS:
        small = QRectF(40, 40, int(w * 0.18), int(h * 0.20))
        big = QRectF(w * 0.45, h * 0.40, w * 0.50, h * 0.55)
        rows = {
            "跳变": [small, big],
            "补间": frame_rects(small, big),
        }
        for label, rects in rows.items():
            per_frame, mpx = measure(w, h, rects)
            worst = max(per_frame)
            print("%9dx%-5d %4s %8d %9.2fms %9.2fms %8.1fMpx %8.0f%%"
                  % (w, h, label, len(per_frame), worst, sum(per_frame), mpx,
                     worst / FRAME_BUDGET_MS * 100))
        print()


if __name__ == "__main__":
    main()
