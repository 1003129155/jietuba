"""
钉图控制按钮管理器

负责管理钉图窗口的控制按钮（关闭、工具栏切换、翻译按钮）
"""

from PyQt6.QtWidgets import QPushButton, QWidget
from PyQt6.QtGui import QIcon
from PyQt6.QtCore import QSize, pyqtSignal
from core.resource_manager import ResourceManager


class PinControlButtons:
    """
    钉图控制按钮管理器
    
    管理关闭按钮、工具栏切换按钮、翻译按钮的创建、样式和位置
    """
    
    # 按钮尺寸常量
    BUTTON_SIZE = 24
    MARGIN = 5
    SPACING = 10
    
    def __init__(self, parent: QWidget):
        """
        初始化控制按钮管理器
        
        Args:
            parent: 父窗口（PinWindow）
        """
        self.parent = parent
        self.close_button = None
        self.toolbar_toggle_button = None
        self.translate_button = None
        
        self._create_buttons()
    
    def _create_buttons(self):
        """创建所有控制按钮"""
        # 1. 关闭按钮
        self.close_button = self._create_button(
            icon_name="关闭.svg",
            tooltip="Close (ESC)",
            style=self._get_close_button_style()
        )
        
        # 2. 工具栏切换按钮
        self.toolbar_toggle_button = self._create_button(
            icon_name="工具栏.svg",
            tooltip="Show toolbar",
            style=self._get_toolbar_button_style()
        )
        
        # 3. 翻译按钮
        self.translate_button = self._create_button(
            icon_name="翻译.svg",
            tooltip="Translate OCR text",
            style=self._get_translate_button_style()
        )
    
    def _create_button(self, icon_name: str, tooltip: str, style: str) -> QPushButton:
        """
        创建单个按钮
        
        Args:
            icon_name: SVG 图标文件名
            tooltip: 按钮提示文字
            style: 按钮样式表
            
        Returns:
            创建的按钮
        """
        button = QPushButton(self.parent)
        button.setFixedSize(self.BUTTON_SIZE, self.BUTTON_SIZE)
        
        icon_path = ResourceManager.get_resource_path(f"svg/{icon_name}")
        button.setIcon(QIcon(icon_path))
        button.setIconSize(QSize(self.BUTTON_SIZE, self.BUTTON_SIZE))
        button.setStyleSheet(style)
        button.setToolTip(self.parent.tr(tooltip))
        button.hide()  # 初始隐藏
        
        return button
    
    def _get_close_button_style(self) -> str:
        """关闭按钮样式（红色光晕）"""
        return """
            QPushButton {
                background-color: transparent;
                border: 2px solid transparent;
                border-radius: 14px;
                padding: 0px;
            }
            QPushButton:hover {
                background-color: rgba(231, 76, 60, 40);
                border: 2px solid rgba(231, 76, 60, 120);
            }
            QPushButton:pressed {
                background-color: rgba(231, 76, 60, 80);
                border: 2px solid rgba(231, 76, 60, 180);
            }
        """
    
    def _get_toolbar_button_style(self) -> str:
        """工具栏按钮样式（蓝色光晕）"""
        return """
            QPushButton {
                background-color: transparent;
                border: 2px solid transparent;
                border-radius: 14px;
                padding: 0px;
            }
            QPushButton:hover {
                background-color: rgba(52, 152, 219, 40);
                border: 2px solid rgba(52, 152, 219, 120);
            }
            QPushButton:pressed {
                background-color: rgba(52, 152, 219, 80);
                border: 2px solid rgba(52, 152, 219, 180);
            }
        """
    
    def _get_translate_button_style(self) -> str:
        """翻译按钮样式（紫色光晕）"""
        return """
            QPushButton {
                background-color: transparent;
                border: 2px solid transparent;
                border-radius: 14px;
                padding: 0px;
            }
            QPushButton:hover {
                background-color: rgba(142, 95, 217, 40);
                border: 2px solid rgba(142, 95, 217, 120);
            }
            QPushButton:pressed {
                background-color: rgba(142, 95, 217, 80);
                border: 2px solid rgba(142, 95, 217, 180);
            }
        """
    
    def update_positions(self, window_width: int):
        """
        更新按钮位置
        
        Args:
            window_width: 窗口宽度
        """
        # 关闭按钮在右上角
        close_x = window_width - self.BUTTON_SIZE - self.MARGIN
        close_y = self.MARGIN
        self.close_button.move(close_x, close_y)
        self.close_button.raise_()
        
        # 工具栏切换按钮在关闭按钮左边
        toolbar_x = close_x - self.BUTTON_SIZE - self.SPACING
        toolbar_y = self.MARGIN
        self.toolbar_toggle_button.move(toolbar_x, toolbar_y)
        self.toolbar_toggle_button.raise_()
        
        # 翻译按钮在工具栏切换按钮左边
        translate_x = toolbar_x - self.BUTTON_SIZE - self.SPACING
        translate_y = self.MARGIN
        self.translate_button.move(translate_x, translate_y)
        self.translate_button.raise_()
    
    def show_hover_controls(self, show_toolbar_button: bool = True):
        """
        显示悬停时的控制按钮
        
        Args:
            show_toolbar_button: 是否显示工具栏切换按钮
        """
        self.close_button.show()
        self.close_button.raise_()
        
        if show_toolbar_button:
            self.toolbar_toggle_button.show()
            self.toolbar_toggle_button.raise_()
        
        # 翻译按钮由 OCR 结果控制，这里只确保 raise
        if self.translate_button.isVisible():
            self.translate_button.raise_()
    
    def hide_all(self):
        """隐藏所有控制按钮"""
        self.close_button.hide()
        self.toolbar_toggle_button.hide()
        # 翻译按钮不隐藏（由 OCR 结果控制）
    
    def show_translate_button(self):
        """显示翻译按钮（OCR 完成后调用）"""
        self.translate_button.show()
        self.translate_button.raise_()
    
    def hide_translate_button(self):
        """隐藏翻译按钮"""
        self.translate_button.hide()
    
    def connect_signals(self, close_handler, toggle_toolbar_handler, translate_handler):
        """
        连接按钮信号
        
        Args:
            close_handler: 关闭按钮点击处理函数
            toggle_toolbar_handler: 工具栏切换按钮点击处理函数
            translate_handler: 翻译按钮点击处理函数
        """
        self.close_button.clicked.connect(close_handler)
        self.toolbar_toggle_button.clicked.connect(toggle_toolbar_handler)
        self.translate_button.clicked.connect(translate_handler)
