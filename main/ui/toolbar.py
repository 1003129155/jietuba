"""
工具栏 - 截图工具栏UI (完整商业版本)
"""
import os
import sys
from PyQt6.QtCore import Qt, QSize, pyqtSignal, QRect, QRectF, QPoint
from PyQt6.QtGui import QIcon, QColor, QCursor, QFont
from PyQt6.QtWidgets import (
    QWidget, QPushButton, QSlider, QLabel, 
    QApplication, QColorDialog
)
from core.resource_manager import ResourceManager

def resource_path(relative_path):
    """获取资源文件路径（兼容函数）"""
    return ResourceManager.get_resource_path(relative_path)

class Toolbar(QWidget):
    """
    截图工具栏
    """
    # 信号定义
    tool_changed = pyqtSignal(str)  # 工具切换信号(tool_id)
    save_clicked = pyqtSignal()  # 保存按钮
    copy_clicked = pyqtSignal()  # 复制按钮
    pin_clicked = pyqtSignal()  # 钉图按钮
    confirm_clicked = pyqtSignal()  # 确认按钮
    undo_clicked = pyqtSignal()  # 撤销
    redo_clicked = pyqtSignal()  # 重做
    long_screenshot_clicked = pyqtSignal()  # 长截图按钮
    color_changed = pyqtSignal(QColor)  # 颜色改变
    stroke_width_changed = pyqtSignal(int)  # 线宽改变
    opacity_changed = pyqtSignal(int)  # 透明度改变(0-255)
    
    # 文字工具专用信号
    text_font_changed = pyqtSignal(QFont)
    text_outline_changed = pyqtSignal(bool, QColor, int)
    text_shadow_changed = pyqtSignal(bool, QColor)
    text_background_changed = pyqtSignal(bool, QColor)
    
    def __init__(self, parent=None):
        super().__init__(parent)  # 使用父窗口（如果有）
        
        # 当前选中的工具
        self.current_tool = None  # 初始无工具选中，用户点击后才激活
        
        # 当前颜色
        self.current_color = QColor(255, 0, 0)  # 默认红色
        
        self.init_ui()
        
    def init_ui(self):
        """初始化UI"""
        # 设置窗口属性
        if self.parent() is None:
            # 独立顶层窗口，强制置顶
            flags = (Qt.WindowType.FramelessWindowHint | 
                    Qt.WindowType.WindowStaysOnTopHint | 
                    Qt.WindowType.Tool |
                    Qt.WindowType.X11BypassWindowManagerHint)  # 绕过窗口管理器
            self.setWindowFlags(flags)
            self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)  # 显示但不获取焦点
        else:
            # 作为子窗口
            self.setWindowFlags(Qt.WindowType.FramelessWindowHint)
            
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)  # 确保样式表背景生效
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不接受焦点
        
        btn_width = 45
        btn_height = 45
        
        # 左侧按钮区域
        left_x = 0
        
        # 0. 长截图按钮（放在最左边）
        self.long_screenshot_btn = QPushButton(self)
        self.long_screenshot_btn.setGeometry(left_x, 0, 50, btn_height)
        self.long_screenshot_btn.setToolTip('長スクリーンショット（スクロール）')
        self.long_screenshot_btn.setIcon(QIcon(resource_path("svg/长截图.svg")))
        self.long_screenshot_btn.setIconSize(QSize(36, 36))
        self.long_screenshot_btn.clicked.connect(self.long_screenshot_clicked.emit)
        left_x += 50
        
        # 1. 保存按钮
        self.save_btn = QPushButton(self)
        self.save_btn.setGeometry(left_x, 0, 50, btn_height)
        self.save_btn.setToolTip('ファイルに保存')
        self.save_btn.setIcon(QIcon(resource_path("svg/下载.svg")))
        self.save_btn.setIconSize(QSize(36, 36))
        self.save_btn.clicked.connect(self.save_clicked.emit)
        left_x += 50
        
        # 2. 复制按钮（暂时隐藏在截图模式）
        self.copy_btn = QPushButton(self)
        self.copy_btn.setGeometry(left_x, 0, 50, btn_height)
        self.copy_btn.setToolTip('画像をコピー')
        self.copy_btn.setIcon(QIcon(resource_path("svg/copy.svg")))
        self.copy_btn.setIconSize(QSize(36, 36))
        self.copy_btn.clicked.connect(self.copy_clicked.emit)
        self.copy_btn.hide()  # 截图模式下隐藏，只在钉图模式显示
        # left_x += 50  # 不增加位置，因为隐藏了
        
        # 3. 画笔工具
        self.pen_btn = QPushButton(self)
        self.pen_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.pen_btn.setToolTip('ペンツール (Shiftキー押しながらで直線)')
        self.pen_btn.setIcon(QIcon(resource_path("svg/画笔.svg")))
        self.pen_btn.setIconSize(QSize(32, 32))
        self.pen_btn.setCheckable(True)
        self.pen_btn.setChecked(False)  # 默认不选中，因为默认是 cursor 模式
        self.pen_btn.clicked.connect(lambda: self._on_tool_clicked("pen"))
        left_x += btn_width
        
        # 4. 荧光笔工具
        self.highlighter_btn = QPushButton(self)
        self.highlighter_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.highlighter_btn.setToolTip('蛍光ペン (Shiftキー押しながらで直線)')
        self.highlighter_btn.setIcon(QIcon(resource_path("svg/荧光笔.svg")))
        self.highlighter_btn.setIconSize(QSize(32, 32))
        self.highlighter_btn.setCheckable(True)
        self.highlighter_btn.clicked.connect(lambda: self._on_tool_clicked("highlighter"))
        left_x += btn_width
        
        # 5. 箭头工具
        self.arrow_btn = QPushButton(self)
        self.arrow_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.arrow_btn.setToolTip('矢印を描画')
        self.arrow_btn.setIcon(QIcon(resource_path("svg/箭头.svg")))
        self.arrow_btn.setIconSize(QSize(32, 32))
        self.arrow_btn.setCheckable(True)
        self.arrow_btn.clicked.connect(lambda: self._on_tool_clicked("arrow"))
        left_x += btn_width
        
        # 6. 序号工具
        self.number_btn = QPushButton(self)
        self.number_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.number_btn.setToolTip('番号を追加 (クリックで自動採番)')
        self.number_btn.setIcon(QIcon(resource_path("svg/序号.svg")))
        self.number_btn.setIconSize(QSize(32, 32))
        self.number_btn.setCheckable(True)
        self.number_btn.clicked.connect(lambda: self._on_tool_clicked("number"))
        left_x += btn_width
        
        # 7. 矩形工具
        self.rect_btn = QPushButton(self)
        self.rect_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.rect_btn.setToolTip('矩形を描画')
        self.rect_btn.setIcon(QIcon(resource_path("svg/方框.svg")))
        self.rect_btn.setIconSize(QSize(32, 32))
        self.rect_btn.setCheckable(True)
        self.rect_btn.clicked.connect(lambda: self._on_tool_clicked("rect"))
        left_x += btn_width
        
        # 8. 圆形工具
        self.ellipse_btn = QPushButton(self)
        self.ellipse_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.ellipse_btn.setToolTip('円を描画')
        self.ellipse_btn.setIcon(QIcon(resource_path("svg/圆框.svg")))
        self.ellipse_btn.setIconSize(QSize(32, 32))
        self.ellipse_btn.setCheckable(True)
        self.ellipse_btn.clicked.connect(lambda: self._on_tool_clicked("ellipse"))
        left_x += btn_width
        
        # 9. 文字工具
        self.text_btn = QPushButton(self)
        self.text_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.text_btn.setToolTip('テキストを追加')
        self.text_btn.setIcon(QIcon(resource_path("svg/文字.svg")))
        self.text_btn.setIconSize(QSize(32, 32))
        self.text_btn.setCheckable(True)
        self.text_btn.clicked.connect(lambda: self._on_tool_clicked("text"))
        left_x += btn_width
        
        # 10. 橡皮擦工具
        self.eraser_btn = QPushButton(self)
        self.eraser_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.eraser_btn.setToolTip('消しゴムツール')
        self.eraser_btn.setIcon(QIcon(resource_path("svg/橡皮.svg")))
        self.eraser_btn.setIconSize(QSize(28, 28))
        self.eraser_btn.setCheckable(True)
        self.eraser_btn.clicked.connect(lambda: self._on_tool_clicked("eraser"))
        left_x += btn_width
        
        # 11. 撤销按钮
        self.undo_btn = QPushButton(self)
        self.undo_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.undo_btn.setToolTip('元に戻す')
        self.undo_btn.setIcon(QIcon(resource_path("svg/撤回.svg")))
        self.undo_btn.setIconSize(QSize(32, 32))
        self.undo_btn.clicked.connect(self.undo_clicked.emit)
        left_x += btn_width
        
        # 12. 重做按钮
        self.redo_btn = QPushButton(self)
        self.redo_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.redo_btn.setToolTip('やり直す')
        self.redo_btn.setIcon(QIcon(resource_path("svg/复原.svg")))
        self.redo_btn.setIconSize(QSize(32, 32))
        self.redo_btn.clicked.connect(self.redo_clicked.emit)
        left_x += btn_width
        
        # 右侧按钮区域（钉图按钮 + 确定按钮）
        right_buttons_width = 50 + 50  # 钉图按钮50 + 确定按钮50
        toolbar_total_width = left_x + 20 + right_buttons_width  # 增加间隔到20px，避免按钮重叠
        
        # 钉图按钮（确定按钮左边，右边数第二个）
        self.pin_btn = QPushButton(self)
        self.pin_btn.setGeometry(toolbar_total_width - 100, 0, 50, btn_height)
        self.pin_btn.setToolTip('画像を固定 (ピン)')
        self.pin_btn.setIcon(QIcon(resource_path("svg/钉图.svg")))
        self.pin_btn.setIconSize(QSize(36, 36))
        self.pin_btn.clicked.connect(self.pin_clicked.emit)
        
        # 确定按钮(吸附最右边)
        self.confirm_btn = QPushButton(self)
        self.confirm_btn.setGeometry(toolbar_total_width - 50, 0, 50, btn_height)
        self.confirm_btn.setToolTip('確定して保存')
        self.confirm_btn.setIcon(QIcon(resource_path("svg/确定.svg")))
        self.confirm_btn.setIconSize(QSize(36, 36))
        self.confirm_btn.clicked.connect(self.confirm_clicked.emit)
        
        # 设置工具栏大小
        self.resize(toolbar_total_width, btn_height)
        
        # 设置样式
        self.setStyleSheet("""
            QWidget {
                background-color: white;
                border: 2px solid #333333;
                border-radius: 6px;
                padding: 2px;
            }
            QPushButton {
                background-color: rgba(0, 0, 0, 0.02);
                border: none;
                border-radius: 0px;
            }
            QPushButton:hover {
                background-color: rgba(0, 0, 0, 0.08);
                border-radius: 0px;
            }
            QPushButton:pressed {
                background-color: rgba(0, 0, 0, 0.15);
                border-radius: 0px;
            }
            QPushButton:checked {
                background-color: rgba(64, 224, 208, 0.3);
                border: 1px solid #40E0D0;
            }
        """)
        
        # 收集所有工具按钮
        self.tool_buttons = {
            "pen": self.pen_btn,
            "highlighter": self.highlighter_btn,
            "arrow": self.arrow_btn,
            "number": self.number_btn,
            "rect": self.rect_btn,
            "ellipse": self.ellipse_btn,
            "text": self.text_btn,
            "eraser": self.eraser_btn,
        }
        
        # 创建二级设置面板
        self.init_settings_panels()
        
    def init_settings_panels(self):
        """初始化所有工具的设置面板"""
        from .paint_settings_panel import PaintSettingsPanel
        from .shape_settings_panel import ShapeSettingsPanel
        from .arrow_settings_panel import ArrowSettingsPanel
        from .number_settings_panel import NumberSettingsPanel
        from .text_settings_panel import TextSettingsPanel
        
        # 确定父窗口和窗口标志
        parent = self.parent()
        if parent is None:
            # 独立顶层窗口，强制置顶
            flags = (Qt.WindowType.FramelessWindowHint | 
                    Qt.WindowType.WindowStaysOnTopHint | 
                    Qt.WindowType.Tool |
                    Qt.WindowType.X11BypassWindowManagerHint)
        else:
            # 作为子窗口
            flags = Qt.WindowType.FramelessWindowHint
        
        # === 1. 画笔类设置面板 (pen, highlighter) ===
        self.paint_panel = PaintSettingsPanel(parent)
        self.paint_panel.setWindowFlags(flags)
        if parent is None:
            self.paint_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.paint_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.paint_panel.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        
        # 连接信号
        self.paint_panel.color_changed.connect(self._on_panel_color_changed)
        self.paint_panel.size_changed.connect(self._on_panel_size_changed)
        self.paint_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        self.paint_panel.hide()
        
        # === 2. 形状类设置面板 (rect, ellipse) ===
        self.shape_panel = ShapeSettingsPanel(parent)
        self.shape_panel.setWindowFlags(flags)
        if parent is None:
            self.shape_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.shape_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.shape_panel.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        
        # 连接信号
        self.shape_panel.color_changed.connect(self._on_panel_color_changed)
        self.shape_panel.size_changed.connect(self._on_panel_size_changed)
        self.shape_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        self.shape_panel.hide()
        
        # === 3. 箭头设置面板 (arrow) ===
        self.arrow_panel = ArrowSettingsPanel(parent)
        self.arrow_panel.setWindowFlags(flags)
        if parent is None:
            self.arrow_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.arrow_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.arrow_panel.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        
        # 连接信号
        self.arrow_panel.color_changed.connect(self._on_panel_color_changed)
        self.arrow_panel.size_changed.connect(self._on_panel_size_changed)
        self.arrow_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        self.arrow_panel.hide()
        
        # === 4. 序号设置面板 (number) ===
        self.number_panel = NumberSettingsPanel(parent)
        self.number_panel.setWindowFlags(flags)
        if parent is None:
            self.number_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.number_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.number_panel.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        
        # 连接信号
        self.number_panel.color_changed.connect(self._on_panel_color_changed)
        self.number_panel.size_changed.connect(self._on_panel_size_changed)
        self.number_panel.hide()
        
        # === 5. 文字设置面板 (text) ===
        self.text_panel = TextSettingsPanel(parent)

        # === 5. 文字设置面板 (text) ===
        print("📝 [Toolbar] 创建文字设置面板...")
        self.text_panel = TextSettingsPanel(parent)
        self.text_panel.setWindowFlags(flags)
        if parent is None:
            self.text_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.text_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.text_panel.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        
        # 连接信号
        self.text_panel.font_changed.connect(self._on_text_font_changed)
        self.text_panel.color_changed.connect(self._on_text_color_changed)
        self.text_panel.hide()
        
        # 加载保存的设置
        self._load_saved_settings()
        
        # 保持兼容性:paint_menu 和 text_menu 别名
        self.paint_menu = self.paint_panel
        self.text_menu = self.text_panel
        
    def _load_saved_settings(self):
        """加载保存的工具设置"""
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        
        # 加载文字工具设置
        text_settings = manager.get_tool_settings("text")
        if text_settings:
            font = QFont(text_settings.get("font_family", "Microsoft YaHei"))
            font.setPointSize(text_settings.get("font_size", 16))
            font.setBold(text_settings.get("font_bold", False))
            font.setItalic(text_settings.get("font_italic", False))
            font.setUnderline(text_settings.get("font_underline", False))
            color = QColor(text_settings.get("color", "#FF0000"))
            
            # 更新文字面板状态
            self.text_panel.font_combo.setCurrentText(font.family())
            self.text_panel.size_spin.setValue(font.pointSize())
            self.text_panel.bold_btn.setChecked(font.bold())
            self.text_panel.italic_btn.setChecked(font.italic())
            self.text_panel.underline_btn.setChecked(font.underline())
            self.text_panel.current_color = color
            self.text_panel._update_color_btn(color)
        
    def _on_tool_clicked(self, tool_id: str):
        """工具按钮点击 - 支持再次点击取消"""
        # 如果点击的是当前工具，取消选中（退出绘制模式）
        if self.current_tool == tool_id:
            # 取消所有按钮选中
            for btn in self.tool_buttons.values():
                btn.setChecked(False)
            self.current_tool = None
            tool_to_emit = "cursor"
            self.tool_changed.emit(tool_to_emit)
            self._hide_all_panels()
        else:
            # 更新按钮状态
            for tid, btn in self.tool_buttons.items():
                btn.setChecked(tid == tool_id)
            
            self.current_tool = tool_id
            self.tool_changed.emit(tool_id)
            
            # 显示对应的设置面板
            self._show_panel_for_tool(tool_id)
            
    def _hide_all_panels(self):
        """隐藏所有设置面板"""
        if hasattr(self, 'paint_panel'): self.paint_panel.hide()
        if hasattr(self, 'shape_panel'): self.shape_panel.hide()
        if hasattr(self, 'arrow_panel'): self.arrow_panel.hide()
        if hasattr(self, 'number_panel'): self.number_panel.hide()
        if hasattr(self, 'text_panel'): self.text_panel.hide()
        
    def _show_panel_for_tool(self, tool_id: str):
        """显示指定工具的设置面板"""
        self._hide_all_panels()
        
        panel_map = {
            "pen": self.paint_panel,
            "highlighter": self.paint_panel,
            "rect": self.shape_panel,
            "ellipse": self.shape_panel,
            "arrow": self.arrow_panel,
            "number": self.number_panel,
            "text": self.text_panel,
        }
        
        panel = panel_map.get(tool_id)
        if panel:
            panel.show()
            panel.raise_()
            self._sync_panel_position(panel)
            # 确保工具栏在面板之上
            self.raise_()

    # ========================================================================
    #  设置面板信号处理
    # ========================================================================
    
    def _on_panel_color_changed(self, color):
        """面板颜色改变"""
        self.current_color = color
        self.color_changed.emit(color)
        
    def _on_panel_size_changed(self, size):
        """面板大小改变"""
        print(f"[Toolbar] panel size_changed -> {size}")
        self.stroke_width_changed.emit(size)
        
    def _on_panel_opacity_changed(self, opacity):
        """面板透明度改变"""
        print(f"[Toolbar] panel opacity_changed -> {opacity}")
        self.opacity_changed.emit(opacity)
    
    def _on_text_font_changed(self, font):
        """文字字体改变"""
        self.text_font_changed.emit(font)
        # 保存字体设置
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        manager.update_settings("text", 
            font_family=font.family(),
            font_size=font.pointSize(),
            font_bold=font.bold(),
            font_italic=font.italic(),
            font_underline=font.underline()
        )

    def _on_text_color_changed(self, color):
        """文字颜色改变"""
        self.current_color = color
        self.color_changed.emit(color)
        # 保存颜色设置
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        manager.update_settings("text", color=color.name())

    # ========================================================================
    #  工具设置同步方法（用于从设置管理器更新UI）
    # ========================================================================
    
    def set_current_color(self, color: QColor):
        """设置当前颜色（更新内部状态，但不触发信号）"""
        self.current_color = color
        # 更新所有面板的颜色显示
        if hasattr(self, 'paint_panel'):
            self.paint_panel.set_color(color)
        if hasattr(self, 'shape_panel'):
            self.shape_panel.set_color(color)
        if hasattr(self, 'arrow_panel'):
            self.arrow_panel.set_color(color)
        if hasattr(self, 'number_panel'):
            self.number_panel.set_color(color)
    
    def set_stroke_width(self, width: int):
        """设置笔触宽度（更新UI显示）"""
        width = int(width)
        # 更新所有面板的大小显示
        if hasattr(self, 'paint_panel'):
            self.paint_panel.set_size(width)
        if hasattr(self, 'shape_panel'):
            self.shape_panel.set_size(width)
        if hasattr(self, 'arrow_panel'):
            self.arrow_panel.set_size(width)
        if hasattr(self, 'number_panel'):
            self.number_panel.set_size(width)
    
    def set_opacity(self, opacity_255: int):
        """设置透明度（更新UI显示）"""
        # 更新所有面板的透明度显示
        if hasattr(self, 'paint_panel'):
            self.paint_panel.set_opacity(opacity_255)
        if hasattr(self, 'shape_panel'):
            self.shape_panel.set_opacity(opacity_255)
        if hasattr(self, 'arrow_panel'):
            self.arrow_panel.set_opacity(opacity_255)
    
    
    def position_near_rect(self, rect: QRectF, parent_widget=None):
        """
        智能定位工具栏（商业级算法）
        优先级：选区下方右对齐 > 上方右对齐 > 右侧 > 左侧 > 内部右下角
        
        Args:
            rect: 选区矩形（场景坐标）
            parent_widget: 父窗口（用于坐标转换）
        """
        # 如果有父窗口，转换为全局坐标
        if parent_widget:
            view_rect_tl = parent_widget.mapToGlobal(QPoint(int(rect.x()), int(rect.y())))
            view_rect_br = parent_widget.mapToGlobal(QPoint(int(rect.right()), int(rect.bottom())))
            global_rect = QRect(view_rect_tl, view_rect_br)
        else:
            global_rect = rect.toRect()
        
        # 工具栏尺寸
        toolbar_w = self.width()
        toolbar_h = self.height()
        
        # 获取屏幕信息
        screen = QApplication.screenAt(QPoint(global_rect.center().x(), global_rect.center().y()))
        if not screen:
            screen = QApplication.primaryScreen()
        screen_rect = screen.geometry()
        
        margin = 10  # 边距
        
        target_x = 0
        target_y = 0
        
        # 策略1: 下方右对齐
        x = global_rect.right() - toolbar_w
        y = global_rect.bottom() + margin
        
        if (y + toolbar_h <= screen_rect.bottom() and 
            x >= screen_rect.left() and 
            x + toolbar_w <= screen_rect.right()):
            target_x, target_y = x, y
        else:
            # 策略2: 上方右对齐
            y = global_rect.top() - toolbar_h - margin
            if (y >= screen_rect.top() and 
                x >= screen_rect.left() and 
                x + toolbar_w <= screen_rect.right()):
                target_x, target_y = x, y
            else:
                # 策略3: 右侧居中
                x = global_rect.right() + margin
                y = global_rect.center().y() - toolbar_h // 2
                if x + toolbar_w <= screen_rect.right():
                    y = max(screen_rect.top(), min(y, screen_rect.bottom() - toolbar_h))
                    target_x, target_y = x, y
                else:
                    # 策略4: 左侧居中
                    x = global_rect.left() - toolbar_w - margin
                    if x >= screen_rect.left():
                        y = max(screen_rect.top(), min(y, screen_rect.bottom() - toolbar_h))
                        target_x, target_y = x, y
                    else:
                        # 策略5: 选区内部右下角
                        x = global_rect.right() - toolbar_w - margin
                        y = global_rect.bottom() - toolbar_h - margin
                        x = max(screen_rect.left(), min(x, screen_rect.right() - toolbar_w))
                        y = max(screen_rect.top(), min(y, screen_rect.bottom() - toolbar_h))
                        target_x, target_y = x, y

        # 移动工具栏
        final_pos = QPoint(target_x, target_y)
        if self.parent():
            final_pos = self.parent().mapFromGlobal(final_pos)
        self.move(final_pos)
    
    def moveEvent(self, event):
        """工具栏移动时同步所有可见面板位置"""
        super().moveEvent(event)
        self._sync_all_panels_position()
    
    def _sync_all_panels_position(self):
        """同步所有可见面板的位置"""
        if hasattr(self, 'paint_panel') and self.paint_panel.isVisible():
            self._sync_panel_position(self.paint_panel)
        if hasattr(self, 'shape_panel') and self.shape_panel.isVisible():
            self._sync_panel_position(self.shape_panel)
        if hasattr(self, 'arrow_panel') and self.arrow_panel.isVisible():
            self._sync_panel_position(self.arrow_panel)
        if hasattr(self, 'number_panel') and self.number_panel.isVisible():
            self._sync_panel_position(self.number_panel)
        if hasattr(self, 'text_panel') and self.text_panel.isVisible():
            self._sync_panel_position(self.text_panel)
    
    def _sync_panel_position(self, panel):
        """同步单个面板的位置"""
        if not panel:
            return
        
        # 获取工具栏的全局坐标
        toolbar_global_pos = self.mapToGlobal(QPoint(0, 0))
        
        # 默认定位在工具栏下方
        panel_global_x = toolbar_global_pos.x()
        panel_global_y = toolbar_global_pos.y() + self.height() + 5
        
        # 获取屏幕几何信息
        screen = QApplication.screenAt(toolbar_global_pos)
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_rect = screen.geometry()
        
        # 检查是否超出屏幕底部
        if panel_global_y + panel.height() > screen_rect.y() + screen_rect.height():
            # 显示在工具栏上方
            panel_global_y = toolbar_global_pos.y() - panel.height() - 5
        
        # 确保水平方向也在屏幕内
        if panel_global_x + panel.width() > screen_rect.x() + screen_rect.width():
            panel_global_x = screen_rect.x() + screen_rect.width() - panel.width() - 5
        if panel_global_x < screen_rect.x():
            panel_global_x = screen_rect.x() + 5
        
        # 转换为本地坐标（如果面板是子窗口）
        final_pos = QPoint(panel_global_x, panel_global_y)
        if panel.parent():
            final_pos = panel.parent().mapFromGlobal(final_pos)
        
        # 移动面板
        panel.move(final_pos)

