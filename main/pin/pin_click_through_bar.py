"""
钉图鼠标穿透工具条

穿透时钉图本身收不到鼠标，移动、调不透明度和退出穿透都靠这条贴在钉图上方的小窗。
它是钉图的子窗口，系统层面归钉图所有，截图时随钉图一起降到截图层下面。
"""

from PySide6.QtCore import Qt, QSize
from PySide6.QtWidgets import QApplication, QHBoxLayout, QLabel, QPushButton, QSlider, QWidget

from core import safe_event
from core.resource_manager import ResourceManager
from core.theme import get_theme
from core.ui_scale import get_ui_scale, scaled
from core.ui_theme import set_own_style
from ui.base_settings_panel import CONTROL_INK, CONTROL_SUB, paint_rounded_panel


class _MoveHandle(QLabel):
    """按住拖动钉图，复用钉图自己的拖动逻辑（锁定时同样拖不动）。"""

    def __init__(self, bar):
        super().__init__(bar)
        self._pin = bar.pin_window
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)

    @safe_event
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._pin.start_window_drag(event.globalPosition().toPoint())
            event.accept()

    @safe_event
    def mouseMoveEvent(self, event):
        self._pin.update_window_drag(event.globalPosition().toPoint())
        event.accept()

    @safe_event
    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._pin.end_window_drag()
            event.accept()


class PinClickThroughBar(QWidget):

    BASE_HEIGHT = 34
    BASE_ICON = 16
    BASE_SLIDER_WIDTH = 96
    MIN_OPACITY = 15   # 和滚轮调透明度的下限一致

    def __init__(self, pin_window):
        super().__init__(
            pin_window,
            Qt.WindowType.Tool
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.NoDropShadowWindowHint,
        )
        self.pin_window = pin_window
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        self._handle = _MoveHandle(self)
        self._handle.setToolTip(self.tr("Drag to move"))

        self._title = QLabel(self.tr("Opacity"), self)

        self._slider = QSlider(Qt.Orientation.Horizontal, self)
        self._slider.setRange(self.MIN_OPACITY, 100)
        self._slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._slider.valueChanged.connect(self._on_slider_changed)

        self._value = QLabel(self)
        self._value.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        self._exit_button = QPushButton(self)
        self._exit_button.setIcon(
            ResourceManager.get_icon(ResourceManager.get_resource_path("svg/fluent_icons/Close.svg"))
        )
        self._exit_button.setToolTip(self.tr("Exit click-through"))
        self._exit_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._exit_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self._exit_button.clicked.connect(lambda: self.pin_window.set_click_through(False))

        layout = QHBoxLayout(self)
        layout.addWidget(self._handle)
        layout.addWidget(self._title)
        layout.addWidget(self._slider)
        layout.addWidget(self._value)
        layout.addWidget(self._exit_button)
        self._layout = layout

        self.apply_scale()
        get_ui_scale().scale_changed.connect(self.apply_scale)

    def apply_scale(self):
        height = scaled(self.BASE_HEIGHT)
        icon = scaled(self.BASE_ICON)
        self._layout.setContentsMargins(scaled(4), 0, scaled(4), 0)
        self._layout.setSpacing(scaled(6))
        self._handle.setFixedSize(height - scaled(6), height - scaled(6))
        self._handle.setPixmap(
            ResourceManager.get_icon(ResourceManager.get_resource_path("svg/移动窗口.svg"))
            .pixmap(QSize(icon, icon), self.devicePixelRatioF())
        )
        self._exit_button.setFixedSize(height - scaled(6), height - scaled(6))
        self._exit_button.setIconSize(QSize(scaled(12), scaled(12)))
        self._slider.setFixedWidth(scaled(self.BASE_SLIDER_WIDTH))
        self._value.setFixedWidth(scaled(34))

        theme = get_theme().theme_color_hex
        self._slider.setStyleSheet(f"""
            QSlider::groove:horizontal {{
                height: {scaled(4)}px; background: #d9dce3; border-radius: {scaled(2)}px;
            }}
            QSlider::handle:horizontal {{
                width: {scaled(12)}px; height: {scaled(12)}px; margin: {-scaled(4)}px 0;
                background: {theme}; border-radius: {scaled(6)}px;
            }}
            QSlider::sub-page:horizontal {{
                background: {theme}; border-radius: {scaled(2)}px;
            }}
        """)
        text_style = f"font-size: {scaled(12)}px; background: transparent;"
        set_own_style(self._title, f"color: {CONTROL_INK.name()}; {text_style}")
        set_own_style(self._value, f"color: {CONTROL_SUB.name()}; {text_style}")
        self._exit_button.setStyleSheet(f"""
            QPushButton {{ background: transparent; border: none; border-radius: {scaled(6)}px; }}
            QPushButton:hover {{ background: rgba(231, 76, 60, 40); }}
            QPushButton:pressed {{ background: rgba(231, 76, 60, 80); }}
        """)
        self.setFixedHeight(height)
        self.adjustSize()
        if self.isVisible():
            self.follow_pin()

    def set_opacity(self, opacity: float):
        """同步滑块位置，不回写钉图。"""
        value = round(opacity * 100)
        self._slider.blockSignals(True)
        self._slider.setValue(value)
        self._slider.blockSignals(False)
        self._value.setText(f"{self._slider.value()}%")

    def _on_slider_changed(self, value: int):
        self._value.setText(f"{value}%")
        self.pin_window.set_click_through_opacity(value / 100)

    def follow_pin(self):
        """贴在钉图左上角的上方；上方放不下改到下方，都放不下就压在钉图内侧。"""
        pin_geometry = self.pin_window.frameGeometry()
        screen = self.pin_window.screen() or QApplication.primaryScreen()
        area = screen.geometry()
        gap = scaled(4)
        y = pin_geometry.top() - self.height() - gap
        if y < area.top():
            y = pin_geometry.bottom() + 1 + gap
            if y + self.height() > area.bottom():
                y = pin_geometry.top() + gap
        x = max(area.left(), min(pin_geometry.left(), area.right() - self.width()))
        self.move(x, y)

    def show(self):
        self.follow_pin()
        super().show()
        self.raise_()

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)
