"""
自定义 HSV 颜色选择对话框
- HSV 矩形饱和/明度选色块
- 色相滑条 + 透明度滑条
- 屏幕吸管取色
- Hex / RGB 输入框 + 透明度百分比
"""
from __future__ import annotations

from PySide6.QtWidgets import (
    QDialog, QWidget, QVBoxLayout, QHBoxLayout, QLineEdit,
    QPushButton, QComboBox, QApplication, QSizePolicy,
)
from PySide6.QtCore import Qt, Signal, QPoint
from PySide6.QtGui import (
    QColor, QPainter, QPen, QBrush, QLinearGradient, QCursor, QPainterPath,
)
from core import safe_event

# 如果你没有这个常量，可以暂时用默认字体替换
# from core.constants import CSS_FONT_FAMILY
CSS_FONT_FAMILY = '"Microsoft YaHei", "Segoe UI", sans-serif'


# ─────────────────────────────────────────────
# 子组件
# ─────────────────────────────────────────────

class _HsvCanvas(QWidget):
    """HSV 饱和/明度选色块"""
    sv_changed = Signal(float, float)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._hue = 0.0
        self._s = 1.0
        self._v = 1.0
        self._dragging = False
        self.setFixedSize(240, 160)
        self.setCursor(Qt.CursorShape.CrossCursor)

    def set_hue(self, h: float):
        self._hue = max(0.0, min(360.0, float(h)))
        self.update()

    def set_sv(self, s: float, v: float):
        self._s = max(0.0, min(1.0, float(s)))
        self._v = max(0.0, min(1.0, float(v)))
        self.update()

    @safe_event
    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()

        # 圆角裁剪
        path = QPainterPath()
        path.addRoundedRect(0, 0, w, h, 6, 6)
        p.setClipPath(path)

        # 底层：纯色相色
        p.fillRect(0, 0, w, h, QColor.fromHsvF(self._hue / 360.0, 1.0, 1.0))

        # 白→透明
        g1 = QLinearGradient(0, 0, w, 0)
        g1.setColorAt(0.0, QColor(255, 255, 255, 255))
        g1.setColorAt(1.0, QColor(255, 255, 255, 0))
        p.fillRect(0, 0, w, h, g1)

        # 透明→黑
        g2 = QLinearGradient(0, 0, 0, h)
        g2.setColorAt(0.0, QColor(0, 0, 0, 0))
        g2.setColorAt(1.0, QColor(0, 0, 0, 255))
        p.fillRect(0, 0, w, h, g2)

        # 光标圆圈
        cx = int(self._s * w)
        cy = int((1.0 - self._v) * h)
        
        # 边界限制防止圆圈画到外面
        cx = max(6, min(w - 6, cx))
        cy = max(6, min(h - 6, cy))

        p.setClipping(False) # 恢复裁剪以绘制边框
        p.setPen(QPen(QColor(255, 255, 255), 2.0))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(QPoint(cx, cy), 5, 5)
        p.end()

    def _apply_pos(self, x: int, y: int):
        s = max(0.0, min(1.0, x / self.width()))
        v = max(0.0, min(1.0, 1.0 - y / self.height()))
        self._s, self._v = s, v
        self.update()
        self.sv_changed.emit(s, v)

    @safe_event
    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._apply_pos(e.pos().x(), e.pos().y())

    @safe_event
    def mouseMoveEvent(self, e):
        if self._dragging:
            self._apply_pos(e.pos().x(), e.pos().y())

    @safe_event
    def mouseReleaseEvent(self, _e):
        self._dragging = False


class _GradientSlider(QWidget):
    """横向渐变滑条"""
    value_changed = Signal(float)

    _HUE_STOPS = [
        (0.000, "#FF0000"), (0.167, "#FFFF00"), (0.333, "#00FF00"),
        (0.500, "#00FFFF"), (0.667, "#0000FF"), (0.833, "#FF00FF"),
        (1.000, "#FF0000"),
    ]

    def __init__(self, mode: str = "hue", parent=None):
        super().__init__(parent)
        self._mode = mode
        self._value = 0.0
        self._base_color = QColor(255, 0, 0)
        self._dragging = False
        self.setFixedHeight(14)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def set_value(self, v: float):
        self._value = max(0.0, min(1.0, float(v)))
        self.update()

    def set_base_color(self, color: QColor):
        self._base_color = QColor(color)
        self.update()

    @property
    def value(self) -> float:
        return self._value

    @safe_event
    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        radius = h // 2

        # 给滑块留出边距
        margin = 6 
        track_w = w - margin * 2

        path = QPainterPath()
        path.addRoundedRect(margin, 0, track_w, h, radius, radius)
        p.setClipPath(path)

        if self._mode == "alpha":
            tile = 4
            for row in range(0, h, tile):
                for col in range(0, w, tile):
                    c = QColor(200, 200, 200) if (row // tile + col // tile) % 2 == 0 else QColor(255, 255, 255)
                    p.fillRect(col, row, tile, tile, c)

        grad = QLinearGradient(margin, 0, w - margin, 0)
        if self._mode == "hue":
            for pos, hex_str in self._HUE_STOPS:
                grad.setColorAt(pos, QColor(hex_str))
        else:
            opaque = QColor(self._base_color)
            opaque.setAlpha(255)
            transparent = QColor(self._base_color)
            transparent.setAlpha(0)
            grad.setColorAt(0.0, transparent)
            grad.setColorAt(1.0, opaque)
            
        p.fillRect(margin, 0, track_w, h, grad)
        p.setClipping(False)

        # 边框
        p.setPen(QPen(QColor(210, 210, 210), 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRoundedRect(margin, 0, track_w, h, radius, radius)

        # 游标 (仿截图中的白底红边圆圈)
        cx = int(self._value * track_w) + margin
        cy = h // 2
        p.setPen(QPen(QColor(200, 40, 40) if self._mode == "hue" else QColor(150, 150, 150), 2.5))
        p.setBrush(QBrush(QColor(255, 255, 255)))
        p.drawEllipse(QPoint(cx, cy), 6, 6)
        p.end()

    def _apply_x(self, x: int):
        margin = 6
        track_w = self.width() - margin * 2
        v = max(0.0, min(1.0, (x - margin) / track_w))
        self._value = v
        self.update()
        self.value_changed.emit(v)

    @safe_event
    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._dragging = True
            self._apply_x(e.pos().x())

    @safe_event
    def mouseMoveEvent(self, e):
        if self._dragging:
            self._apply_x(e.pos().x())

    @safe_event
    def mouseReleaseEvent(self, _e):
        self._dragging = False


class _ScreenColorTracker(QWidget):
    """全屏透明覆盖层取色器：覆盖所有屏幕，直接接收鼠标/键盘事件"""
    color_picked = Signal(QColor)
    color_hover = Signal(QColor)
    cancelled = Signal()

    POLL_MS = 30  # 轮询间隔

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self.setMouseTracking(True)

        self._active = False
        self._current_color = QColor(0, 0, 0)

        from PySide6.QtCore import QTimer
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll_cursor)

    def start(self):
        self._active = True
        # 计算覆盖所有屏幕的矩形
        from PySide6.QtCore import QRect
        combined = QRect()
        for screen in QApplication.screens():
            combined = combined.united(screen.geometry())
        self.setGeometry(combined)
        self.show()
        self.raise_()
        self.activateWindow()
        self.setFocus()
        self._timer.start(self.POLL_MS)
        self._poll_cursor()

    def stop(self):
        self._active = False
        self._timer.stop()
        self.close()

    def _poll_cursor(self):
        pos = QCursor.pos()
        screen = QApplication.screenAt(pos)
        if not screen:
            screen = QApplication.primaryScreen()
        geo = screen.geometry()
        local = pos - geo.topLeft()
        pix = screen.grabWindow(0, local.x(), local.y(), 1, 1)
        img = pix.toImage()
        if not img.isNull() and img.width() > 0 and img.height() > 0:
            self._current_color = QColor(img.pixel(0, 0))
            self.color_hover.emit(self._current_color)

    @safe_event
    def paintEvent(self, _event):
        # alpha=1 几乎完全透明，但足以让窗口接收鼠标事件
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(0, 0, 0, 1))
        p.end()

    @safe_event
    def mousePressEvent(self, event):
        if not self._active:
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self.color_picked.emit(QColor(self._current_color))
            self.stop()
        elif event.button() == Qt.MouseButton.RightButton:
            self.cancelled.emit()
            self.stop()

    @safe_event
    def keyPressEvent(self, event):
        if not self._active:
            return
        if event.key() == Qt.Key.Key_Escape:
            self.cancelled.emit()
            self.stop()


# ─────────────────────────────────────────────
# 主对话框
# ─────────────────────────────────────────────

_INPUT_STYLE = f"QLineEdit {{ border: 1px solid #d0d0d0; border-radius: 4px; font-family: {CSS_FONT_FAMILY}; font-size: 12px; padding: 0 4px; }}"
_COMBO_STYLE = f"""
    QComboBox {{ border: 1px solid #d0d0d0; border-radius: 4px; padding: 2px 6px; font-family: {CSS_FONT_FAMILY}; font-size: 12px; }}
    QComboBox::drop-down {{ border: none; width: 18px; }}
"""


class ColorPickerDialog(QDialog):
    def __init__(self, initial_color: QColor | None = None, show_alpha: bool = True, parent=None):
        super().__init__(parent)
        self._show_alpha = show_alpha
        self._color = QColor(initial_color) if initial_color and initial_color.isValid() else QColor(255, 0, 0)
        self._updating = False
        self._dropper_active = False
        self._tracker: _ScreenColorTracker | None = None
        self._saved_color: QColor | None = None

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Dialog
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self.setStyleSheet("QDialog { background-color: #ffffff; }")
        self.setFixedWidth(300)

        self._build_ui()
        self._connect_signals()
        self._load_color(self._color)
        self._switch_input_mode()

        # 用于拖拽无边框窗口
        self._drag_pos = None

    def selectedColor(self) -> QColor:
        return QColor(self._color)

    # ── 拖拽标题栏 ──
    @safe_event
    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton and e.pos().y() < 30:
            self._drag_pos = e.globalPosition().toPoint() - self.pos()
        super().mousePressEvent(e)

    @safe_event
    def mouseMoveEvent(self, e):
        if self._drag_pos is not None:
            self.move(e.globalPosition().toPoint() - self._drag_pos)
        super().mouseMoveEvent(e)

    @safe_event
    def mouseReleaseEvent(self, e):
        self._drag_pos = None
        super().mouseReleaseEvent(e)

    # ── 构建 UI ──
    def _build_ui(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(8)

        # 1. HSV 画布（撑满宽度）
        self._canvas = _HsvCanvas()
        self._canvas.setFixedSize(284, 160)
        root.addWidget(self._canvas)

        # 2. 滑条区域：吸管 + 色相 + alpha
        slider_row = QHBoxLayout()
        slider_row.setSpacing(8)

        self._dropper_btn = QPushButton("💉")
        self._dropper_btn.setFixedSize(32, 32)
        self._dropper_btn.setToolTip("屏幕取色")
        self._dropper_btn.setCheckable(True)
        self._update_dropper_style(False)
        slider_row.addWidget(self._dropper_btn)

        slider_col = QVBoxLayout()
        slider_col.setSpacing(6)
        self._hue_slider = _GradientSlider("hue")
        self._alpha_slider = _GradientSlider("alpha")
        slider_col.addWidget(self._hue_slider)
        if self._show_alpha:
            slider_col.addWidget(self._alpha_slider)
        slider_row.addLayout(slider_col)
        root.addLayout(slider_row)

        # 3. 格式选择 + 输入
        inputs_row = QHBoxLayout()
        inputs_row.setSpacing(4)

        self._type_combo = QComboBox()
        self._type_combo.addItems(["Hex", "RGB", "HSV"])
        self._type_combo.setFixedSize(60, 28)
        self._type_combo.setStyleSheet(_COMBO_STYLE)
        inputs_row.addWidget(self._type_combo)

        # Hex 输入
        self._hex_input = QLineEdit()
        self._hex_input.setFixedHeight(28)
        self._hex_input.setMaxLength(7)
        self._hex_input.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._hex_input.setStyleSheet(_INPUT_STYLE)
        inputs_row.addWidget(self._hex_input, 1)

        # RGB / HSV 三个输入框
        self._val_inputs = []
        for _ in range(3):
            inp = QLineEdit()
            inp.setFixedHeight(28)
            inp.setMaxLength(3)
            inp.setAlignment(Qt.AlignmentFlag.AlignCenter)
            inp.setStyleSheet(_INPUT_STYLE)
            inputs_row.addWidget(inp, 1)
            self._val_inputs.append(inp)

        # Alpha 百分比
        self._alpha_input = QLineEdit()
        self._alpha_input.setFixedSize(48, 28)
        self._alpha_input.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._alpha_input.setStyleSheet(_INPUT_STYLE)
        inputs_row.addWidget(self._alpha_input)
        if not self._show_alpha:
            self._alpha_input.hide()

        root.addLayout(inputs_row)

        # 4. 按钮
        btn_row = QHBoxLayout()
        btn_row.addStretch()
        self._ok_btn = QPushButton("确认")
        self._ok_btn.setFixedSize(60, 28)
        self._ok_btn.setStyleSheet(f"""
            QPushButton {{ background-color: #c81623; color: white; border: none; border-radius: 4px; font-family: {CSS_FONT_FAMILY}; font-size: 12px; }}
            QPushButton:hover {{ background-color: #e31b28; }}
        """)
        self._cancel_btn = QPushButton("取消")
        self._cancel_btn.setFixedSize(60, 28)
        self._cancel_btn.setStyleSheet(f"""
            QPushButton {{ background-color: #fff; color: #333; border: 1px solid #d0d0d0; border-radius: 4px; font-family: {CSS_FONT_FAMILY}; font-size: 12px; }}
            QPushButton:hover {{ background-color: #f5f5f5; }}
        """)
        btn_row.addWidget(self._ok_btn)
        btn_row.addWidget(self._cancel_btn)
        root.addLayout(btn_row)

    # ── 信号 ──
    def _connect_signals(self):
        self._canvas.sv_changed.connect(self._on_sv)
        self._hue_slider.value_changed.connect(self._on_hue)
        self._alpha_slider.value_changed.connect(self._on_alpha)
        self._dropper_btn.clicked.connect(self._toggle_dropper)
        self._type_combo.currentIndexChanged.connect(self._switch_input_mode)
        self._hex_input.textChanged.connect(self._on_hex_input)
        self._alpha_input.textChanged.connect(self._on_alpha_input)
        for inp in self._val_inputs:
            inp.textChanged.connect(self._on_val_input)
        self._ok_btn.clicked.connect(self.accept)
        self._cancel_btn.clicked.connect(self.reject)

    # ── 颜色加载/同步 ──
    def _load_color(self, color: QColor):
        if self._updating:
            return
        self._updating = True
        h, s, v, a = color.hsvHueF(), color.saturationF(), color.valueF(), color.alphaF()
        if h < 0:
            h = 0.0
        self._canvas.set_hue(h * 360)
        self._canvas.set_sv(s, v)
        self._hue_slider.set_value(h)
        self._alpha_slider.set_base_color(color)
        self._alpha_slider.set_value(a)
        self._update_text_inputs()
        self._updating = False

    def _on_sv(self, s: float, v: float):
        if self._updating:
            return
        h = self._hue_slider.value
        a = self._alpha_slider.value if self._show_alpha else 1.0
        self._color = QColor.fromHsvF(h, s, v, a)
        self._sync_visuals()

    def _on_hue(self, h: float):
        if self._updating:
            return
        s, v = self._canvas._s, self._canvas._v
        a = self._alpha_slider.value if self._show_alpha else 1.0
        self._color = QColor.fromHsvF(h, s, v, a)
        self._updating = True
        self._canvas.set_hue(h * 360)
        self._alpha_slider.set_base_color(self._color)
        self._update_text_inputs()
        self._updating = False

    def _on_alpha(self, a: float):
        if self._updating:
            return
        self._color.setAlphaF(a)
        self._sync_visuals()

    def _sync_visuals(self):
        self._updating = True
        self._alpha_slider.set_base_color(self._color)
        self._update_text_inputs()
        self._updating = False

    # ── 输入框模式切换 ──
    def _switch_input_mode(self):
        mode = self._type_combo.currentText()
        if mode == "Hex":
            self._hex_input.show()
            for inp in self._val_inputs:
                inp.hide()
        else:
            self._hex_input.hide()
            for inp in self._val_inputs:
                inp.show()
        self._update_text_inputs()

    def _update_text_inputs(self):
        mode = self._type_combo.currentText()
        c = self._color
        if mode == "Hex":
            self._hex_input.setText("#{:02X}{:02X}{:02X}".format(c.red(), c.green(), c.blue()))
        elif mode == "RGB":
            self._val_inputs[0].setText(str(c.red()))
            self._val_inputs[1].setText(str(c.green()))
            self._val_inputs[2].setText(str(c.blue()))
        elif mode == "HSV":
            h = c.hsvHue()
            if h < 0:
                h = 0
            self._val_inputs[0].setText(str(h))
            self._val_inputs[1].setText(str(c.saturation()))
            self._val_inputs[2].setText(str(c.value()))
        self._alpha_input.setText(f"{int(c.alphaF() * 100)}%")

    # ── 输入回调 ──
    def _on_hex_input(self):
        if self._updating:
            return
        text = self._hex_input.text().strip()
        if not text.startswith("#"):
            text = "#" + text
        # 等到完整6位hex再应用，避免输入中途频繁切换
        if len(text) != 7:
            return
        color = QColor(text)
        if color.isValid():
            color.setAlpha(self._color.alpha())
            self._color = color
            self._load_color(color)

    def _on_val_input(self):
        if self._updating:
            return
        mode = self._type_combo.currentText()
        try:
            vals = [int(inp.text()) for inp in self._val_inputs]
        except ValueError:
            return
        if mode == "RGB":
            r, g, b = [max(0, min(255, v)) for v in vals]
            color = QColor(r, g, b, self._color.alpha())
        elif mode == "HSV":
            h = max(0, min(359, vals[0]))
            s = max(0, min(255, vals[1]))
            v = max(0, min(255, vals[2]))
            color = QColor.fromHsv(h, s, v, self._color.alpha())
        else:
            return
        if color.isValid():
            self._color = color
            self._load_color(color)

    def _on_alpha_input(self):
        if self._updating:
            return
        text = self._alpha_input.text().strip().rstrip('%')
        if not text:
            return
        try:
            pct = max(0, min(100, int(text)))
        except ValueError:
            return
        self._color.setAlphaF(pct / 100.0)
        self._load_color(self._color)

    # ── 吸管（切换模式）──
    def _update_dropper_style(self, active: bool):
        if active:
            self._dropper_btn.setStyleSheet("""
                QPushButton { font-size: 14px; background: #333; color: white;
                    border: 1px solid #333; border-radius: 6px; }
            """)
        else:
            self._dropper_btn.setStyleSheet("""
                QPushButton { font-size: 14px; background: #fff;
                    border: 1px solid #e0e0e0; border-radius: 6px; }
                QPushButton:hover { background: #f5f5f5; }
            """)

    def _toggle_dropper(self):
        if self._dropper_active:
            self._stop_dropper(cancel=True)
        else:
            self._start_dropper()

    def _start_dropper(self):
        self._dropper_active = True
        self._dropper_btn.setChecked(True)
        self._update_dropper_style(True)
        self._saved_color = QColor(self._color)
        self._tracker = _ScreenColorTracker()
        self._tracker.color_hover.connect(self._on_dropper_hover)
        self._tracker.color_picked.connect(self._on_dropper_pick)
        self._tracker.cancelled.connect(lambda: self._stop_dropper(cancel=True))
        self._tracker.start()

    def _stop_dropper(self, cancel: bool = False):
        self._dropper_active = False
        self._dropper_btn.setChecked(False)
        self._update_dropper_style(False)
        if self._tracker:
            self._tracker.stop()
            self._tracker = None
        if cancel and self._saved_color is not None:
            self._color = self._saved_color
            self._load_color(self._color)
        self._saved_color = None

    def _on_dropper_hover(self, color: QColor):
        if color.isValid():
            self._color = color
            self._load_color(color)

    def _on_dropper_pick(self, color: QColor):
        if color.isValid():
            self._color = color
            self._load_color(color)
        self._stop_dropper(cancel=False)

# 仅供独立测试运行使用
# if __name__ == "__main__":
#  import sys
#  app = QApplication(sys.argv)
#  dlg = ColorPickerDialog()
#  if dlg.exec():
#      pass 