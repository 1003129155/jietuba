"""
钉图工具栏 - 继承并轻微改造截图工具栏
"""

from PyQt6.QtCore import Qt, QPoint, QTimer
from PyQt6.QtWidgets import QApplication, QWidget
from ui.toolbar import Toolbar


class PinToolbar(Toolbar):
    """
    钉图工具栏 - 继承自截图工具栏，针对钉图场景做轻微调整
    
    主要变化:
    1. 隐藏"确定"按钮（钉图不需要确定，保持打开状态）
    2. 显示"复制"按钮（支持复制当前图像）
    3. 隐藏"长截图"按钮（钉图不支持长截图）
    4. 自动定位在钉图窗口附近（上方或下方）
    5. 支持自动隐藏功能（鼠标离开后自动隐藏）
    
    注意: 钉图窗口的关闭功能由窗口右上角的 ❌ 按钮提供，不在工具栏中
    """
    
    def __init__(self, parent_pin_window=None, config_manager=None):
        """
        Args:
            parent_pin_window: 父钉图窗口（用于定位和关闭）
            config_manager: 配置管理器（用于加载和保存工具设置）
        """
        # 调用父类初始化（传入 None 使其成为独立窗口）
        super().__init__(parent=None)
        
        self.parent_pin_window = parent_pin_window
        self.config_manager = config_manager
        
        # 🔥 缩放工具栏，使其比截图时更小更紧凑
        self._scale_toolbar(0.85)
        
        # 自定义钉图工具栏的按钮显示/隐藏
        self._customize_for_pin()
        
        # 自动隐藏定时器
        self.auto_hide_timer = QTimer(self)
        self.auto_hide_timer.setSingleShot(True)
        self.auto_hide_timer.setInterval(2000)  # 2秒后自动隐藏
        self.auto_hide_timer.timeout.connect(self._auto_hide)
        
        # 是否启用自动隐藏
        self.auto_hide_enabled = False
        self._parent_hovering = False
    
    def _scale_toolbar(self, scale: float):
        """缩放整个工具栏和所有子控件"""
        # 先调用父类的resize确保工具栏已经初始化完成
        if hasattr(self, 'botton_box') and self.botton_box:
            original_width = self.botton_box.width()
            original_height = self.botton_box.height()
            
            new_width = int(original_width * scale)
            new_height = int(original_height * scale)
            
            self.botton_box.setFixedSize(new_width, new_height)
            self.resize(new_width, new_height)
            
            # 递归缩放所有子控件
            def scale_children(parent_widget, scale_factor):
                for child in parent_widget.findChildren(QWidget):
                    if child.parent() == parent_widget:  # 只处理直接子控件
                        geo = child.geometry()
                        child.setGeometry(
                            int(geo.x() * scale_factor),
                            int(geo.y() * scale_factor),
                            int(geo.width() * scale_factor),
                            int(geo.height() * scale_factor)
                        )
                        
                        # 缩放字体
                        font = child.font()
                        original_size = font.pointSize()
                        if original_size > 0:
                            font.setPointSize(max(6, int(original_size * scale_factor)))
                            child.setFont(font)
                        
                        # 缩放图标
                        if hasattr(child, 'iconSize') and callable(child.iconSize):
                            icon_size = child.iconSize()
                            new_icon_width = int(icon_size.width() * scale_factor)
                            new_icon_height = int(icon_size.height() * scale_factor)
                            from PyQt6.QtCore import QSize
                            child.setIconSize(QSize(new_icon_width, new_icon_height))
                        
                        # 递归处理子控件的子控件
                        scale_children(child, scale_factor)
            
            scale_children(self.botton_box, scale)

        
    def _customize_for_pin(self):
        """自定义钉图模式的按钮布局"""
        btn_width = 45
        btn_height = 45
        left_x = 0
        
        # 1. 隐藏不需要的按钮
        if hasattr(self, 'confirm_btn'):
            self.confirm_btn.hide()
        
        if hasattr(self, 'long_screenshot_btn'):
            self.long_screenshot_btn.hide()
        
        if hasattr(self, 'pin_btn'):
            self.pin_btn.hide()
        
        # 2. 保存按钮（最左边）
        if hasattr(self, 'save_btn'):
            self.save_btn.setGeometry(left_x, 0, 50, btn_height)
            self.save_btn.show()
            left_x += 50
        
        # 3. 显示并定位"复制"按钮
        if hasattr(self, 'copy_btn'):
            self.copy_btn.setGeometry(left_x, 0, 50, btn_height)
            self.copy_btn.show()
            left_x += 50
        
        # 4. 重新定位所有绘图工具按钮
        if hasattr(self, 'pen_btn'):
            self.pen_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'highlighter_btn'):
            self.highlighter_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'arrow_btn'):
            self.arrow_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'number_btn'):
            self.number_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'rect_btn'):
            self.rect_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'ellipse_btn'):
            self.ellipse_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'text_btn'):
            self.text_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'eraser_btn'):
            self.eraser_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'undo_btn'):
            self.undo_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        if hasattr(self, 'redo_btn'):
            self.redo_btn.setGeometry(left_x, 0, btn_width, btn_height)
            left_x += btn_width
        
        # 5. 重新计算工具栏宽度
        new_width = left_x
        self.resize(new_width, btn_height)

    
    def position_near_window(self, pin_window):
        """
        定位工具栏到钉图窗口附近 - 右对齐，紧凑布局
        
        参考老代码逻辑：
        1. 优先显示在钉图窗口下方，右对齐
        2. 下方不够则显示在上方，右对齐
        3. 上下都不够则显示在右侧或左侧
        
        Args:
            pin_window: PinWindow 实例
        """
        if not pin_window:
            return
        
        # 获取钉图窗口的全局位置和大小
        pin_pos = pin_window.pos()
        pin_size = pin_window.size()
        
        # 获取屏幕信息
        screen = QApplication.screenAt(pin_pos)
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_rect = screen.geometry()
        
        toolbar_width = self.width()
        toolbar_height = self.height()
        
        # 间距设为负值，让工具栏向上偏移，重叠到钉图窗口边缘上方
        spacing = -7
        
        # 🔥 优先方案：钉图窗口下方，右对齐
        below_y = pin_pos.y() + pin_size.height() + spacing
        toolbar_x_right_aligned = pin_pos.x() + pin_size.width() - toolbar_width
        
        if below_y + toolbar_height <= screen_rect.y() + screen_rect.height() and toolbar_x_right_aligned >= screen_rect.x():
            # 下方有足够空间且右对齐位置合理
            toolbar_x = max(screen_rect.x(), toolbar_x_right_aligned)
            toolbar_y = below_y
        else:
            # 下方空间不足，尝试上方右对齐
            above_y = pin_pos.y() - toolbar_height - spacing
            if above_y >= screen_rect.y() and toolbar_x_right_aligned >= screen_rect.x():
                toolbar_x = max(screen_rect.x(), toolbar_x_right_aligned)
                toolbar_y = above_y
            else:
                # 上下都不够，显示在右侧
                toolbar_x = pin_pos.x() + pin_size.width() + spacing
                toolbar_y = max(screen_rect.y(), pin_pos.y())
                
                if toolbar_x + toolbar_width > screen_rect.x() + screen_rect.width():
                    # 右侧也不够，显示在左侧
                    toolbar_x = pin_pos.x() - toolbar_width - spacing
                    if toolbar_x < screen_rect.x():
                        # 左侧也不够，显示在钉图窗口内部右下角
                        toolbar_x = pin_pos.x() + pin_size.width() - toolbar_width - spacing
                        toolbar_y = pin_pos.y() + pin_size.height() - toolbar_height - spacing
        
        # 确保工具栏完全在屏幕内
        toolbar_x = max(screen_rect.x(), min(toolbar_x, screen_rect.x() + screen_rect.width() - toolbar_width))
        toolbar_y = max(screen_rect.y(), min(toolbar_y, screen_rect.y() + screen_rect.height() - toolbar_height))
        
        # 移动工具栏
        self.move(toolbar_x, toolbar_y)
    
    def show(self):
        """显示工具栏并重新定位"""
        super().show()
        
        # 重新定位到钉图窗口附近
        if self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)
        
        # 🔥 显示时停止自动隐藏定时器（等鼠标离开才启动）
        if self.auto_hide_timer.isActive():
            self.auto_hide_timer.stop()
    
    def enterEvent(self, event):
        """鼠标进入工具栏，停止自动隐藏定时器"""
        super().enterEvent(event)
        if self.auto_hide_timer.isActive():
            self.auto_hide_timer.stop()
            print("⏸️ [钉图工具栏] 鼠标进入，停止自动隐藏")
    
    def leaveEvent(self, event):
        """鼠标离开工具栏，启动自动隐藏定时器"""
        super().leaveEvent(event)
        if self._should_auto_hide():
            self.auto_hide_timer.start()
            print("▶️ [钉图工具栏] 鼠标离开，启动自动隐藏定时器（2秒后隐藏）")
    
    def _auto_hide(self):
        """自动隐藏工具栏"""
        if not self.auto_hide_enabled:
            return
        
        if self._is_parent_editing() or self._parent_hovering:
            # 编辑状态保持可见，稍后再次检查
            self.auto_hide_timer.start()
            return
        
        self.hide()
        print("🙈 [钉图工具栏] 自动隐藏")
    
    def enable_auto_hide(self, enabled: bool = True):
        """
        启用/禁用自动隐藏功能
        
        Args:
            enabled: 是否启用自动隐藏
        """
        self.auto_hide_enabled = enabled
        
        if enabled and self._should_auto_hide():
            self.auto_hide_timer.start()
        else:
            # 禁用时停止定时器
            self.auto_hide_timer.stop()
        
        print(f"⏰ [钉图工具栏] 自动隐藏: {'启用' if enabled else '禁用'}")
    
    def set_auto_hide_delay(self, milliseconds: int):
        """
        设置自动隐藏延迟时间
        
        Args:
            milliseconds: 延迟毫秒数
        """
        self.auto_hide_timer.setInterval(milliseconds)
        print(f"⏰ [钉图工具栏] 自动隐藏延迟设置为: {milliseconds}ms")
    
    def sync_with_pin_window(self):
        """
        与钉图窗口同步位置（当钉图窗口移动/缩放时调用）
        """
        if self.isVisible() and self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)
            
            # 🔥 同步二级菜单位置（如果可见）
            if hasattr(self, 'paint_menu') and self.paint_menu.isVisible():
                self._sync_paint_menu_position()

    def on_parent_editing_state_changed(self, editing: bool):
        """父窗口编辑状态变化时的回调"""
        if editing:
            if self.auto_hide_timer.isActive():
                self.auto_hide_timer.stop()
        else:
            if self._should_auto_hide() and not self.underMouse():
                self.auto_hide_timer.start()

    def _is_parent_editing(self) -> bool:
        return bool(self.parent_pin_window and getattr(self.parent_pin_window, '_is_editing', False))

    def on_parent_hover(self, hovering: bool):
        """由 PinWindow 通知鼠标是否仍在钉图窗口范围内"""
        self._parent_hovering = hovering
        if hovering:
            if self.auto_hide_timer.isActive():
                self.auto_hide_timer.stop()
        else:
            if self._should_auto_hide() and not self.underMouse():
                self.auto_hide_timer.start()

    def _should_auto_hide(self) -> bool:
        return (
            self.auto_hide_enabled and
            self.isVisible() and
            not self._is_parent_editing() and
            not self._parent_hovering
        )


# 使用示例
if __name__ == "__main__":
    import sys
    from PyQt6.QtWidgets import QApplication, QWidget, QLabel
    from PyQt6.QtGui import QPixmap
    
    app = QApplication(sys.argv)
    
    # 创建一个模拟的钉图窗口
    mock_pin = QWidget()
    mock_pin.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint)
    mock_pin.setGeometry(100, 100, 400, 300)
    mock_pin.setStyleSheet("background-color: lightblue; border: 2px solid black;")
    label = QLabel("模拟钉图窗口", mock_pin)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setGeometry(0, 0, 400, 300)
    mock_pin.show()
    
    # 创建钉图工具栏
    toolbar = PinToolbar(parent_pin_window=mock_pin)
    toolbar.enable_auto_hide(True)
    toolbar.set_auto_hide_delay(3000)  # 3秒后自动隐藏
    
    # 连接信号
    toolbar.tool_changed.connect(lambda tool: print(f"工具切换: {tool}"))
    toolbar.save_clicked.connect(lambda: print("保存点击"))
    toolbar.copy_clicked.connect(lambda: print("复制点击"))
    toolbar.undo_clicked.connect(lambda: print("撤销点击"))
    toolbar.redo_clicked.connect(lambda: print("重做点击"))
    
    toolbar.show()
    
    sys.exit(app.exec())
