# -*- coding: utf-8 -*-
"""
第4页 — 智能选区 & OCR

上半部：仿桌面场景，多窗口互相重叠，鼠标自动游走并高亮选中窗口
下半部：智能选区开关说明
"""

from PySide6.QtWidgets import QVBoxLayout, QWidget, QSizePolicy
from PySide6.QtCore import Qt, QTimer, QPointF, QRectF
from PySide6.QtGui import (
    QPainter, QColor, QPen, QFont, QPolygonF,
    QLinearGradient, QPainterPath, QBrush, QRadialGradient
)
from core import safe_event
from core.i18n import make_tr

if __package__:
    from .base_page import BasePage, IllustrationArea, ToggleSwitch, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_ILLUS
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import BasePage, IllustrationArea, ToggleSwitch, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_ILLUS


_tr = make_tr("WelcomeWizard")


# ── 插画区 ──────────────────────────────────────────────
class _SmartSelectIllus(IllustrationArea):
    def _build_content(self):
        self.setStyleSheet("background: transparent; border: none;")
        self._layout.setContentsMargins(0, 0, 0, 0)
        canvas = _DesktopAnim(self)
        canvas.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self._layout.addWidget(canvas)


# ── 窗口定义：(相对于画布的比例坐标 rx,ry,rw,rh, 标题色, 标题文字, 内容色) ───
# 坐标和尺寸用 0~1 比例，运行时乘以实际宽高，支持任意画布大小
_WINS = [
    # rx,   ry,   rw,   rh,   title_color,   label,        body_color
    (0.04, 0.08, 0.44, 0.58, "#4A90D9",  "Browser",    "#F0F6FF"),
    (0.30, 0.22, 0.42, 0.55, "#5C6BC0",  "Code Editor","#F5F4FF"),
    (0.52, 0.06, 0.40, 0.50, "#43A047",  "File Manager","#F0FFF4"),
    (0.10, 0.50, 0.38, 0.44, "#F57C00",  "Terminal",   "#FFFDF0"),
    (0.54, 0.44, 0.42, 0.50, "#E53935",  "Settings",   "#FFF5F5"),
]

# 鼠标在每个窗口内停留的相对位置（窗口内比例偏移）
_CURSOR_IN_WIN = [
    (0.55, 0.55),
    (0.50, 0.50),
    (0.60, 0.45),
    (0.45, 0.55),
    (0.50, 0.50),
]

_DWELL_FRAMES = 100   # 停留帧数
_MOVE_FRAMES  = 35    # 移动帧数


class _DesktopAnim(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        self._frame = 0
        self._focus = 0
        self._next  = 1
        self._pulse = 0.0
        self._cx = 0.0   # 光标位置（画布绝对坐标）
        self._cy = 0.0
        self._initialized = False  # 等首次 resize 后再初始化光标位置

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(16)

    def _win_rect(self, idx):
        """把比例坐标换算为当前画布像素坐标"""
        W, H = self.width(), self.height()
        rx, ry, rw, rh = _WINS[idx][:4]
        return (int(rx * W), int(ry * H), int(rw * W), int(rh * H))

    def _cursor_abs(self, idx):
        """返回第 idx 个窗口内光标的绝对坐标"""
        x, y, w, h = self._win_rect(idx)
        cfx, cfy = _CURSOR_IN_WIN[idx]
        return x + cfx * w, y + cfy * h

    @safe_event
    def showEvent(self, event):
        super().showEvent(event)
        if not self._initialized and self.width() > 10:
            self._cx, self._cy = self._cursor_abs(0)
            self._initialized = True

    def _tick(self):
        cycle = _DWELL_FRAMES + _MOVE_FRAMES
        phase = self._frame % cycle

        if phase < _DWELL_FRAMES:
            t = phase / _DWELL_FRAMES
            # 三角波脉冲
            self._pulse = 0.4 + 0.6 * (1.0 - abs(2 * t - 1))
        else:
            self._pulse = 0.0
            move_t = (phase - _DWELL_FRAMES) / _MOVE_FRAMES
            ease = move_t * move_t * (3 - 2 * move_t)
            if self._initialized:
                fx, fy = self._cursor_abs(self._focus)
                nx, ny = self._cursor_abs(self._next)
                self._cx = fx + (nx - fx) * ease
                self._cy = fy + (ny - fy) * ease
            if phase == cycle - 1:
                self._focus = self._next
                self._next  = (self._next + 1) % len(_WINS)

        self._frame += 1
        self.update()

    @safe_event
    def resizeEvent(self, event):
        super().resizeEvent(event)
        if not self._initialized and self.width() > 10:
            self._cx, self._cy = self._cursor_abs(0)
            self._initialized = True
        elif self._initialized:
            # 重新对齐光标到当前焦点窗口
            self._cx, self._cy = self._cursor_abs(self._focus)

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.TextAntialiasing)

        W, H = self.width(), self.height()

        # ── 桌面背景：深蓝渐变 ────────────────────────
        bg = QLinearGradient(0, 0, W, H)
        bg.setColorAt(0.0, QColor("#1A2A4A"))
        bg.setColorAt(0.6, QColor("#1E3557"))
        bg.setColorAt(1.0, QColor("#162038"))
        p.fillRect(0, 0, W, H, bg)

        # 细腻网格纹（模拟桌面质感）
        p.setPen(QPen(QColor(255, 255, 255, 6), 1))
        step = 28
        for gx in range(0, W, step):
            p.drawLine(gx, 0, gx, H)
        for gy in range(0, H, step):
            p.drawLine(0, gy, W, gy)

        # ── 固定 z-order 绘制窗口（不随 focus 改变堆叠顺序）─────────
        for idx in range(len(_WINS)):
            # focused 一律 False：避免选中时改变窗口视觉/阴影
            self._draw_window(p, idx, focused=False)

        # ── 仅绘制蓝色虚线高亮（overlay，不改变窗口层级）────────────
        if self._pulse > 0:
            x, y, w, h = self._win_rect(self._focus)
            self._draw_focus_border(p, x, y, w, h)

        # ── 光标 ───────────────────────────────────────
        if self._initialized:
            self._draw_cursor(p, int(self._cx), int(self._cy))

    def _draw_window(self, p, idx, focused):
        x, y, w, h = self._win_rect(idx)
        _, _, _, _, title_color, label, body_color = _WINS[idx]
        title_h = max(20, int(h * 0.12))

        # ── 阴影 ──────────────────────────────────────
        shadow_r = 10 if focused else 4
        for s in range(shadow_r, 0, -1):
            alpha = int(80 * (shadow_r - s + 1) / shadow_r) if focused else int(40 * (shadow_r - s + 1) / shadow_r)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 0, 0, alpha))
            p.drawRoundedRect(x + s, y + s, w, h, 8, 8)

        # ── 窗口体 ────────────────────────────────────
        p.setBrush(QColor(body_color) if focused else QColor(body_color).darker(105))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(x, y, w, h, 8, 8)

        # ── 标题栏渐变 ────────────────────────────────
        tg = QLinearGradient(x, y, x, y + title_h)
        base_c = QColor(title_color)
        tg.setColorAt(0, base_c.lighter(115) if focused else base_c.lighter(80))
        tg.setColorAt(1, base_c if focused else base_c.darker(120))

        path = QPainterPath()
        path.addRoundedRect(QRectF(x, y, w, title_h + 4), 8, 8)
        path.addRect(QRectF(x, y + title_h - 4, w, 8))
        p.fillPath(path, QBrush(tg))

        # 交通灯
        dot_y = y + title_h // 2
        colors = ["#FF5F57", "#FFBD2E", "#28C840"] if focused else ["#774444", "#777744", "#447744"]
        for di, dc in enumerate(colors):
            p.setBrush(QColor(dc))
            p.setPen(Qt.PenStyle.NoPen)
            dot_r = 5 if focused else 4
            p.drawEllipse(x + 8 + di * 13, dot_y - dot_r, dot_r * 2, dot_r * 2)

        # 标题文字
        f = QFont("Microsoft YaHei", max(7, int(title_h * 0.42)))
        f.setBold(focused)
        p.setFont(f)
        p.setPen(QColor(255, 255, 255, 230 if focused else 160))
        p.drawText(x + 48, y, w - 56, title_h,
                   Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, label)

        # ── 内容区模拟（代码行 / 文件图标 / 色块）──────
        self._draw_content(p, x, y + title_h, w, h - title_h, idx, focused)

        # 注意：不在这里画 focus border，避免“选中窗口”被当作置顶重绘

    def _draw_content(self, p, x, y, w, h, idx, focused):
        """根据窗口类型画不同的内容示意"""
        alpha = 200 if focused else 120
        p.setClipRect(x, y, w, h)

        if idx == 0:  # Browser：地址栏 + 色块卡片
            bar_h = int(h * 0.15)
            p.setBrush(QColor(220, 230, 245, alpha))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawRoundedRect(x + 6, y + 4, w - 12, bar_h - 4, 4, 4)
            card_colors = ["#BBD6F7", "#C8E6C9", "#FFE0B2"]
            cw = (w - 18) // 3
            for ci, cc in enumerate(card_colors):
                p.setBrush(QColor(cc).lighter(105 if focused else 95))
                p.drawRoundedRect(x + 6 + ci * (cw + 3), y + bar_h + 4, cw, h - bar_h - 10, 5, 5)

        elif idx == 1:  # Code Editor：行号 + 色块代码
            line_colors = ["#A5D6A7", "#90CAF9", "#CE93D8", "#80DEEA", "#FFCC80", "#EF9A9A"]
            lh = max(6, int(h * 0.11))
            for li in range(min(6, int(h // (lh + 4)))):
                lw = int((0.35 + (li * 0.13) % 0.5) * (w - 28))
                p.setBrush(QColor(line_colors[li % len(line_colors)]).darker(110 if not focused else 100))
                p.setPen(Qt.PenStyle.NoPen)
                p.drawRoundedRect(x + 24, y + 6 + li * (lh + 4), lw, lh, 2, 2)
                # 行号
                p.setPen(QColor(100, 120, 150, alpha))
                p.setFont(QFont("Courier New", max(6, lh - 2)))
                p.drawText(x + 4, y + 6 + li * (lh + 4), 18, lh,
                           Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                           str(li + 1))

        elif idx == 2:  # File Manager：文件图标网格
            icons = ["📄", "📁", "🖼", "📄", "📁", "📄"]
            iw = (w - 8) // 3
            ih = (h - 4) // 2
            for fi, ico in enumerate(icons):
                fx = x + 4 + (fi % 3) * iw
                fy = y + 4 + (fi // 3) * ih
                p.setBrush(QColor(255, 255, 255, 60 if focused else 30))
                p.setPen(Qt.PenStyle.NoPen)
                p.drawRoundedRect(fx + 2, fy + 2, iw - 4, ih - 4, 4, 4)
                p.setFont(QFont("Segoe UI Emoji", max(10, int(iw * 0.32))))
                p.setPen(QColor(0, 0, 0, 200))
                p.drawText(fx + 2, fy + 2, iw - 4, ih - 4, Qt.AlignmentFlag.AlignCenter, ico)

        elif idx == 3:  # Terminal：命令行文字
            p.setBrush(QColor(20, 20, 20, 200 if focused else 160))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawRect(x, y, w, h)
            lines = ["$ git status", "> On branch main", "> nothing to commit", "$ █"]
            lh = max(8, int(h * 0.18))
            for li, line in enumerate(lines):
                color = QColor("#A8FF78") if line.startswith("$") else QColor("#88BBFF")
                p.setPen(color if focused else color.darker(150))
                p.setFont(QFont("Courier New", max(6, lh - 4)))
                p.drawText(x + 6, y + 4 + li * (lh + 2), w - 8, lh,
                           Qt.AlignmentFlag.AlignVCenter, line)

        elif idx == 4:  # Settings：开关行
            sw_colors = [ACCENT, "#4CAF50", "#9C27B0"]
            row_h = int(h // 3)
            for ri in range(3):
                ry2 = y + ri * row_h + 4
                p.setBrush(QColor(240, 240, 240, alpha))
                p.setPen(Qt.PenStyle.NoPen)
                p.drawRoundedRect(x + 6, ry2, w - 12, row_h - 6, 4, 4)
                # 模拟开关
                sw_on = ri % 2 == 0
                sw_x = x + w - 34
                sw_y = ry2 + (row_h - 12) // 2
                p.setBrush(QColor(sw_colors[ri] if sw_on else "#CCCCCC"))
                p.drawRoundedRect(sw_x, sw_y, 24, 12, 6, 6)
                circle_x = sw_x + (12 if sw_on else 0)
                p.setBrush(QColor("white"))
                p.drawEllipse(circle_x + 1, sw_y + 1, 10, 10)

        p.setClipping(False)

    def _draw_focus_border(self, p, x, y, w, h):
        pulse = self._pulse

        # 蓝色半透明遮罩
        p.setBrush(QColor(33, 150, 243, int(pulse * 25)))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(x, y, w, h, 8, 8)

        # 脉冲虚线外框
        pen = QPen(QColor(33, 150, 243, int(80 + pulse * 175)), 2.0)
        pen.setStyle(Qt.PenStyle.DashLine)
        pen.setDashPattern([5, 3])
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        m = 3
        p.drawRoundedRect(x - m, y - m, w + m * 2, h + m * 2, 10, 10)

        # 四角 L 形装饰
        pen2 = QPen(QColor(ACCENT), 2.5)
        pen2.setCapStyle(Qt.PenCapStyle.RoundCap)
        p.setPen(pen2)
        sz = 12
        for (ox, oy, sx, sy) in [
            (x - m,     y - m,      sz,  0),
            (x - m,     y - m,       0, sz),
            (x + w + m - sz, y - m, sz,  0),
            (x + w + m, y - m,       0, sz),
            (x - m,     y + h + m - sz, 0, sz),
            (x - m,     y + h + m,  sz,  0),
            (x + w + m - sz, y + h + m, sz, 0),
            (x + w + m, y + h + m - sz,  0, sz),
        ]:
            p.drawLine(int(ox), int(oy), int(ox + sx), int(oy + sy))

    def _draw_cursor(self, p, x, y):
        pts = [
            QPointF(0,   0),
            QPointF(0,  20),
            QPointF(4.5, 15),
            QPointF(8,  22),
            QPointF(10.5, 21),
            QPointF(7,  14),
            QPointF(12, 14),
        ]
        # 投影
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(0, 0, 0, 55))
        shadow = QPolygonF([QPointF(x + pt.x() + 2, y + pt.y() + 2) for pt in pts])
        p.drawPolygon(shadow)
        # 白色箭头主体
        p.setBrush(QColor(255, 255, 255, 245))
        p.setPen(QPen(QColor(40, 40, 40, 200), 1.2))
        arrow = QPolygonF([QPointF(x + pt.x(), y + pt.y()) for pt in pts])
        p.drawPolygon(arrow)


# ── 页面主体 ────────────────────────────────────────────
class SmartSelectPage(BasePage):
    """第4页：智能选区"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title="智能选区",
            subtitle="截图时将鼠标悬停在任意窗口上，自动识别并高亮其边界。\n"
                     "单击即可精准截取该窗口；也可以像平时一样拖拽框选任意区域。",
            parent=parent,
        )

    def _create_illustration(self):
        return _SmartSelectIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        self._smart_toggle = ToggleSwitch()
        self._smart_toggle.setChecked(self._config.get_smart_selection())
        row1, self._row1_lbl, self._row1_desc = self._make_setting_row_with_refs(
            "🪟 启用智能选区",
            self._smart_toggle,
            "开启：悬停自动高亮窗口，单击即完成截取。\n"
            "关闭：恢复手动拖拽框选模式。"
        )
        layout.addWidget(row1)

    def retranslate(self):
        self.title_label.setText(_tr("智能选区"))
        self.subtitle_label.setText(_tr(
            "截图时将鼠标悬停在任意窗口上，自动识别并高亮其边界。\n"
            "单击即可精准截取该窗口；也可以像平时一样拖拽框选任意区域。"))
        if hasattr(self, "_row1_lbl") and self._row1_lbl:
            self._row1_lbl.setText(_tr("🪟 启用智能选区"))
        if hasattr(self, "_row1_desc") and self._row1_desc:
            self._row1_desc.setText(_tr(
                "开启：悬停自动高亮窗口，单击即完成截取。\n"
                "关闭：恢复手动拖拽框选模式。"))

    def save(self):
        self._config.set_smart_selection(self._smart_toggle.isChecked())



if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(3)   # 跳到第4页
    w._update_nav()
    w.show()
    sys.exit(app.exec()) 