"""
设置面板基类
提供通用的颜色、大小、透明度选择组件
统一为箭头/文字风格布局
"""
from PySide6.QtWidgets import (
    QApplication,
    QWidget,
    QPushButton,
    QHBoxLayout,
    QFrame,
    QToolButton,
    QLabel,
    QVBoxLayout,
)
from PySide6.QtCore import Qt, Signal, QEvent, QPoint, QRect, QRectF, QSize, QTimer
from PySide6.QtGui import QColor, QPainter, QPen, QBrush, QPainterPath
from core import safe_event
from core.constants import CSS_FONT_FAMILY
from core.i18n import tr as translate_text
from core.resource_manager import ResourceManager
from core.ui_scale import scaled, scaled_f
from core.ui_theme import set_own_style
from .color_picker_button import ColorPickerButton


PANEL_RADIUS = 6.0


def paint_rounded_panel(widget):
    """白底圆角 + 浅灰细描边。设置面板、截图工具栏和「…」弹层共用，四角靠 WA_TranslucentBackground 保持透明"""
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    radius = scaled_f(PANEL_RADIUS)
    pen_width = scaled_f(1.0)
    half = pen_width / 2
    rect = QRectF(widget.rect()).adjusted(half, half, -half, -half)

    path = QPainterPath()
    path.addRoundedRect(rect, radius, radius)

    # 半透明黑：白色截图上能看出边界，深色截图上几乎不可见，不会像描边框
    painter.setPen(QPen(QColor(0, 0, 0, 36), pen_width))
    painter.setBrush(QBrush(QColor("#ffffff")))
    painter.drawPath(path)
    painter.end()


def set_step_button_icon(button: QToolButton, direction: str):
    """为步进按钮使用固定深色 SVG，避免箭头颜色受系统调色板影响。"""
    icon_name = "step_up.svg" if direction == "up" else "step_down.svg"
    button.setArrowType(Qt.ArrowType.NoArrow)
    button.setIcon(ResourceManager.get_icon(ResourceManager.get_icon_path(icon_name)))
    button.setIconSize(QSize(scaled(9), scaled(5)))


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
    """
    font_px = scaled(12)
    radius = scaled(3)
    combo_block = ""
    if combo_enabled:
        combo_block = f"""
            QComboBox {{
                border: 1px solid #ccc;
                border-radius: {radius}px;
                padding: 0px;
                padding-right: 0px;
                background: white;
                min-width: {scaled(combo_min_width)}px;
                max-width: {scaled(combo_max_width)}px;
                font-family: {CSS_FONT_FAMILY};
                font-size: {font_px}px;
                color: #333;
            }}
            QComboBox:hover {{
                border: 1px solid #999;
            }}
            QComboBox::drop-down {{
                subcontrol-origin: content;
                subcontrol-position: top right;
                width: 0px;
                border: none;
                border-top-right-radius: {radius}px;
                border-bottom-right-radius: {radius}px;
                background: white;
            }}
            QComboBox::down-arrow {{
                image: none;
                border: none;
                width: 0px;
                height: 0px;
                margin: 0px;
            }}
            QComboBox QAbstractItemView::item {{
                text-align: center;
            }}
            QComboBox QAbstractItemView {{
                border: 1px solid #999;
                background: white;
                selection-background-color: #0078d7;
                selection-color: white;
                font-family: {CSS_FONT_FAMILY};
                font-size: {font_px}px;
                color: #333;
                outline: none;
            }}
            QComboBox QAbstractItemView::item {{
                padding: {scaled(3)}px {scaled(6)}px;
                min-height: {scaled(18)}px;
                color: #333;
                background: white;
            }}
            QComboBox QAbstractItemView::item:hover {{
                background-color: #e5f3ff;
                color: #000;
            }}
            QComboBox QAbstractItemView::item:selected {{
                background-color: #0078d7;
                color: white;
            }}
        """

    return f"""
        QWidget {{
            background-color: transparent;
            border: none;
        }}
        QPushButton {{
            background-color: white;
            border: 1px solid #ddd;
            border-radius: {radius}px;
            padding: {scaled(4)}px;
            font-family: {CSS_FONT_FAMILY};
            font-size: {font_px}px;
        }}
        QPushButton:hover {{
            background-color: #f0f0f0;
            border: 1px solid #bbb;
        }}
        QPushButton:checked {{
            background-color: #e0e0e0;
            border: 1px solid #999;
        }}
        QSpinBox {{
            border: 1px solid #ccc;
            border-radius: {radius}px;
            padding: {scaled(2)}px {scaled(2)}px {scaled(2)}px {scaled(4)}px;
            background: white;
            font-family: {CSS_FONT_FAMILY};
            font-size: {font_px}px;
            color: #333;
        }}
        QSpinBox::up-button {{
            subcontrol-origin: border;
            subcontrol-position: top right;
            width: {scaled(18)}px;
            border-left: 1px solid #ccc;
            border-bottom: 1px solid #ccc;
            border-top-right-radius: {radius}px;
            background: white;
        }}
        QSpinBox::up-button:hover {{
            background: #f0f0f0;
        }}
        QSpinBox::up-button:pressed {{
            background: #e0e0e0;
        }}
        QSpinBox::up-arrow {{
            image: none;
            border-left: {scaled(4)}px solid transparent;
            border-right: {scaled(4)}px solid transparent;
            border-bottom: {scaled(6)}px solid #666;
            width: 0;
            height: 0;
        }}
        QSpinBox::down-button {{
            subcontrol-origin: border;
            subcontrol-position: bottom right;
            width: {scaled(18)}px;
            border-left: 1px solid #ccc;
            border-bottom-right-radius: {radius}px;
            background: white;
        }}
        QSpinBox::down-button:hover {{
            background: #f0f0f0;
        }}
        QSpinBox::down-button:pressed {{
            background: #e0e0e0;
        }}
        QSpinBox::down-arrow {{
            image: none;
            border-left: {scaled(4)}px solid transparent;
            border-right: {scaled(4)}px solid transparent;
            border-top: {scaled(6)}px solid #666;
            width: 0;
            height: 0;
        }}
        QLabel {{
            color: #333;
            background-color: transparent;
            border: none;
            font-family: {CSS_FONT_FAMILY};
            font-size: {font_px}px;
        }}
        QFrame#separator {{
            background-color: #ddd;
            width: 1px;
            margin: {scaled(4)}px {scaled(4)}px;
            border: none;
        }}
        {combo_block}
    """


class StepperWidget(QWidget):
    """带上下按钮的紧凑数值控件（替代 QSpinBox）"""

    valueChanged = Signal(int)

    # 基准尺寸（100% 下的实际像素）
    BASE_LABEL_HEIGHT = 23
    BASE_BTN_WIDTH = 16
    BASE_BTN_HEIGHT = 13
    BASE_LABEL_MIN_WIDTH = 26
    BASE_BTN_COLUMN = 22   # setFixedWidth 里留给上下按钮那一列的宽度
    BASE_SPACING = 4

    def __init__(self, value: int = 0, minimum: int = 0, maximum: int = 100, suffix: str = "", parent=None):
        super().__init__(parent)
        self._min = int(minimum)
        self._max = int(maximum)
        self._value = int(value)
        self._suffix = str(suffix)

        self._label = QLabel()
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)

        btn_wrap = QWidget()
        btn_layout = QVBoxLayout(btn_wrap)
        btn_layout.setContentsMargins(0, 0, 0, 0)
        btn_layout.setSpacing(scaled(2))

        self._up_btn = QToolButton()
        self._up_btn.setAutoRepeat(True)
        self._up_btn.setAutoRepeatDelay(300)
        self._up_btn.setAutoRepeatInterval(60)
        btn_layout.addWidget(self._up_btn)

        self._down_btn = QToolButton()
        self._down_btn.setAutoRepeat(True)
        self._down_btn.setAutoRepeatDelay(300)
        self._down_btn.setAutoRepeatInterval(60)
        btn_layout.addWidget(self._down_btn)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._label)
        layout.addWidget(btn_wrap)

        self._up_btn.clicked.connect(lambda: self._step(1))
        self._down_btn.clicked.connect(lambda: self._step(-1))
        self._apply_scale_sizes()
        self._refresh_label()

    def _apply_scale_sizes(self):
        """按当前比例重算标签和步进按钮的尺寸"""
        self._label.setFixedHeight(scaled(self.BASE_LABEL_HEIGHT))
        set_own_style(
            self._label,
            f"background: transparent; color: #333; border: 1px solid #999;"
            f" border-radius: {scaled(6)}px; padding: 0 {scaled(6)}px; font-weight: bold;",
        )
        for direction, button in (("up", self._up_btn), ("down", self._down_btn)):
            set_step_button_icon(button, direction)
            button.setFixedSize(scaled(self.BASE_BTN_WIDTH), scaled(self.BASE_BTN_HEIGHT))
        self.layout().setSpacing(scaled(self.BASE_SPACING))

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
        super().setFixedWidth(width)
        # width 已是按比例算好的像素，扣掉的按钮列宽和最小值也要跟着比例走
        self._apply_scale_sizes()
        label_width = max(scaled(self.BASE_LABEL_MIN_WIDTH),
                          int(width) - scaled(self.BASE_BTN_COLUMN))
        self._label.setFixedWidth(label_width)

    @safe_event
    def wheelEvent(self, event):
        """鼠标滚轮调整数值"""
        delta = event.angleDelta().y()
        if delta > 0:
            self._step(1)
        elif delta < 0:
            self._step(-1)
        event.accept()


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
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # 只描弹出层自己这一层：QSS 的类型选择器连子类一起匹配，写成裸 QWidget
        # 会给里面每个控件都套上边框。配色沿用设置面板的白底浅色，不另造深色。
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
        self.setStyleSheet(
            f"#HoverPopup {{ background: white; border: 1px solid #ccc;"
            f" border-radius: {scaled(3)}px; }}" + self._extra_qss
        )

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
    BASE_MARGIN_H = 9
    BASE_MARGIN_V = 7
    BASE_SPACING = 9
    BASE_SIZE_SPIN_WIDTH = 54
    BASE_OPACITY_SPIN_WIDTH = 65
    BASE_COLOR_BTN = 25
    BASE_PRESET_BTN = 22
    BASE_PRESET_RADIUS = 6
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

        self.size_spin = StepperWidget(self.current_size, self.SIZE_RANGE[0], self.SIZE_RANGE[1])
        self.size_spin.setToolTip(self._tr(self.SIZE_TOOLTIP))
        layout.addWidget(self.size_spin)

        self.opacity_spin = StepperWidget(self._opacity_to_percent(self.current_opacity), 0, 100, "%")
        self.opacity_spin.setToolTip(self._tr(self.OPACITY_TOOLTIP))
        layout.addWidget(self.opacity_spin)

        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)

        self.color_picker_btn = ColorPickerButton(
            self.current_color, size=scaled(self.BASE_COLOR_BTN), show_alpha=True
        )
        self.color_picker_btn.setToolTip(self._tr("Custom Color"))
        layout.addWidget(self.color_picker_btn)

        preset_colors = [
            "#FF0000",
            "#FFFF00",
            "#00FF00",
            "#0000FF",
            "#000000",
            "#FFFFFF",
        ]

        self._preset_buttons = []
        for color_str in preset_colors:
            btn = QPushButton()
            btn.setToolTip(color_str)
            btn.clicked.connect(lambda checked, c=color_str: self._apply_preset_color(c))
            layout.addWidget(btn)
            self._preset_buttons.append((btn, color_str))

        layout.addStretch()

        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_picker_btn.color_changed.connect(self._on_color_picked)

        self._build_extra_controls(layout)
        self.apply_scale()

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
        self.size_spin.setFixedWidth(scaled(self.BASE_SIZE_SPIN_WIDTH))
        self.opacity_spin.setFixedWidth(scaled(self.BASE_OPACITY_SPIN_WIDTH))
        color_btn = scaled(self.BASE_COLOR_BTN)
        self.color_picker_btn.setFixedSize(color_btn, color_btn)
        preset = scaled(self.BASE_PRESET_BTN)
        radius = scaled(self.BASE_PRESET_RADIUS)
        for btn, color_str in self._preset_buttons:
            btn.setFixedSize(preset, preset)
            border_color = "#888888" if color_str == "#FFFFFF" else "#333333"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {color_str};
                    border: 1px solid {border_color};
                    border-radius: {radius}px;
                }}
                QPushButton:hover {{
                    border: 2px solid #000;
                }}
            """)

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
        
    def _get_preset_button_style(self, color: str) -> str:
        """根据颜色生成预设按钮样式"""
        if color == "#FFFFFF":
            border_color = "#888888"
        elif color == "#000000":
            border_color = "#666666"
        else:
            border_color = "#333333"
        
        return f"""
            QPushButton {{
                background-color: {color};
                border: 2px solid {border_color};
                border-radius: 8px;
            }}
            QPushButton:hover {{
                border: 3px solid #000;
            }}
        """

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
 
