"""
scroll_toolbar.py - 滚动截图浮动工具栏模块

提供滚动截图窗口使用的可拖动浮动工具栏及其辅助部件。

主要类:
- _DragHandle     : 工具栏左端拖动手柄（主题色圆角竖条，支持手动模式圆点 + 双击复位信号）
- FloatingToolbar : 可拖动的浮动工具栏，包含方向切换、手动截图、钉图、完成、取消等按钮
"""

from PySide6.QtWidgets import QWidget, QPushButton, QVBoxLayout, QHBoxLayout
from PySide6.QtCore import Qt, QRect, QPoint, Signal
from PySide6.QtGui import QPainter, QColor, QPainterPath
from core.theme import get_theme
from core import safe_event


class _DragHandle(QWidget):
    """工具栏左端拖动手柄 —— 主题色圆角竖条

    两种视觉状态:
    - 自动定位模式: 纯色填充
    - 手动定位模式: 纯色填充 + 三个白色圆点（提示可双击复位）
    """

    _DOT_COLOR = QColor(255, 255, 255)
    reset_requested = Signal()  # 双击时发出，请求切回自动定位

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedWidth(14)
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self._manual_mode = False

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

        if self._manual_mode:
            cx = r.center().x()
            cy = r.center().y()
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(self._DOT_COLOR)
            dot_r, gap = 3, 9
            for dy in (-gap, 0, gap):
                painter.drawEllipse(QPoint(cx, cy + dy), dot_r, dot_r)

        painter.end()


class FloatingToolbar(QWidget):
    """可拖动的浮动工具栏窗口"""

    # 信号定义
    direction_changed = Signal()
    manual_capture = Signal()
    pin_clicked = Signal()   # 钉图信号
    finish_clicked = Signal()
    cancel_clicked = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.parent_window = parent

        # 拖动相关
        self._dragging = False
        self._drag_offset = QPoint()
        self._manual_positioned = False  # 用户手动拖动后为 True，阻止自动定位

        self._setup_toolbar_window()
        self._setup_toolbar_ui()

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

        self.setFixedHeight(40)
        self.setMinimumWidth(200)

    def _setup_toolbar_ui(self):
        """设置工具栏 UI"""
        theme_hex = get_theme().theme_color_hex
        container = QWidget()
        container.setObjectName("toolbar_container")
        container.setStyleSheet(f"""
            QWidget#toolbar_container {{
                background-color: white;
                border: 2px solid {theme_hex};
                border-radius: 5px;
            }}
        """)

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(container)

        toolbar_layout = QHBoxLayout(container)
        toolbar_layout.setContentsMargins(0, 0, 10, 0)
        toolbar_layout.setSpacing(8)

        # 左侧拖动手柄
        left_handle = _DragHandle(container)
        left_handle.setToolTip(self.tr("Drag to move"))
        left_handle.installEventFilter(self)
        left_handle.reset_requested.connect(self._reset_auto_position)
        toolbar_layout.addWidget(left_handle)
        self.left_handle = left_handle

        # 方向切换按钮
        self.direction_btn = QPushButton("↕️ " + self.tr("Vertical"))
        self.direction_btn.setStyleSheet("""
            QPushButton {
                background-color: #2196F3;
                color: white;
                border: none;
                padding: 5px 10px;
                font-size: 9pt;
                border-radius: 3px;
                font-weight: bold;
                min-width: 50px;
            }
            QPushButton:hover {
                background-color: #1976D2;
            }
        """)
        self.direction_btn.clicked.connect(self.direction_changed.emit)
        toolbar_layout.addWidget(self.direction_btn, 0, Qt.AlignmentFlag.AlignVCenter)

        # 手动截图按钮（SVG 图标 - 相机样式）
        from core.resource_manager import ResourceManager
        from PySide6.QtGui import QIcon
        from PySide6.QtCore import QSize

        self.manual_capture_btn = QPushButton()
        self.manual_capture_btn.setIcon(QIcon(ResourceManager.get_resource_path("svg/托盘.svg")))
        self.manual_capture_btn.setIconSize(QSize(24, 24))
        self.manual_capture_btn.setFixedSize(32, 32)
        self.manual_capture_btn.setToolTip(self.tr("Take screenshot manually"))
        self.manual_capture_btn.setStyleSheet(self._icon_btn_style())
        self.manual_capture_btn.clicked.connect(self.manual_capture.emit)
        toolbar_layout.addWidget(self.manual_capture_btn, 0, Qt.AlignmentFlag.AlignVCenter)

        # 钉图按钮
        self.pin_btn = QPushButton()
        self.pin_btn.setIcon(QIcon(ResourceManager.get_resource_path("svg/钉图.svg")))
        self.pin_btn.setIconSize(QSize(24, 24))
        self.pin_btn.setFixedSize(32, 32)
        self.pin_btn.setToolTip(self.tr("Pin to desktop"))
        self.pin_btn.setStyleSheet(self._icon_btn_style())
        self.pin_btn.clicked.connect(self.pin_clicked.emit)
        toolbar_layout.addWidget(self.pin_btn, 0, Qt.AlignmentFlag.AlignVCenter)

        # 完成按钮
        self.finish_btn = QPushButton()
        self.finish_btn.setIcon(QIcon(ResourceManager.get_resource_path("svg/确定.svg")))
        self.finish_btn.setIconSize(QSize(24, 24))
        self.finish_btn.setFixedSize(32, 32)
        self.finish_btn.setToolTip(self.tr("Finish and save"))
        self.finish_btn.setStyleSheet(self._icon_btn_style())
        self.finish_btn.clicked.connect(self.finish_clicked.emit)
        toolbar_layout.addWidget(self.finish_btn, 0, Qt.AlignmentFlag.AlignVCenter)

        # 取消按钮
        self.cancel_btn = QPushButton()
        self.cancel_btn.setIcon(QIcon(ResourceManager.get_resource_path("svg/关闭.svg")))
        self.cancel_btn.setIconSize(QSize(24, 24))
        self.cancel_btn.setFixedSize(32, 32)
        self.cancel_btn.setToolTip(self.tr("Cancel long screenshot"))
        self.cancel_btn.setStyleSheet(self._icon_btn_style())
        self.cancel_btn.clicked.connect(self.cancel_clicked.emit)
        toolbar_layout.addWidget(self.cancel_btn, 0, Qt.AlignmentFlag.AlignVCenter)

    @staticmethod
    def _icon_btn_style() -> str:
        """通用图标按钮样式"""
        return """
            QPushButton {
                background-color: transparent;
                border: none;
                border-radius: 3px;
            }
            QPushButton:hover {
                background-color: rgba(0, 0, 0, 0.05);
            }
            QPushButton:pressed {
                background-color: rgba(0, 0, 0, 0.1);
            }
        """

    # ------------------------------------------------------------------
    # 公开方法
    # ------------------------------------------------------------------

    def update_direction(self, direction: str):
        """更新方向按钮文字显示"""
        if direction == "horizontal":
            self.direction_btn.setText("↔️ " + self.tr("Horizontal"))
        else:
            self.direction_btn.setText("↕️ " + self.tr("Vertical"))

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
 