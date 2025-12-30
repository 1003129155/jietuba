"""
箭头工具设置面板
适用于：箭头 (arrow)
"""
from PyQt6.QtWidgets import QPushButton, QLabel, QSlider
from PyQt6.QtCore import Qt, QSize
from PyQt6.QtGui import QIcon
from .base_settings_panel import BaseSettingsPanel
from core.resource_manager import ResourceManager


class ArrowSettingsPanel(BaseSettingsPanel):
    """箭头工具设置面板 - 暂时使用与画笔相同的布局"""
    
    def __init__(self, parent=None):
        super().__init__(parent)
        
    def _build_controls(self):
        """构建控件"""
        # 1. 线宽滑动条
        self.size_slider = QSlider(Qt.Orientation.Horizontal, self)
        self.size_slider.setGeometry(5, 8, 80, 18)
        self.size_slider.setRange(1, 20)
        self.size_slider.setValue(3)
        self.size_slider.setToolTip('線の太さを設定')
        self.size_slider.valueChanged.connect(self._on_size_changed)
        
        QLabel("大小:", self).setGeometry(90, 8, 35, 18)
        self.size_value_label = QLabel("3", self)
        self.size_value_label.setGeometry(125, 8, 25, 18)
        
        # 2. 透明度滑动条
        self.opacity_slider = QSlider(Qt.Orientation.Horizontal, self)
        self.opacity_slider.setGeometry(5, 32, 80, 18)
        self.opacity_slider.setRange(1, 255)
        self.opacity_slider.setValue(255)
        self.opacity_slider.setToolTip('透明度を設定')
        self.opacity_slider.valueChanged.connect(self._on_opacity_changed)
        
        QLabel("透明:", self).setGeometry(90, 32, 35, 18)
        self.opacity_value_label = QLabel("255", self)
        self.opacity_value_label.setGeometry(125, 32, 30, 18)
        
        # 3. 颜色选择按钮
        self.color_picker_btn = QPushButton(self)
        self.color_picker_btn.setGeometry(185, 9, 40, 40)
        self.color_picker_btn.setToolTip('色を選択')
        self.color_picker_btn.setIcon(QIcon(ResourceManager.get_resource_path("svg/颜色设置.svg")))
        self.color_picker_btn.setIconSize(QSize(32, 32))
        self.color_picker_btn.clicked.connect(self._pick_color)
        self._update_color_picker_style()
        
        # 4. 颜色预设按钮
        preset_colors = [("#FF0000", "赤色"), ("#FFFF00", "黄色"), ("#00FF00", "緑色"), 
                        ("#0000FF", "青色"), ("#000000", "黒色"), ("#FFFFFF", "白色")]
        self.preset_buttons = []
        for i, (color_hex, tooltip) in enumerate(preset_colors):
            btn = QPushButton("", self)
            btn.setGeometry(240 + i * 38, 11, 34, 34)
            btn.setToolTip(f"{tooltip}\n{color_hex}")
            btn.clicked.connect(lambda checked, c=color_hex: self._apply_preset_color(c))
            btn.setStyleSheet(self._get_preset_button_style(color_hex))
            self.preset_buttons.append(btn)
