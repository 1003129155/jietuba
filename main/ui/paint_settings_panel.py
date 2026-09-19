"""
画笔类工具设置面板
适用于：画笔 (pen) 和 荧光笔 (highlighter)
"""
from PySide6.QtWidgets import QComboBox, QHBoxLayout, QPushButton, QButtonGroup, QWidget
from PySide6.QtCore import Signal, QSize
from core.resource_manager import ResourceManager
from core.ui_scale import scaled
from .base_settings_panel import BaseSettingsPanel, StepperWidget, build_settings_panel_stylesheet
from .color_picker_button import ColorPickerButton

def _cached_icon(svg_name):
    """获取缓存的 QIcon"""
    return ResourceManager.get_icon(ResourceManager.get_icon_path(svg_name))


class PaintSettingsPanel(BaseSettingsPanel):
    """画笔类工具设置面板 - 统一风格"""

    SIZE_RANGE = (1, 99)
    SIZE_DEFAULT = 5
    SIZE_TOOLTIP = "Line Width"

    # 基准尺寸（100% 下的实际像素）
    BASE_COMBO_WIDTH = 88
    BASE_COMBO_ICON_H = 16
    BASE_MODE_BTN = 27
    BASE_MODE_ICON = 22
    
    # 线条样式改变信号
    line_style_changed = Signal(str)  # solid / dashed / dashed_dense
    # 荧光笔绘制模式改变信号
    highlighter_mode_changed = Signal(str)  # freehand / rect
    
    def __init__(self, parent=None):
        self.current_line_style = "solid"
        self.current_highlighter_mode = "freehand"
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
        mode_btn = scaled(self.BASE_MODE_BTN)
        mode_icon = scaled(self.BASE_MODE_ICON)
        for btn in (self.freehand_btn, self.rect_btn):
            btn.setFixedSize(mode_btn, mode_btn)
            btn.setIconSize(QSize(mode_icon, mode_icon))
        self.mode_widget.layout().setSpacing(scaled(2))
        self.line_style_combo.setIconSize(
            QSize(scaled(self.BASE_COMBO_WIDTH), scaled(self.BASE_COMBO_ICON_H)))
        self._color_layout.setSpacing(scaled(self.BASE_SPACING))

    def _init_ui(self):
        """重写初始化UI以添加线条样式选择"""
        from PySide6.QtWidgets import QFrame

        layout = QHBoxLayout(self)

        # === 0. 荧光笔模式切换 ===
        self.mode_widget = QWidget()
        mode_layout = QHBoxLayout(self.mode_widget)
        mode_layout.setContentsMargins(0, 0, 0, 0)

        self.freehand_btn = QPushButton()
        self.freehand_btn.setCheckable(True)
        self.freehand_btn.setToolTip(self._tr("Freehand Highlight"))
        if ResourceManager.get_icon_path("画笔.svg"):
            self.freehand_btn.setIcon(_cached_icon("画笔.svg"))
        self.freehand_btn.setStyleSheet("QPushButton { padding: 0px; }")

        self.rect_btn = QPushButton()
        self.rect_btn.setCheckable(True)
        self.rect_btn.setToolTip(self._tr("Rect Highlight"))
        if ResourceManager.get_icon_path("方框.svg"):
            self.rect_btn.setIcon(_cached_icon("方框.svg"))
        self.rect_btn.setStyleSheet("QPushButton { padding: 0px; }")

        self.mode_group = QButtonGroup(self)
        self.mode_group.setExclusive(True)
        self.mode_group.addButton(self.freehand_btn)
        self.mode_group.addButton(self.rect_btn)

        mode_layout.addWidget(self.freehand_btn)
        mode_layout.addWidget(self.rect_btn)
        layout.addWidget(self.mode_widget)
        
        # === 1. 线条样式选择 ===
        self.line_style_combo = QComboBox()
        # 添加实线和虚线选项
        from pathlib import Path
        solid_icon_path = ResourceManager.get_icon_path("line_solid.svg")
        dashed_icon_path = ResourceManager.get_icon_path("line_dash.svg")
        dense_icon_path = ResourceManager.get_icon_path("line_dot.svg")
        
        # 检查文件是否存在
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

        # === 3. 颜色选择（连同前面的分隔线收成一组，聚光灯借用本面板时整组隐藏）===
        self.color_widget = QWidget()
        color_layout = QHBoxLayout(self.color_widget)
        color_layout.setContentsMargins(0, 0, 0, 0)
        self._color_layout = color_layout
        layout.addWidget(self.color_widget)

        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        color_layout.addWidget(line1)

        self.color_picker_btn = ColorPickerButton(
            self.current_color, size=scaled(self.BASE_COLOR_BTN), show_alpha=True
        )
        self.color_picker_btn.setToolTip(self._tr("Custom Color"))
        color_layout.addWidget(self.color_picker_btn)

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
            color_layout.addWidget(btn)
            self._preset_buttons.append((btn, color_str))

        layout.addStretch()

        # 连接信号
        self.mode_group.buttonClicked.connect(self._on_highlighter_mode_clicked)
        self.line_style_combo.currentIndexChanged.connect(self._on_line_style_changed)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_picker_btn.color_changed.connect(self._on_color_picked)

        self._build_extra_controls(layout)
        self.apply_scale()

        # 默认模式
        self.set_highlighter_mode(self.current_highlighter_mode)
        
    def _on_line_style_changed(self):
        """线条样式改变"""
        style = self.line_style_combo.currentData()
        if not style:
            # 如果没有data，使用text
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
        """获取当前线条样式"""
        return self.current_line_style
    
    @line_style.setter
    def line_style(self, value: str):
        """设置当前线条样式（不触发信号）"""
        if value in ("solid", "dashed", "dashed_dense"):
            self.current_line_style = value
            idx = self.line_style_combo.findData(value)
            if idx >= 0:
                self.line_style_combo.blockSignals(True)
                self.line_style_combo.setCurrentIndex(idx)
                self.line_style_combo.blockSignals(False)
    
    def set_state_from_item(self, item):
        """根据选中的 StrokeItem 更新面板状态"""
        if not item:
            return

        self.blockSignals(True)

        # 检查线条样式
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
                
            # 更新尺寸
            if hasattr(self, 'size_spin'):
                width = pen.widthF()
                if getattr(item, "is_highlighter", False) and hasattr(item, "get_stroke_width"):
                    logical_width = item.get_stroke_width()
                    if logical_width is not None:
                        width = logical_width
                self.size_spin.setValue(int(round(width)))
                
            # 更新颜色
            color = pen.color()
            if color is not None:
                self.current_color = color
                self.color_picker_btn.set_color(color)
                
            # 更新透明度
            opacity = color.alpha()
            if opacity is not None:
                self.current_opacity = int(opacity)
                if hasattr(self, 'opacity_spin'):
                    self.opacity_spin.setValue(self._opacity_to_percent(self.current_opacity))

        self.blockSignals(False)

    def set_line_style_visible(self, visible: bool):
        """控制线条样式控件显示（高亮笔不显示）"""
        if hasattr(self, "line_style_combo"):
            self.line_style_combo.setVisible(bool(visible))

    def set_size_visible(self, visible: bool):
        """控制线宽控件显示（聚光灯借用本面板，它的孔没有线宽）"""
        if hasattr(self, "size_spin"):
            self.size_spin.setVisible(bool(visible))

    def set_color_visible(self, visible: bool):
        """控制颜色控件显示（聚光灯借用本面板，它的幕布固定为黑色）"""
        if hasattr(self, "color_widget"):
            self.color_widget.setVisible(bool(visible))

    def set_highlighter_mode_visible(self, visible: bool):
        if hasattr(self, "mode_widget"):
            self.mode_widget.setVisible(bool(visible))

    def set_highlighter_mode(self, mode: str):
        if mode not in ("freehand", "rect"):
            mode = "freehand"
        self.current_highlighter_mode = mode
        if hasattr(self, "freehand_btn") and hasattr(self, "rect_btn"):
            self.freehand_btn.blockSignals(True)
            self.rect_btn.blockSignals(True)
            self.freehand_btn.setChecked(mode == "freehand")
            self.rect_btn.setChecked(mode == "rect")
            self.freehand_btn.blockSignals(False)
            self.rect_btn.blockSignals(False)

    def _on_highlighter_mode_clicked(self):
        if self.freehand_btn.isChecked():
            mode = "freehand"
        else:
            mode = "rect"
        self.current_highlighter_mode = mode
        self.highlighter_mode_changed.emit(mode)
