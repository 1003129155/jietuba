"""
水印设置面板：水印文字、位置、样式（不透明度/大小/间距/角度/网格线/颜色）、字体。

文字框点一下弹出输入框：工具栏面板不接收键盘焦点，截图窗口要独占键盘，不能就地打字。
"""
from datetime import datetime

from PySide6.QtCore import QEvent, QPointF, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QButtonGroup, QHBoxLayout, QLabel, QPushButton, QSlider, QVBoxLayout, QWidget

from canvas.items.watermark_item import (
    LAYOUT_DIAMOND, LAYOUT_TILE, LAYOUTS, LINE_STYLES, MAX_ANGLE, MAX_FONT_SIZE, MAX_SPACING, MIN_ANGLE,
    MIN_FONT_SIZE, MIN_SPACING, WatermarkStyle, expand_text,
)
from core import safe_event
from core.constants import get_available_text_fonts, get_default_text_font_for_language
from core.i18n import make_tr
from core.resource_manager import ResourceManager
from core.ui_scale import scaled, scaled_f
from .base_settings_panel import (
    CONTROL_FILL, CONTROL_FILL_HOVER, CONTROL_INK, CONTROL_SUB, PRESET_COLORS, ColorRow, HoverPopup,
    PillComboBox, accent_color, build_settings_panel_stylesheet, make_separator, paint_rounded_panel,
)
from .color_picker_button import ColorPickerButton

_tr = make_tr("WatermarkPanel")

LAYOUT_NAMES = {
    "tile": "Tiled",
    "diamond": "Diamond",
    "center": "Center",
    "top_left": "Top Left",
    "top": "Top Center",
    "top_right": "Top Right",
    "bottom_left": "Bottom Left",
    "bottom": "Bottom Center",
    "bottom_right": "Bottom Right",
}

LINE_STYLE_NAMES = {
    "none": "None",
    "solid": "Solid",
    "dash": "Dashed",
}


class WatermarkTextField(QWidget):
    """面板上的水印文字：显示加上去的样子，点一下编辑，右侧 × 清空。"""

    clicked = Signal()
    cleared = Signal()

    BASE_WIDTH = 168
    BASE_HEIGHT = 26
    BASE_PADDING = 12
    BASE_CLEAR = 16

    def __init__(self, parent=None):
        super().__init__(parent)
        self._template = ""
        self._text = ""
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.apply_scale()

    def apply_scale(self):
        self.setFixedSize(scaled(self.BASE_WIDTH), scaled(self.BASE_HEIGHT))
        self.update()

    def set_template(self, template: str, when: datetime = None):
        """when 是图上那层水印的时间；只在这里展开一次，重绘时不再取当前时间，显示的和图上的一致。"""
        self._template = template or ""
        self._text = expand_text(self._template, when or datetime.now())
        self.setToolTip(self._text or _tr("Input watermark"))
        self.update()

    def template(self) -> str:
        return self._template

    def text(self) -> str:
        return self._text

    def _clear_rect(self) -> QRectF:
        side = scaled_f(self.BASE_CLEAR)
        return QRectF(self.width() - scaled_f(5) - side, (self.height() - side) / 2, side, side)

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect())
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(CONTROL_FILL_HOVER if self.underMouse() else CONTROL_FILL)
        painter.drawRoundedRect(rect, rect.height() / 2, rect.height() / 2)

        clear = self._clear_rect()
        right = clear.left() - scaled_f(4) if self._template else rect.right() - scaled_f(self.BASE_PADDING)
        content = QRectF(rect.left() + scaled_f(self.BASE_PADDING), rect.top(), 0, rect.height())
        content.setRight(right)
        # 多行水印在一行里用空格隔开显示，完整内容看悬停提示
        text = self._text.replace("\n", "  ")
        painter.setFont(self.font())
        painter.setPen(CONTROL_INK if text else CONTROL_SUB)
        shown = self.fontMetrics().elidedText(text or _tr("Input watermark"), Qt.TextElideMode.ElideRight,
                                              int(content.width()))
        painter.drawText(content, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, shown)

        if self._template:
            hovered = clear.contains(self.mapFromGlobal(self.cursor().pos()).toPointF())
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(CONTROL_SUB.red(), CONTROL_SUB.green(), CONTROL_SUB.blue(),
                                    200 if hovered else 120))
            painter.drawEllipse(clear)
            painter.setPen(QPen(QColor("white"), scaled_f(1.4), Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            c, d = clear.center(), clear.width() * 0.2
            painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d))
            painter.drawLine(QPointF(c.x() - d, c.y() + d), QPointF(c.x() + d, c.y() - d))
        painter.end()

    def retranslate(self):
        self.setToolTip(self._text or _tr("Input watermark"))
        self.update()

    def mouseMoveEvent(self, event):
        self.update()
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.rect().contains(event.position().toPoint()):
            if self._template and self._clear_rect().contains(event.position()):
                self.cleared.emit()
            else:
                self.clicked.emit()
        super().mouseReleaseEvent(event)

    def enterEvent(self, event):
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.update()
        super().leaveEvent(event)


class WatermarkStylePopup(HoverPopup):
    """悬停在样式按钮上弹出：不透明度、大小、间距、角度四条滑块，菱形的网格线线型，下面一排颜色。

    「角度」这一条在平铺里转文字、在菱形里转整张网格，两种各存各的（见 WatermarkStyle），
    滑块跟着当前位置读写对应的那个字段。
    """

    changed = Signal(str, object)   # (WatermarkStyle 的字段名, 新值)

    ROWS = (
        # 字段名, 文案, 范围, 滑块值 ↔ 样式值
        ("opacity", "Opacity", (0, 100), lambda v: v / 100.0, lambda s: round(s.opacity * 100)),
        ("font_size", "Size", (MIN_FONT_SIZE, MAX_FONT_SIZE), int, lambda s: s.font_size),
        ("spacing", "Spacing", (MIN_SPACING, MAX_SPACING), int, lambda s: s.spacing),
        ("angle", "Angle", (MIN_ANGLE, MAX_ANGLE), int, lambda s: s.angle),
    )
    SUFFIXES = {"opacity": "%", "angle": "°"}
    BASE_LABEL_WIDTH = 64   # 放得下英文的 Grid Lines
    BASE_VALUE_WIDTH = 36
    BASE_SLIDER_WIDTH = 150

    def __init__(self, parent=None):
        super().__init__(parent)
        self._sliders = {}
        self._values = {}
        self._labels = {}
        self._layout = LAYOUT_TILE
        layout = QVBoxLayout(self)
        for key, text, (low, high), _to_style, _from_style in self.ROWS:
            label = QLabel(_tr(text))
            slider = QSlider(Qt.Orientation.Horizontal)
            slider.setRange(low, high)
            slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            value = QLabel()
            value.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            slider.valueChanged.connect(lambda v, k=key: self._on_slider(k, v))
            row = QHBoxLayout()
            row.addWidget(label)
            row.addWidget(slider, 1)
            row.addWidget(value)
            layout.addLayout(row)
            self._sliders[key], self._values[key], self._labels[key] = slider, value, label

        self._line_label = QLabel(_tr("Grid Lines"))
        self._line_buttons = {}
        self._line_group = QButtonGroup(self)
        line_row = QHBoxLayout()
        line_row.addWidget(self._line_label)
        for key in LINE_STYLES:
            button = QPushButton(_tr(LINE_STYLE_NAMES[key]))
            button.setObjectName("GridLineButton")
            button.setCheckable(True)
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.clicked.connect(lambda _checked=False, k=key: self.changed.emit("line_style", k))
            self._line_group.addButton(button)
            line_row.addWidget(button, 1)
            self._line_buttons[key] = button
        layout.addLayout(line_row)

        self.color_button = ColorPickerButton(QColor(WatermarkStyle.color), show_alpha=False, round_style=True)
        self.color_button.setToolTip(_tr("Custom Color"))
        self.color_button.color_changed.connect(lambda color: self.changed.emit("color", color.name()))
        self.color_row = ColorRow(self.color_button, PRESET_COLORS,
                                  lambda color_hex: self._on_preset(color_hex))
        colors = QHBoxLayout()
        colors.addWidget(self.color_row)
        colors.addStretch()
        layout.addLayout(colors)
        self.apply_scale()

    def apply_scale(self):
        self._extra_qss = self._controls_qss()
        super().apply_scale()
        layout = self.layout()
        if layout is None:
            return
        margin = scaled(12)
        layout.setContentsMargins(margin, scaled(10), margin, scaled(10))
        layout.setSpacing(scaled(6))
        for key in self._sliders:
            self._labels[key].setFixedWidth(scaled(self.BASE_LABEL_WIDTH))
            self._values[key].setFixedWidth(scaled(self.BASE_VALUE_WIDTH))
            self._sliders[key].setFixedWidth(scaled(self.BASE_SLIDER_WIDTH))
        self._line_label.setFixedWidth(scaled(self.BASE_LABEL_WIDTH))
        self.color_row.apply_scale()
        self.adjustSize()

    @staticmethod
    def _controls_qss() -> str:
        accent = accent_color().name()
        groove = "rgba(15, 23, 42, 0.12)"
        ink = CONTROL_INK.name()
        return f"""
        QLabel {{ color: {ink}; background: transparent; font-size: {scaled(12)}px; }}
        QSlider {{ background: transparent; min-height: {scaled(20)}px; }}
        QSlider::groove:horizontal {{ height: {scaled(4)}px; background: {groove}; border-radius: {scaled(2)}px; }}
        QSlider::sub-page:horizontal {{ background: {accent}; border-radius: {scaled(2)}px; }}
        QSlider::add-page:horizontal {{ background: {groove}; border-radius: {scaled(2)}px; }}
        QSlider::handle:horizontal {{
            width: {scaled(12)}px; height: {scaled(12)}px; margin: {-scaled(4)}px 0;
            background: white; border: 1px solid rgba(0, 0, 0, 0.25); border-radius: {scaled(6)}px;
        }}
        QSlider::handle:horizontal:hover {{ border-color: {accent}; }}
        QSlider::groove:horizontal:disabled, QSlider::sub-page:horizontal:disabled {{ background: {groove}; }}
        QLabel:disabled {{ color: rgba(42, 50, 67, 0.4); }}
        QPushButton#GridLineButton {{
            background: {CONTROL_FILL.name(QColor.NameFormat.HexArgb)}; color: {ink}; border: none;
            border-radius: {scaled(6)}px; padding: {scaled(3)}px {scaled(6)}px; font-size: {scaled(12)}px;
        }}
        QPushButton#GridLineButton:hover {{ background: {CONTROL_FILL_HOVER.name(QColor.NameFormat.HexArgb)}; }}
        QPushButton#GridLineButton:checked {{ background: {accent}; color: white; }}
        QPushButton#GridLineButton:disabled {{ color: rgba(42, 50, 67, 0.4); }}
        QPushButton#GridLineButton:checked:disabled {{ background: {groove}; color: rgba(42, 50, 67, 0.4); }}
        """

    def _show_value(self, key, value):
        self._values[key].setText(f"{value}{self.SUFFIXES.get(key, '')}")

    @staticmethod
    def _angle_field(layout: str) -> str:
        return "diamond_angle" if layout == LAYOUT_DIAMOND else "angle"

    def _on_slider(self, key, value):
        self._show_value(key, value)
        to_style = next(row[3] for row in self.ROWS if row[0] == key)
        field = self._angle_field(self._layout) if key == "angle" else key
        self.changed.emit(field, to_style(value))

    def _on_preset(self, color_hex: str):
        self.color_button.set_color(QColor(color_hex))
        self.changed.emit("color", QColor(color_hex).name())

    def load_style(self, style: WatermarkStyle):
        self._layout = style.layout
        for key, _text, _range, _to_style, from_style in self.ROWS:
            slider = self._sliders[key]
            slider.blockSignals(True)
            slider.setValue(getattr(style, self._angle_field(style.layout)) if key == "angle" else from_style(style))
            slider.blockSignals(False)
            self._show_value(key, slider.value())
        # 角度只对平铺和菱形起作用，网格线只对菱形起作用
        rotates = style.layout in (LAYOUT_TILE, LAYOUT_DIAMOND)
        for widget in (self._sliders["angle"], self._labels["angle"], self._values["angle"]):
            widget.setEnabled(rotates)
        self._sliders["angle"].setToolTip("" if rotates else _tr("Angle applies to tiled and diamond watermarks"))
        diamond = style.layout == LAYOUT_DIAMOND
        for widget in (self._line_label, *self._line_buttons.values()):
            widget.setEnabled(diamond)
            widget.setToolTip("" if diamond else _tr("Grid lines apply to diamond watermarks"))
        for key, button in self._line_buttons.items():
            button.setChecked(key == style.line_style)
        self.color_button.set_color(QColor(style.color))

    def retranslate(self):
        for key, text, *_rest in self.ROWS:
            self._labels[key].setText(_tr(text))
        self._line_label.setText(_tr("Grid Lines"))
        for key, button in self._line_buttons.items():
            button.setText(_tr(LINE_STYLE_NAMES[key]))
        self.color_button.setToolTip(_tr("Custom Color"))


class WatermarkSettingsPanel(QWidget):
    """水印工具二级菜单"""

    style_changed = Signal(str, object)   # (WatermarkStyle 的字段名, 新值)
    text_edit_requested = Signal()
    text_cleared = Signal()

    BASE_MARGIN_H = 12
    BASE_MARGIN_V = 6
    BASE_SPACING = 10
    BASE_BTN = 28
    BASE_ICON = 20
    BASE_LAYOUT_COMBO_MIN_WIDTH = 96
    BASE_FONT_COMBO_WIDTH = 132

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self._style = WatermarkStyle()

        layout = QHBoxLayout(self)
        self.text_field = WatermarkTextField()
        self.text_field.clicked.connect(self.text_edit_requested)
        self.text_field.cleared.connect(self.text_cleared)
        layout.addWidget(self.text_field)
        layout.addWidget(make_separator())

        self.layout_combo = PillComboBox(text_alignment=Qt.AlignmentFlag.AlignHCenter)
        for key in LAYOUTS:
            self.layout_combo.addItem(_tr(LAYOUT_NAMES[key]), key)
        self.layout_combo.setToolTip(_tr("Position"))
        self.layout_combo.currentIndexChanged.connect(self._on_layout_changed)
        layout.addWidget(self.layout_combo)

        self.style_button = QPushButton()
        self.style_button.setIcon(ResourceManager.get_icon(ResourceManager.get_icon_path("颜色设置.svg")))
        self.style_button.setToolTip(_tr("Watermark Style"))
        self.style_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.style_button.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.style_button.installEventFilter(self)
        layout.addWidget(self.style_button)

        self.font_combo = PillComboBox()
        self.font_combo.setToolTip(_tr("Font"))
        self.font_combo.setMaxVisibleItems(12)
        self._fill_fonts()
        self.font_combo.currentIndexChanged.connect(self._on_font_changed)
        layout.addWidget(self.font_combo)

        self._style_popup = None
        self.apply_scale()

    # ------------------------------------------------------------------
    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def _build_stylesheet(self) -> str:
        # 两个下拉框宽度不同，各自 setFixedWidth；样式表里的宽度限制会盖过它，放开
        return build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="1px",
            combo_min_width=0,
            combo_max_width=2000,
            combo_padding_compact=True,
        )

    def apply_scale(self):
        self.setStyleSheet(self._build_stylesheet())
        mh, mv = scaled(self.BASE_MARGIN_H), scaled(self.BASE_MARGIN_V)
        layout = self.layout()
        layout.setContentsMargins(mh, mv, mh, mv)
        layout.setSpacing(scaled(self.BASE_SPACING))
        self.text_field.apply_scale()
        self._fit_layout_combo()
        self.font_combo.setFixedWidth(scaled(self.BASE_FONT_COMBO_WIDTH))
        side = scaled(self.BASE_BTN)
        self.style_button.setFixedSize(side, side)
        self.style_button.setIconSize(QSize(scaled(self.BASE_ICON), scaled(self.BASE_ICON)))
        self.style_button.setStyleSheet(
            "QPushButton { padding: 0px; border: none; border-radius: %dpx; background: transparent; }"
            "QPushButton:hover { background: %s; }" % (scaled(6), CONTROL_FILL_HOVER.name(QColor.NameFormat.HexArgb))
        )
        if self._style_popup is not None:
            self._style_popup.apply_scale()
        self.adjustSize()
        self.update()

    def _fit_layout_combo(self):
        """位置名的长短随语言差很多（「平铺」和「Bottom Center」），按最长的一项定宽，不截断。"""
        combo = self.layout_combo
        combo.ensurePolished()   # 字号来自样式表，量之前先让它生效
        metrics = combo.fontMetrics()
        longest = max(metrics.horizontalAdvance(combo.itemText(i)) for i in range(combo.count()))
        chrome = scaled(PillComboBox.BASE_PADDING_LEFT + PillComboBox.BASE_ARROW_COLUMN + 6)
        combo.setFixedWidth(max(scaled(self.BASE_LAYOUT_COMBO_MIN_WIDTH), longest + chrome))

    def _fill_fonts(self):
        self.font_combo.blockSignals(True)
        self.font_combo.clear()
        self.font_combo.addItem(_tr("Default Font"), "")
        self.font_combo.setItemData(0, get_default_text_font_for_language(), Qt.ItemDataRole.ToolTipRole)
        for family in get_available_text_fonts():
            self.font_combo.addItem(family, family)
        self.font_combo.blockSignals(False)

    # ------------------------------------------------------------------
    def eventFilter(self, watched, event):
        if watched is self.style_button:
            if event.type() == QEvent.Type.Enter:
                self.show_style_popup()
            elif event.type() == QEvent.Type.Leave and self._style_popup is not None:
                self._style_popup.close_soon()
        return super().eventFilter(watched, event)

    def show_style_popup(self):
        if self._style_popup is None:
            popup = WatermarkStylePopup(self)
            popup.changed.connect(self._on_popup_changed)
            self._style_popup = popup
        self._style_popup.load_style(self._style)
        self._style_popup.show_beside(self, self.style_button)

    def _on_popup_changed(self, key, value):
        self._style = self._style.with_changes(**{key: value})
        self.style_changed.emit(key, value)

    def _on_layout_changed(self):
        layout_key = self.layout_combo.currentData() or LAYOUT_TILE
        self._style = self._style.with_changes(layout=layout_key)
        if self._style_popup is not None:
            self._style_popup.load_style(self._style)
        self.style_changed.emit("layout", layout_key)

    def _on_font_changed(self):
        family = self.font_combo.currentData() or ""
        self._style = self._style.with_changes(font_family=family)
        self.style_changed.emit("font_family", family)

    # ------------------------------------------------------------------
    # 供 Toolbar 调用的公共接口（不触发信号）
    # ------------------------------------------------------------------
    def load_style(self, style: WatermarkStyle):
        self._style = style.normalized()
        for combo, value in ((self.layout_combo, self._style.layout), (self.font_combo, self._style.font_family)):
            index = combo.findData(value)
            combo.blockSignals(True)
            combo.setCurrentIndex(max(0, index))
            combo.blockSignals(False)
        if self._style_popup is not None:
            self._style_popup.load_style(self._style)

    def set_template(self, template: str, when=None):
        self.text_field.set_template(template, when)

    def retranslate(self):
        for index, key in enumerate(LAYOUTS):
            self.layout_combo.setItemText(index, _tr(LAYOUT_NAMES[key]))
        self.layout_combo.setToolTip(_tr("Position"))
        self._fit_layout_combo()
        self.style_button.setToolTip(_tr("Watermark Style"))
        self.font_combo.setToolTip(_tr("Font"))
        current = self.font_combo.currentData() or ""
        self._fill_fonts()
        self.font_combo.blockSignals(True)
        self.font_combo.setCurrentIndex(max(0, self.font_combo.findData(current)))
        self.font_combo.blockSignals(False)
        self.text_field.retranslate()
        if self._style_popup is not None:
            self._style_popup.retranslate()
