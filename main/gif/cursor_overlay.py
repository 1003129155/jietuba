# -*- coding: utf-8 -*-
"""
回放鼠标光标覆盖层 — 独立透明窗口

录制时 FrameRecorder 采集鼠标坐标、左/右键、滚轮状态到 FrameData.cursor，
回放时本覆盖层根据当前帧绘制：
  - 正常：鼠标.svg 图标
  - 左键：左半边烟花（小圆心 + 左侧放射线），3帧扩散动画，按住持续显示
  - 右键：右半边烟花（小圆心 + 右侧放射线），同上
  - 滚动：光标右侧 3 个叠排主题色三角箭头

导出时通过 rasterize_cursor_sprites() 一次性栅格化所有 sprite（~0.4ms），
再把 sprite bytes + 每帧参数传给 Rust，由 Rust 做 alpha blend。
"""

from __future__ import annotations

import math

from PySide6.QtWidgets import QWidget

from core.logger import log_exception
from core import safe_event
from PySide6.QtCore import Qt, QRect, QPoint, QPointF, QRectF, QTimer
from PySide6.QtGui import QPainter, QColor, QBrush, QPen, QPainterPath, QImage
from PySide6.QtSvg import QSvgRenderer

from core.resource_manager import ResourceManager
from core.theme import ThemeManager
from .frame_recorder import CursorSnapshot


# 烟花动画帧数：3 帧，每帧放射线长度递增
_BURST_FRAMES = 3


class CursorOverlay(QWidget):
    """回放时的鼠标光标叠加层 — 透明无边框窗口"""

    _CURSOR_SIZE   = 24   # 光标渲染尺寸（px）
    _BURST_R_START = 10   # 烟花第 1 帧放射线起始外径
    _BURST_R_STEP  = 9    # 每帧外径增量
    _DOT_R         = 6    # 圆心半径

    # 类级别缓存：所有实例共享同一个 QSvgRenderer（只加载一次）
    _svg_renderer: QSvgRenderer | None = None

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        # WS_EX_TRANSPARENT: 鼠标事件完全穿透
        try:
            import ctypes
            hwnd = int(self.winId())
            GWL_EXSTYLE       = -20
            WS_EX_TRANSPARENT = 0x00000020
            user32 = ctypes.windll.user32
            style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style | WS_EX_TRANSPARENT)
        except Exception as e:
            log_exception(e, "设置光标覆盖层透明")

        # 懒加载 SVG（类级别，只加载一次）
        if CursorOverlay._svg_renderer is None:
            svg_path = ResourceManager.get_icon_path("鼠标.svg")
            CursorOverlay._svg_renderer = QSvgRenderer(svg_path)

        self._cursor: CursorSnapshot | None = None
        self._visible_flag = True

        # 烟花动画状态
        self._burst_frame: int = 0       # 0=不显示，1/2/3=当前帧
        self._burst_side:  int = 0       # 1=左键，2=右键
        self._burst_timer = QTimer(self)
        self._burst_timer.setInterval(40)  # 每帧约 40ms（~25fps 动画）
        self._burst_timer.timeout.connect(self._on_burst_tick)

    # ── 外部 API ──

    def update_rect(self, rect: QRect):
        """同步位置到录制/回放区域"""
        self.setGeometry(rect)

    def set_cursor_visible(self, visible: bool):
        """用户开关鼠标光标显示"""
        self._visible_flag = visible
        self.update()

    def is_cursor_visible(self) -> bool:
        return self._visible_flag

    def set_frame_cursor(self, cursor: CursorSnapshot | None):
        """设置当前帧的鼠标状态，触发重绘"""
        prev_press = self._cursor.press if self._cursor else 0
        self._cursor = cursor
        cur_press = cursor.press if cursor else 0

        # 按下瞬间（从 0 → 非0）启动动画；持续按住则保持在第 1 帧
        if cur_press != 0 and prev_press == 0:
            self._burst_side  = cur_press
            self._burst_frame = 1
            self._burst_timer.start()
        elif cur_press == 0:
            # 松开：如果动画还没跑完就让它自然走完，Timer 会自己停
            pass

        self.update()

    # ── 动画 tick ──

    def _on_burst_tick(self):
        """每 40ms 推进一帧"""
        if self._cursor and self._cursor.press != 0:
            # 仍在按住：停在第 1 帧（持续显示），不推进
            return
        self._burst_frame += 1
        if self._burst_frame > _BURST_FRAMES:
            self._burst_frame = 0
            self._burst_timer.stop()
        self.update()

    # ── 绘制 ──

    @safe_event
    def paintEvent(self, ev):
        if not self._visible_flag or self._cursor is None or not self._cursor.visible:
            return

        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        x = self._cursor.x
        y = self._cursor.y
        scroll = self._cursor.scroll

        # ── 烟花（按下或动画进行中）──
        if self._burst_frame > 0:
            self._draw_burst(p, x, y, self._burst_side, self._burst_frame)

        # ── 滚动箭头 ──
        if scroll != 0:
            self._draw_scroll_arrows(p, x, y, scroll)

        # ── 绘制鼠标.svg 光标（热点在左上角）──
        s = self._CURSOR_SIZE
        renderer = CursorOverlay._svg_renderer
        if renderer and renderer.isValid():
            renderer.render(p, QRectF(x, y, s, s))
        else:
            p.setPen(QColor(0, 0, 0))
            p.drawLine(x - 6, y, x + 6, y)
            p.drawLine(x, y - 6, x, y + 6)

        p.end()

    def _draw_burst(self, p: QPainter, x: int, y: int, side: int, frame: int):
        """
        左/右半边烟花：小圆心 + 半侧放射线。

        鼠标箭头倾斜约 45°（左上热点，箭头指向右下），
        分割线沿左上→右下对角（135° 方向），因此：
          side=1（左键）→ 左上侧（以 225° 为中心，覆盖 135°~315°）
          side=2（右键）→ 右下侧（以  45° 为中心，覆盖 315°~135°）
        frame=1~3，外径逐帧增大；越晚的帧透明度越低。
        """
        cx = x + self._DOT_R
        cy = y + self._DOT_R
        theme = ThemeManager().theme_color

        # 透明度随帧数衰减（frame1=220, frame2=150, frame3=80）
        alpha = max(60, 240 - (frame - 1) * 80)

        # 圆心点
        dot_color = QColor(theme)
        dot_color.setAlpha(alpha)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(dot_color))
        p.drawEllipse(QPoint(cx, cy), self._DOT_R, self._DOT_R)

        # 放射线外径随帧扩散
        r_outer = self._BURST_R_START + (frame - 1) * self._BURST_R_STEP
        r_inner = self._DOT_R + 3

        line_color = QColor(theme)
        line_color.setAlpha(alpha)
        p.setPen(QPen(line_color, 2.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))

        # 以 135° 对角线为分割：
        # 左键（左上侧）：中心 225°，5 条线：135, 180, 225, 270, 315
        # 右键（右下侧）：中心  45°，5 条线： -45,   0,  45,  90, 135
        if side == 1:
            angles_deg = [135, 180, 225, 270, 315]
        else:
            angles_deg = [-45, 0, 45, 90, 135]

        for deg in angles_deg:
            angle = math.radians(deg)
            x1 = cx + r_inner * math.cos(angle)
            y1 = cy + r_inner * math.sin(angle)
            x2 = cx + r_outer * math.cos(angle)
            y2 = cy + r_outer * math.sin(angle)
            p.drawLine(QPointF(x1, y1), QPointF(x2, y2))

    def _draw_scroll_arrows(self, p: QPainter, x: int, y: int, scroll: int):
        """滚动指示：光标右侧 3 个叠排大三角箭头，主题色，越靠运动方向越不透明"""
        theme = ThemeManager().theme_color
        ax = x + self._CURSOR_SIZE + 10
        ay = y + self._CURSOR_SIZE // 2

        arrow_h = 9
        arrow_w = 15
        gap = 5

        p.setPen(Qt.PenStyle.NoPen)

        for i in range(3):
            offset = (i - 1) * (arrow_h + gap)
            alpha = (80 + i * 70) if scroll == 1 else (220 - i * 70)
            color = QColor(theme)
            color.setAlpha(max(60, alpha))
            p.setBrush(QBrush(color))

            path = QPainterPath()
            if scroll == 1:
                tip_y  = ay + offset - arrow_h
                base_y = ay + offset
                path.moveTo(ax, tip_y)
                path.lineTo(ax - arrow_w // 2, base_y)
                path.lineTo(ax + arrow_w // 2, base_y)
            else:
                tip_y  = ay + offset + arrow_h
                base_y = ay + offset
                path.moveTo(ax, tip_y)
                path.lineTo(ax - arrow_w // 2, base_y)
                path.lineTo(ax + arrow_w // 2, base_y)
            path.closeSubpath()
            p.drawPath(path)


# ═══════════════════════════════════════════════════════════
# Sprite 栅格化 — 供 GIF 导出使用
# ═══════════════════════════════════════════════════════════

def _rasterize_qimage(w: int, h: int, draw_fn) -> bytes:
    """将 draw_fn(QPainter, w, h) 绘制的内容栅格化为 RGBA32 bytes"""
    img = QImage(w, h, QImage.Format.Format_RGBA8888_Premultiplied)
    img.fill(0)
    p = QPainter(img)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    draw_fn(p, w, h)
    p.end()
    bpl = img.bytesPerLine()
    expected = w * 4
    ptr = img.bits()
    if bpl == expected:
        # PySide6: memoryview 直接转 bytes
        return bytes(ptr[:h * expected])
    # 有 padding，逐行取有效像素
    raw = bytes(ptr[:h * bpl])
    return b"".join(raw[r * bpl : r * bpl + expected] for r in range(h))


def rasterize_cursor_sprites(scale: float = 1.0) -> dict:
    """
    一次性栅格化所有光标相关 sprite，返回字典。

    Parameters
    ----------
    scale : 导出尺寸与录制尺寸之比（如 640/1280 = 0.5）

    Returns
    -------
    dict 包含:
      "cursor"            : (bytes, w, h)  — 鼠标箭头
      "burst_left_1/2/3"  : (bytes, w, h)  — 左键烟花 3 帧
      "burst_right_1/2/3" : (bytes, w, h)  — 右键烟花 3 帧
      "scroll_up"         : (bytes, w, h)  — 向上滚动箭头
      "scroll_down"       : (bytes, w, h)  — 向下滚动箭头
    """
    theme = ThemeManager().theme_color

    # 缩放后的尺寸参数
    cs  = max(8, int(CursorOverlay._CURSOR_SIZE * scale + 0.5))
    dot_r    = max(2, int(CursorOverlay._DOT_R * scale + 0.5))
    r_start  = max(4, int(CursorOverlay._BURST_R_START * scale + 0.5))
    r_step   = max(3, int(CursorOverlay._BURST_R_STEP * scale + 0.5))
    line_w   = max(1.0, 2.5 * scale)

    # burst sprite 尺寸：需要容纳最大帧的射线
    max_r = r_start + (_BURST_FRAMES - 1) * r_step + 4
    burst_size = max(16, (max_r + dot_r) * 2 + 4)

    # scroll sprite 尺寸
    arrow_h = max(4, int(9 * scale))
    arrow_w = max(6, int(15 * scale))
    gap     = max(2, int(5 * scale))
    scroll_h = 3 * arrow_h + 2 * gap + 4
    scroll_w = arrow_w + 4

    # 加载 SVG renderer
    renderer = CursorOverlay._svg_renderer
    if renderer is None:
        svg_path = ResourceManager.get_icon_path("鼠标.svg")
        renderer = QSvgRenderer(svg_path)
        CursorOverlay._svg_renderer = renderer

    result = {}

    # ── cursor sprite ──
    def draw_cursor(p, w, h):
        if renderer and renderer.isValid():
            renderer.render(p, QRectF(0, 0, w, h))
    result["cursor"] = (_rasterize_qimage(cs, cs, draw_cursor), cs, cs)

    # ── burst sprites (6 张: left/right × 3帧) ──
    for side, side_name in [(1, "left"), (2, "right")]:
        if side == 1:
            angles = [135, 180, 225, 270, 315]
        else:
            angles = [-45, 0, 45, 90, 135]
        for frame in range(1, _BURST_FRAMES + 1):
            alpha = max(60, 240 - (frame - 1) * 80)
            r_outer = r_start + (frame - 1) * r_step
            r_inner = dot_r + 3

            def draw_burst(p, w, h, _alpha=alpha, _r_outer=r_outer,
                           _r_inner=r_inner, _angles=angles):
                cx, cy = w // 2, h // 2
                color = QColor(theme)
                color.setAlpha(_alpha)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QBrush(color))
                p.drawEllipse(QPointF(cx, cy), dot_r, dot_r)
                p.setPen(QPen(color, line_w, Qt.PenStyle.SolidLine,
                              Qt.PenCapStyle.RoundCap))
                for deg in _angles:
                    a = math.radians(deg)
                    p.drawLine(
                        QPointF(cx + _r_inner * math.cos(a),
                                cy + _r_inner * math.sin(a)),
                        QPointF(cx + _r_outer * math.cos(a),
                                cy + _r_outer * math.sin(a)),
                    )

            key = f"burst_{side_name}_{frame}"
            result[key] = (_rasterize_qimage(burst_size, burst_size, draw_burst),
                           burst_size, burst_size)

    # ── scroll sprites (2 张: up/down) ──
    for scroll_dir, dir_name in [(1, "up"), (-1, "down")]:
        def draw_scroll(p, w, h, _scroll=scroll_dir):
            ax, ay = w // 2, h // 2
            p.setPen(Qt.PenStyle.NoPen)
            for i in range(3):
                offset = (i - 1) * (arrow_h + gap)
                alpha_val = (80 + i * 70) if _scroll == 1 else (220 - i * 70)
                color = QColor(theme)
                color.setAlpha(max(60, alpha_val))
                p.setBrush(QBrush(color))
                path = QPainterPath()
                if _scroll == 1:
                    path.moveTo(ax, ay + offset - arrow_h)
                    path.lineTo(ax - arrow_w // 2, ay + offset)
                    path.lineTo(ax + arrow_w // 2, ay + offset)
                else:
                    path.moveTo(ax, ay + offset + arrow_h)
                    path.lineTo(ax - arrow_w // 2, ay + offset)
                    path.lineTo(ax + arrow_w // 2, ay + offset)
                path.closeSubpath()
                p.drawPath(path)

        result[f"scroll_{dir_name}"] = (
            _rasterize_qimage(scroll_w, scroll_h, draw_scroll),
            scroll_w, scroll_h,
        )

    return result


def compute_burst_states(cursors: list) -> list:
    """
    遍历帧序列推断每帧的 burst 动画状态。

    Parameters
    ----------
    cursors : list[CursorSnapshot | None]

    Returns
    -------
    list of (burst_frame: int, burst_side: int)
      burst_frame: 0=无, 1/2/3=动画帧
      burst_side:  1=左键, 2=右键
    """
    n = len(cursors)
    states = [(0, 0)] * n
    burst_frame = 0
    burst_side = 0
    prev_press = 0

    for i in range(n):
        cur = cursors[i]
        cur_press = cur.press if cur and cur.visible else 0

        if cur_press != 0 and prev_press == 0:
            burst_side = cur_press
            burst_frame = 1
        elif cur_press != 0:
            burst_frame = 1
        elif cur_press == 0 and burst_frame > 0:
            burst_frame += 1
            if burst_frame > _BURST_FRAMES:
                burst_frame = 0
                burst_side = 0

        states[i] = (burst_frame, burst_side)
        prev_press = cur_press

    return states
 