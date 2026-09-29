"""快速截图透明浮层：复用普通截图的选框、信息面板和放大镜。"""

import ctypes
from ctypes import wintypes
from types import SimpleNamespace

from PySide6.QtCore import QPoint, QPointF, QRect, QRectF, QSizeF, Qt
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QWidget

from canvas.items.selection_item import SelectionItem
from canvas.selection_model import SelectionModel
from core.platform_utils import set_window_exclude_from_capture
from settings import get_tool_settings_manager
from ui.magnifier import MagnifierOverlay
from ui.selection_info.panel import SelectionInfoPanel
from ui.selection_overlay import SelectionOverlayWidget


def _disable_native_frame(hwnd):
    """DWM 的边框、圆角和阴影不属于我们绘制的选区，也必须关闭。"""
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
    """仅适配现有浮层所需的坐标与交互状态，不创建画布或绘图工具。"""

    def __init__(self, window):
        self.window = window
        self.drawing = SimpleNamespace(active=False)
        self.text_drag = SimpleNamespace(active=False)

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
        self._sample_ready = False
        self._cursor = QPointF()

        self.model = SelectionModel()
        self.model.setParent(self)
        self.model.min_size = QSizeF(0, 0)
        self.selection_item = SelectionItem(self.model)
        self.selection_overlay = SelectionOverlayWidget(self, self.selection_item, self.model)
        self.view = _CaptureView(self)
        self.scene = SimpleNamespace(
            scene_rect=QRectF(), selection_model=self.model, background=None,
            tool_controller=SimpleNamespace(current_tool_id="cursor"),
        )
        self.info_panel = SelectionInfoPanel(self, self.view)
        self.magnifier_overlay = MagnifierOverlay(self, self.scene, self.view, self.config_manager)

    def show_selection(self, start: QPoint, end: QPoint, bounds: QRect):
        """根据绝对物理坐标更新原有截图部件，不绘制底图或灰色遮罩。"""
        if bounds.isEmpty():
            self.hide()
            return
        new_session = not self._session_active
        if new_session:
            self._session_active = True
            self._sample_ready = False
            self.magnifier_overlay.rebind(self.scene, self.view)
            self._hide_info = self.config_manager.get_app_setting("screenshot_info_hide_on_drag", False)
            self.info_panel.set_confirmed(False)
            self.info_panel.apply_scale()
            self.model.activate()
            self.model.start_dragging()

        scene_bounds = QRectF(bounds)
        bounds_changed = scene_bounds != self.scene.scene_rect
        if bounds_changed:
            self.setGeometry(bounds)
            self.scene.scene_rect = scene_bounds
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

        cursor = QPointF(end)
        if new_session or cursor != self._cursor or bounds_changed:
            self._cursor = cursor
            self._update_magnifier()
        if not self.isVisible():
            self.show()
        if not self._capture_exclusion_requested:
            _disable_native_frame(int(self.winId()))
            # 截图控制器仍必须先隐藏浮层再抓屏，兼容不支持此标志的系统。
            self.capture_excluded = set_window_exclude_from_capture(int(self.winId()), True)
            self._capture_exclusion_requested = True

    def _update_magnifier(self):
        sample_rect = self.magnifier_overlay._sample_rect
        side = self.magnifier_overlay.sample_size
        needed = QRect(int(self._cursor.x()) - side // 2, int(self._cursor.y()) - side // 2,
                       side, side).intersected(self.scene.scene_rect.toAlignedRect())
        if (self._sample_ready and not needed.isEmpty()
                and sample_rect.toAlignedRect().contains(needed)
                and sample_rect.toAlignedRect().contains(self._cursor.toPoint())):
            self.magnifier_overlay.update_cursor(self._cursor)
        else:
            self.magnifier_overlay.clear_cursor()

    def set_sample_image(self, image, global_rect):
        """GUI 线程接收本次拖动的局部图像；迟到的帧不重新显示已结束的浮层。"""
        if not self._session_active:
            return
        self._sample_ready = image is not None and not image.isNull()
        self.magnifier_overlay.set_sample_image(image if self._sample_ready else None, global_rect)
        self._update_magnifier()

    def handle_command(self, command):
        if not self._session_active:
            return
        magnifier = self.magnifier_overlay
        if command == "cycle_color":
            magnifier.cycle_color_format()
        elif command == "copy_color":
            if self._sample_ready and magnifier.cursor_scene_pos is not None:
                magnifier.copy_color_info()
        elif command == "zoom_in":
            magnifier.adjust_zoom(1)
        elif command == "zoom_out":
            magnifier.adjust_zoom(-1)
        self._update_magnifier()

    def clear(self):
        self._session_active = False
        self._sample_ready = False
        self._selection_rect = QRect()
        self.model.stop_dragging()
        self.model.deactivate()
        self.selection_overlay.hide()
        self.info_panel.hide()
        self.magnifier_overlay.clear_cursor()
        self.magnifier_overlay.set_sample_image(None, None)

    def hideEvent(self, event):
        self.clear()
        super().hideEvent(event)
