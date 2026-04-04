"""
箭头工具设置面板
基于文字面板布局，仅用于箭头工具
"""
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QPushButton, QLabel,
    QComboBox, QFrame,
    QListView
)
from PySide6.QtCore import Qt, Signal, QSize
from PySide6.QtGui import QColor, QIcon
from core.resource_manager import ResourceManager
from .base_settings_panel import StepperWidget, build_settings_panel_stylesheet, paint_rounded_panel, PANEL_SCALE
from .color_picker_button import ColorPickerButton
from core import safe_event

def _cached_icon(svg_name):
    """获取缓存的 QIcon"""
    return ResourceManager.get_icon(ResourceManager.get_icon_path(svg_name))


class ArrowSettingsPanel(QWidget):
    """箭头工具二级菜单"""

    arrow_style_changed = Signal(str)
    size_changed = Signal(int)
    color_changed = Signal(QColor)
    opacity_changed = Signal(int)  # 兼容旧接口（无控件）

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.current_arrow_style = "single"
        self.current_size = 3
        self.current_color = QColor(Qt.GlobalColor.red)
        self._cached_opacity = 255

        self._init_ui()
        self._connect_signals()

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def _init_ui(self):
        """初始化UI布局"""
        self.setStyleSheet(build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="1px",
            combo_min_width=88,
            combo_max_width=88,
            combo_padding_compact=True
        ))

        layout = QHBoxLayout(self)
        layout.setContentsMargins(round(10 * PANEL_SCALE), round(8 * PANEL_SCALE),
                                  round(10 * PANEL_SCALE), round(8 * PANEL_SCALE))
        layout.setSpacing(round(10 * PANEL_SCALE))

        # === 1. 基础样式区 ===
        self.arrow_style_combo = QComboBox()
        self.arrow_style_combo.setIconSize(QSize(88, 16))
        self.arrow_style_combo.addItem(_cached_icon("arrow_single.svg"), "", "single")
        self.arrow_style_combo.addItem(_cached_icon("arrow_double.svg"), "", "double")
        self.arrow_style_combo.addItem(_cached_icon("arrow_bar.svg"), "", "bar")
        self.arrow_style_combo.setToolTip(self.tr("Arrow Style"))
        self.arrow_style_combo.setMaxVisibleItems(10)
        layout.addWidget(self.arrow_style_combo)

        # 线宽选择
        self.size_spin = StepperWidget(self.current_size, 1, 20)
        self.size_spin.setFixedWidth(round(60 * PANEL_SCALE))
        self.size_spin.setToolTip(self.tr("Line Width"))
        layout.addWidget(self.size_spin)

        # 透明度选择（百分比）
        self.opacity_spin = StepperWidget(self._opacity_to_percent(self._cached_opacity), 0, 100, "%")
        self.opacity_spin.setFixedWidth(round(72 * PANEL_SCALE))
        self.opacity_spin.setToolTip(self.tr("Opacity (%)"))
        layout.addWidget(self.opacity_spin)

        # 分隔线
        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)

        # === 2. 颜色预设区 ===
        self.color_btn = ColorPickerButton(
            self.current_color, size=round(28 * PANEL_SCALE), show_alpha=True
        )
        self.color_btn.setToolTip(self.tr("Custom Color"))
        layout.addWidget(self.color_btn)

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
            btn.clicked.connect(lambda checked, c=color_str: self._on_preset_color_clicked(c))
            layout.addWidget(btn)

        layout.addStretch()

    def _connect_signals(self):
        """连接内部信号"""
        self.arrow_style_combo.currentIndexChanged.connect(self._on_arrow_style_changed)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_btn.color_changed.connect(self._on_color_picked)

    def _on_color_picked(self, color: QColor):
        """颜色选择器回调"""
        self.current_color = color
        # 同步 alpha 到 opacity_spin
        alpha = color.alpha()
        self._cached_opacity = alpha
        if hasattr(self, 'opacity_spin'):
            self.opacity_spin.blockSignals(True)
            self.opacity_spin.setValue(self._opacity_to_percent(alpha))
            self.opacity_spin.blockSignals(False)
        self.color_changed.emit(color)

    def _on_preset_color_clicked(self, color_str: str):
        """点击预设颜色"""
        color = QColor(color_str)
        self.current_color = color
        self.color_btn.set_color(color)
        self.color_changed.emit(color)

    def _on_arrow_style_changed(self):
        """箭头样式改变"""
        style = self.arrow_style_combo.currentData()
        if not style:
            style = self.arrow_style_combo.currentText()
        self.current_arrow_style = style
        self.arrow_style_changed.emit(style)

    def _on_size_changed(self, value: int):
        """线宽改变"""
        self.current_size = value
        self.size_changed.emit(value)

    def _on_opacity_changed(self, value: int):
        """透明度改变"""
        self._cached_opacity = self._percent_to_opacity(value)
        self.opacity_changed.emit(self._cached_opacity)

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
        self.color_btn.set_color(color)

    def set_size(self, size: int):
        """设置线宽（不触发信号）"""
        self.current_size = int(size)
        self.size_spin.blockSignals(True)
        self.size_spin.setValue(self.current_size)
        self.size_spin.blockSignals(False)

    def set_opacity(self, opacity: int):
        """兼容接口：箭头面板无透明度控件"""
        self._cached_opacity = int(opacity)
        self.opacity_spin.blockSignals(True)
        self.opacity_spin.setValue(self._opacity_to_percent(self._cached_opacity))
        self.opacity_spin.blockSignals(False)

    @property
    def arrow_style(self) -> str:
        """获取当前箭头样式"""
        return self.current_arrow_style

    @arrow_style.setter
    def arrow_style(self, value: str):
        """设置当前箭头样式（不触发信号）"""
        if value in ("single", "double", "bar"):
            self.current_arrow_style = value
            idx = self.arrow_style_combo.findData(value)
            if idx >= 0:
                self.arrow_style_combo.blockSignals(True)
                self.arrow_style_combo.setCurrentIndex(idx)
                self.arrow_style_combo.blockSignals(False)

    def set_state_from_item(self, item):
        """根据选中的 ArrowItem 更新面板状态"""
        if not item:
            return

        self.blockSignals(True)

        arrow_style = getattr(item, "arrow_style", None) or getattr(item, "_arrow_style", None)
        if arrow_style:
            idx = self.arrow_style_combo.findData(arrow_style)
            if idx >= 0:
                self.arrow_style_combo.setCurrentIndex(idx)

        if hasattr(item, "base_width"):
            self.size_spin.setValue(int(round(float(item.base_width))))

        color = getattr(item, "color", None)
        if color is None and hasattr(item, "pen"):
            color = item.pen().color()
        if color is not None:
            self.current_color = color
        self._update_color_btn(self.current_color)

        opacity = getattr(item, "opacity", None) or getattr(item, "_opacity", None)
        if opacity is None and color is not None:
            opacity = color.alpha()
        if opacity is not None:
            self._cached_opacity = int(opacity)
            self.opacity_spin.setValue(self._opacity_to_percent(self._cached_opacity))

        self.blockSignals(False)
 