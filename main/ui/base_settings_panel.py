"""
设置面板基类
提供通用的颜色、大小、透明度选择组件
统一为箭头/文字风格布局
"""
# ── 整体缩放因子 ────────────────────────────────────────────
# 与 Toolbar.SCALE 联动，修改此值可等比例缩放所有二级设置面板。
# 1.0  → 默认尺寸
# 0.8  → 缩小 20%
# 1.2  → 放大 20%
PANEL_SCALE: float = 0.95
from PySide6.QtWidgets import (
    QWidget,
    QPushButton,
    QHBoxLayout,
    QSpinBox,
    QFrame,
    QToolButton,
    QLabel,
    QVBoxLayout,
)
from PySide6.QtCore import Qt, Signal, QCoreApplication, QRectF
from PySide6.QtGui import QColor, QPainter, QPen, QBrush, QPainterPath
from core import log_debug, safe_event
from core.constants import CSS_FONT_FAMILY
from .color_picker_button import ColorPickerButton


def paint_rounded_panel(widget):
    """为设置面板绘制圆角背景 + 描边（公共函数，供所有面板调用）"""
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    radius = 6.0
    pen_width = 2.0
    half = pen_width / 2
    rect = QRectF(widget.rect()).adjusted(half, half, -half, -half)

    path = QPainterPath()
    path.addRoundedRect(rect, radius, radius)

    painter.setPen(QPen(QColor("#333333"), pen_width))
    painter.setBrush(QBrush(QColor("#ffffff")))
    painter.drawPath(path)
    painter.end()


def build_settings_panel_stylesheet(
    *,
    combo_enabled: bool = False,
    combo_padding: str = "2px 8px 2px 4px",
    combo_min_width: int = 32,
    combo_max_width: int = 90,
    combo_padding_compact: bool = False
) -> str:
    """构建设置面板统一样式"""
    combo_block = ""
    if combo_enabled:
        combo_padding_value = "1px" if combo_padding_compact else combo_padding
        combo_block = f"""
            QComboBox {{
                border: 1px solid #ccc;
                border-radius: 3px;
                padding: 0px;
                padding-right: 0px;
                background: white;
                min-width: {combo_min_width}px;
                max-width: {combo_max_width}px;
                font-family: {CSS_FONT_FAMILY};
                font-size: 12px;
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
                border-top-right-radius: 3px;
                border-bottom-right-radius: 3px;
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
                font-size: 12px;
                color: #333;
                outline: none;
            }}
            QComboBox QAbstractItemView::item {{
                padding: 3px 6px;
                min-height: 18px;
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
            border-radius: 3px;
            padding: 4px;
            font-family: {CSS_FONT_FAMILY};
            font-size: 12px;
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
            border-radius: 3px;
            padding: 2px 2px 2px 4px;
            background: white;
            font-family: {CSS_FONT_FAMILY};
            font-size: 12px;
            color: #333;
        }}
        QSpinBox::up-button {{
            subcontrol-origin: border;
            subcontrol-position: top right;
            width: 18px;
            border-left: 1px solid #ccc;
            border-bottom: 1px solid #ccc;
            border-top-right-radius: 3px;
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
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            border-bottom: 6px solid #666;
            width: 0;
            height: 0;
        }}
        QSpinBox::down-button {{
            subcontrol-origin: border;
            subcontrol-position: bottom right;
            width: 18px;
            border-left: 1px solid #ccc;
            border-bottom-right-radius: 3px;
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
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            border-top: 6px solid #666;
            width: 0;
            height: 0;
        }}
        QLabel {{
            color: #333;
            background-color: transparent;
            border: none;
            font-family: {CSS_FONT_FAMILY};
            font-size: 12px;
        }}
        QFrame#separator {{
            background-color: #ddd;
            width: 1px;
            margin: 4px 4px;
            border: none;
        }}
        {combo_block}
    """


class StepperWidget(QWidget):
    """带上下按钮的紧凑数值控件（替代 QSpinBox）"""

    valueChanged = Signal(int)

    def __init__(self, value: int = 0, minimum: int = 0, maximum: int = 100, suffix: str = "", parent=None):
        super().__init__(parent)
        self._min = int(minimum)
        self._max = int(maximum)
        self._value = int(value)
        self._suffix = str(suffix)

        self._label = QLabel()
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._label.setFixedHeight(round(26 * PANEL_SCALE))
        self._label.setStyleSheet(
            "QLabel { background: transparent; color: #333; border: 1px solid #999; border-radius: 6px; padding: 0 6px; font-weight: bold; }"
        )

        btn_wrap = QWidget()
        btn_layout = QVBoxLayout(btn_wrap)
        btn_layout.setContentsMargins(0, 0, 0, 0)
        btn_layout.setSpacing(2)

        self._up_btn = QToolButton()
        self._up_btn.setArrowType(Qt.ArrowType.UpArrow)
        self._up_btn.setFixedSize(round(18 * PANEL_SCALE), round(14 * PANEL_SCALE))
        self._up_btn.setAutoRepeat(True)
        self._up_btn.setAutoRepeatDelay(300)
        self._up_btn.setAutoRepeatInterval(60)
        btn_layout.addWidget(self._up_btn)

        self._down_btn = QToolButton()
        self._down_btn.setArrowType(Qt.ArrowType.DownArrow)
        self._down_btn.setFixedSize(round(18 * PANEL_SCALE), round(14 * PANEL_SCALE))
        self._down_btn.setAutoRepeat(True)
        self._down_btn.setAutoRepeatDelay(300)
        self._down_btn.setAutoRepeatInterval(60)
        btn_layout.addWidget(self._down_btn)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)
        layout.addWidget(self._label)
        layout.addWidget(btn_wrap)

        self._up_btn.clicked.connect(lambda: self._step(1))
        self._down_btn.clicked.connect(lambda: self._step(-1))
        self._refresh_label()

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
        label_width = max(26, int(width) - 22)
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


class BaseSettingsPanel(QWidget):
    """设置面板基类 - 统一风格布局"""

    TRANSLATION_CONTEXT = "ArrowSettingsPanel"
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
        self.setStyleSheet(build_settings_panel_stylesheet())

        layout = QHBoxLayout(self)
        layout.setContentsMargins(round(10 * PANEL_SCALE), round(8 * PANEL_SCALE),
                                  round(10 * PANEL_SCALE), round(8 * PANEL_SCALE))
        layout.setSpacing(round(10 * PANEL_SCALE))

        self.size_spin = StepperWidget(self.current_size, self.SIZE_RANGE[0], self.SIZE_RANGE[1])
        self.size_spin.setFixedWidth(round(60 * PANEL_SCALE))
        self.size_spin.setToolTip(self._tr(self.SIZE_TOOLTIP))
        layout.addWidget(self.size_spin)

        self.opacity_spin = StepperWidget(self._opacity_to_percent(self.current_opacity), 0, 100, "%")
        self.opacity_spin.setFixedWidth(round(72 * PANEL_SCALE))
        self.opacity_spin.setToolTip(self._tr(self.OPACITY_TOOLTIP))
        layout.addWidget(self.opacity_spin)

        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)

        self.color_picker_btn = ColorPickerButton(
            self.current_color, size=round(28 * PANEL_SCALE), show_alpha=True
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

        for color_str in preset_colors:
            btn = QPushButton()
            btn.setFixedSize(round(24 * PANEL_SCALE), round(24 * PANEL_SCALE))
            btn.setToolTip(color_str)
            border_color = "#888888" if color_str == "#FFFFFF" else "#333333"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {color_str};
                    border: 1px solid {border_color};
                    border-radius: 6px;
                }}
                QPushButton:hover {{
                    border: 2px solid #000;
                }}
            """)
            btn.clicked.connect(lambda checked, c=color_str: self._apply_preset_color(c))
            layout.addWidget(btn)

        layout.addStretch()

        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_picker_btn.color_changed.connect(self._on_color_picked)

        self._build_extra_controls(layout)

    def _build_extra_controls(self, layout: QHBoxLayout):
        """扩展控件 - 子类可选实现"""
        return

    def _tr(self, text: str) -> str:
        """统一翻译入口（与箭头面板一致的翻译上下文）"""
        return QCoreApplication.translate(self.TRANSLATION_CONTEXT, text)
        
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
        """设置大小（不触发信号）"""
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
 