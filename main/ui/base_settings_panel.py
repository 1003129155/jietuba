"""
设置面板基类
提供通用的颜色、大小、透明度选择组件
统一为箭头/文字风格布局
"""
import random

from PySide6.QtWidgets import (
    QApplication,
    QWidget,
    QPushButton,
    QHBoxLayout,
    QFrame,
    QToolButton,
    QLabel,
    QComboBox,
)
from PySide6.QtCore import Qt, Signal, QEvent, QPoint, QPointF, QRect, QRectF, QSize, QTimer
from PySide6.QtGui import (
    QColor, QPainter, QPen, QBrush, QPainterPath, QImage, QPixmap, QIcon,
    QLinearGradient, QRadialGradient, QTransform,
)
from core import safe_event
from core.constants import CSS_FONT_FAMILY
from core.i18n import tr as translate_text
from core.ui_scale import scaled, scaled_f, scale_factor
from core.ui_theme import apply_style_sheet, set_own_style
from .color_picker_button import ColorPickerButton


PANEL_RADIUS = 12.0

# 面板底是画出来的固定纹理，不读背后的像素：钉图工具栏浮在真实桌面上，没有可模糊的底图。
PANEL_TOP = QColor("#fcfdff")
PANEL_BOTTOM = QColor("#eef2f8")
# (横向位置比例, 纵向位置比例, 基准半径, RGBA)：几团低饱和光晕，模拟背后糊开的颜色
PANEL_GLOWS = (
    (0.10, 0.0, 230, (120, 170, 255, 46)),
    (0.58, 1.2, 260, (190, 160, 255, 34)),
    (0.96, 0.0, 200, (255, 190, 160, 36)),
)
PANEL_NOISE_OPACITY = 0.035
PANEL_HIGHLIGHT = QColor(255, 255, 255, 235)
PANEL_INNER_RING = QColor(255, 255, 255, 190)
# 半透明深色外圈：白色截图上能看出边界，深色截图上几乎不可见
PANEL_OUTER_RING = QColor(15, 23, 42, 30)

_noise_tile = None


def _panel_noise_tile() -> QImage:
    """64×64 的黑色噪点，透明度随机；固定种子，每次启动纹理一致"""
    global _noise_tile
    if _noise_tile is None:
        data = bytearray(64 * 64 * 4)
        data[3::4] = random.Random(7).randbytes(64 * 64)
        _noise_tile = QImage(bytes(data), 64, 64, 64 * 4, QImage.Format.Format_ARGB32_Premultiplied).copy()
    return _noise_tile


def _draw_panel_background(painter: QPainter, rect: QRectF, dpr: float) -> None:
    radius = scaled_f(PANEL_RADIUS)
    line = scaled_f(1.0)
    half = line / 2
    path = QPainterPath()
    path.addRoundedRect(rect.adjusted(half, half, -half, -half), radius, radius)

    gradient = QLinearGradient(rect.topLeft(), rect.bottomLeft())
    gradient.setColorAt(0, PANEL_TOP)
    gradient.setColorAt(1, PANEL_BOTTOM)
    painter.fillPath(path, QBrush(gradient))

    painter.save()
    painter.setClipPath(path)
    for fx, fy, base_radius, rgba in PANEL_GLOWS:
        glow = QRadialGradient(rect.left() + fx * rect.width(), rect.top() + fy * rect.height(),
                               scaled_f(base_radius))
        glow.setColorAt(0, QColor(*rgba))
        glow.setColorAt(1, QColor(rgba[0], rgba[1], rgba[2], 0))
        painter.fillRect(rect, QBrush(glow))
    noise = QBrush(_panel_noise_tile())
    noise.setTransform(QTransform.fromScale(1 / dpr, 1 / dpr))   # 颗粒固定为 1 个物理像素
    painter.setOpacity(PANEL_NOISE_OPACITY)
    painter.fillRect(rect, noise)
    painter.setOpacity(1.0)
    painter.setPen(QPen(PANEL_HIGHLIGHT, line))
    y = rect.top() + line * 1.5
    painter.drawLine(QPointF(rect.left() + radius * 0.6, y), QPointF(rect.right() - radius * 0.6, y))
    painter.restore()

    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.setPen(QPen(PANEL_INNER_RING, line))
    inset = line * 1.5
    painter.drawRoundedRect(rect.adjusted(inset, inset, -inset, -inset), radius - line, radius - line)
    painter.setPen(QPen(PANEL_OUTER_RING, line))
    painter.drawPath(path)


def paint_rounded_panel(widget):
    """雾面圆角底。设置面板、截图工具栏和「…」弹层共用，四角靠 WA_TranslucentBackground 保持透明。

    纹理画进挂在控件上的缓存位图，尺寸、DPR 或界面比例变了才重画，平时的重绘只是贴图。
    """
    width, height = widget.width(), widget.height()
    if width <= 0 or height <= 0:
        return
    dpr = widget.devicePixelRatioF()
    key = (width, height, dpr, scale_factor())
    cached = getattr(widget, "_panel_background", None)
    if cached is None or cached[0] != key:
        pixmap = QPixmap(round(width * dpr), round(height * dpr))
        pixmap.setDevicePixelRatio(dpr)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        _draw_panel_background(painter, QRectF(0, 0, width, height), dpr)
        painter.end()
        cached = (key, pixmap)
        widget._panel_background = cached

    painter = QPainter(widget)
    painter.drawPixmap(0, 0, cached[1])
    painter.end()


# 面板控件配色：数值和线型预览用深墨色，图标和小箭头用浅一档的灰
CONTROL_INK = QColor("#2a3243")
CONTROL_SUB = QColor("#6a7387")
CONTROL_FILL = QColor(15, 23, 42, 15)
CONTROL_FILL_HOVER = QColor(15, 23, 42, 26)
DIVIDER_COLOR = QColor(15, 23, 42, 30)


def _rgba(color: QColor) -> str:
    return f"rgba({color.red()}, {color.green()}, {color.blue()}, {color.alpha()})"


def accent_color() -> QColor:
    """选中态用的主题色，用户可在设置里改"""
    from core.theme import get_theme
    return QColor(get_theme().theme_color)


def _chevron_pixmap(width: int, height: int, up: bool) -> QPixmap:
    """箭头画在靠近另一个按钮的那一侧：上下两个按钮各占半个胶囊高，箭头却要挨着中线"""
    ratio = 3.0
    pixmap = QPixmap(round(width * ratio), round(height * ratio))
    pixmap.setDevicePixelRatio(ratio)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setPen(QPen(CONTROL_SUB, scaled_f(1.4), Qt.PenStyle.SolidLine,
                        Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
    near, far = scaled_f(2.2), scaled_f(5.0)
    if up:
        tip, base = height - far, height - near
    else:
        tip, base = far, near
    painter.drawPolyline([QPointF(width * 0.12, base), QPointF(width / 2, tip), QPointF(width * 0.88, base)])
    painter.end()
    return pixmap


def set_step_button_icon(button: QToolButton, direction: str):
    """步进按钮的小箭头。自己画，颜色不受系统调色板影响。图标和按钮等高，按钮上下叠放时箭头贴着两者的交界。"""
    width, height = scaled(8), scaled(13)
    button.setArrowType(Qt.ArrowType.NoArrow)
    button.setIcon(QIcon(_chevron_pixmap(width, height, direction == "up")))
    button.setIconSize(QSize(width, height))


def style_step_button(button: QToolButton, direction: str):
    """胶囊里的步进按钮：透明底，悬停时浅浅垫一层"""
    set_step_button_icon(button, direction)
    apply_style_sheet(
        button,
        f".QToolButton {{ background: transparent; border: none; border-radius: {scaled(4)}px; }}"
        " .QToolButton:hover { background: rgba(15, 23, 42, 0.08); }",
    )


def paint_line_style_preview(painter: QPainter, rect: QRectF, style) -> None:
    """线型下拉收起时的预览：一条 2px 的线，虚线按线型取节奏"""
    pen = QPen(CONTROL_INK, scaled_f(2.0), Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
    if style == "dashed":
        pen.setStyle(Qt.PenStyle.CustomDashLine)
        pen.setCapStyle(Qt.PenCapStyle.FlatCap)
        pen.setDashPattern([3.0, 2.0])
    elif style == "dashed_dense":
        pen.setStyle(Qt.PenStyle.CustomDashLine)
        pen.setCapStyle(Qt.PenCapStyle.FlatCap)
        pen.setDashPattern([1.0, 1.5])
    painter.setPen(pen)
    y = rect.center().y()
    painter.drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y))


def build_settings_panel_stylesheet(
    *,
    combo_enabled: bool = False,
    combo_padding: str = "2px 8px 2px 4px",
    combo_min_width: int = 32,
    combo_max_width: int = 90,
    combo_padding_compact: bool = False
) -> str:
    """构建设置面板统一样式。

    传入的宽度和样式里的字号、内边距、圆角都是 100% 下的基准值，按当前比例换算；
    改比例后要重新调用一次，样式表本身不会跟着比例走。
    收起的下拉框由 PillComboBox 自己画，这里只管它的尺寸和弹出列表。
    """
    font_px = scaled(12)
    accent = accent_color()
    accent_soft = f"rgba({accent.red()}, {accent.green()}, {accent.blue()}, 0.3)"
    ink = CONTROL_INK.name()
    combo_block = ""
    if combo_enabled:
        combo_block = f"""
            QComboBox {{
                border: none;
                background: transparent;
                min-height: {scaled(PillComboBox.BASE_HEIGHT)}px;
                max-height: {scaled(PillComboBox.BASE_HEIGHT)}px;
                min-width: {scaled(combo_min_width)}px;
                max-width: {scaled(combo_max_width)}px;
                font-family: {CSS_FONT_FAMILY};
                font-size: {font_px}px;
                color: {ink};
            }}
            QComboBox::drop-down {{
                width: 0px;
                border: none;
            }}
            QComboBox::down-arrow {{
                image: none;
                width: 0px;
                height: 0px;
            }}
            QComboBox QAbstractItemView {{
                border: 1px solid {_rgba(DIVIDER_COLOR)};
                background: white;
                selection-background-color: {accent_soft};
                selection-color: {ink};
                font-family: {CSS_FONT_FAMILY};
                font-size: {font_px}px;
                color: {ink};
                outline: none;
                padding: {scaled(2)}px;
            }}
            QComboBox QAbstractItemView::item {{
                padding: {scaled(3)}px {scaled(6)}px;
                min-height: {scaled(18)}px;
                color: {ink};
                background: white;
            }}
            QComboBox QAbstractItemView::item:hover {{
                background-color: {_rgba(CONTROL_FILL)};
            }}
            QComboBox QAbstractItemView::item:selected {{
                background-color: {accent_soft};
                color: {ink};
            }}
        """

    return f"""
        QWidget {{
            background-color: transparent;
            border: none;
        }}
        QPushButton {{
            background-color: transparent;
            border: none;
            border-radius: {scaled(8)}px;
            padding: {scaled(2)}px;
            font-family: {CSS_FONT_FAMILY};
            font-size: {font_px}px;
            color: {ink};
        }}
        QPushButton:hover {{
            background-color: rgba(15, 23, 42, 0.06);
        }}
        QPushButton:pressed {{
            background-color: rgba(15, 23, 42, 0.12);
        }}
        QPushButton:checked {{
            background-color: {accent_soft};
        }}
        QLabel {{
            color: {ink};
            background-color: transparent;
            border: none;
            font-family: {CSS_FONT_FAMILY};
            font-size: {font_px}px;
        }}
        QFrame#separator {{
            background-color: {_rgba(DIVIDER_COLOR)};
            margin: {scaled(3)}px 0px;
            border: none;
        }}
        {combo_block}
    """


def _paint_width_glyph(painter: QPainter, x: float, cy: float) -> None:
    """粗细图标：三条由细到粗的横条"""
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(CONTROL_SUB)
    width = scaled_f(12)
    for index, height in enumerate((1.2, 2.2, 3.4)):
        top = cy - scaled_f(5.5) + index * scaled_f(4.6)
        painter.drawRoundedRect(QRectF(x, top, width, scaled_f(height)), scaled_f(1), scaled_f(1))


def _paint_opacity_glyph(painter: QPainter, x: float, cy: float) -> None:
    """透明度图标：左半实心的圆"""
    radius = scaled_f(5.4)
    center = QPointF(x + scaled_f(6), cy)
    painter.setPen(QPen(CONTROL_SUB, scaled_f(1.4)))
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawEllipse(center, radius, radius)
    half = QPainterPath()
    half.moveTo(center.x(), center.y() - radius)
    half.arcTo(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2), 90, 180)
    half.closeSubpath()
    painter.setBrush(CONTROL_SUB)
    painter.drawPath(half)


_GLYPH_PAINTERS = {"width": _paint_width_glyph, "opacity": _paint_opacity_glyph}


class StepperWidget(QWidget):
    """胶囊形数值控件：左边可带一个说明图标，胶囊里是数值和上下两个小箭头，滚轮也能调。

    glyph 取 "width"（粗细）或 "opacity"（透明度），不传就没有图标、整个宽度都是胶囊。
    """

    valueChanged = Signal(int)

    # 基准尺寸（100% 下的实际像素）
    BASE_HEIGHT = 26
    BASE_GLYPH_COLUMN = 18   # 图标 12 + 到胶囊的间隙 6
    BASE_ARROW_COLUMN = 16   # 胶囊右端留给上下箭头的宽度
    BASE_LABEL_MIN_WIDTH = 26

    def __init__(self, value: int = 0, minimum: int = 0, maximum: int = 100, suffix: str = "",
                 parent=None, *, glyph: str = ""):
        super().__init__(parent)
        self._min = int(minimum)
        self._max = int(maximum)
        self._value = int(value)
        self._suffix = str(suffix)
        self._glyph = glyph if glyph in _GLYPH_PAINTERS else ""
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)

        self._label = QLabel(self)
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._up_btn = QToolButton(self)
        self._down_btn = QToolButton(self)
        for button in (self._up_btn, self._down_btn):
            button.setAutoRepeat(True)
            button.setAutoRepeatDelay(300)
            button.setAutoRepeatInterval(60)
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.setCursor(Qt.CursorShape.PointingHandCursor)

        self._up_btn.clicked.connect(lambda: self._step(1))
        self._down_btn.clicked.connect(lambda: self._step(-1))
        self._apply_scale_sizes()
        self._refresh_label()

    def _glyph_width(self) -> int:
        return scaled(self.BASE_GLYPH_COLUMN) if self._glyph else 0

    def _pill_rect(self) -> QRect:
        height = scaled(self.BASE_HEIGHT)
        left = self._glyph_width()
        return QRect(left, (self.height() - height) // 2, self.width() - left, height)

    def _apply_scale_sizes(self):
        """按当前比例重算胶囊、数值和箭头的尺寸"""
        self.setFixedHeight(scaled(self.BASE_HEIGHT))
        set_own_style(
            self._label,
            f"background: transparent; color: {CONTROL_INK.name()}; border: none;"
            f" font-family: {CSS_FONT_FAMILY}; font-size: {scaled(12)}px; font-weight: bold;",
        )
        for direction, button in (("up", self._up_btn), ("down", self._down_btn)):
            style_step_button(button, direction)
        self._layout_children()

    def _layout_children(self):
        pill = self._pill_rect()
        arrow = scaled(self.BASE_ARROW_COLUMN)
        label_width = max(scaled(self.BASE_LABEL_MIN_WIDTH), pill.width() - arrow)
        self._label.setGeometry(pill.left(), pill.top(), label_width, pill.height())
        half = pill.height() // 2
        button_left = pill.right() + 1 - arrow
        button_width = arrow - scaled(3)
        self._up_btn.setGeometry(button_left, pill.top(), button_width, half)
        self._down_btn.setGeometry(button_left, pill.top() + half, button_width, pill.height() - half)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._layout_children()

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pill = QRectF(self._pill_rect())
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(CONTROL_FILL_HOVER if self.underMouse() else CONTROL_FILL)
        radius = pill.height() / 2
        painter.drawRoundedRect(pill, radius, radius)
        if self._glyph:
            _GLYPH_PAINTERS[self._glyph](painter, 0.0, pill.center().y())
        painter.end()

    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.update()
        super().leaveEvent(event)

    def _clamp(self, value: int) -> int:
        return max(self._min, min(self._max, int(value)))

    def _step(self, delta: int):
        self.setValue(self._value + int(delta))

    def _refresh_label(self):
        self._label.setText(f"{self._value}{self._suffix}")

    def setRange(self, minimum: int, maximum: int):
        self._min = int(minimum)
        self._max = int(maximum)
        self.setValue(self._value)

    def setValue(self, value: int):
        value = self._clamp(value)
        if value == self._value:
            self._refresh_label()
            return
        self._value = int(value)
        self._refresh_label()
        if not self.signalsBlocked():
            self.valueChanged.emit(int(self._value))

    def value(self) -> int:
        return int(self._value)

    def setSuffix(self, suffix: str):
        self._suffix = str(suffix)
        self._refresh_label()

    def setToolTip(self, text: str):
        super().setToolTip(text)
        self._label.setToolTip(text)
        self._up_btn.setToolTip(text)
        self._down_btn.setToolTip(text)

    def setFixedWidth(self, width: int):
        """width 含左侧图标列，是按比例算好的像素"""
        super().setFixedWidth(width)
        self._apply_scale_sizes()

    @safe_event
    def wheelEvent(self, event):
        """鼠标滚轮调整数值"""
        delta = event.angleDelta().y()
        if delta > 0:
            self._step(1)
        elif delta < 0:
            self._step(-1)
        event.accept()


class PillComboBox(QComboBox):
    """胶囊形下拉框。收起时自己画底、当前项和下拉箭头，弹出列表仍交给样式表。

    当前项的画法三选一：设了 preview_painter 就交给它（线型），有图标画图标（箭头样式），
    否则画文字。
    """

    BASE_HEIGHT = 26
    BASE_PADDING_LEFT = 14
    BASE_ARROW_COLUMN = 30

    def __init__(self, parent=None, *, text_alignment=Qt.AlignmentFlag.AlignLeft):
        super().__init__(parent)
        self._preview_painter = None
        self._text_alignment = text_alignment
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def set_preview_painter(self, painter_fn):
        """painter_fn(painter, rect, current_data)"""
        self._preview_painter = painter_fn
        self.update()

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect())
        radius = rect.height() / 2
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(CONTROL_FILL_HOVER if self.underMouse() else CONTROL_FILL)
        painter.drawRoundedRect(rect, radius, radius)

        arrow = scaled_f(self.BASE_ARROW_COLUMN)
        content = rect.adjusted(scaled_f(self.BASE_PADDING_LEFT), 0, -arrow, 0)
        icon = self.itemIcon(self.currentIndex()) if self.currentIndex() >= 0 else QIcon()
        if self._preview_painter is not None:
            self._preview_painter(painter, content, self.currentData())
        elif not icon.isNull():
            icon.paint(painter, content.toRect(), Qt.AlignmentFlag.AlignCenter)
        else:
            painter.setPen(CONTROL_INK)
            painter.setFont(self.font())
            text = self.fontMetrics().elidedText(
                self.currentText(), Qt.TextElideMode.ElideRight, int(content.width()))
            painter.drawText(content, self._text_alignment | Qt.AlignmentFlag.AlignVCenter, text)

        cx = rect.right() - scaled_f(15.5)
        cy = rect.center().y()
        dx, dy = scaled_f(3.5), scaled_f(1.8)
        painter.setPen(QPen(CONTROL_SUB, scaled_f(1.4), Qt.PenStyle.SolidLine,
                            Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        painter.drawPolyline([QPointF(cx - dx, cy - dy), QPointF(cx, cy + dy), QPointF(cx + dx, cy - dy)])
        painter.end()

    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.update()
        super().leaveEvent(event)


class PillFrame(QWidget):
    """给一组控件垫一块和数值胶囊一样的底（序号面板的「下一个序号」用）"""

    BASE_HEIGHT = 26

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.apply_scale()

    def apply_scale(self):
        self.setFixedHeight(scaled(self.BASE_HEIGHT))

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect())
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(CONTROL_FILL_HOVER if self.underMouse() else CONTROL_FILL)
        painter.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2)
        painter.end()

    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.update()
        super().leaveEvent(event)


class ColorSwatch(QPushButton):
    """圆形预设色块。选中的套一圈主题色，点击区比圆本身大一圈，选中环才不会被裁掉。"""

    BASE_SIDE = 28
    BASE_RADIUS = 10
    BASE_RING_GAP = 3.5

    def __init__(self, color_hex: str, parent=None):
        super().__init__(parent)
        self.color_hex = color_hex
        self._color = QColor(color_hex)
        self._selected = False
        self.setFlat(True)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setToolTip(color_hex)
        self.apply_scale()

    def apply_scale(self):
        side = scaled(self.BASE_SIDE)
        self.setFixedSize(side, side)
        self.update()

    def set_selected(self, selected: bool):
        if bool(selected) != self._selected:
            self._selected = bool(selected)
            self.update()

    def is_selected(self) -> bool:
        return self._selected

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        center = QRectF(self.rect()).center()
        radius = scaled_f(self.BASE_RADIUS)
        if self._selected:
            ring = radius + scaled_f(self.BASE_RING_GAP)
            painter.setPen(QPen(accent_color(), scaled_f(2.0)))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawEllipse(center, ring, ring)
        elif self.underMouse():
            ring = radius + scaled_f(2.5)
            painter.setPen(QPen(QColor(15, 23, 42, 50), scaled_f(1.0)))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawEllipse(center, ring, ring)
        if self.isDown():
            radius *= 0.9
        painter.setPen(QPen(QColor(0, 0, 0, 56 if self._color.lightness() > 240 else 36), 1))
        painter.setBrush(self._color)
        painter.drawEllipse(center, radius, radius)
        painter.end()


class ColorRow(QWidget):
    """开头是自定义色按钮（彩虹描边里显示当前色），后面一排圆形预设色。

    选中哪个由自定义色按钮统一判断：面板上所有改颜色的入口最后都会落到它的 set_color
    或取色对话框，所以在那里比对一次就覆盖了全部入口。
    """

    BASE_SPACING = 1   # 色块的点击区已经比圆大 8px，圆与圆之间实际隔 9px

    def __init__(self, picker: ColorPickerButton, colors, on_preset, parent=None):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        self.picker = picker
        self.swatches = []
        layout.addWidget(picker)
        for color_hex in colors:
            swatch = ColorSwatch(color_hex)
            swatch.clicked.connect(lambda _checked=False, c=color_hex: on_preset(c))
            layout.addWidget(swatch)
            self.swatches.append((swatch, color_hex))
        picker.link_swatches([swatch for swatch, _ in self.swatches])
        self.apply_scale()

    def apply_scale(self):
        self.layout().setSpacing(scaled(self.BASE_SPACING))
        for swatch, _ in self.swatches:
            swatch.apply_scale()
        side = scaled(ColorSwatch.BASE_SIDE)
        self.picker.setFixedSize(side, side)


PRESET_COLORS = ("#FF0000", "#FFFF00", "#00FF00", "#0000FF", "#000000", "#FFFFFF")


def make_separator() -> QFrame:
    line = QFrame()
    line.setObjectName("separator")
    line.setFrameShape(QFrame.Shape.VLine)
    line.setFixedWidth(1)
    return line


def popup_position(panel: QWidget, anchor: QWidget, popup: QWidget) -> QPoint:
    """悬停弹出层的位置：背离工具栏的方向弹，那一边放不下就换另一边。

    面板在工具栏下方就往下弹，在上方就往上弹，这样天然不会盖住一级/二级菜单。
    工具栏由它自己登记在面板的 _owner_toolbar 上：面板的 Qt parent 是工具栏的
    parent，不是工具栏本身。没登记（比如 GIF 录制的面板）就按"在下方"处理。
    """
    gap = scaled(4)
    panel_rect = QRect(panel.mapToGlobal(QPoint(0, 0)), panel.size())
    anchor_top_left = anchor.mapToGlobal(QPoint(0, 0))

    screen = QApplication.screenAt(anchor_top_left) or QApplication.primaryScreen()
    area = screen.availableGeometry()

    toolbar = getattr(panel, "_owner_toolbar", None)
    below_toolbar = True
    if toolbar is not None:
        try:
            below_toolbar = panel_rect.top() >= toolbar.mapToGlobal(QPoint(0, 0)).y()
        except RuntimeError:
            pass

    outward = panel_rect.bottom() + gap if below_toolbar else panel_rect.top() - popup.height() - gap
    other = panel_rect.top() - popup.height() - gap if below_toolbar else panel_rect.bottom() + gap

    y = outward
    if not (area.top() <= y and y + popup.height() <= area.bottom()):
        y = other

    x = anchor_top_left.x() + anchor.width() // 2 - popup.width() // 2
    x = max(area.left(), min(x, area.right() - popup.width()))
    y = max(area.top(), min(y, area.bottom() - popup.height()))
    return QPoint(int(x), int(y))


class HoverPopup(QWidget):
    """悬停在面板某个控件上弹出的小设置层（序号样式条、文字的背景/描边/阴影）。

    打开、收起、定位只有这一份：鼠标从控件移向弹出层要跨过中间的空隙，所以
    离开时不立刻关、留一点缓冲，进入弹出层就取消；位置见 popup_position。
    """

    CLOSE_DELAY_MS = 260

    def __init__(self, parent=None):
        super().__init__(parent)
        # 与设置面板同样的标志：浮在最上层且不抢焦点，否则一弹出面板就没了
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # 底和设置面板是同一个雾面，由 paintEvent 画；样式表只管里面的控件
        self.setObjectName("HoverPopup")
        self._extra_qss = ""
        self.apply_scale()

        self._close_timer = QTimer(self)
        self._close_timer.setSingleShot(True)
        self._close_timer.setInterval(self.CLOSE_DELAY_MS)
        self._close_timer.timeout.connect(self.hide)

        if parent is not None:
            parent.installEventFilter(self)

    def eventFilter(self, watched, event):
        # 面板一收起来（切工具、结束截图），挂在它上面的弹出层必须跟着消失。
        # 弹出层是独立的顶层窗口，Qt 不会因为父面板隐藏就隐藏它，留在屏幕上
        # 就是一块盖在截图上的白框。
        if watched is self.parentWidget() and event.type() == QEvent.Type.Hide:
            self.hide()
        return super().eventFilter(watched, event)

    def set_extra_stylesheet(self, qss: str):
        """弹出层内部控件的样式。和基础样式分开存，改比例时两段一起重挂。"""
        self._extra_qss = qss or ""
        self.apply_scale()

    def apply_scale(self):
        """按当前比例重挂样式。子类补自己的控件尺寸时记得先调 super()。"""
        self.setStyleSheet("#HoverPopup { background: transparent; border: none; }" + self._extra_qss)

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def show_beside(self, panel: QWidget, anchor: QWidget):
        self.keep_open()
        self.adjustSize()
        self.move(popup_position(panel, anchor, self))
        self.show()
        self.raise_()

    def keep_open(self):
        self._close_timer.stop()

    def close_soon(self):
        self._close_timer.start()

    def enterEvent(self, event):
        self.keep_open()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.close_soon()
        super().leaveEvent(event)


class BaseSettingsPanel(QWidget):
    """设置面板基类 - 统一风格布局"""

    TRANSLATION_CONTEXT = "ArrowSettingsPanel"
    # 基准尺寸 —— 100% 比例下的实际像素。原先这些值写成 round(N * PANEL_SCALE)，
    # 0.90 已折进基准值；改比例时一律从基准重算，不在已缩放的结果上再乘。
    BASE_MARGIN_H = 12
    BASE_MARGIN_V = 6        # 色块点击区 28 + 上下各 6 = 40，与主工具栏同高
    BASE_SPACING = 10
    BASE_SIZE_SPIN_WIDTH = 68      # 图标列 18 + 胶囊 50
    BASE_OPACITY_SPIN_WIDTH = 80   # 图标列 18 + 胶囊 62
    SIZE_GLYPH = "width"
    SIZE_RANGE = (1, 99)
    SIZE_DEFAULT = 5
    SIZE_TOOLTIP = "Line Width"
    OPACITY_DEFAULT = 255
    OPACITY_TOOLTIP = "Opacity (%)"
    
    # 通用信号
    color_changed = Signal(QColor)
    size_changed = Signal(int)
    opacity_changed = Signal(int)  # 0-255
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.current_color = QColor(255, 0, 0)  # 默认红色
        self.current_size = self.SIZE_DEFAULT
        self.current_opacity = self.OPACITY_DEFAULT
        
        self._init_ui()

    @safe_event
    def paintEvent(self, event):
        """手动绘制圆角背景 + 描边，确保顶级无框窗口也能正确显示"""
        paint_rounded_panel(self)
        
    def _init_ui(self):
        """初始化UI - 统一布局"""
        layout = QHBoxLayout(self)

        self._build_size_and_opacity(layout)
        layout.addWidget(make_separator())
        layout.addWidget(self._build_color_row())
        layout.addStretch()

        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_picker_btn.color_changed.connect(self._on_color_picked)

        self._build_extra_controls(layout)
        self.apply_scale()

    def _build_size_and_opacity(self, layout: QHBoxLayout):
        self.size_spin = StepperWidget(self.current_size, self.SIZE_RANGE[0], self.SIZE_RANGE[1],
                                       glyph=self.SIZE_GLYPH)
        self.size_spin.setToolTip(self._tr(self.SIZE_TOOLTIP))
        layout.addWidget(self.size_spin)

        self.opacity_spin = StepperWidget(self._opacity_to_percent(self.current_opacity), 0, 100, "%",
                                          glyph="opacity")
        self.opacity_spin.setToolTip(self._tr(self.OPACITY_TOOLTIP))
        layout.addWidget(self.opacity_spin)

    def _build_color_row(self) -> "ColorRow":
        """预设色 + 末尾的自定义色；_preset_buttons 和 color_picker_btn 都挂在面板上"""
        self.color_picker_btn = ColorPickerButton(self.current_color, show_alpha=True, round_style=True)
        self.color_picker_btn.setToolTip(self._tr("Custom Color"))
        self._color_row = ColorRow(self.color_picker_btn, PRESET_COLORS, self._apply_preset_color)
        self._preset_buttons = self._color_row.swatches
        return self._color_row

    def _build_extra_controls(self, layout: QHBoxLayout):
        """扩展控件 - 子类可选实现"""
        return

    # ========================================================================
    # 缩放
    # ========================================================================

    def _build_stylesheet(self) -> str:
        """面板样式。要定制下拉框参数的子类覆盖这里，改比例后会被重新调用。"""
        return build_settings_panel_stylesheet()

    def _apply_scale_sizes(self):
        """把基准尺寸按当前比例落到各控件上。

        子类的扩展控件在覆盖里补上，并先调用 super()。
        """
        layout = self.layout()
        if layout is not None:
            mh, mv = scaled(self.BASE_MARGIN_H), scaled(self.BASE_MARGIN_V)
            layout.setContentsMargins(mh, mv, mh, mv)
            layout.setSpacing(scaled(self.BASE_SPACING))
        size_width = self.BASE_SIZE_SPIN_WIDTH
        if not self.SIZE_GLYPH:
            size_width -= StepperWidget.BASE_GLYPH_COLUMN
        self.size_spin.setFixedWidth(scaled(size_width))
        self.opacity_spin.setFixedWidth(scaled(self.BASE_OPACITY_SPIN_WIDTH))
        self._color_row.apply_scale()

    def apply_scale(self):
        """按当前比例重算面板尺寸。数值、颜色、工具状态都不动，只改显示大小。"""
        self.setStyleSheet(self._build_stylesheet())
        self._apply_scale_sizes()
        self.adjustSize()
        self.update()

    def _tr(self, text: str) -> str:
        """统一翻译入口（与箭头面板一致的翻译上下文）"""
        return translate_text(text, self.TRANSLATION_CONTEXT)
        
    # ========================================================================
    # 信号处理
    # ========================================================================
    
    def _on_size_changed(self, value: int):
        """大小改变"""
        self.current_size = value
        self.size_changed.emit(value)
        
    def _on_opacity_changed(self, value: int):
        """透明度改变"""
        self.current_opacity = self._percent_to_opacity(value)
        # 同步到颜色的 alpha，这样下次打开颜色选择器时初始值正确
        synced = QColor(self.current_color)
        synced.setAlpha(self.current_opacity)
        self.current_color = synced
        self.color_picker_btn.set_color(synced)
        self.opacity_changed.emit(self.current_opacity)
        
    def _on_color_picked(self, color: QColor):
        """颜色选择器回调"""
        self.current_color = color
        # 同步 alpha 到 opacity_spin，并发射 opacity_changed 让 ctx.opacity 更新
        alpha = color.alpha()
        if hasattr(self, 'opacity_spin'):
            self.current_opacity = alpha
            self.opacity_spin.blockSignals(True)
            self.opacity_spin.setValue(self._opacity_to_percent(alpha))
            self.opacity_spin.blockSignals(False)
            self.opacity_changed.emit(self.current_opacity)
        self.color_changed.emit(color)
            
    def _apply_preset_color(self, color_hex: str):
        """应用预设颜色"""
        self.current_color = QColor(color_hex)
        self.color_changed.emit(self.current_color)
        self.color_picker_btn.set_color(self.current_color)
        

    def _opacity_to_percent(self, opacity: int) -> int:
        """0-255 透明度转百分比"""
        return max(0, min(100, int(round(opacity / 255 * 100))))

    def _percent_to_opacity(self, percent: int) -> int:
        """百分比转 0-255 透明度"""
        return max(0, min(255, int(round(percent / 100 * 255))))
        
    # ========================================================================
    # 公共接口方法（供 Toolbar 调用）
    # ========================================================================
    
    def set_color(self, color: QColor):
        """设置颜色（不触发信号）"""
        self.current_color = color
        self.color_picker_btn.set_color(color)
        
    def set_size(self, size: int):
        """设置大小（不触发信号）。

        步进器本来就会把超出范围的值钳回去，但 current_size 以前存的是原始值，
        于是面板显示和面板状态对不上。这里一起钳制，两者保持一致。
        """
        low, high = self.SIZE_RANGE
        size = max(int(low), min(int(high), int(size)))
        self.current_size = size
        if hasattr(self, 'size_spin'):
            self.size_spin.blockSignals(True)
            self.size_spin.setValue(int(size))
            self.size_spin.blockSignals(False)
        
    def set_opacity(self, opacity: int):
        """设置透明度（不触发信号）"""
        self.current_opacity = opacity
        if hasattr(self, 'opacity_spin'):
            self.opacity_spin.blockSignals(True)
            self.opacity_spin.setValue(self._opacity_to_percent(opacity))
            self.opacity_spin.blockSignals(False)
 
