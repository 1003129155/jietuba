"""
文字工具设置面板
提供字体、字号、颜色、描边、阴影等高级设置
"""
from PyQt6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QPushButton, QLabel, 
    QComboBox, QSpinBox, QCheckBox, QColorDialog, QFrame, QButtonGroup
)
from PyQt6.QtCore import Qt, pyqtSignal, QSize
from PyQt6.QtGui import QIcon, QColor, QFont, QFontDatabase

class TextSettingsPanel(QWidget):
    """文字工具二级菜单"""
    
    # 信号定义
    font_changed = pyqtSignal(QFont)
    color_changed = pyqtSignal(QColor)
    # 移除特效信号，因为不再需要单独控制
    # outline_changed = pyqtSignal(bool, QColor, int) 
    # shadow_changed = pyqtSignal(bool, QColor)       
    # background_changed = pyqtSignal(bool, QColor)   
    
    def __init__(self, parent=None):
        super().__init__(parent)
        # 使用更安全的默认字体选择
        default_font_family = "Microsoft YaHei"
        # 检查默认字体是否可用
        available_fonts = QFontDatabase.families()
        if default_font_family not in available_fonts:
            # fallback到系统可用的字体
            if "Arial" in available_fonts:
                default_font_family = "Arial"
            elif "SimSun" in available_fonts:
                default_font_family = "SimSun"
            elif len(available_fonts) > 0:
                default_font_family = available_fonts[0]
        
        self.current_font = QFont(default_font_family, 16)
        self.current_color = QColor(Qt.GlobalColor.red)
        
        self._init_ui()
        self._connect_signals()
        
    def _init_ui(self):
        """初始化UI布局"""
        # 设置面板样式 - 统一风格
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
                padding: 4px;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
            }
            QPushButton:hover {
                background-color: #f0f0f0;
                border: 1px solid #bbb;
            }
            QPushButton:checked {
                background-color: #e0e0e0;
                border: 1px solid #999;
            }
            QComboBox {
                border: 1px solid #ccc;
                border-radius: 3px;
                padding: 2px 8px 2px 4px;
                background: white;
                min-width: 40px;
                max-width: 120px;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
                color: #333;
            }
            QComboBox:hover {
                border: 1px solid #999;
            }
            QComboBox::drop-down {
                subcontrol-origin: padding;
                subcontrol-position: top right;
                width: 20px;
                border-left: 1px solid #ccc;
                border-top-right-radius: 3px;
                border-bottom-right-radius: 3px;
                background: white;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-top: 6px solid #666;
                width: 0;
                height: 0;
                margin-right: 6px;
            }
            QComboBox QAbstractItemView {
                border: 1px solid #999;
                background: white;
                selection-background-color: #0078d7;
                selection-color: white;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
                color: #333;
                outline: none;
            }
            QComboBox QAbstractItemView::item {
                padding: 4px 8px;
                min-height: 20px;
                color: #333;
                background: white;
            }
            QComboBox QAbstractItemView::item:hover {
                background-color: #e5f3ff;
                color: #000;
            }
            QComboBox QAbstractItemView::item:selected {
                background-color: #0078d7;
                color: white;
            }
            QSpinBox {
                border: 1px solid #ccc;
                border-radius: 3px;
                padding: 2px 2px 2px 6px;
                background: white;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
                color: #333;
            }
            QSpinBox::up-button {
                subcontrol-origin: border;
                subcontrol-position: top right;
                width: 18px;
                border-left: 1px solid #ccc;
                border-bottom: 1px solid #ccc;
                border-top-right-radius: 3px;
                background: white;
            }
            QSpinBox::up-button:hover {
                background: #f0f0f0;
            }
            QSpinBox::up-button:pressed {
                background: #e0e0e0;
            }
            QSpinBox::up-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-bottom: 6px solid #666;
                width: 0;
                height: 0;
            }
            QSpinBox::down-button {
                subcontrol-origin: border;
                subcontrol-position: bottom right;
                width: 18px;
                border-left: 1px solid #ccc;
                border-bottom-right-radius: 3px;
                background: white;
            }
            QSpinBox::down-button:hover {
                background: #f0f0f0;
            }
            QSpinBox::down-button:pressed {
                background: #e0e0e0;
            }
            QSpinBox::down-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-top: 6px solid #666;
                width: 0;
                height: 0;
            }
            QCheckBox {
                spacing: 5px;
                background-color: transparent;
                border: none;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
                color: #333;
            }
            QLabel {
                color: #333;
                background-color: transparent;
                border: none;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
            }
            QFrame#separator {
                background-color: #ddd;
                width: 1px;
                margin: 4px 4px;
                border: none;
            }
        """)
        
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(10)
        
        # === 1. 基础样式区 ===
        
        # 字体选择 - 加载系统字体
        self.font_combo = QComboBox()
        # 获取系统所有字体
        system_fonts = QFontDatabase.families()
        # 过滤出常用中文字体放在前面
        priority_fonts = ["Microsoft YaHei", "SimSun", "SimHei", "KaiTi", "Arial", "Times New Roman"]
        sorted_fonts = []
        # 先添加优先字体
        for f in priority_fonts:
            if f in system_fonts:
                sorted_fonts.append(f)
        # 再添加其他字体
        for f in system_fonts:
            if f not in sorted_fonts:
                sorted_fonts.append(f)
                
        self.font_combo.addItems(sorted_fonts)
        # 设置当前字体，如果不可用则使用第一个可用字体
        if "Microsoft YaHei" in sorted_fonts:
            self.font_combo.setCurrentText("Microsoft YaHei")
        elif len(sorted_fonts) > 0:
            self.font_combo.setCurrentIndex(0)
        self.font_combo.setToolTip("字体")
        
        # 确保字体组合框有合理的最大可见项数
        self.font_combo.setMaxVisibleItems(15)
        # 设置下拉列表的最小宽度，确保字体名称完整显示
        self.font_combo.view().setMinimumWidth(200)
        
        layout.addWidget(self.font_combo)
        
        # 字号选择
        self.size_spin = QSpinBox()
        self.size_spin.setRange(8, 144)
        self.size_spin.setValue(16)
        self.size_spin.setFixedWidth(60)
        self.size_spin.setToolTip("字号")
        layout.addWidget(self.size_spin)
        
        # 样式按钮组 (粗体/斜体/下划线)
        self.bold_btn = QPushButton("B")
        self.bold_btn.setCheckable(True)
        self.bold_btn.setFixedSize(28, 28)
        self.bold_btn.setToolTip("粗体")
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
        self.italic_btn.setFixedSize(28, 28)
        self.italic_btn.setToolTip("斜体")
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
        self.underline_btn.setFixedSize(28, 28)
        self.underline_btn.setToolTip("下划线")
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
        
        # 分隔线
        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)
        
        # === 2. 颜色预设区 ===
        
        # 颜色选择按钮
        self.color_btn = QPushButton()
        self.color_btn.setFixedSize(28, 28)
        self.color_btn.setToolTip("自定义颜色")
        self._update_color_btn(self.current_color)
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
        
        for color_str in preset_colors:
            btn = QPushButton()
            btn.setFixedSize(24, 24)
            btn.setToolTip(color_str)
            
            # 设置样式
            border_color = "#888888" if color_str == "#FFFFFF" else "#333333"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {color_str};
                    border: 1px solid {border_color};
                    border-radius: 12px;
                }}
                QPushButton:hover {{
                    border: 2px solid #000;
                }}
            """)
            
            # 连接点击事件
            # 注意：在循环中使用 lambda 需要捕获变量
            btn.clicked.connect(lambda checked, c=color_str: self._on_preset_color_clicked(c))
            layout.addWidget(btn)
        
        layout.addStretch()
        
    def _connect_signals(self):
        """连接内部信号"""
        self.font_combo.currentTextChanged.connect(self._on_font_changed)
        self.size_spin.valueChanged.connect(self._on_font_changed)
        self.bold_btn.toggled.connect(self._on_font_changed)
        self.italic_btn.toggled.connect(self._on_font_changed)
        self.underline_btn.toggled.connect(self._on_font_changed)
        
        self.color_btn.clicked.connect(self._pick_color)
        
    def _update_color_btn(self, color):
        """更新颜色按钮显示"""
        self.color_btn.setStyleSheet(f"""
            background-color: {color.name()};
            border: 1px solid #999;
            border-radius: 3px;
        """)
        
    def _pick_color(self):
        """选择颜色"""
        color = QColorDialog.getColor(self.current_color, self, "选择文字颜色")
        if color.isValid():
            self.current_color = color
            self._update_color_btn(color)
            self.color_changed.emit(color)
            
    def _on_preset_color_clicked(self, color_str):
        """点击预设颜色"""
        color = QColor(color_str)
        self.current_color = color
        self._update_color_btn(color)
        self.color_changed.emit(color)
            
    def _on_font_changed(self):
        """字体属性改变"""
        font = QFont(self.font_combo.currentText())
        font.setPointSize(self.size_spin.value())
        font.setBold(self.bold_btn.isChecked())
        font.setItalic(self.italic_btn.isChecked())
        font.setUnderline(self.underline_btn.isChecked())
        
        self.current_font = font
        self.font_changed.emit(font)
        
    # 移除特效改变处理函数
    # def _on_effect_changed(self): ...

    def set_state_from_item(self, item):
        """根据选中的 TextItem 更新面板状态"""
        if not item: return
        
        # 阻断信号防止循环触发
        self.blockSignals(True)
        
        # 字体
        font = item.font()
        self.font_combo.setCurrentText(font.family())
        self.size_spin.setValue(font.pointSize())
        self.bold_btn.setChecked(font.bold())
        self.italic_btn.setChecked(font.italic())
        self.underline_btn.setChecked(font.underline())
        
        # 颜色
        self.current_color = item.defaultTextColor()
        self._update_color_btn(self.current_color)
        
        # 移除特效状态同步
            
        self.blockSignals(False)
