"""
scroll_toolbar.py - 滚动截图浮动工具栏模块

提供滚动截图窗口使用的可拖动浮动工具栏及其辅助部件。

主要类:
- _DragHandle     : 工具栏左端拖动手柄（竖排灰点，手动模式加深 + 双击复位信号）
- _Separator      : 按钮分组之间的细竖线
- FloatingToolbar : 可拖动的浮动工具栏：长图尺寸 | 方向、自动滚动、手动截图、裁剪 | 钉图、完成、取消
"""

from PySide6.QtWidgets import QWidget, QPushButton, QVBoxLayout, QHBoxLayout, QMenu, QLabel
from PySide6.QtCore import Qt, QPoint, QSize, QTimer, Signal
from PySide6.QtGui import QPainter, QColor
from core.theme import get_theme
from core.ui_scale import get_ui_scale, scaled
from core import safe_event
from core.platform_utils import set_window_rounded_corners
from core.resource_manager import ResourceManager
from core.ui_theme import set_own_style


class _DragHandle(QWidget):
    """工具栏左端拖动手柄 —— 竖排灰点，不填主题色

    两种视觉状态:
    - 自动定位模式: 浅灰圆点
    - 手动定位模式: 深灰圆点（提示可双击复位）
    """

    _DOT_COLOR = QColor(0x5F, 0x63, 0x68)
    _DOT_COLOR_IDLE = QColor(0xB4, 0xB8, 0xBE)
    BASE_WIDTH = 14
    BASE_DOT_RADIUS = 2
    BASE_DOT_GAP = 7

    reset_requested = Signal()  # 双击时发出，请求切回自动定位

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self._manual_mode = False
        self.apply_scale()

    def apply_scale(self):
        self.setFixedWidth(scaled(self.BASE_WIDTH))
        self.update()

    def set_manual_mode(self, manual: bool):
        if self._manual_mode != manual:
            self._manual_mode = manual
            self.update()

    @safe_event
    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self._manual_mode:
            self.reset_requested.emit()
        super().mouseDoubleClickEvent(event)

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = self.rect()
        cx = r.center().x()
        cy = r.center().y()
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self._DOT_COLOR if self._manual_mode else self._DOT_COLOR_IDLE)
        dot_r, gap = scaled(self.BASE_DOT_RADIUS), scaled(self.BASE_DOT_GAP)
        for dy in (-gap, 0, gap):
            painter.drawEllipse(QPoint(cx, cy + dy), dot_r, dot_r)

        painter.end()


class _Separator(QWidget):
    """按钮分组之间的细竖线。"""

    _COLOR = QColor(0xE3, 0xE5, 0xE8)
    BASE_HEIGHT = 20

    def apply_scale(self):
        self.setFixedSize(max(1, scaled(1)), scaled(self.BASE_HEIGHT))

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.fillRect(self.rect(), self._COLOR)
        painter.end()


def _icon(name: str):
    return ResourceManager.get_icon(ResourceManager.get_resource_path(f"svg/{name}"))


class FloatingToolbar(QWidget):
    """可拖动的浮动工具栏窗口"""

    # 信号定义
    direction_changed = Signal()
    auto_scroll_clicked = Signal()
    manual_capture = Signal()
    crop_requested = Signal(str)  # "top" 去掉当前画面之前的内容，"bottom" 去掉之后的内容
    pin_clicked = Signal()   # 钉图信号
    finish_clicked = Signal()
    cancel_clicked = Signal()

    # 基准尺寸（100% 下的实际像素）
    BASE_HEIGHT = 40
    BASE_MIN_WIDTH = 200
    BASE_BTN = 32
    BASE_ICON = 24

    def __init__(self, parent=None):
        super().__init__(parent)
        self.parent_window = parent

        # 拖动相关
        self._dragging = False
        self._drag_offset = QPoint()
        self._manual_positioned = False  # 用户手动拖动后为 True，阻止自动定位
        self._direction = "vertical"

        self._setup_toolbar_window()
        self._setup_toolbar_ui()
        # 改比例后自行重算尺寸（连接随本部件销毁自动断开）
        get_ui_scale().scale_changed.connect(self.apply_scale)

    # ------------------------------------------------------------------
    # 窗口与 UI 初始化
    # ------------------------------------------------------------------

    def _setup_toolbar_window(self):
        """设置工具栏窗口属性"""
        self.setWindowFlags(
            Qt.WindowType.WindowStaysOnTopHint |
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

    @safe_event
    def showEvent(self, event):
        super().showEvent(event)
        # 隐藏再显示后系统会恢复阴影，所以每次显示都要关；原生窗口显示出来之后设才生效
        QTimer.singleShot(0, self._drop_system_shadow)

    def _drop_system_shadow(self):
        """工具栏紧挨着截图区，系统给窗口加的阴影会被截进长图顶部。关掉圆角时阴影随之去掉。"""
        set_window_rounded_corners(int(self.winId()), False)

    def apply_scale(self):
        """按当前比例重算浮动工具栏的尺寸，方向、按钮状态都不动。"""
        self.setFixedHeight(scaled(self.BASE_HEIGHT))
        self.setMinimumWidth(scaled(self.BASE_MIN_WIDTH))
        self._container.setStyleSheet(self._container_qss())
        self._row_layout.setContentsMargins(0, 0, scaled(10), 0)
        self._row_layout.setSpacing(scaled(6))
        self.left_handle.apply_scale()
        for separator in self._separators:
            separator.apply_scale()
        set_own_style(self.size_label, f"color: #5F6368; font-size: {scaled(9)}pt;")
        self.size_label.ensurePolished()
        # 按最长的尺寸定宽，数字变长时工具栏不跟着变宽、挪位置
        self.size_label.setFixedWidth(self.size_label.fontMetrics().horizontalAdvance("88888 × 88888") + scaled(8))
        self.direction_btn.setStyleSheet(self._direction_btn_style())
        self.direction_btn.setIconSize(QSize(scaled(14), scaled(14)))
        btn_sz = scaled(self.BASE_BTN)
        icon_sz = scaled(self.BASE_ICON)
        for button in self._icon_buttons:
            button.setFixedSize(btn_sz, btn_sz)
            button.setIconSize(QSize(icon_sz, icon_sz))
            button.setStyleSheet(self._icon_btn_style())
        self.adjustSize()
        # 宽高变了要重新贴回滚动窗口旁边；用户手动拖过位置的不动
        if (not self._manual_positioned and self.parent_window is not None
                and hasattr(self.parent_window, '_position_floating_toolbar')):
            self.parent_window._position_floating_toolbar()

    def _container_qss(self) -> str:
        theme_hex = get_theme().theme_color_hex
        return f"""
            QWidget#toolbar_container {{
                background-color: white;
                border: 2px solid {theme_hex};
                border-radius: {scaled(5)}px;
            }}
        """

    def _setup_toolbar_ui(self):
        """设置工具栏 UI"""
        container = QWidget()
        container.setObjectName("toolbar_container")
        self._container = container

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(container)

        toolbar_layout = QHBoxLayout(container)
        self._row_layout = toolbar_layout
        self._icon_buttons = []

        # 左侧拖动手柄
        left_handle = _DragHandle(container)
        left_handle.setToolTip(self.tr("Drag to move"))
        left_handle.installEventFilter(self)
        left_handle.reset_requested.connect(self._reset_auto_position)
        toolbar_layout.addWidget(left_handle)
        self.left_handle = left_handle

        self._separators = []

        # 长图尺寸：每拼一帧、每次裁剪都会更新
        self.size_label = QLabel()
        self.size_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.size_label.setToolTip(self.tr("Size of the long screenshot"))
        toolbar_layout.addWidget(self.size_label, 0, Qt.AlignmentFlag.AlignVCenter)
        self._add_separator(toolbar_layout)

        # 方向切换按钮
        self.direction_btn = QPushButton()
        self.direction_btn.setToolTip(self.tr("Switch scroll direction"))
        self.direction_btn.clicked.connect(self.direction_changed.emit)
        toolbar_layout.addWidget(self.direction_btn, 0, Qt.AlignmentFlag.AlignVCenter)
        self.update_direction("vertical")

        # 自动滚动按钮：滚动期间保持按下状态
        self.auto_scroll_btn = self._add_icon_button(
            toolbar_layout, "自动滚动.svg", self.tr("Auto scroll (move the mouse to stop)"), self.auto_scroll_clicked.emit)
        self.auto_scroll_btn.setCheckable(True)
        self.manual_capture_btn = self._add_icon_button(
            toolbar_layout, "托盘.svg", self.tr("Take screenshot manually"), self.manual_capture.emit)
        # 裁剪按钮：把长图裁到当前画面（预览里的绿框）
        self.crop_btn = self._add_icon_button(toolbar_layout, "裁剪.svg", self.tr("Crop"), self._show_crop_menu)
        self._add_separator(toolbar_layout)

        self.pin_btn = self._add_icon_button(toolbar_layout, "钉图.svg", self.tr("Pin to desktop"), self.pin_clicked.emit)
        self.finish_btn = self._add_icon_button(
            toolbar_layout, "确定.svg", self.tr("Finish and save"), self.finish_clicked.emit)
        self.cancel_btn = self._add_icon_button(
            toolbar_layout, "关闭.svg", self.tr("Cancel long screenshot"), self.cancel_clicked.emit)

        self.apply_scale()

    def _add_icon_button(self, layout, icon: str, tooltip: str, slot) -> QPushButton:
        button = QPushButton()
        button.setIcon(_icon(icon))
        button.setToolTip(tooltip)
        button.clicked.connect(slot)
        layout.addWidget(button, 0, Qt.AlignmentFlag.AlignVCenter)
        self._icon_buttons.append(button)
        return button

    def _add_separator(self, layout):
        separator = _Separator()
        layout.addWidget(separator, 0, Qt.AlignmentFlag.AlignVCenter)
        self._separators.append(separator)

    @staticmethod
    def _direction_btn_style() -> str:
        """方向切换按钮样式。

        字号用 pt 而不是 px：pt 跟随系统 DPI，本项目关掉了 Qt 的高 DPI 缩放，
        换成 px 会让这个按钮在高分屏上比原来小一圈。
        """
        return f"""
            QPushButton {{
                background-color: #F1F3F4;
                color: #202124;
                border: none;
                padding: {scaled(4)}px {scaled(10)}px;
                font-size: {scaled(9)}pt;
                border-radius: {scaled(13)}px;
                min-width: {scaled(50)}px;
            }}
            QPushButton:hover {{
                background-color: #E3E5E8;
            }}
        """

    @staticmethod
    def _icon_btn_style() -> str:
        """通用图标按钮样式"""
        return f"""
            QPushButton {{
                background-color: transparent;
                border: none;
                border-radius: {scaled(3)}px;
            }}
            QPushButton:hover {{
                background-color: rgba(0, 0, 0, 0.05);
            }}
            QPushButton:pressed {{
                background-color: rgba(0, 0, 0, 0.1);
            }}
            QPushButton:checked {{
                background-color: rgba(33, 150, 243, 0.18);
            }}
        """

    # ------------------------------------------------------------------
    # 公开方法
    # ------------------------------------------------------------------

    def update_direction(self, direction: str):
        """更新方向按钮的图标和文字"""
        self._direction = direction
        if direction == "horizontal":
            self.direction_btn.setIcon(_icon("横向.svg"))
            self.direction_btn.setText(self.tr("Horizontal"))
        else:
            self.direction_btn.setIcon(_icon("竖向.svg"))
            self.direction_btn.setText(self.tr("Vertical"))

    def set_result_size(self, width: int, height: int):
        self.size_label.setText(f"{width} × {height}")

    def set_auto_scrolling(self, running: bool):
        """自动滚动按钮的按下状态跟随实际状态（点击时 Qt 会先自行切换一次）。"""
        self.auto_scroll_btn.setChecked(running)

    def _crop_menu(self) -> QMenu:
        menu = QMenu(self)
        if self._direction == "horizontal":
            before = self.tr("Crop left: remove everything left of the current view")
            after = self.tr("Crop right: remove everything right of the current view")
        else:
            before = self.tr("Crop top: remove everything above the current view")
            after = self.tr("Crop bottom: remove everything below the current view")
        menu.addAction(before, lambda: self.crop_requested.emit("top"))
        menu.addAction(after, lambda: self.crop_requested.emit("bottom"))
        return menu

    def _show_crop_menu(self):
        self._crop_menu().exec(self.crop_btn.mapToGlobal(QPoint(0, self.crop_btn.height())))

    # ------------------------------------------------------------------
    # 鼠标事件 —— 拖动 & 调整大小
    # ------------------------------------------------------------------

    # ------------------------------------------------------------------
    # 手动/自动定位状态
    # ------------------------------------------------------------------

    def _set_manual_positioned(self, manual: bool):
        """统一设置手动/自动定位状态，同步手柄视觉"""
        self._manual_positioned = manual
        self.left_handle.set_manual_mode(manual)

    def _reset_auto_position(self):
        """双击手柄 → 切回自动定位并立即重新定位到截图区域附近"""
        self._set_manual_positioned(False)
        if self.parent_window and hasattr(self.parent_window, '_position_floating_toolbar'):
            self.parent_window._position_floating_toolbar()

    # ------------------------------------------------------------------
    # 拖动事件
    # ------------------------------------------------------------------

    @safe_event
    def eventFilter(self, obj, event):
        """将 left_handle 的鼠标事件统一转发给工具栏处理（双击穿透）"""
        from PySide6.QtCore import QEvent
        if obj is self.left_handle:
            etype = event.type()
            # 双击事件不拦截，让 _DragHandle.mouseDoubleClickEvent 自行处理
            if etype == QEvent.Type.MouseButtonDblClick:
                return False
            if etype == QEvent.Type.MouseButtonPress:
                if event.button() == Qt.MouseButton.LeftButton:
                    self._dragging = True
                    self._drag_offset = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
                    self.left_handle.setCursor(Qt.CursorShape.ClosedHandCursor)
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
                    self.left_handle.setCursor(Qt.CursorShape.SizeAllCursor)
                return True
        return super().eventFilter(obj, event)
 