"""快速截图透明浮层：复用普通截图的选框和尺寸信息，不画放大镜。"""

import ctypes
from ctypes import wintypes

from PySide6.QtCore import QPoint, QRect, QRectF, QSizeF, Qt
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QWidget

from canvas.items.selection_item import SelectionItem
from canvas.selection_model import SelectionModel
from core.platform_utils import set_window_exclude_from_capture
from settings import get_tool_settings_manager
from ui.selection_info.panel import SelectionInfoPanel
from ui.selection_overlay import SelectionOverlayWidget


def _disable_native_frame(hwnd):
    """关掉 DWM 给窗口加的边框、圆角和阴影，浮层上只剩选区本身。"""
    if QGuiApplication.platformName() != "windows":
        return
    try:
        set_attribute = ctypes.WinDLL("dwmapi").DwmSetWindowAttribute
        set_attribute.argtypes = [wintypes.HWND, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        set_attribute.restype = ctypes.c_long
        for attribute, value in (
            (2, 1),            # DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED
            (33, 1),           # DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND
            (34, 0xFFFFFFFE),  # DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE
        ):
            native_value = wintypes.DWORD(value)
            # Windows 10 不支持后两项，会返回 HRESULT 失败；第一项仍然生效。
            set_attribute(hwnd, attribute, ctypes.byref(native_value), ctypes.sizeof(native_value))
    except (AttributeError, OSError):
        # 非 DWM 环境仍然使用 Qt 的无边框/无阴影窗口标志。
        pass


def selection_rect(start: QPoint, end: QPoint, bounds: QRect) -> QRect:
    """物理像素端点转选区；终点不包含在宽高内，不使用 QRect 的双点构造。"""
    rect = QRect(
        min(start.x(), end.x()), min(start.y(), end.y()),
        abs(end.x() - start.x()), abs(end.y() - start.y()),
    )
    return rect.intersected(bounds)


class _CaptureView:
    """尺寸信息面板只用到坐标换算，不创建画布。"""

    def __init__(self, window):
        self.window = window

    def viewport(self):
        return self.window

    def mapFromScene(self, point):
        return self.window.mapFromGlobal(point.toPoint())


class QuickCaptureOverlay(QWidget):
    """实时桌面保持透明，所有可见装饰直接使用普通截图的部件。"""

    def __init__(self, config_manager=None):
        super().__init__()
        self.config_manager = config_manager or get_tool_settings_manager()
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.WindowTransparentForInput
            | Qt.WindowType.WindowDoesNotAcceptFocus
            | Qt.WindowType.NoDropShadowWindowHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAutoFillBackground(False)
        self._selection_rect = QRect()
        self._capture_exclusion_requested = False
        self.capture_excluded = False
        self._session_active = False

        self.model = SelectionModel()
        self.model.setParent(self)
        self.model.min_size = QSizeF(0, 0)
        self.selection_item = SelectionItem(self.model)
        self.selection_overlay = SelectionOverlayWidget(self, self.selection_item, self.model)
        self.view = _CaptureView(self)
        self._bounds = QRectF()
        self.info_panel = SelectionInfoPanel(self, self.view)

    def show_selection(self, start: QPoint, end: QPoint, bounds: QRect):
        """根据绝对物理坐标更新原有截图部件，不绘制底图或灰色遮罩。"""
        if bounds.isEmpty():
            self.hide()
            return
        new_session = not self._session_active
        if new_session:
            self._session_active = True
            self._hide_info = self.config_manager.get_app_setting("screenshot_info_hide_on_drag", False)
            self.info_panel.set_confirmed(False)
            self.info_panel.apply_scale()
            self.model.activate()
            self.model.start_dragging()

        scene_bounds = QRectF(bounds)
        bounds_changed = scene_bounds != self._bounds
        if bounds_changed:
            self.setGeometry(bounds)
            self._bounds = scene_bounds
            self.selection_overlay.setGeometry(self.rect())
        if new_session:
            self.selection_overlay.show()

        rect = selection_rect(start, end, bounds)
        rect_changed = rect != self._selection_rect
        if new_session or rect_changed:
            self._selection_rect = rect
            self.model.set_rect(QRectF(rect))
        if bounds_changed:
            # 桌面原点改变时，即使绝对选区未变，其本地绘制位置也已改变。
            self.selection_overlay.refresh()
        if rect.isEmpty():
            if self.info_panel.isVisible():
                self.info_panel.hide()
        elif not self._hide_info:
            if new_session or rect_changed:
                # HTML 文本和 adjustSize 只在选区变化时更新，避免静止时反复布局。
                self.info_panel.update_info_text(QRectF(rect))
            if new_session or rect_changed or bounds_changed:
                self.info_panel.follow_rect(QRectF(rect))
            if not self.info_panel.isVisible():
                self.info_panel.show()
                self.info_panel.raise_()

        if not self.isVisible():
            self.show()
        if not self._capture_exclusion_requested:
            _disable_native_frame(int(self.winId()))
            # 截图控制器仍必须先隐藏浮层再抓屏，兼容不支持此标志的系统。
            self.capture_excluded = set_window_exclude_from_capture(int(self.winId()), True)
            self._capture_exclusion_requested = True

    def dismiss(self):
        """结束这次拖动并隐藏。分层窗口隐藏后仍保留最后一帧，下次显示时会先闪出上次的选框，
        所以先把整窗清成透明再隐藏。"""
        if self.isVisible():
            self.clear()
            self.repaint()
        self.hide()

    def clear(self):
        self._session_active = False
        self._selection_rect = QRect()
        self.model.stop_dragging()
        self.model.deactivate()
        self.selection_overlay.hide()
        self.info_panel.hide()

    def hideEvent(self, event):
        self.clear()
        super().hideEvent(event)
