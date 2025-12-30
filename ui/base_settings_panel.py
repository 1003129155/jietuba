"""
设置面板基类
提供通用的颜色、大小、透明度选择组件
使用绝对定位（setGeometry）以保持与原版一致的布局
"""
from PyQt6.QtWidgets import QWidget, QPushButton, QLabel, QSlider, QColorDialog
from PyQt6.QtCore import Qt, pyqtSignal, QSize
from PyQt6.QtGui import QIcon, QColor
from core.resource_manager import ResourceManager
from core import log_debug


class BaseSettingsPanel(QWidget):
    """设置面板基类 - 使用绝对定位"""
    
    # 通用信号
    color_changed = pyqtSignal(QColor)
    size_changed = pyqtSignal(int)
    opacity_changed = pyqtSignal(int)  # 0-255
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.current_color = QColor(255, 0, 0)  # 默认红色
        self.current_size = 5
        self.current_opacity = 255
        
        self._init_ui()
        
    def _init_ui(self):
        """初始化UI - 子类可以覆盖"""
        # 设置面板基础样式
        self.setStyleSheet("""
            QWidget {
                background-color: white;
                border: 1px solid #cccccc;
                border-radius: 4px;
            }
            QPushButton {
                background-color: white;
                border: 1px solid #ddd;
                border-radius: 3px;
            }
            QPushButton:hover {
                background-color: #f0f0f0;
                border: 1px solid #bbb;
            }
            QPushButton:pressed {
                background-color: #e0e0e0;
            }
            QSlider {
                background-color: transparent;
            }
            QLabel {
                background-color: transparent;
                color: rgb(51,51,51);
                font-size: 12px;
            }
        """)
        
        # 设置固定大小
        self.resize(485, 55)
        
        # 子类实现具体控件
        self._build_controls()
        
    def _build_controls(self):
        """构建控件 - 子类必须实现"""
        pass
    def _build_controls(self):
        """构建控件 - 子类必须实现"""
        pass
        
    # ========================================================================
    #  信号处理
    # ========================================================================
    
    def _on_size_changed(self, value: int):
        """大小改变"""
        self.current_size = value
        log_debug(f"size slider -> {value}", f"SettingsPanel:{self.__class__.__name__}")
        if hasattr(self, 'size_value_label'):
            self.size_value_label.setText(str(value))
        self.size_changed.emit(value)
        
    def _on_opacity_changed(self, value: int):
        """透明度改变"""
        self.current_opacity = value
        log_debug(f"opacity slider -> {value}", f"SettingsPanel:{self.__class__.__name__}")
        if hasattr(self, 'opacity_value_label'):
            self.opacity_value_label.setText(str(value))
        self.opacity_changed.emit(value)
        
    def _pick_color(self):
        """打开颜色选择器"""
        color = QColorDialog.getColor(self.current_color, self, "选择颜色")
        if color.isValid():
            self.current_color = color
            self.color_changed.emit(color)
            self._update_color_picker_style()
            
    def _apply_preset_color(self, color_hex: str):
        """应用预设颜色"""
        self.current_color = QColor(color_hex)
        self.color_changed.emit(self.current_color)
        self._update_color_picker_style()
        
    def _update_color_picker_style(self):
        """更新颜色选择按钮的背景色"""
        if not hasattr(self, 'color_picker_btn'):
            return
            
        color_hex = self.current_color.name()
        if self.current_color.lightness() > 200:
            border_color = "#888888"
        else:
            border_color = "#333333"
        
        self.color_picker_btn.setStyleSheet(f"""
            QPushButton {{
                background-color: {color_hex};
                border: 2px solid {border_color};
                border-radius: 3px;
            }}
            QPushButton:hover {{
                background-color: {color_hex};
                border: 2px solid #000;
            }}
            QPushButton:pressed {{
                background-color: {color_hex};
                border: 3px solid #000;
            }}
        """)
        
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
        
    # ========================================================================
    #  公共接口方法（供 Toolbar 调用）
    # ========================================================================
    
    def set_color(self, color: QColor):
        """设置颜色（不触发信号）"""
        self.current_color = color
        self._update_color_picker_style()
        
    def set_size(self, size: int):
        """设置大小（不触发信号）"""
        self.current_size = size
        if hasattr(self, 'size_slider'):
            self.size_slider.blockSignals(True)
            self.size_slider.setValue(size)
            self.size_slider.blockSignals(False)
            if hasattr(self, 'size_value_label'):
                self.size_value_label.setText(str(size))
        
    def set_opacity(self, opacity: int):
        """设置透明度（不触发信号）"""
        self.current_opacity = opacity
        if hasattr(self, 'opacity_slider'):
            self.opacity_slider.blockSignals(True)
            self.opacity_slider.setValue(opacity)
            self.opacity_slider.blockSignals(False)
            if hasattr(self, 'opacity_value_label'):
                self.opacity_value_label.setText(str(opacity))
