"""
工具栏 - 截图工具栏UI
"""
import os
import sys
from PySide6.QtCore import Qt, QSize, Signal, QRect, QRectF, QPoint
from PySide6.QtGui import QIcon, QColor, QCursor, QFont, QPainter, QPainterPath, QPen, QBrush
from PySide6.QtWidgets import (
    QWidget, QPushButton, QSlider, QLabel, 
    QApplication, QColorDialog
)
from core.resource_manager import ResourceManager
from core.theme import get_theme
from core import log_debug, safe_event
from core.logger import log_exception
from core.constants import DEFAULT_FONT_FAMILY


class _DragHandle(QWidget):
    """工具栏左端拖动手柄 —— 青绿色圆角竖条，与选区框配色一致
    
    两种视觉状态:
    - 自动定位模式: 纯色填充
    - 手动定位模式: 纯色填充 + 三个白色圆点（提示可双击复位）
    """

    _DOT_COLOR = QColor(255, 255, 255)

    reset_requested = Signal()  # 双击时发出，请求切回自动定位

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self._manual_mode = False   # 外部设置

    def set_manual_mode(self, manual: bool):
        """切换视觉状态"""
        if self._manual_mode != manual:
            self._manual_mode = manual
            self.update()

    @safe_event
    def mouseDoubleClickEvent(self, event):
        """双击切回自动定位"""
        if event.button() == Qt.MouseButton.LeftButton and self._manual_mode:
            self.reset_requested.emit()
        super().mouseDoubleClickEvent(event)

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        # 左侧两角圆角、右侧直角，与工具栏整体左侧圆角对齐
        radius = 4
        r = self.rect()
        path = QPainterPath()
        path.moveTo(r.left() + radius, r.top())
        path.lineTo(r.right(), r.top())
        path.lineTo(r.right(), r.bottom())
        path.lineTo(r.left() + radius, r.bottom())
        path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - radius)
        path.lineTo(r.left(), r.top() + radius)
        path.quadTo(r.left(), r.top(), r.left() + radius, r.top())
        painter.fillPath(path, get_theme().theme_color)

        # 手动定位模式: 画三个白色圆点
        if self._manual_mode:
            cx = r.center().x()
            cy = r.center().y()
            dot_r = 3
            gap = 9
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(self._DOT_COLOR)
            for dy in (-gap, 0, gap):
                painter.drawEllipse(QPoint(cx, cy + dy), dot_r, dot_r)

        painter.end()

def resource_path(relative_path):
    """获取资源文件路径（兼容函数）"""
    return ResourceManager.get_resource_path(relative_path)

def cached_icon(relative_path):
    """获取缓存的 QIcon（首次加载 SVG，后续复用）"""
    return ResourceManager.get_icon(ResourceManager.get_resource_path(relative_path))

class Toolbar(QWidget):
    """
    截图工具栏
    """
    # ── 整体缩放因子 ──────────────────────────────────────
    # 修改这一个数值即可等比例缩放整个工具栏。
    # 1.0  → 默认尺寸（按钮 45px，图标 32/36px）
    # 0.8  → 缩小 20%
    # 1.2  → 放大 20%
    SCALE: float = 0.95

    # 信号定义
    tool_changed = Signal(str)  # 工具切换信号(tool_id)
    save_clicked = Signal()  # 保存按钮
    copy_clicked = Signal()  # 复制按钮
    pin_clicked = Signal()  # 钉图按钮
    confirm_clicked = Signal()  # 确认按钮
    cancel_clicked = Signal()  # 结束截图按钮
    undo_clicked = Signal()  # 撤销
    redo_clicked = Signal()  # 重做
    long_screenshot_clicked = Signal()  # 长截图按钮
    screenshot_translate_clicked = Signal()  # 截图翻译按钮
    gif_record_clicked = Signal()  # GIF录制按钮
    color_changed = Signal(QColor)  # 颜色改变
    stroke_width_changed = Signal(int)  # 线宽改变
    opacity_changed = Signal(int)  # 透明度改变(0-255)
    number_next_changed = Signal(int)  # 序号工具下一数字改变
    
    # 文字工具专用信号
    text_font_changed = Signal(QFont)
    text_color_changed = Signal(QColor)  # 文字颜色改变
    text_outline_changed = Signal(bool, QColor, int)
    text_shadow_changed = Signal(bool, QColor)
    text_background_changed = Signal(bool, QColor, int)
    
    # 箭头工具专用信号
    arrow_style_changed = Signal(str)  # 箭头样式改变(single/double/bar)
    
    # 画笔工具专用信号
    line_style_changed = Signal(str)  # 线条样式改变(solid/dashed)
    
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
            
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # 不接受焦点

        # ── 根据 SCALE 计算实际尺寸 ──────────────────────
        s = self.SCALE
        btn_width  = round(45 * s)   # 工具按钮宽
        btn_height = round(45 * s)   # 所有按钮高（工具栏高度）
        wide_w     = round(50 * s)   # 功能按钮宽（左侧4个 + 右侧3个）
        icon_wide  = round(36 * s)   # 功能按钮图标尺寸
        icon_tool  = round(32 * s)   # 工具按钮图标尺寸
        icon_eraser = round(28 * s)  # 橡皮擦图标尺寸

        # 左侧拖动手柄（青绿色竖条）
        handle_w = round(btn_height * 0.32)   # 宽度约为高度的 1/3
        self.drag_handle = _DragHandle(self)
        self.drag_handle.setGeometry(0, 0, handle_w, btn_height)
        self.drag_handle.setToolTip(self.tr("Drag to move"))
        self.drag_handle.installEventFilter(self)   # 事件透传，拖动由 Toolbar 统一处理
        self.drag_handle.reset_requested.connect(self._reset_auto_position)

        # 拖动状态
        self._dragging = False
        self._drag_offset = QPoint()
        self._manual_positioned = False   # 用户手动拖动后为 True，阻止自动定位

        # 左侧按钮区域（从手柄右侧开始）
        left_x = handle_w

        # 0. 长截图按钮（放在最左边）
        self.long_screenshot_btn = QPushButton(self)
        self.long_screenshot_btn.setGeometry(left_x, 0, wide_w, btn_height)
        self.long_screenshot_btn.setToolTip(self.tr('Long screenshot (scroll)'))
        self.long_screenshot_btn.setIcon(cached_icon("svg/长截图.svg"))
        self.long_screenshot_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.long_screenshot_btn.clicked.connect(self.long_screenshot_clicked.emit)
        left_x += wide_w
        
        # 1. 保存按钮
        self.save_btn = QPushButton(self)
        self.save_btn.setGeometry(left_x, 0, wide_w, btn_height)
        self.save_btn.setToolTip(self.tr('Save to file'))
        self.save_btn.setIcon(cached_icon("svg/下载.svg"))
        self.save_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.save_btn.clicked.connect(self.save_clicked.emit)
        left_x += wide_w
        
        # 1.5 截图翻译按钮（保存按钮右侧）
        self.screenshot_translate_btn = QPushButton(self)
        self.screenshot_translate_btn.setGeometry(left_x, 0, wide_w, btn_height)
        self.screenshot_translate_btn.setToolTip(self.tr('Screenshot translate (OCR + Translate)'))
        self.screenshot_translate_btn.setIcon(cached_icon("svg/翻译.svg"))
        self.screenshot_translate_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.screenshot_translate_btn.clicked.connect(self.screenshot_translate_clicked.emit)
        left_x += wide_w
        
        # GIF 录制按钮
        self.gif_btn = QPushButton(self)
        self.gif_btn.setGeometry(left_x, 0, wide_w, btn_height)
        self.gif_btn.setToolTip(self.tr('GIF recording'))
        self.gif_btn.setIcon(cached_icon("svg/gif.svg"))
        self.gif_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.gif_btn.clicked.connect(self.gif_record_clicked.emit)
        left_x += wide_w
        
        # 2. 复制按钮（暂时隐藏在截图模式）
        self.copy_btn = QPushButton(self)
        self.copy_btn.setGeometry(left_x, 0, wide_w, btn_height)
        self.copy_btn.setToolTip(self.tr('Copy image'))
        self.copy_btn.setIcon(cached_icon("svg/copy.svg"))
        self.copy_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.copy_btn.clicked.connect(self.copy_clicked.emit)
        self.copy_btn.hide()  # 截图模式下隐藏，只在钉图模式显示
        # left_x += wide_w  # 不增加位置，因为隐藏了
        
        # 3. 画笔工具
        self.pen_btn = QPushButton(self)
        self.pen_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.pen_btn.setToolTip(self.tr('Pen tool (hold Shift for straight line)'))
        self.pen_btn.setIcon(cached_icon("svg/画笔.svg"))
        self.pen_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.pen_btn.setCheckable(True)
        self.pen_btn.setChecked(False)  # 默认不选中，因为默认是 cursor 模式
        self.pen_btn.clicked.connect(lambda: self._on_tool_clicked("pen"))
        left_x += btn_width
        
        # 4. 荧光笔工具
        self.highlighter_btn = QPushButton(self)
        self.highlighter_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.highlighter_btn.setToolTip(self.tr('Highlighter (hold Shift for straight line)'))
        self.highlighter_btn.setIcon(cached_icon("svg/荧光笔.svg"))
        self.highlighter_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.highlighter_btn.setCheckable(True)
        self.highlighter_btn.clicked.connect(lambda: self._on_tool_clicked("highlighter"))
        left_x += btn_width
        
        # 5. 箭头工具
        self.arrow_btn = QPushButton(self)
        self.arrow_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.arrow_btn.setToolTip(self.tr('Draw arrow'))
        self.arrow_btn.setIcon(cached_icon("svg/箭头.svg"))
        self.arrow_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.arrow_btn.setCheckable(True)
        self.arrow_btn.clicked.connect(lambda: self._on_tool_clicked("arrow"))
        left_x += btn_width
        
        # 6. 序号工具
        self.number_btn = QPushButton(self)
        self.number_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.number_btn.setToolTip(self.tr('Number (Shift+scroll to change number)'))
        self.number_btn.setIcon(cached_icon("svg/序号.svg"))
        self.number_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.number_btn.setCheckable(True)
        self.number_btn.clicked.connect(lambda: self._on_tool_clicked("number"))
        left_x += btn_width
        
        # 7. 矩形工具
        self.rect_btn = QPushButton(self)
        self.rect_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.rect_btn.setToolTip(self.tr('Draw rectangle'))
        self.rect_btn.setIcon(cached_icon("svg/方框.svg"))
        self.rect_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.rect_btn.setCheckable(True)
        self.rect_btn.clicked.connect(lambda: self._on_tool_clicked("rect"))
        left_x += btn_width
        
        # 8. 圆形工具
        self.ellipse_btn = QPushButton(self)
        self.ellipse_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.ellipse_btn.setToolTip(self.tr('Draw ellipse'))
        self.ellipse_btn.setIcon(cached_icon("svg/圆框.svg"))
        self.ellipse_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.ellipse_btn.setCheckable(True)
        self.ellipse_btn.clicked.connect(lambda: self._on_tool_clicked("ellipse"))
        left_x += btn_width
        
        # 9. 文字工具
        self.text_btn = QPushButton(self)
        self.text_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.text_btn.setToolTip(self.tr('Add text'))
        self.text_btn.setIcon(cached_icon("svg/文字.svg"))
        self.text_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.text_btn.setCheckable(True)
        self.text_btn.clicked.connect(lambda: self._on_tool_clicked("text"))
        left_x += btn_width
        
        # 10. 橡皮擦工具
        self.eraser_btn = QPushButton(self)
        self.eraser_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.eraser_btn.setToolTip(self.tr('Eraser tool'))
        self.eraser_btn.setIcon(cached_icon("svg/橡皮.svg"))
        self.eraser_btn.setIconSize(QSize(icon_eraser, icon_eraser))
        self.eraser_btn.setCheckable(True)
        self.eraser_btn.clicked.connect(lambda: self._on_tool_clicked("eraser"))
        left_x += btn_width
        
        # 11. 撤销按钮
        self.undo_btn = QPushButton(self)
        self.undo_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.undo_btn.setToolTip(self.tr('Undo'))
        self.undo_btn.setIcon(cached_icon("svg/撤回.svg"))
        self.undo_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.undo_btn.clicked.connect(self.undo_clicked.emit)
        left_x += btn_width
        
        # 12. 重做按钮
        self.redo_btn = QPushButton(self)
        self.redo_btn.setGeometry(left_x, 0, btn_width, btn_height)
        self.redo_btn.setToolTip(self.tr('Redo'))
        self.redo_btn.setIcon(cached_icon("svg/复原.svg"))
        self.redo_btn.setIconSize(QSize(icon_tool, icon_tool))
        self.redo_btn.clicked.connect(self.redo_clicked.emit)
        left_x += btn_width
        
        # 右侧按钮区域（结束截图 + 钉图 + 确定）
        right_buttons_width = wide_w * 3  # 结束截图 + 钉图 + 确定
        toolbar_total_width = left_x + right_buttons_width
        
        # 结束截图按钮（最左）
        self.cancel_btn = QPushButton(self)
        self.cancel_btn.setGeometry(toolbar_total_width - wide_w * 3, 0, wide_w, btn_height)
        self.cancel_btn.setToolTip(self.tr('Cancel screenshot (ESC)'))
        self.cancel_btn.setIcon(cached_icon("svg/结束截图.svg"))
        self.cancel_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.cancel_btn.clicked.connect(self.cancel_clicked.emit)
        
        # 钉图按钮（中间）
        self.pin_btn = QPushButton(self)
        self.pin_btn.setGeometry(toolbar_total_width - wide_w * 2, 0, wide_w, btn_height)
        self.pin_btn.setToolTip(self.tr('Pin image (Ctrl+D)'))
        self.pin_btn.setIcon(cached_icon("svg/钉图.svg"))
        self.pin_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.pin_btn.clicked.connect(self.pin_clicked.emit)
        
        # 确定按钮(吸附最右边)
        self.confirm_btn = QPushButton(self)
        self.confirm_btn.setGeometry(toolbar_total_width - wide_w, 0, wide_w, btn_height)
        self.confirm_btn.setToolTip(self.tr('Confirm and save (Ctrl+C / Enter)'))
        self.confirm_btn.setIcon(cached_icon("svg/确定.svg"))
        self.confirm_btn.setIconSize(QSize(icon_wide, icon_wide))
        self.confirm_btn.clicked.connect(self.confirm_clicked.emit)
        
        # 设置工具栏大小
        self.setObjectName("toolbar_root")
        self.resize(toolbar_total_width, btn_height)
        
        # 设置样式 —— 背景和圆角描边由 paintEvent 手动绘制，
        # 这里只设置按钮样式，不设置 #toolbar_root 的 background/border
        theme_hex = get_theme().theme_color_hex
        tc = get_theme().theme_color
        self.setStyleSheet(f"""
            #toolbar_root {{
                background-color: transparent;
                border: none;
            }}
            QPushButton {{
                background-color: rgba(0, 0, 0, 0.02);
                border: none;
                border-radius: 0px;
                padding: 0px;
            }}
            QPushButton:hover {{
                background-color: rgba(0, 0, 0, 0.08);
                border-radius: 0px;
            }}
            QPushButton:pressed {{
                background-color: rgba(0, 0, 0, 0.15);
                border-radius: 0px;
            }}
            QPushButton:checked {{
                background-color: rgba({tc.red()}, {tc.green()}, {tc.blue()}, 0.3);
                border: 1px solid {theme_hex};
            }}
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

        # 所有按钮不接受键盘焦点，防止 Space/Enter 等按键通过按钮意外触发逻辑
        for btn in self.findChildren(QPushButton):
            btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)

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
        self.paint_panel.line_style_changed.connect(self._on_line_style_changed)
        if hasattr(self.paint_panel, "highlighter_mode_changed"):
            self.paint_panel.highlighter_mode_changed.connect(self._on_highlighter_mode_changed)
        # 初始化线条样式（同步上次设置）
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            pen_settings = manager.get_tool_settings("pen") if manager else None
            if pen_settings:
                line_style = pen_settings.get("line_style", "solid")
                self.paint_panel.line_style = line_style
        except Exception as exc:
            log_debug(f"初始化线条样式失败: {exc}", "Toolbar")
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
        self.shape_panel.line_style_changed.connect(self._on_line_style_changed)
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
        self.arrow_panel.arrow_style_changed.connect(self._on_arrow_style_changed)
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
        self.number_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        if hasattr(self.number_panel, "next_number_changed"):
            self.number_panel.next_number_changed.connect(self._on_number_next_changed)
        self.number_panel.hide()
        
        # === 5. 文字设置面板 (text) ===
        self.text_panel = TextSettingsPanel(parent)
        self.text_panel.setWindowFlags(flags)
        if parent is None:
            self.text_panel.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.text_panel.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.text_panel.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        
        # 连接信号
        self.text_panel.font_changed.connect(self._on_text_font_changed)
        self.text_panel.color_changed.connect(self._on_text_color_changed)
        if hasattr(self.text_panel, 'background_changed'):
            self.text_panel.background_changed.connect(self._on_text_background_changed)
        self.text_panel.hide()
        
        # 加载保存的设置
        self._load_saved_settings()
        
        # 保持兼容性:paint_menu 和 text_menu 别名
        self.paint_menu = self.paint_panel
        self.text_menu = self.text_panel

    @safe_event
    def paintEvent(self, event):
        """手动绘制圆角白色背景 + 主题色描边，确保四角真正透明"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        radius = 6.0
        pen_width = 2.0
        half = pen_width / 2
        rect = QRectF(self.rect()).adjusted(half, half, -half, -half)

        path = QPainterPath()
        path.addRoundedRect(rect, radius, radius)

        painter.setPen(QPen(get_theme().theme_color, pen_width))
        painter.setBrush(QBrush(QColor(255, 255, 255)))
        painter.drawPath(path)
        painter.end()
        
    def _load_saved_settings(self):
        """加载保存的工具设置"""
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        
        # 加载文字工具设置（委托给面板）
        self.text_panel.load_from_config()
        
        # 加载箭头工具设置
        arrow_settings = manager.get_tool_settings("arrow")
        if arrow_settings and hasattr(self, 'arrow_panel'):
            arrow_style = arrow_settings.get("arrow_style", "single")
            self.arrow_panel.arrow_style = arrow_style
        
    def reset_session_state(self):
        """重置工具栏状态（新截图会话开始时调用）。"""
        # 取消所有工具选中
        for btn in self.tool_buttons.values():
            btn.setChecked(False)
        self.current_tool = None
        # 隐藏所有二级面板
        self._hide_all_panels()
        # 重置拖动定位
        self._manual_positioned = False
        self._dragging = False
        self.drag_handle.set_manual_mode(False)
        # 隐藏自身（选区确认后再显示）
        self.hide()
    
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

            # 同步线条样式到面板（rect / ellipse）
            if tool_id in ("rect", "ellipse") and hasattr(self, 'shape_panel'):
                try:
                    from settings import get_tool_settings_manager
                    manager = get_tool_settings_manager()
                    shape_settings = manager.get_tool_settings(tool_id) if manager else None
                    if shape_settings:
                        self.shape_panel.line_style = shape_settings.get("line_style", "solid")
                except Exception as exc:
                    log_debug(f"同步形状线条样式失败: {exc}", "Toolbar")
            
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

        if tool_id in ("pen", "highlighter") and hasattr(self, "paint_panel"):
            self.paint_panel.set_line_style_visible(tool_id == "pen")
            if hasattr(self.paint_panel, "set_highlighter_mode_visible"):
                self.paint_panel.set_highlighter_mode_visible(tool_id == "highlighter")
            if tool_id == "highlighter" and hasattr(self.paint_panel, "set_highlighter_mode"):
                try:
                    from settings import get_tool_settings_manager
                    manager = get_tool_settings_manager()
                    settings = manager.get_tool_settings("highlighter") if manager else None
                    if settings:
                        self.paint_panel.set_highlighter_mode(settings.get("draw_mode", "freehand"))
                except Exception as exc:
                    log_debug(f"同步荧光笔模式失败: {exc}", "Toolbar")
        
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
    # 设置面板信号处理
    # ========================================================================
    
    def _on_panel_color_changed(self, color):
        """面板颜色改变"""
        self.current_color = color
        self.color_changed.emit(color)
        
    def _on_panel_size_changed(self, size):
        """面板大小改变"""
        self.stroke_width_changed.emit(size)
        
    def _on_panel_opacity_changed(self, opacity):
        """面板透明度改变"""
        self.opacity_changed.emit(opacity)

    def _on_highlighter_mode_changed(self, mode: str):
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            if manager:
                manager.update_settings("highlighter", draw_mode=mode)
        except Exception as exc:
            log_debug(f"保存荧光笔模式失败: {exc}", "Toolbar")

        # 刷新光标（矩形模式使用十字光标）
        try:
            parent = self.parent()
            view = getattr(parent, "view", None) if parent else None
            cursor_manager = getattr(view, "cursor_manager", None) if view else None
            if cursor_manager:
                cursor_manager.set_tool_cursor("highlighter", force=True)
        except Exception as e:
            log_exception(e, "设置高亮笔光标")
    
    def _on_text_font_changed(self, font):
        """文字字体改变"""
        self.text_font_changed.emit(font)
        from .text_settings_panel import TextSettingsPanel
        TextSettingsPanel.save_font_to_config(font)

    def _on_text_color_changed(self, color):
        """文字颜色改变"""
        self.current_color = color
        self.color_changed.emit(color)
        self.text_color_changed.emit(color)  # 发射文字专用颜色信号
        # 保存颜色设置
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        manager.update_settings("text", color=color.name())

    def _on_text_background_changed(self, enabled: bool, color: QColor, opacity: int):
        """文字背景改变"""
        self.text_background_changed.emit(enabled, color, opacity)
        from .text_settings_panel import TextSettingsPanel
        TextSettingsPanel.save_background_to_config(enabled, color, opacity)

    def _on_arrow_style_changed(self, style: str):
        """箭头样式改变"""
        # 保存箭头样式设置
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        manager.update_settings("arrow", arrow_style=style)
        # 发射信号，通知截图窗口/钉图窗口更新选中的箭头项
        self.arrow_style_changed.emit(style)
    
    def _on_line_style_changed(self, style: str):
        """线条样式改变"""
        # 保存画笔线条样式设置
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        tool_id = self.current_tool or "pen"
        if tool_id in ("rect", "ellipse"):
            manager.update_settings(tool_id, line_style=style)
        elif tool_id == "pen":
            manager.update_settings("pen", line_style=style)
        # 发射信号，通知截图窗口/钉图窗口
        self.line_style_changed.emit(style)

    def _on_number_next_changed(self, value: int):
        """序号工具下一数字改变"""
        self.number_next_changed.emit(int(value))

    # ========================================================================
    # 工具设置同步方法（用于从设置管理器更新UI）
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
        if hasattr(self, 'number_panel'):
            self.number_panel.set_opacity(opacity_255)

    def set_number_next_value(self, value: int):
        """设置序号工具下一数字（更新UI显示）"""
        if hasattr(self, 'number_panel') and hasattr(self.number_panel, 'set_next_number'):
            self.number_panel.set_next_number(int(value))
    
    
    def position_near_rect(self, rect: QRectF, parent_widget=None):
        """
        智能定位工具栏
        优先级：选区下方右对齐 > 上方右对齐
        工具栏归属于选框中心点所在的屏幕
        
        Args:
            rect: 选区矩形（场景坐标）
            parent_widget: 父窗口（用于坐标转换）
        """
        # 用户手动拖动过 → 不自动回弹
        if getattr(self, '_manual_positioned', False):
            return

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
        
        # 二级菜单的最大高度（加上间距），一级+二级作为整体判断能否放下
        panel_extra = self._get_max_panel_height()
        
        # 根据选框中心点确定归属屏幕
        screen = self._get_screen_by_center(global_rect)
        screen_rect = screen.geometry()
        
        margin = 10  # 边距
        
        # 策略1: 下方右对齐（需要放得下工具栏 + 二级菜单的总高度）
        x = global_rect.right() - toolbar_w
        y = global_rect.bottom() + margin
        toolbar_below = True

        # 若下方放不下（含二级菜单），则回退到上方
        if y + toolbar_h + panel_extra > screen_rect.bottom():
            y = global_rect.top() - toolbar_h - margin
            toolbar_below = False

        # Y 轴夹紧到屏幕内
        y = max(screen_rect.top(), min(y, screen_rect.bottom() - toolbar_h))

        # X 轴夹紧到屏幕内（屏幕边界对工具栏有阻拦效果）
        x = max(screen_rect.left(), min(x, screen_rect.right() - toolbar_w))

        # 记录工具栏在选区的哪一侧，二级菜单据此决定弹出方向
        self._toolbar_below_selection = toolbar_below

        # 移动工具栏
        final_pos = QPoint(x, y)
        if self.parent():
            final_pos = self.parent().mapFromGlobal(final_pos)
        self.move(final_pos)

    def _get_screen_by_center(self, window_rect: QRect):
        """
        根据窗口/选框的中心点确定所属屏幕
        
        Args:
            window_rect: 窗口或选框的矩形区域
            
        Returns:
            中心点所在的屏幕，找不到则返回主屏幕
        """
        center_point = window_rect.center()
        screen = QApplication.screenAt(center_point)
        if screen:
            return screen
        return QApplication.primaryScreen()

    def _get_max_panel_height(self) -> int:
        """获取所有二级菜单面板的最大高度（含间距），用于一级工具栏定位时预留空间"""
        gap = 5
        max_h = 0
        for attr in ('paint_panel', 'shape_panel', 'arrow_panel', 'number_panel', 'text_panel'):
            panel = getattr(self, attr, None)
            if panel:
                max_h = max(max_h, panel.sizeHint().height())
        return (max_h + gap) if max_h else 0

    # ========================================================================
    # 拖动手柄 —— 定位模式切换
    # ========================================================================

    def _set_manual_positioned(self, manual: bool):
        """统一设置手动/自动定位状态，同步手柄视觉"""
        self._manual_positioned = manual
        if hasattr(self, 'drag_handle'):
            self.drag_handle.set_manual_mode(manual)

    def _reset_auto_position(self):
        """双击手柄 → 切回自动定位模式并立即重新定位一次"""
        self._set_manual_positioned(False)
        # 截图工具栏：通过 update_toolbar_position 间接调用 position_near_rect
        parent = self.parent()
        if parent and hasattr(parent, 'update_toolbar_position'):
            parent.update_toolbar_position()

    # ========================================================================
    # 拖动手柄 —— 事件处理
    # ========================================================================

    @safe_event
    def eventFilter(self, obj, event):
        """将 drag_handle 的鼠标事件统一转发给 Toolbar 处理（双击穿透）"""
        from PySide6.QtCore import QEvent
        if hasattr(self, 'drag_handle') and obj is self.drag_handle:
            etype = event.type()
            # 双击事件不拦截，让 _DragHandle.mouseDoubleClickEvent 自行处理
            if etype == QEvent.Type.MouseButtonDblClick:
                return False
            if etype == QEvent.Type.MouseButtonPress:
                if event.button() == Qt.MouseButton.LeftButton:
                    self._dragging = True
                    self._drag_offset = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
                    self.drag_handle.setCursor(Qt.CursorShape.ClosedHandCursor)
                return True
            elif etype == QEvent.Type.MouseMove:
                if event.buttons() == Qt.MouseButton.LeftButton and self._dragging:
                    self.move(event.globalPosition().toPoint() - self._drag_offset)
                return True
            elif etype == QEvent.Type.MouseButtonRelease:
                if event.button() == Qt.MouseButton.LeftButton:
                    if self._dragging:
                        self._set_manual_positioned(True)
                    self._dragging = False
                    self.drag_handle.setCursor(Qt.CursorShape.SizeAllCursor)
                return True
        return super().eventFilter(obj, event)

    @safe_event
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
        """同步单个面板的位置
        
        优先跟一级工具栏同方向弹出（远离选区），空间不够时翻到另一侧。
        """
        if not panel:
            return
        
        toolbar_global_pos = self.mapToGlobal(QPoint(0, 0))
        gap = 5
        panel_h = panel.height()
        toolbar_h = self.height()
        
        # 获取屏幕信息
        screen = QApplication.screenAt(toolbar_global_pos)
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_rect = screen.geometry()
        
        # 两个候选位置
        below_y = toolbar_global_pos.y() + toolbar_h + gap
        above_y = toolbar_global_pos.y() - panel_h - gap
        
        below_ok = (below_y + panel_h <= screen_rect.bottom())
        above_ok = (above_y >= screen_rect.top())
        
        toolbar_below = getattr(self, '_toolbar_below_selection', True)
        
        if toolbar_below:
            # 工具栏在选区下方 → 优先下方，不行就上方
            if below_ok:
                panel_y = below_y
            elif above_ok:
                panel_y = above_y
            else:
                # 都放不下，夹紧到屏幕底部
                panel_y = screen_rect.bottom() - panel_h
        else:
            # 工具栏在选区上方 → 优先上方，不行就下方
            if above_ok:
                panel_y = above_y
            elif below_ok:
                panel_y = below_y
            else:
                # 都放不下，夹紧到屏幕顶部
                panel_y = screen_rect.top()
        
        # X 轴：与工具栏左对齐，夹紧在屏幕内
        panel_x = toolbar_global_pos.x()
        if panel_x + panel.width() > screen_rect.right():
            panel_x = screen_rect.right() - panel.width() - 5
        if panel_x < screen_rect.left():
            panel_x = screen_rect.left() + 5
        
        final_pos = QPoint(panel_x, panel_y)
        if panel.parent():
            final_pos = panel.parent().mapFromGlobal(final_pos)
        
        panel.move(final_pos)

 