"""
形状工具设置面板
适用于：矩形 (rect) 和 圆形 (ellipse)
"""
from PySide6.QtWidgets import QComboBox, QHBoxLayout
from PySide6.QtCore import Signal, QSize
from core.resource_manager import ResourceManager
from core.ui_scale import scaled
from .base_settings_panel import BaseSettingsPanel, StepperWidget, build_settings_panel_stylesheet
from .color_picker_button import ColorPickerButton

def _cached_icon(svg_name):
    """获取缓存的 QIcon"""
    return ResourceManager.get_icon(ResourceManager.get_icon_path(svg_name))


class ShapeSettingsPanel(BaseSettingsPanel):
    """形状工具设置面板 - 统一风格"""

    SIZE_RANGE = (1, 40)
    SIZE_DEFAULT = 3
    SIZE_TOOLTIP = "Line Width"

    # 基准尺寸（100% 下的实际像素）
    BASE_COMBO_WIDTH = 88
    BASE_COMBO_ICON_H = 16

    line_style_changed = Signal(str)  # solid / dashed / dashed_dense

    def __init__(self, parent=None):
        self.current_line_style = "solid"
        super().__init__(parent)

    def _build_stylesheet(self) -> str:
        return build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="1px",
            combo_min_width=self.BASE_COMBO_WIDTH,
            combo_max_width=self.BASE_COMBO_WIDTH,
            combo_padding_compact=True
        )

    def _apply_scale_sizes(self):
        super()._apply_scale_sizes()
        self.line_style_combo.setIconSize(
            QSize(scaled(self.BASE_COMBO_WIDTH), scaled(self.BASE_COMBO_ICON_H)))

    def _init_ui(self):
        """重写初始化UI以添加线条样式选择"""

        from PySide6.QtWidgets import QPushButton, QFrame

        layout = QHBoxLayout(self)

        # === 1. 线条样式选择 ===
        self.line_style_combo = QComboBox()
        from pathlib import Path
        solid_icon_path = ResourceManager.get_icon_path("line_solid.svg")
        dashed_icon_path = ResourceManager.get_icon_path("line_dash.svg")
        dense_icon_path = ResourceManager.get_icon_path("line_dot.svg")

        if solid_icon_path and Path(solid_icon_path).exists():
            self.line_style_combo.addItem(_cached_icon("line_solid.svg"), "", "solid")
        else:
            self.line_style_combo.addItem("──", "solid")

        if dashed_icon_path and Path(dashed_icon_path).exists():
            self.line_style_combo.addItem(_cached_icon("line_dash.svg"), "", "dashed")
        else:
            self.line_style_combo.addItem("- - -", "dashed")

        if dense_icon_path and Path(dense_icon_path).exists():
            self.line_style_combo.addItem(_cached_icon("line_dot.svg"), "", "dashed_dense")
        else:
            self.line_style_combo.addItem("- -", "dashed_dense")

        self.line_style_combo.setToolTip(self._tr("Line Style"))
        self.line_style_combo.setMaxVisibleItems(10)
        layout.addWidget(self.line_style_combo)

        # === 2. 基础控件（尺寸、透明度等）===
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

        # === 3. 颜色选择 ===
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

        self.line_style_combo.currentIndexChanged.connect(self._on_line_style_changed)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_picker_btn.color_changed.connect(self._on_color_picked)

        self._build_extra_controls(layout)
        self.apply_scale()

    def _on_line_style_changed(self):
        """线条样式改变"""
        style = self.line_style_combo.currentData()
        if not style:
            style = self.line_style_combo.currentText()
            if style == "──":
                style = "solid"
            elif style == "- - -":
                style = "dashed"
            elif style == "- -":
                style = "dashed_dense"
        self.current_line_style = style
        self.line_style_changed.emit(style)

    @property
    def line_style(self) -> str:
        return self.current_line_style

    @line_style.setter
    def line_style(self, value: str):
        if value in ("solid", "dashed", "dashed_dense"):
            self.current_line_style = value
            idx = self.line_style_combo.findData(value)
            if idx >= 0:
                self.line_style_combo.blockSignals(True)
                self.line_style_combo.setCurrentIndex(idx)
                self.line_style_combo.blockSignals(False)

    def set_state_from_item(self, item):
        if not item:
            return

        self.blockSignals(True)

        if hasattr(item, "pen"):
            pen = item.pen()
            from PySide6.QtCore import Qt
            pen_style = pen.style()
            dash_pattern = [round(x, 1) for x in pen.dashPattern()]
            has_dash = bool(dash_pattern)
            if has_dash:
                if dash_pattern[:2] == [1.0, 2.0]:
                    self.line_style = "dashed_dense"
                else:
                    self.line_style = "dashed"
            elif pen_style in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine):
                self.line_style = "dashed"
            else:
                self.line_style = "solid"

            if hasattr(self, 'size_spin'):
                self.size_spin.setValue(int(round(pen.widthF())))

            color = pen.color()
            if color is not None:
                self.current_color = color
                self.color_picker_btn.set_color(color)

            opacity = color.alpha()
            if opacity is not None:
                self.current_opacity = int(opacity)
                if hasattr(self, 'opacity_spin'):
                    self.opacity_spin.setValue(self._opacity_to_percent(self.current_opacity))

        self.blockSignals(False)
 