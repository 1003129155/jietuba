"""
文字工具设置面板
字体、字号、粗体/斜体/下划线，背景/描边/阴影三种效果，文字颜色
"""
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QPushButton, QComboBox, QFrame, QSlider, QLabel,
    QStyle, QStyleOptionGraphicsItem, QStyleOptionSlider,
)
from PySide6.QtCore import Qt, Signal, QEvent, QPointF, QSize
from PySide6.QtGui import QColor, QFont, QIcon, QPainter, QPixmap
from .base_settings_panel import (
    HoverPopup, StepperWidget, build_settings_panel_stylesheet,
    paint_rounded_panel, PANEL_SCALE,
)
from .color_picker_button import ColorPickerButton
from canvas.items import TextItem
from core.constants import (
    CSS_FONT_FAMILY,
    get_available_text_fonts,
    get_default_text_font_for_language,
    normalize_text_font_family,
)
from core import safe_event
from core.logger import log_exception, T


# 背景、描边、阴影三个弹出层共用同一组预设色：弹层长一个样，白色在哪个里面都在同一个位置
EFFECT_PRESET_COLORS = (
    ("#FFFFFF", "White"),
    ("#000000", "Black"),
    ("#FFFF00", "Yellow"),
)

# 描边档位的提示，与 TextItem.OUTLINE_WIDTH_LEVELS 一一对应。不报数字：档位是字号的
# 比例，"0.07"对用户没有意义，他挑的是观感。
OUTLINE_WIDTH_TIPS = (
    "Outline: Thin",
    "Outline: Medium",
    "Outline: Thick",
    "Outline: Extra Thick",
)

# 效果图标只说明"这是什么效果、有多粗"，不表示颜色，所以固定用中性色（与序号样式条
# 同理）：跟着当前描边色走的话，白色描边在白底按钮上什么都看不见。颜色由弹出层里的
# 色块表示。
EFFECT_ICON_INK = QColor("#444444")


def render_text_effect_icon(side: int, ratio: float = 1.0, *, outline_width: float = None,
                            shadow: bool = False) -> QPixmap:
    """把一个真的 TextItem 画成图标，图标里的描边粗细、阴影距离和画出来的是同一套算法。

    描边图标是深色边的白字，档位之间粗细一眼可比；阴影图标是带影子的深色字。
    整个图元按比例缩进图标：描边和阴影本来就按字号比例算，缩放不改变它们的观感。
    ratio 传屏幕的 devicePixelRatio，按物理像素出图，高分屏上才不糊。
    """
    pixels = max(1, round(side * max(1.0, float(ratio))))
    pixmap = QPixmap(pixels, pixels)
    pixmap.fill(Qt.GlobalColor.transparent)

    font = QFont("Arial", 48)
    font.setBold(True)
    ink = QColor("white") if outline_width is not None else EFFECT_ICON_INK
    item = TextItem("A", QPointF(0, 0), font, ink)
    item.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
    item.document().setDocumentMargin(0)
    if outline_width is not None:
        item.set_outline(True, EFFECT_ICON_INK, outline_width)
    item.set_shadow(shadow)

    rect = item.boundingRect()
    scale = pixels / max(rect.width(), rect.height())
    painter = QPainter(pixmap)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.translate(pixels / 2.0, pixels / 2.0)
        painter.scale(scale, scale)
        painter.translate(-rect.center())
        item.paint(painter, QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    pixmap.setDevicePixelRatio(max(1.0, float(ratio)))
    return pixmap


def _preset_color_button(color_hex: str, side: int, tooltip: str) -> QPushButton:
    button = QPushButton()
    button.setFixedSize(side, side)
    button.setToolTip(tooltip)
    border_color = "#888888" if color_hex == "#FFFFFF" else "#333333"
    button.setStyleSheet(f"""
        QPushButton {{
            background-color: {color_hex};
            border: 1px solid {border_color};
            border-radius: 6px;
        }}
        QPushButton:hover {{
            border: 2px solid #000;
        }}
    """)
    return button


class _FourStepSlider(QSlider):
    """在标准滑条轨道上补四个明确的档位点，避免离散选项看起来像连续值。"""

    def paintEvent(self, event):
        super().paintEvent(event)

        option = QStyleOptionSlider()
        self.initStyleOption(option)
        style = self.style()
        groove = style.subControlRect(
            QStyle.ComplexControl.CC_Slider,
            option,
            QStyle.SubControl.SC_SliderGroove,
            self,
        )
        handle = style.subControlRect(
            QStyle.ComplexControl.CC_Slider,
            option,
            QStyle.SubControl.SC_SliderHandle,
            self,
        )
        span = groove.width() - handle.width()
        if span <= 0:
            return

        painter = QPainter(self)
        try:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#9da5ad"))
            for value in range(self.minimum(), self.maximum() + 1):
                if value == self.value():
                    continue
                offset = QStyle.sliderPositionFromValue(
                    self.minimum(), self.maximum(), value, span, option.upsideDown,
                )
                center = QPointF(
                    groove.x() + offset + handle.width() / 2,
                    groove.center().y(),
                )
                painter.drawEllipse(center, 1.75, 1.75)
        finally:
            painter.end()


class TextSettingsPanel(QWidget):
    """文字工具二级菜单"""

    # 信号定义
    font_changed = Signal(QFont)
    color_changed = Signal(QColor)
    background_changed = Signal(bool, QColor, int)
    outline_changed = Signal(bool, QColor, float)  # 宽度是 TextItem.OUTLINE_WIDTH_LEVELS 之一
    shadow_changed = Signal(bool, QColor)          # 颜色的 alpha 即阴影不透明度

    # 弹出层里的控件。描边粗细虽只有四档，但用带刻度的离散滑条呈现：
    # 比四个零散的小方块更紧凑，也不会伪装成可取任意数值的连续滑条。
    _POPUP_CONTROLS_QSS = f"""
        QPushButton {{
            background-color: white;
            border: 1px solid #ddd;
            border-radius: 3px;
            padding: 0px;
        }}
        QPushButton:hover {{
            border-color: #bbb;
        }}
        QPushButton:checked {{
            border: 2px solid #0078d7;
        }}
        QSlider {{
            background-color: transparent;
        }}
        QSlider#outlineWidthSlider {{
            min-height: 24px;
        }}
        QSlider#outlineWidthSlider::groove:horizontal {{
            height: 4px;
            margin: 0 6px;
            background: #d7d7d7;
            border-radius: 2px;
        }}
        QSlider#outlineWidthSlider::sub-page:horizontal {{
            margin: 0 6px;
            background: #0078d7;
            border-radius: 2px;
        }}
        QSlider#outlineWidthSlider::add-page:horizontal {{
            margin: 0 6px;
            background: #d7d7d7;
            border-radius: 2px;
        }}
        QSlider#outlineWidthSlider::handle:horizontal {{
            width: 12px;
            height: 12px;
            margin: -5px -6px;
            background: #0078d7;
            border: 2px solid white;
            border-radius: 7px;
        }}
        QSlider#outlineWidthSlider::handle:horizontal:hover {{
            background: #0067b8;
        }}
        QFrame#popupDivider {{
            background: #e8e8e8;
            border: none;
            min-height: 1px;
            max-height: 1px;
        }}
        QLabel {{
            color: #333;
            font-family: {CSS_FONT_FAMILY};
            font-size: 11px;
        }}
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self._cached_fonts = get_available_text_fonts()
        default_font_family = get_default_text_font_for_language()

        self.current_font = QFont(default_font_family, 16)
        self.current_color = QColor(Qt.GlobalColor.red)
        self.background_enabled = False
        self.background_color = QColor(255, 255, 255)
        self.background_opacity = 255
        self.outline_enabled = False
        self.outline_color = QColor(TextItem.DEFAULT_OUTLINE_COLOR)
        self.outline_width = TextItem.DEFAULT_OUTLINE_WIDTH
        self.shadow_enabled = False
        self.shadow_color = QColor(TextItem.DEFAULT_SHADOW_COLOR)
        # 效果开关 → 它的悬停弹出层
        self._effect_popups = {}

        self._init_ui()
        self._connect_signals()

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def _init_ui(self):
        """初始化UI布局"""
        self.setStyleSheet(build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="2px 8px 2px 4px",
            combo_min_width=40,
            combo_max_width=120
        ) + f"""
            QCheckBox {{
                spacing: 5px;
                background-color: transparent;
                border: none;
                font-family: {CSS_FONT_FAMILY};
                font-size: 12px;
                color: #333;
            }}
        """)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(round(10 * PANEL_SCALE), round(8 * PANEL_SCALE),
                                  round(10 * PANEL_SCALE), round(8 * PANEL_SCALE))
        layout.setSpacing(round(10 * PANEL_SCALE))

        # === 1. 基础样式区 ===

        # 字体选择 - 使用轻量白名单，避免扫描系统字体库
        self.font_combo = QComboBox()
        sorted_fonts = list(self._cached_fonts)
        self.font_combo.addItems(sorted_fonts)
        # 设置当前语言对应的默认字体
        default_font_family = get_default_text_font_for_language()
        if default_font_family in sorted_fonts:
            self.font_combo.setCurrentText(default_font_family)
        elif len(sorted_fonts) > 0:
            self.font_combo.setCurrentIndex(0)
        self.font_combo.setToolTip(self.tr("Font"))

        # 确保字体组合框有合理的最大可见项数
        self.font_combo.setMaxVisibleItems(15)
        # 设置下拉列表的最小宽度，确保字体名称完整显示
        self.font_combo.view().setMinimumWidth(200)

        layout.addWidget(self.font_combo)

        # 字号选择
        self.size_spin = StepperWidget(16, 8, 144)
        self.size_spin.setFixedWidth(round(60 * PANEL_SCALE))
        self.size_spin.setToolTip(self.tr("Font Size"))
        layout.addWidget(self.size_spin)

        # 样式按钮组 (粗体/斜体/下划线)
        _btn_sz = round(28 * PANEL_SCALE)
        self.bold_btn = QPushButton("B")
        self.bold_btn.setCheckable(True)
        self.bold_btn.setFixedSize(_btn_sz, _btn_sz)
        self.bold_btn.setToolTip(self.tr("Bold"))
        # 使用CSS确保粗体效果，不依赖字体变体
        self.bold_btn.setStyleSheet("""
            QPushButton {
                font-weight: bold;
                font-size: 14px;
                font-family: Arial, sans-serif;
            }
        """)

        self.italic_btn = QPushButton("I")
        self.italic_btn.setCheckable(True)
        self.italic_btn.setFixedSize(_btn_sz, _btn_sz)
        self.italic_btn.setToolTip(self.tr("Italic"))
        # 使用CSS确保斜体效果
        self.italic_btn.setStyleSheet("""
            QPushButton {
                font-style: italic;
                font-size: 14px;
                font-family: Arial, sans-serif;
            }
        """)

        self.underline_btn = QPushButton("U")
        self.underline_btn.setCheckable(True)
        self.underline_btn.setFixedSize(_btn_sz, _btn_sz)
        self.underline_btn.setToolTip(self.tr("Underline"))
        # 使用CSS确保下划线效果
        self.underline_btn.setStyleSheet("""
            QPushButton {
                text-decoration: underline;
                font-size: 14px;
                font-family: Arial, sans-serif;
            }
        """)

        layout.addWidget(self.bold_btn)
        layout.addWidget(self.italic_btn)
        layout.addWidget(self.underline_btn)

        # 背景 / 描边 / 阴影：都是"开关 + 打开后悬停弹出设置层"
        _icon_sz = round(20 * PANEL_SCALE)
        ratio = self.devicePixelRatioF()
        self.background_btn = QPushButton("BG")
        self.outline_btn = QPushButton()
        self.outline_btn.setIcon(QIcon(render_text_effect_icon(
            _icon_sz, ratio, outline_width=TextItem.DEFAULT_OUTLINE_WIDTH)))
        self.shadow_btn = QPushButton()
        self.shadow_btn.setIcon(QIcon(render_text_effect_icon(_icon_sz, ratio, shadow=True)))
        for button, tip in (
            (self.background_btn, self.tr("Text Background")),
            (self.outline_btn, self.tr("Text Outline")),
            (self.shadow_btn, self.tr("Text Shadow")),
        ):
            button.setCheckable(True)
            button.setFixedSize(_btn_sz, _btn_sz)
            button.setIconSize(QSize(_icon_sz, _icon_sz))
            button.setToolTip(tip)
            layout.addWidget(button)
        self._update_background_btn_style()

        # 分隔线
        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)

        # === 2. 颜色预设区 ===

        # 颜色选择按钮
        self.color_btn = ColorPickerButton(
            self.current_color, size=_btn_sz, show_alpha=True
        )
        self.color_btn.setToolTip(self.tr("Custom Color"))
        layout.addWidget(self.color_btn)

        # 预设颜色按钮
        preset_colors = [
            "#FF0000", # 红色
            "#FFFF00", # 黄色
            "#00FF00", # 绿色
            "#0000FF", # 蓝色
            "#000000", # 黑色
            "#FFFFFF", # 白色
        ]

        _preset_sz = round(24 * PANEL_SCALE)
        for color_str in preset_colors:
            btn = _preset_color_button(color_str, _preset_sz, color_str)
            btn.clicked.connect(lambda checked, c=color_str: self._on_preset_color_clicked(c))
            layout.addWidget(btn)

        layout.addStretch()

        self._init_effect_popups()

    def _init_effect_popups(self):
        """三个效果各自的弹出层：第一行都是颜色，第二行是各自的"量"。"""
        # 背景：颜色 + 不透明度
        popup, self.background_color_btn = self._build_effect_popup(
            self.background_btn, self.tr("Custom Background Color"), self._on_background_color_picked,
        )
        self.background_opacity_slider, self.background_opacity_label = self._add_opacity_row(
            popup, self.tr("Background Opacity"), self._on_background_opacity_changed,
        )
        self.set_background_settings(self.background_enabled, self.background_color, self.background_opacity)
        self._fit_effect_popup_width(popup)

        # 描边：颜色 + 四档离散粗细滑条
        popup, self.outline_color_btn = self._build_effect_popup(
            self.outline_btn, self.tr("Custom Outline Color"), self._on_outline_color_picked,
        )
        self.outline_width_slider = self._add_outline_width_slider(popup)
        self.set_outline_settings(self.outline_enabled, self.outline_color, self.outline_width)
        self._fit_effect_popup_width(popup)

        # 阴影：颜色 + 不透明度
        popup, self.shadow_color_btn = self._build_effect_popup(
            self.shadow_btn, self.tr("Custom Shadow Color"), self._on_shadow_color_picked,
        )
        self.shadow_opacity_slider, self.shadow_opacity_label = self._add_opacity_row(
            popup, self.tr("Shadow Opacity"), self._on_shadow_opacity_changed,
        )
        self.set_shadow_settings(self.shadow_enabled, self.shadow_color)
        self._fit_effect_popup_width(popup)

    @staticmethod
    def _fit_effect_popup_width(popup: HoverPopup):
        """让顶层弹层按内容收口，而不是被 QWidget 的 200px 默认宽度撑开。"""
        # HoverPopup 是顶层窗口。里面有可扩展的 QSlider 时，adjustSize() 会给它兜底
        # 到 200px，导致滑条无意义地被拉长；限制最大宽度后仍可由布局决定实际尺寸。
        popup.setMaximumWidth(popup.sizeHint().width())

    def _build_effect_popup(self, toggle: QPushButton, custom_tip: str, on_color):
        """建一个挂在效果开关上的弹出层，放好颜色那一行；返回弹出层和它的自定义颜色按钮。"""
        popup = HoverPopup(self)
        popup.setStyleSheet(popup.styleSheet() + self._POPUP_CONTROLS_QSS)
        popup_layout = QVBoxLayout(popup)
        popup_layout.setContentsMargins(8, 6, 8, 6)
        popup_layout.setSpacing(6)

        colors_layout = QHBoxLayout()
        colors_layout.setSpacing(6)
        colors_layout.addStretch()
        # 颜色和不透明度分开调（不透明度有自己的滑块），自定义色不带 alpha 通道
        custom = ColorPickerButton(QColor("white"), show_alpha=False, size=24)
        custom.setToolTip(custom_tip)
        custom.color_changed.connect(on_color)
        colors_layout.addWidget(custom)
        for color_hex, name in EFFECT_PRESET_COLORS:
            preset = _preset_color_button(color_hex, 24, self.tr(name))
            preset.clicked.connect(lambda _checked=False, c=color_hex: on_color(QColor(c)))
            colors_layout.addWidget(preset)
        colors_layout.addStretch()
        popup_layout.addLayout(colors_layout)

        divider = QFrame()
        divider.setObjectName("popupDivider")
        divider.setFrameShape(QFrame.Shape.HLine)
        divider.setFixedHeight(1)
        popup_layout.addWidget(divider)

        toggle.installEventFilter(self)
        toggle.toggled.connect(lambda checked, b=toggle: self._sync_effect_popup(b, checked))
        self._effect_popups[toggle] = popup
        popup.hide()
        return popup, custom

    def _add_opacity_row(self, popup: HoverPopup, tip: str, on_change):
        slider = QSlider(Qt.Orientation.Horizontal)
        slider.setRange(0, 255)
        slider.setToolTip(tip)
        label = QLabel()
        label.setFixedWidth(round(40 * PANEL_SCALE))
        row = QHBoxLayout()
        row.addWidget(slider)
        row.addWidget(label)
        popup.layout().addLayout(row)
        slider.valueChanged.connect(on_change)
        return slider, label

    def _add_outline_width_slider(self, popup: HoverPopup) -> QSlider:
        """建立四档离散滑条；两端图标说明从细到粗的方向。"""
        icon_side = round(20 * PANEL_SCALE)
        ratio = self.devicePixelRatioF()
        row = QHBoxLayout()
        row.setSpacing(round(6 * PANEL_SCALE))

        previews = []
        for level, tip in (
            (TextItem.OUTLINE_WIDTH_LEVELS[0], OUTLINE_WIDTH_TIPS[0]),
            (TextItem.OUTLINE_WIDTH_LEVELS[-1], OUTLINE_WIDTH_TIPS[-1]),
        ):
            preview = QLabel()
            preview.setFixedSize(icon_side, icon_side)
            preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
            preview.setPixmap(render_text_effect_icon(icon_side, ratio, outline_width=level))
            preview.setToolTip(self.tr(tip))
            previews.append(preview)

        slider = _FourStepSlider(Qt.Orientation.Horizontal)
        slider.setObjectName("outlineWidthSlider")
        slider.setRange(0, len(TextItem.OUTLINE_WIDTH_LEVELS) - 1)
        slider.setSingleStep(1)
        slider.setPageStep(1)
        slider.setTickInterval(1)
        slider.setTickPosition(QSlider.TickPosition.TicksBelow)
        slider.setTracking(False)
        slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        slider.setFixedWidth(round(120 * PANEL_SCALE))

        row.addWidget(previews[0])
        row.addWidget(slider)
        row.addWidget(previews[1])
        popup.layout().addLayout(row)
        slider.valueChanged.connect(self._on_outline_width_changed)
        self._outline_width_previews = tuple(previews)
        return slider

    def _connect_signals(self):
        """连接内部信号"""
        self.font_combo.currentTextChanged.connect(self._on_font_changed)
        self.size_spin.valueChanged.connect(self._on_font_changed)
        self.bold_btn.toggled.connect(self._on_font_changed)
        self.italic_btn.toggled.connect(self._on_font_changed)
        self.underline_btn.toggled.connect(self._on_font_changed)

        self.color_btn.color_changed.connect(self._on_color_picked)
        self.background_btn.toggled.connect(self._on_background_toggled)
        self.outline_btn.toggled.connect(self._on_outline_toggled)
        self.shadow_btn.toggled.connect(self._on_shadow_toggled)

    def _on_color_picked(self, color: QColor):
        """颜色选择器回调"""
        self.current_color = color
        self.color_changed.emit(color)

    def _on_preset_color_clicked(self, color_str):
        """点击预设颜色"""
        color = QColor(color_str)
        self.current_color = color
        self.color_btn.set_color(color)
        self.color_changed.emit(color)

    # ------------------------------------------------------------------
    # 效果开关的弹出层
    # ------------------------------------------------------------------

    def _sync_effect_popup(self, toggle: QPushButton, checked: bool):
        """开关刚打开、鼠标还停在上面，就地弹出设置层；关掉就收起。"""
        popup = self._effect_popups[toggle]
        if not checked:
            popup.hide()
        elif toggle.underMouse():
            popup.show_beside(self, toggle)

    @safe_event
    def eventFilter(self, obj, event):
        popup = self._effect_popups.get(obj)
        if popup is not None:
            if event.type() == QEvent.Type.Enter and obj.isChecked():
                popup.show_beside(self, obj)
            elif event.type() == QEvent.Type.Leave:
                popup.close_soon()
        return super().eventFilter(obj, event)

    @staticmethod
    def _refresh_opacity_label(label: QLabel, alpha: int):
        percent = max(0, min(100, round(int(alpha) / 255 * 100)))
        label.setText(f"{percent}%")

    # ------------------------------------------------------------------
    # 背景
    # ------------------------------------------------------------------

    def _update_background_btn_style(self):
        """更新背景按钮样式"""
        if self.background_btn.isChecked():
            color = QColor(self.background_color)
            alpha = max(0, min(255, int(self.background_opacity)))
            luminance = color.red() * 3 + color.green() * 6 + color.blue()
            text_color = "#000000" if luminance >= 1280 else "#ffffff"
            self.background_btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: rgba({color.red()}, {color.green()}, {color.blue()}, {alpha});
                    color: {text_color};
                    border: 1px solid #999;
                    font-size: 12px;
                }}
            """)
        else:
            self.background_btn.setStyleSheet("""
                QPushButton {
                    background-color: white;
                    border: 1px solid #ddd;
                    font-size: 12px;
                }
                QPushButton:hover {
                    background-color: #f0f0f0;
                    border: 1px solid #bbb;
                }
            """)

    def _on_background_toggled(self, checked: bool):
        self.background_enabled = checked
        self._update_background_btn_style()
        self._emit_background_changed()

    def _on_background_color_picked(self, color: QColor):
        self.background_color = QColor(color)
        self.background_color_btn.set_color(self.background_color)
        self._update_background_btn_style()
        self._emit_background_changed()

    def _on_background_opacity_changed(self, value: int):
        self.background_opacity = value
        self._refresh_opacity_label(self.background_opacity_label, value)
        self._update_background_btn_style()
        self._emit_background_changed()

    def _emit_background_changed(self):
        color = QColor(self.background_color)
        color.setAlpha(self.background_opacity)
        self.background_changed.emit(self.background_enabled, color, self.background_opacity)

    def set_background_settings(self, enabled: bool, color: QColor, opacity: int):
        """设置背景配置（不触发信号）"""
        self.background_enabled = bool(enabled)
        self.background_color = QColor(color)
        self.background_opacity = int(opacity)
        self.background_btn.blockSignals(True)
        self.background_btn.setChecked(self.background_enabled)
        self.background_btn.blockSignals(False)
        self._update_background_btn_style()
        self.background_color_btn.set_color(self.background_color)
        self.background_opacity_slider.blockSignals(True)
        self.background_opacity_slider.setValue(self.background_opacity)
        self.background_opacity_slider.blockSignals(False)
        self._refresh_opacity_label(self.background_opacity_label, self.background_opacity)

    # ------------------------------------------------------------------
    # 描边
    # ------------------------------------------------------------------

    def _on_outline_toggled(self, checked: bool):
        self.outline_enabled = checked
        self._emit_outline_changed()

    def _on_outline_color_picked(self, color: QColor):
        self.outline_color = QColor(color)
        self.outline_color_btn.set_color(self.outline_color)
        self._emit_outline_changed()

    def _on_outline_width_changed(self, index: int):
        level = TextItem.OUTLINE_WIDTH_LEVELS[index]
        self._update_outline_width_slider_tooltip(index)
        if level == self.outline_width:
            # 回到当前档不算改动：白发一次信号，还白写一次设置。
            return
        self.outline_width = level
        self._emit_outline_changed()

    def _update_outline_width_slider_tooltip(self, index: int = None):
        if index is None:
            index = self.outline_width_slider.value()
        self.outline_width_slider.setToolTip(self.tr(OUTLINE_WIDTH_TIPS[index]))

    def _emit_outline_changed(self):
        self.outline_changed.emit(self.outline_enabled, QColor(self.outline_color), self.outline_width)

    def set_outline_settings(self, enabled: bool, color: QColor, width: float):
        """设置描边配置（不触发信号）"""
        self.outline_enabled = bool(enabled)
        self.outline_color = QColor(color)
        # 吸附规则只有 TextItem 一份，面板高亮的档才和实际画出来的粗细对得上
        self.outline_width = TextItem.normalize_outline_width(width)
        self.outline_btn.blockSignals(True)
        self.outline_btn.setChecked(self.outline_enabled)
        self.outline_btn.blockSignals(False)
        self.outline_color_btn.set_color(self.outline_color)
        index = TextItem.OUTLINE_WIDTH_LEVELS.index(self.outline_width)
        self.outline_width_slider.blockSignals(True)
        self.outline_width_slider.setValue(index)
        self.outline_width_slider.blockSignals(False)
        self._update_outline_width_slider_tooltip(index)

    # ------------------------------------------------------------------
    # 阴影
    # ------------------------------------------------------------------

    def _on_shadow_toggled(self, checked: bool):
        self.shadow_enabled = checked
        self._emit_shadow_changed()

    def _on_shadow_color_picked(self, color: QColor):
        # 换色不动不透明度：它由滑块单独管
        picked = QColor(color)
        picked.setAlpha(self.shadow_color.alpha())
        self.shadow_color = picked
        self.shadow_color_btn.set_color(picked)
        self._emit_shadow_changed()

    def _on_shadow_opacity_changed(self, value: int):
        self.shadow_color.setAlpha(value)
        self._refresh_opacity_label(self.shadow_opacity_label, value)
        self._emit_shadow_changed()

    def _emit_shadow_changed(self):
        self.shadow_changed.emit(self.shadow_enabled, QColor(self.shadow_color))

    def set_shadow_settings(self, enabled: bool, color: QColor):
        """设置阴影配置（不触发信号）"""
        self.shadow_enabled = bool(enabled)
        self.shadow_color = QColor(color)
        self.shadow_btn.blockSignals(True)
        self.shadow_btn.setChecked(self.shadow_enabled)
        self.shadow_btn.blockSignals(False)
        self.shadow_color_btn.set_color(self.shadow_color)
        self.shadow_opacity_slider.blockSignals(True)
        self.shadow_opacity_slider.setValue(self.shadow_color.alpha())
        self.shadow_opacity_slider.blockSignals(False)
        self._refresh_opacity_label(self.shadow_opacity_label, self.shadow_color.alpha())

    # ------------------------------------------------------------------
    # 字体
    # ------------------------------------------------------------------

    def _on_font_changed(self):
        """字体属性改变"""
        font = QFont(self.font_combo.currentText())
        font.setPointSize(max(1, self.size_spin.value()))
        font.setBold(self.bold_btn.isChecked())
        font.setItalic(self.italic_btn.isChecked())
        font.setUnderline(self.underline_btn.isChecked())

        self.current_font = font
        self.font_changed.emit(font)

    def set_state_from_item(self, item):
        """根据选中的 TextItem 更新面板状态"""
        if not item: return

        # 阻断信号防止循环触发
        self.blockSignals(True)

        # 字体
        font = item.font()
        self.font_combo.setCurrentText(font.family())
        point_size = font.pointSize()
        if point_size <= 0:
            point_size = int(round(font.pointSizeF())) if font.pointSizeF() > 0 else 16
        self.size_spin.setValue(max(1, point_size))
        self.bold_btn.setChecked(font.bold())
        self.italic_btn.setChecked(font.italic())
        self.underline_btn.setChecked(font.underline())

        # 背景：图元里颜色和不透明度存在同一个 QColor 上，面板上分开显示
        background_color = QColor(item.background_color)
        opacity = background_color.alpha()
        background_color.setAlpha(255)
        self.set_background_settings(item.has_background, background_color, opacity)

        # 描边、阴影
        self.set_outline_settings(*item.outline_state())
        self.set_shadow_settings(*item.shadow_state())

        # 颜色
        self.current_color = item.defaultTextColor()
        self.color_btn.set_color(self.current_color)

        self.blockSignals(False)

    def retranslate(self):
        """语言切换后刷新提示文本。面板是常驻的，不刷新就得重启才跟着换语言。"""
        self.font_combo.setToolTip(self.tr("Font"))
        self.size_spin.setToolTip(self.tr("Font Size"))
        self.bold_btn.setToolTip(self.tr("Bold"))
        self.italic_btn.setToolTip(self.tr("Italic"))
        self.underline_btn.setToolTip(self.tr("Underline"))
        self.background_btn.setToolTip(self.tr("Text Background"))
        self.outline_btn.setToolTip(self.tr("Text Outline"))
        self.shadow_btn.setToolTip(self.tr("Text Shadow"))
        self.color_btn.setToolTip(self.tr("Custom Color"))
        self.background_color_btn.setToolTip(self.tr("Custom Background Color"))
        self.background_opacity_slider.setToolTip(self.tr("Background Opacity"))
        self.outline_color_btn.setToolTip(self.tr("Custom Outline Color"))
        self._update_outline_width_slider_tooltip()
        for tip, preview in zip(
            (OUTLINE_WIDTH_TIPS[0], OUTLINE_WIDTH_TIPS[-1]),
            self._outline_width_previews,
        ):
            preview.setToolTip(self.tr(tip))
        self.shadow_color_btn.setToolTip(self.tr("Custom Shadow Color"))
        self.shadow_opacity_slider.setToolTip(self.tr("Shadow Opacity"))

    # ── 配置读写 ──────────────────────────────────────────

    def load_from_config(self):
        """从 ToolSettingsManager 加载文字工具配置到面板控件。"""
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            text_settings = manager.get_tool_settings("text")
            if not text_settings:
                return

            font = QFont(normalize_text_font_family(text_settings.get("font_family", "")))
            font.setPointSize(text_settings.get("font_size", 16))
            font.setBold(text_settings.get("font_bold", False))
            font.setItalic(text_settings.get("font_italic", False))
            font.setUnderline(text_settings.get("font_underline", False))
            color = QColor(text_settings.get("color", "#FF0000"))

            self.font_combo.setCurrentText(font.family())
            point_size = font.pointSize()
            if point_size <= 0:
                point_size = int(round(font.pointSizeF())) if font.pointSizeF() > 0 else 16
            self.size_spin.setValue(max(1, point_size))
            self.bold_btn.setChecked(font.bold())
            self.italic_btn.setChecked(font.italic())
            self.underline_btn.setChecked(font.underline())
            self.current_color = color
            self.color_btn.set_color(color)

            bg_enabled = text_settings.get("background_enabled", False)
            bg_color = QColor(text_settings.get("background_color", "#FFFFFF"))
            bg_opacity = text_settings.get("background_opacity", 255)
            self.set_background_settings(bg_enabled, bg_color, bg_opacity)

            self.set_outline_settings(
                text_settings.get("outline_enabled"),
                QColor(text_settings.get("outline_color")),
                text_settings.get("outline_width"),
            )
            self.set_shadow_settings(
                text_settings.get("shadow_enabled"),
                QColor(text_settings.get("shadow_color")),
            )
        except Exception as e:
            log_exception(e, T("加载文字设置"))

    @staticmethod
    def save_font_to_config(font: QFont):
        """保存字体设置到 ToolSettingsManager。"""
        try:
            from settings import get_tool_settings_manager
            get_tool_settings_manager().update_settings(
                "text",
                font_family=font.family(),
                font_size=font.pointSize(),
                font_bold=font.bold(),
                font_italic=font.italic(),
                font_underline=font.underline(),
            )
        except Exception as e:
            log_exception(e, T("保存字体设置"))

    @staticmethod
    def save_background_to_config(enabled: bool, color: QColor, opacity: int):
        """保存文字背景设置到 ToolSettingsManager。"""
        try:
            from settings import get_tool_settings_manager
            get_tool_settings_manager().update_settings(
                "text",
                background_enabled=bool(enabled),
                background_color=color.name() if hasattr(color, 'name') else str(color),
                background_opacity=int(opacity),
            )
        except Exception as e:
            log_exception(e, T("保存文字背景设置"))

    @staticmethod
    def save_outline_to_config(enabled: bool, color: QColor, width: float):
        """保存文字描边设置到 ToolSettingsManager。"""
        try:
            from settings import get_tool_settings_manager
            get_tool_settings_manager().update_settings(
                "text",
                outline_enabled=bool(enabled),
                outline_color=QColor(color).name(),
                outline_width=TextItem.normalize_outline_width(width),
            )
        except Exception as e:
            log_exception(e, T("保存文字描边设置"))

    @staticmethod
    def save_shadow_to_config(enabled: bool, color: QColor):
        """保存文字阴影设置到 ToolSettingsManager（颜色连同 alpha 存成 #AARRGGBB）。"""
        try:
            from settings import get_tool_settings_manager
            get_tool_settings_manager().update_settings(
                "text",
                shadow_enabled=bool(enabled),
                shadow_color=QColor(color).name(QColor.NameFormat.HexArgb),
            )
        except Exception as e:
            log_exception(e, T("保存文字阴影设置"))
