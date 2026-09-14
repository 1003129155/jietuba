"""
工具栏 - 截图工具栏UI
"""

from PySide6.QtCore import Qt, QSize, Signal, QRect, QRectF, QPoint, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen, QBrush
from PySide6.QtWidgets import (
    QAbstractButton, QWidget, QPushButton, QApplication
)
from core.resource_manager import ResourceManager
from core.theme import get_theme
from core import log_debug, safe_event
from core.logger import log_exception, T
from .toolbar_layout import MORE, SHOW, load_layout, save_layout


class _DragHandle(QWidget):
    """工具栏左端拖动手柄 —— 青绿色圆角竖条，与选区框配色一致
    
    两种视觉状态:
    - 自动定位模式: 纯色填充
    - 手动定位模式: 纯色填充 + 三个白色圆点（提示可双击复位）
    """

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
        _paint_end_strip(self, dots=self._manual_mode)


def _paint_end_strip(widget, *, dots, mirrored=False):
    """绘制工具栏左端的主题色拖动竖条。

    只有朝外的两个角是圆角，贴合工具栏整体的圆角；mirrored 用于需要左右翻转的场景。
    dots 时在正中竖排三个白点。
    """
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    r = widget.rect()
    if mirrored:
        painter.translate(r.width(), 0)
        painter.scale(-1, 1)
    radius = 4
    path = QPainterPath()
    path.moveTo(r.left() + radius, r.top())
    path.lineTo(r.right(), r.top())
    path.lineTo(r.right(), r.bottom())
    path.lineTo(r.left() + radius, r.bottom())
    path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - radius)
    path.lineTo(r.left(), r.top() + radius)
    path.quadTo(r.left(), r.top(), r.left() + radius, r.top())
    painter.fillPath(path, get_theme().theme_color)

    if dots:
        _paint_vertical_dots(painter, r, QColor(255, 255, 255))
    painter.end()


def _paint_vertical_dots(painter, rect, color):
    """在给定区域正中绘制竖排三点。"""
    cx = rect.center().x()
    cy = rect.center().y()
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(color)
    for dy in (-9, 0, 9):
        painter.drawEllipse(QPoint(cx, cy + dy), 3, 3)


class _MoreHandle(QAbstractButton):
    """工具栏右端的「…」：白色工具栏背景上的竖排黑点。

    宽度和拖动手柄一样，但不再重复左侧的主题色竖条，让按钮与工具栏白底融为一体。
    不设 tooltip：悬停本身就会立刻弹出面板，系统提示框反而会盖住刚弹出的面板。
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        _paint_vertical_dots(painter, self.rect(), QColor(0, 0, 0))
        painter.end()


def resource_path(relative_path):
    """获取资源文件路径（兼容函数）"""
    return ResourceManager.get_resource_path(relative_path)

def cached_icon(relative_path):
    """获取缓存的 QIcon（首次加载 SVG，后续复用）"""
    return ResourceManager.get_icon(ResourceManager.get_resource_path(relative_path))

def _paint_toolbar_frame(widget):
    """白底圆角 + 主题色描边。工具栏和「…」弹层共用，四角靠 WA_TranslucentBackground 保持透明"""
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    radius = 6.0
    pen_width = 2.0
    half = pen_width / 2
    rect = QRectF(widget.rect()).adjusted(half, half, -half, -half)

    path = QPainterPath()
    path.addRoundedRect(rect, radius, radius)

    painter.setPen(QPen(get_theme().theme_color, pen_width))
    painter.setBrush(QBrush(QColor(255, 255, 255)))
    painter.drawPath(path)
    painter.end()

def _button_qss():
    """工具栏按钮样式。

    按钮会在工具栏和「…」弹层之间换父部件，而样式表沿父子链级联，所以两边必须挂
    同一份，否则按钮一挪进弹层就变回系统默认外观。
    """
    tc = get_theme().theme_color
    return f"""
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
            border: 1px solid {get_theme().theme_color_hex};
        }}
    """

class _MorePopup(QWidget):
    """「…」弹层：装被收起的按钮，最下面一个「调整」入口。

    按钮不是另建一份，而是把工具栏上同一个 QPushButton 用 setParent 挪进来——信号
    连接、选中态、tool_buttons 映射都跟着按钮走，弹层不需要知道每个按钮是做什么的。
    """

    COLUMNS = 5   # 每行几个按钮
    PADDING = 4

    hover_changed = Signal(bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        from .fluent_lite import FluentIcon

        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setStyleSheet(_button_qss() + """
            QPushButton#more_adjust {
                color: #5F6368;
                font-size: 12px;
                padding: 0px 8px;
            }
        """)
        self.adjust_btn = QPushButton(self)
        self.adjust_btn.setObjectName("more_adjust")
        self.adjust_btn.setIcon(FluentIcon.SETTING.icon())
        self.adjust_btn.setIconSize(QSize(14, 14))
        self.adjust_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)

    def set_buttons(self, buttons, cell):
        """把 buttons 按每行 COLUMNS 个排成网格，每格 cell 大小；「调整」放在网格下方靠右"""
        pad = self.PADDING
        for index, button in enumerate(buttons):
            if button.parent() is not self:
                button.setParent(self)
            row, column = divmod(index, self.COLUMNS)
            button.setGeometry(pad + column * cell.width(), pad + row * cell.height(),
                               cell.width(), cell.height())
            button.show()

        rows = -(-len(buttons) // self.COLUMNS)   # 向上取整
        grid_width = min(len(buttons), self.COLUMNS) * cell.width()
        adjust_width = self.adjust_btn.sizeHint().width()
        adjust_height = round(cell.height() * 0.7)
        width = max(grid_width, adjust_width) + 2 * pad
        top = pad + rows * cell.height()
        self.adjust_btn.setGeometry(width - pad - adjust_width, top, adjust_width, adjust_height)
        self.resize(width, top + adjust_height + pad)

    @safe_event
    def paintEvent(self, event):
        _paint_toolbar_frame(self)

    @safe_event
    def enterEvent(self, event):
        super().enterEvent(event)
        self.hover_changed.emit(True)

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        self.hover_changed.emit(False)

class Toolbar(QWidget):
    """
    截图工具栏
    """
    # ── 整体缩放因子 ──────────────────────────────────────
    # 修改这一个数值即可等比例缩放整个工具栏。
    # 1.0  → 默认尺寸（按钮 45px，图标 32/36px）
    # 0.8  → 缩小 20%
    # 1.2  → 放大 20%
    SCALE: float = 0.90

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
    scan_code_clicked = Signal()  # 扫码按钮
    gif_record_clicked = Signal()  # GIF录制按钮
    color_changed = Signal(QColor)  # 颜色改变
    number_style_changed = Signal(str)  # 序号样式改变
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

    # 马赛克工具专用信号
    mosaic_style_changed = Signal(str)       # 马赛克种类改变(pixelate/blur)
    mosaic_block_size_changed = Signal(int)  # 马赛克/模糊粒度改变

    # 画笔工具专用信号
    line_style_changed = Signal(str)  # 线条样式改变(solid/dashed)
    
    # 有二级面板的工具。pen/highlighter 与 rect/ellipse 各自共用一个面板，
    # 但设置是分工具存的，所以这里按工具而不是按面板列。
    PANEL_TOOLS = ("pen", "highlighter", "rect", "ellipse", "arrow", "number", "text", "mosaic")

    def __init__(self, parent=None):
        super().__init__(parent)  # 使用父窗口（如果有）
        
        # 当前选中的工具
        self.current_tool = None  # 初始无工具选中，用户点击后才激活
        self.temporary_edit_active = False
        
        # 当前颜色
        self.current_color = QColor(255, 0, 0)  # 默认红色
        
        self.init_ui()
        
    def init_ui(self):
        """初始化UI"""
        # 按钮登记表：key → 按钮 / 宽度。这里只建按钮、不定位置，位置统一由 _arrange
        # 按排布摆放——截图读用户配置，钉图用固定列表，两边共用同一段摆放逻辑。
        self._buttons = {}
        self._button_widths = {}
        self._folded_keys = []    # 收进「…」弹层的按钮，弹层展开时才摆进去
        self._more_popup = None   # 用到才建，见 _show_more_popup

        self._make_floating(self)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)

        # ── 根据 SCALE 计算实际尺寸 ──────────────────────
        s = self.SCALE
        btn_width  = round(45 * s)   # 工具按钮宽
        btn_height = round(45 * s)   # 所有按钮高（工具栏高度）
        wide_w     = round(50 * s)   # 功能按钮宽（长截图、保存、结束截图、确定等）
        icon_wide  = round(36 * s)   # 功能按钮图标尺寸
        icon_tool  = round(32 * s)   # 工具按钮图标尺寸
        icon_eraser = round(28 * s)  # 橡皮擦图标尺寸
        self._btn_height = btn_height

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

        # 记录所有 tooltip 源文本，供语言切换后整体刷新（按钮在 _add_button 里登记）
        self._tooltip_sources = {self.drag_handle: "Drag to move"}

        wide = (wide_w, icon_wide)
        tool = (btn_width, icon_tool)

        self.long_screenshot_btn = self._add_button(
            "long_screenshot", "svg/长截图.svg", "Long screenshot (scroll)", wide,
            self.long_screenshot_clicked.emit)
        self.save_btn = self._add_button(
            "save", "svg/下载.svg", "Save to file", wide, self.save_clicked.emit)
        self.screenshot_translate_btn = self._add_button(
            "screenshot_translate", "svg/翻译.svg", "Screenshot translate (OCR + Translate)", wide,
            self.screenshot_translate_clicked.emit)
        self.scan_code_btn = self._add_button(
            "scan_code", "svg/扫码.svg", "Scan QR code / barcode", wide, self.scan_code_clicked.emit)
        self.gif_btn = self._add_button(
            "gif", "svg/gif.svg", "GIF recording", wide, self.gif_record_clicked.emit)
        # 复制按钮只在钉图里摆出来，截图的排布里没有它
        self.copy_btn = self._add_button(
            "copy", "svg/copy.svg", "Copy image", wide, self.copy_clicked.emit)

        self.pen_btn = self._add_tool_button(
            "pen", "svg/画笔.svg", "Pen tool (hold Shift for straight line)", tool)
        self.highlighter_btn = self._add_tool_button(
            "highlighter", "svg/荧光笔.svg", "Highlighter (hold Shift for straight line)", tool)
        self.mosaic_btn = self._add_tool_button(
            "mosaic", "svg/马赛克.svg", "Mosaic (mouse wheel to resize)", tool)
        self.spotlight_btn = self._add_tool_button(
            "spotlight", "svg/聚光灯.svg", "Spotlight (dim outside the box)", tool)
        self.arrow_btn = self._add_tool_button("arrow", "svg/箭头.svg", "Draw arrow", tool)
        self.number_btn = self._add_tool_button(
            "number", "svg/序号.svg", "Number (Shift+scroll to change number)", tool)
        self.rect_btn = self._add_tool_button("rect", "svg/方框.svg", "Draw rectangle", tool)
        self.ellipse_btn = self._add_tool_button("ellipse", "svg/圆框.svg", "Draw ellipse", tool)
        self.text_btn = self._add_tool_button("text", "svg/文字.svg", "Add text", tool)
        self.eraser_btn = self._add_tool_button(
            "eraser", "svg/橡皮.svg", "Eraser tool", (btn_width, icon_eraser))

        self.undo_btn = self._add_button("undo", "svg/撤回.svg", "Undo", tool, self.undo_clicked.emit)
        self.redo_btn = self._add_button("redo", "svg/复原.svg", "Redo", tool, self.redo_clicked.emit)

        self.cancel_btn = self._add_button(
            "cancel", "svg/结束截图.svg", "Cancel screenshot (ESC)", wide, self.cancel_clicked.emit)
        self.pin_btn = self._add_button(
            "pin", "svg/钉图.svg", "Pin image (Ctrl+D)", wide, self.pin_clicked.emit)
        self.confirm_btn = self._add_button(
            "confirm", "svg/确定.svg", "Confirm and save (Ctrl+C / Enter)", wide,
            self.confirm_clicked.emit)

        # 「…」：悬停或点击展开被收起的按钮；和普通按钮一样登记进排布表，由 _arrange 固定摆在最右
        self.more_btn = _MoreHandle(self)
        self.more_btn.clicked.connect(self._show_more_popup)
        self.more_btn.installEventFilter(self)
        self._buttons["more"] = self.more_btn
        self._button_widths["more"] = handle_w

        # 背景和圆角描边由 paintEvent 手动绘制，#toolbar_root 保持透明
        self.setObjectName("toolbar_root")
        self.setStyleSheet("#toolbar_root { background-color: transparent; border: none; }"
                           + _button_qss())

        # 收集所有工具按钮
        self.tool_buttons = {
            "pen": self.pen_btn,
            "highlighter": self.highlighter_btn,
            "mosaic": self.mosaic_btn,
            "spotlight": self.spotlight_btn,
            "arrow": self.arrow_btn,
            "number": self.number_btn,
            "rect": self.rect_btn,
            "ellipse": self.ellipse_btn,
            "text": self.text_btn,
            "eraser": self.eraser_btn,
        }

        # 创建二级设置面板
        self.init_settings_panels()

        self.reload_layout()

        # 语言切换时刷新工具栏按钮提示与各面板文案（连接随本工具栏销毁自动断开）
        from core.i18n import I18nManager
        I18nManager.instance().language_changed.connect(self._retranslate)

    # ========================================================================
    # 按钮排布与「…」弹层
    # ========================================================================

    def _add_button(self, key, icon, tooltip, size, on_click, *, checkable=False):
        """建一个按钮并登记到排布表。size 为 (按钮宽, 图标边长)；位置由 _arrange 决定。"""
        width, icon_size = size
        button = QPushButton(self)
        button.setToolTip(self.tr(tooltip))
        button.setIcon(cached_icon(icon))
        button.setIconSize(QSize(icon_size, icon_size))
        button.setCheckable(checkable)
        # 按钮不接受键盘焦点，防止 Space/Enter 等按键通过按钮意外触发逻辑
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # 点了按钮先收起「…」弹层再执行动作：动作可能弹出模态对话框，弹层不该留在屏幕上。
        # 工具栏上的按钮被点时鼠标早已离开弹层，本来就要收起，不必区分按钮在哪
        button.clicked.connect(self._hide_more_popup)
        button.clicked.connect(on_click)
        self._buttons[key] = button
        self._button_widths[key] = width
        self._tooltip_sources[button] = tooltip
        return button

    def _add_tool_button(self, tool_id, icon, tooltip, size):
        """绘制工具按钮：可选中，点击走 select_tool 的切换逻辑"""
        return self._add_button(
            tool_id, icon, tooltip, size, lambda: self._on_tool_clicked(tool_id), checkable=True)

    def _make_floating(self, widget):
        """工具栏、二级面板、「…」弹层共用的窗口属性。

        截图里它们都是截图窗口的子部件；钉图工具栏没有父窗口，是独立的置顶工具窗，
        面板和弹层跟它一致，显示时也不抢焦点。
        """
        if self.parent() is None:
            widget.setWindowFlags(
                Qt.WindowType.FramelessWindowHint
                | Qt.WindowType.WindowStaysOnTopHint
                | Qt.WindowType.Tool
                | Qt.WindowType.X11BypassWindowManagerHint
            )
            widget.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        else:
            widget.setWindowFlags(Qt.WindowType.FramelessWindowHint)
        widget.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        widget.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

    def _arrange(self, keys):
        """从拖动手柄右侧起把 keys 对应的按钮排成一行，其余按钮一律隐藏，工具栏宽度随之收缩"""
        for key, button in self._buttons.items():
            if key not in keys:
                button.hide()
        x = self.drag_handle.width()
        for key in keys:
            button = self._buttons[key]
            if button.parent() is not self:
                button.setParent(self)   # 刚从「…」弹层里挪回来
            width = self._button_widths[key]
            button.setGeometry(x, 0, width, self._btn_height)
            button.show()
            x += width
        self.resize(x, self._btn_height)

    def reload_layout(self):
        """按用户配置重排：始终显示的上工具栏，收进更多的留给弹层，「…」固定在最右"""
        layout = load_layout()
        self._folded_keys = [key for key, mode in layout if mode == MORE]
        self._arrange([key for key, mode in layout if mode == SHOW] + ["more"])

    def _show_more_popup(self):
        """展开「…」弹层，把收起的按钮摆进去。

        弹层用到才建：钉图工具栏的「…」永远不显示，没必要给每张钉图多建一个窗口。
        """
        if self._more_popup is None:
            self._more_popup = _MorePopup(self.parent())
            self._make_floating(self._more_popup)
            self._more_popup.adjust_btn.setText(self.tr("Adjust"))
            self._more_popup.adjust_btn.clicked.connect(self._open_layout_dialog)
            self._more_popup.hover_changed.connect(self._on_more_hover)
            self._more_close_timer = QTimer(self)
            self._more_close_timer.setSingleShot(True)
            self._more_close_timer.setInterval(300)
            self._more_close_timer.timeout.connect(self._hide_more_popup)

        self._more_close_timer.stop()
        popup = self._more_popup
        if popup.isVisible():
            return
        cell_width = max((self._button_widths[key] for key in self._folded_keys), default=0)
        popup.set_buttons([self._buttons[key] for key in self._folded_keys],
                          QSize(cell_width, self._btn_height))
        self._sync_panel_position(popup, align_right=True)
        popup.show()
        popup.raise_()

    def _on_more_hover(self, hovering):
        """鼠标在「…」或弹层上就展开；两者之间隔着一道缝，离开后稍等再收起，好让鼠标移过去"""
        if hovering:
            self._show_more_popup()
        elif self._more_popup is not None:
            self._more_close_timer.start()

    def _hide_more_popup(self):
        if self._more_popup is not None:
            self._more_close_timer.stop()
            self._more_popup.hide()

    def _open_layout_dialog(self):
        """「调整」：编辑排布，确认后保存并立即重排"""
        from .toolbar_layout_dialog import ToolbarLayoutDialog

        self._hide_more_popup()
        host = self._host_window()
        dialog = ToolbarLayoutDialog(
            load_layout(), {key: button.icon() for key, button in self._buttons.items()}, host)
        # 截图窗口横跨整个虚拟桌面，对话框默认居中到它的中点，多屏时未必落在用户
        # 正在操作的那块屏幕上；改为放到工具栏所在屏幕的中央
        screen = QApplication.screenAt(self.mapToGlobal(QPoint(0, 0))) or QApplication.primaryScreen()
        dialog.move(screen.availableGeometry().center() - dialog.rect().center())
        if not dialog.exec():
            return
        save_layout(dialog.entries())
        self.reload_layout()
        # 宽度变了，重新贴到选区右下角（手动拖过位置的不会动）
        if hasattr(host, "update_toolbar_position"):
            host.update_toolbar_position()
        
    def init_settings_panels(self):
        """初始化所有工具的设置面板"""
        from .paint_settings_panel import PaintSettingsPanel
        from .shape_settings_panel import ShapeSettingsPanel
        from .arrow_settings_panel import ArrowSettingsPanel
        from .number_settings_panel import NumberSettingsPanel
        from .text_settings_panel import TextSettingsPanel
        from .mosaic_settings_panel import MosaicSettingsPanel
        
        parent = self.parent()
        
        # === 1. 画笔类设置面板 (pen, highlighter) ===
        self.paint_panel = PaintSettingsPanel(parent)
        self._make_floating(self.paint_panel)
        
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
            log_debug(T("初始化线条样式失败: {exc}", exc=exc), "Toolbar")
        self.paint_panel.hide()
        
        # === 2. 形状类设置面板 (rect, ellipse) ===
        self.shape_panel = ShapeSettingsPanel(parent)
        self._make_floating(self.shape_panel)
        
        # 连接信号
        self.shape_panel.color_changed.connect(self._on_panel_color_changed)
        self.shape_panel.size_changed.connect(self._on_panel_size_changed)
        self.shape_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        self.shape_panel.line_style_changed.connect(self._on_line_style_changed)
        self.shape_panel.hide()
        
        # === 3. 箭头设置面板 (arrow) ===
        self.arrow_panel = ArrowSettingsPanel(parent)
        self._make_floating(self.arrow_panel)
        
        # 连接信号
        self.arrow_panel.color_changed.connect(self._on_panel_color_changed)
        self.arrow_panel.size_changed.connect(self._on_panel_size_changed)
        self.arrow_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        self.arrow_panel.arrow_style_changed.connect(self._on_arrow_style_changed)
        self.arrow_panel.hide()
        
        # === 4. 序号设置面板 (number) ===
        self.number_panel = NumberSettingsPanel(parent)
        # 面板的 Qt parent 是工具栏的 parent，不是工具栏本身；
        # 样式弹出层要靠它判断该往哪边弹才不会盖住一级/二级菜单。
        self.number_panel._owner_toolbar = self
        self._make_floating(self.number_panel)
        
        # 连接信号
        self.number_panel.color_changed.connect(self._on_panel_color_changed)
        self.number_panel.size_changed.connect(self._on_panel_size_changed)
        self.number_panel.opacity_changed.connect(self._on_panel_opacity_changed)
        if hasattr(self.number_panel, "next_number_changed"):
            self.number_panel.next_number_changed.connect(self._on_number_next_changed)
        if hasattr(self.number_panel, "style_changed"):
            self.number_panel.style_changed.connect(self._on_number_style_changed)
        self.number_panel.hide()
        
        # === 5. 文字设置面板 (text) ===
        self.text_panel = TextSettingsPanel(parent)
        self._make_floating(self.text_panel)
        
        # 连接信号
        self.text_panel.font_changed.connect(self._on_text_font_changed)
        self.text_panel.color_changed.connect(self._on_text_color_changed)
        if hasattr(self.text_panel, 'background_changed'):
            self.text_panel.background_changed.connect(self._on_text_background_changed)
        self.text_panel.hide()

        # === 6. 马赛克设置面板 (mosaic) ===
        self.mosaic_panel = MosaicSettingsPanel(parent)
        self._make_floating(self.mosaic_panel)

        # 连接信号
        self.mosaic_panel.size_changed.connect(self._on_panel_size_changed)
        self.mosaic_panel.draw_mode_changed.connect(self._on_mosaic_mode_changed)
        self.mosaic_panel.style_changed.connect(self._on_mosaic_style_changed)
        self.mosaic_panel.block_size_changed.connect(self._on_mosaic_block_size_changed)
        self.mosaic_panel.hide()

        # 加载保存的设置
        self._load_saved_settings()
        
        # 保持兼容性:paint_menu 和 text_menu 别名
        self.paint_menu = self.paint_panel
        self.text_menu = self.text_panel

    @safe_event
    def paintEvent(self, event):
        """手动绘制圆角白色背景 + 主题色描边，确保四角真正透明"""
        _paint_toolbar_frame(self)
        
    def _load_saved_settings(self):
        """把每个工具的持久化设置回填到它的二级面板。"""
        for tool_id in self.PANEL_TOOLS:
            self._sync_panel_from_settings(tool_id)

    def _sync_panel_from_settings(self, tool_id: str):
        """把某个工具的持久化设置回填到它的面板——「现在画会画出什么」。

        启动加载、切换工具、恢复工具态、单独弹面板，做的都是同一件事，所以只
        留这一份：调用方只决定"什么时候回填"，不再各自重复"回填什么"。

        不在这里写默认值：ToolSettings.get(key) 拿不到就自己回落到
        DEFAULT_SETTINGS，面板的 set_* 还会再 normalize 一次，调用方重复写一遍
        只会多出几份迟早对不上的副本。

        注意调用顺序：选中图元后要把面板改成"这一个图元"的状态，必须放在本函数
        之后，否则会被工具默认值盖掉。
        """
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            settings = manager.get_tool_settings(tool_id) if manager else None
            if not settings:
                return

            if tool_id == "text":
                # 文字面板自己有一份完整的读配置逻辑，别在这里再抄一遍
                self.text_panel.load_from_config()
            elif tool_id in ("pen", "highlighter"):
                self.paint_panel.line_style = settings.get("line_style")
                if tool_id == "highlighter":
                    self.paint_panel.set_highlighter_mode(settings.get("draw_mode"))
            elif tool_id in ("rect", "ellipse"):
                self.shape_panel.line_style = settings.get("line_style")
            elif tool_id == "arrow":
                self.arrow_panel.arrow_style = settings.get("arrow_style")
            elif tool_id == "number":
                self.number_panel.set_style(settings.get("style"))
            elif tool_id == "mosaic":
                self.mosaic_panel.set_draw_mode(settings.get("draw_mode"))
                self.mosaic_panel.set_style(settings.get("style"))
                self.mosaic_panel.set_block_size(settings.get("block_size"))
        except Exception as exc:
            log_debug(T("同步 {tool_id} 面板失败: {exc}", tool_id=tool_id, exc=exc), "Toolbar")

    def reset_session_state(self):
        """重置工具栏状态（新截图会话开始时调用）。"""
        self.set_temporary_edit_active(False)
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
        # 截图窗口复用同一个工具栏，排布可能在上次会话之后改过（比如在设置里重置）
        self.reload_layout()
        # 隐藏自身（选区确认后再显示）
        self.hide()

    def set_temporary_edit_active(self, active: bool):
        """设置仅作用于当前选中图元、不得保存新建默认值的临时编辑态。"""
        self.temporary_edit_active = bool(active)
        enabled = not self.temporary_edit_active
        if hasattr(self, "paint_panel") and hasattr(self.paint_panel, "mode_widget"):
            self.paint_panel.mode_widget.setEnabled(enabled)
        if hasattr(self, "number_panel"):
            for name in ("next_up_btn", "next_down_btn"):
                button = getattr(self.number_panel, name, None)
                if button is not None:
                    button.setEnabled(enabled)

    def _retranslate(self, _lang_code: str = None):
        """语言切换后刷新所有按钮提示与马赛克面板文本。"""
        for button, source in self._tooltip_sources.items():
            try:
                button.setToolTip(self.tr(source))
            except RuntimeError:
                continue
        # 马赛克面板的文本在构造时一次性设置，这里补一次刷新
        mosaic_panel = getattr(self, "mosaic_panel", None)
        if mosaic_panel is not None and hasattr(mosaic_panel, "retranslate"):
            mosaic_panel.retranslate()
        if self._more_popup is not None:
            self._more_popup.adjust_btn.setText(self.tr("Adjust"))

    def restore_active_tool_state(self, tool_id: str, ctx=None, scene=None):
        """Restore the active tool's complete default panel after selection ends."""
        self.set_temporary_edit_active(False)
        if tool_id == "cursor" or not tool_id:
            self._hide_all_panels()
            return

        if ctx is not None:
            color = getattr(ctx, "color", None)
            if color is not None:
                self.set_current_color(QColor(color))
            width = getattr(ctx, "stroke_width", None)
            if width is not None:
                self.set_stroke_width(int(round(width)))
            opacity = getattr(ctx, "opacity", None)
            if opacity is not None:
                self.set_opacity(int(round(float(opacity) * 255)))

        # 面板本身由 _show_panel_for_tool 统一按持久化设置回填；这里只补
        # 「不在设置里」的那部分：下一个序号是场景状态，不是工具默认值。
        if tool_id == "number" and scene is not None:
            try:
                from tools.number import NumberTool
                self.set_number_next_value(NumberTool.get_next_number(scene))
            except Exception as exc:
                log_debug(T("恢复序号预览失败: {exc}", exc=exc), "Toolbar")
        self._show_panel_for_tool(tool_id)
    
    def _on_tool_clicked(self, tool_id: str):
        """工具按钮点击 - 支持再次点击取消"""
        self.select_tool(tool_id, toggle=True)

    def select_tool(self, tool_id: str, *, toggle: bool = False):
        """Select a tool through the authoritative toolbar/UI signal path.

        Mouse buttons opt into toggle-to-cursor; keyboard shortcuts are
        idempotent and therefore leave an already active tool selected.
        """
        if tool_id == "cursor":
            self.set_temporary_edit_active(False)
            # 取消所有按钮选中
            for btn in self.tool_buttons.values():
                btn.setChecked(False)
            self.current_tool = None
            self.tool_changed.emit("cursor")
            self._hide_all_panels()
            return

        if tool_id not in self.tool_buttons:
            return
        if self.current_tool == tool_id:
            if toggle:
                self.select_tool("cursor")
            return

        self.set_temporary_edit_active(False)

        # 更新按钮状态
        for tid, btn in self.tool_buttons.items():
            btn.setChecked(tid == tool_id)

        self.current_tool = tool_id
        self.tool_changed.emit(tool_id)

        # 显示对应的设置面板（面板内容由 _show_panel_for_tool 按设置回填）
        self._show_panel_for_tool(tool_id)
            
    def _hide_all_panels(self):
        """隐藏所有设置面板"""
        if hasattr(self, 'paint_panel'): self.paint_panel.hide()
        if hasattr(self, 'shape_panel'): self.shape_panel.hide()
        if hasattr(self, 'arrow_panel'): self.arrow_panel.hide()
        if hasattr(self, 'number_panel'): self.number_panel.hide()
        if hasattr(self, 'text_panel'): self.text_panel.hide()
        if hasattr(self, 'mosaic_panel'): self.mosaic_panel.hide()

    def _show_panel_for_tool(self, tool_id: str):
        """显示指定工具的设置面板，内容一律回填成该工具的持久化设置。

        面板是共用的（画笔/荧光笔一个，矩形/椭圆一个），上一个工具或上一个被选
        中的图元留下的状态必须在这里被冲掉，否则面板显示的就不是"现在画会画出
        什么"。所以选中图元后的差异化同步必须排在本函数之后。
        """
        self._hide_all_panels()

        # 面板的形态（哪些控件该露出来）跟着工具走，与持久化设置无关
        if tool_id in ("pen", "highlighter", "spotlight") and hasattr(self, "paint_panel"):
            self.paint_panel.set_line_style_visible(tool_id == "pen")
            if hasattr(self.paint_panel, "set_highlighter_mode_visible"):
                self.paint_panel.set_highlighter_mode_visible(tool_id == "highlighter")
            # 聚光灯借用这个面板：透明度就是幕布的暗度；幕布固定黑色，孔没有线宽
            self.paint_panel.set_size_visible(tool_id != "spotlight")
            self.paint_panel.set_color_visible(tool_id != "spotlight")

        self._sync_panel_from_settings(tool_id)

        panel_map = {
            "pen": self.paint_panel,
            "highlighter": self.paint_panel,
            "spotlight": self.paint_panel,
            "rect": self.shape_panel,
            "ellipse": self.shape_panel,
            "arrow": self.arrow_panel,
            "number": self.number_panel,
            "text": self.text_panel,
            "mosaic": self.mosaic_panel,
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
        if self.temporary_edit_active:
            return
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            if manager:
                manager.update_settings("highlighter", draw_mode=mode)
        except Exception as exc:
            log_debug(T("保存荧光笔模式失败: {exc}", exc=exc), "Toolbar")

        # 刷新光标（矩形模式使用十字光标）
        try:
            cursor_manager = self._host_cursor_manager()
            if cursor_manager:
                cursor_manager.set_tool_cursor("highlighter", force=True)
        except Exception as e:
            log_exception(e, T("设置高亮笔光标"))

    def _host_window(self):
        """工具栏所属的宿主窗口。

        截图窗口里工具栏是它的子部件，所以就是 parent()；钉图工具栏是独立的
        顶层窗口，parent() 为 None，由 PinToolbar 覆盖本方法给出真正的宿主。
        """
        return self.parent()

    def _host_cursor_manager(self):
        """宿主画布的光标管理器；宿主还没建好或没有画布时返回 None。

        光标是"会画出什么"的预览，改了绘制设置就得刷新它。两个窗口找宿主的
        方式不同，这件事只在这里判断一次，各个 _on_*_changed 不再各写一遍
        （原来那两份都只认 parent()，钉图里静默失效）。
        """
        window = self._host_window()
        view = getattr(window, "view", None) if window else None
        return getattr(view, "cursor_manager", None) if view else None
    
    def _on_text_font_changed(self, font):
        """文字字体改变"""
        self.text_font_changed.emit(font)
        if self.temporary_edit_active:
            return
        from .text_settings_panel import TextSettingsPanel
        TextSettingsPanel.save_font_to_config(font)

    def _on_text_color_changed(self, color):
        """文字颜色改变"""
        self.current_color = color
        self.color_changed.emit(color)
        self.text_color_changed.emit(color)  # 发射文字专用颜色信号
        if self.temporary_edit_active:
            return
        # 保存颜色设置
        from settings import get_tool_settings_manager
        manager = get_tool_settings_manager()
        manager.update_settings("text", color=color.name())

    def _on_text_background_changed(self, enabled: bool, color: QColor, opacity: int):
        """文字背景改变"""
        self.text_background_changed.emit(enabled, color, opacity)
        if self.temporary_edit_active:
            return
        from .text_settings_panel import TextSettingsPanel
        TextSettingsPanel.save_background_to_config(enabled, color, opacity)

    def _on_arrow_style_changed(self, style: str):
        """箭头样式改变"""
        if not self.temporary_edit_active:
            # 保存箭头样式设置
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            manager.update_settings("arrow", arrow_style=style)
        # 发射信号，通知截图窗口/钉图窗口更新选中的箭头项
        self.arrow_style_changed.emit(style)
    
    def _on_line_style_changed(self, style: str):
        """线条样式改变"""
        if not self.temporary_edit_active:
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

    def _on_mosaic_mode_changed(self, mode: str):
        if self.temporary_edit_active:
            return
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            if manager:
                manager.update_settings("mosaic", draw_mode=mode)
        except Exception as exc:
            log_debug(T("保存马赛克模式失败: {exc}", exc=exc), "Toolbar")

        # 刷新光标（框选模式使用十字光标）
        try:
            cursor_manager = self._host_cursor_manager()
            if cursor_manager:
                cursor_manager.set_tool_cursor("mosaic", force=True)
        except Exception as e:
            log_exception(e, T("设置马赛克光标"))

    def _on_mosaic_style_changed(self, style: str):
        if not self.temporary_edit_active:
            try:
                from settings import get_tool_settings_manager
                manager = get_tool_settings_manager()
                if manager:
                    manager.update_settings("mosaic", style=style)
            except Exception as exc:
                log_debug(T("保存马赛克种类失败: {exc}", exc=exc), "Toolbar")
        # 转发给窗口：如果当前选中的正好是一块框选马赛克，顺带把它也切了。
        self.mosaic_style_changed.emit(style)

    def _on_mosaic_block_size_changed(self, value: int):
        if not self.temporary_edit_active:
            try:
                from settings import get_tool_settings_manager
                manager = get_tool_settings_manager()
                if manager:
                    manager.update_settings("mosaic", block_size=value)
            except Exception as exc:
                log_debug(T("保存马赛克粒度失败: {exc}", exc=exc), "Toolbar")
        # 转发给窗口：如果当前选中的正好是一块马赛克，顺带把它也重新收缩。
        self.mosaic_block_size_changed.emit(value)

    def _on_number_next_changed(self, value: int):
        """序号工具下一数字改变"""
        if self.temporary_edit_active:
            return
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
        if hasattr(self, 'mosaic_panel'):
            self.mosaic_panel.set_size(width)

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

    def _on_number_style_changed(self, style: str):
        """转发给窗口统一处理：落到选中的序号 + 存设置 + 刷新光标。"""
        self.number_style_changed.emit(style)

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
        for attr in ('paint_panel', 'shape_panel', 'arrow_panel', 'number_panel', 'text_panel', 'mosaic_panel'):
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
        """drag_handle 的鼠标事件统一转发给 Toolbar 处理（双击穿透）；「…」按钮悬停时展开弹层"""
        from PySide6.QtCore import QEvent
        if obj is self._buttons.get("more") and event.type() in (QEvent.Type.Enter, QEvent.Type.Leave):
            self._on_more_hover(event.type() == QEvent.Type.Enter)
            return False
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

    @safe_event
    def hideEvent(self, event):
        """工具栏隐藏时收起「…」弹层（截图里拖动选区、开新会话都会先隐藏工具栏）"""
        super().hideEvent(event)
        self._hide_more_popup()
    
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
        if hasattr(self, 'mosaic_panel') and self.mosaic_panel.isVisible():
            self._sync_panel_position(self.mosaic_panel)
        popup = getattr(self, '_more_popup', None)
        if popup is not None and popup.isVisible():
            self._sync_panel_position(popup, align_right=True)

    def _sync_panel_position(self, panel, align_right=False):
        """同步单个面板的位置
        
        优先跟一级工具栏同方向弹出（远离选区），空间不够时翻到另一侧。
        X 轴与工具栏左对齐；「…」弹层挂在工具栏最右端，align_right 时改为右对齐。
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
        
        # X 轴：与工具栏左对齐（或右对齐），夹紧在屏幕内
        panel_x = toolbar_global_pos.x()
        if align_right:
            panel_x += self.width() - panel.width()
        if panel_x + panel.width() > screen_rect.right():
            panel_x = screen_rect.right() - panel.width() - 5
        if panel_x < screen_rect.left():
            panel_x = screen_rect.left() + 5
        
        final_pos = QPoint(panel_x, panel_y)
        if panel.parent():
            final_pos = panel.parent().mapFromGlobal(final_pos)
        
        panel.move(final_pos)
