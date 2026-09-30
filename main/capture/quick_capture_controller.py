"""全局鼠标快捷键：按住修饰键拖出选区，拖动期间桌面保持实时，松开后截图并执行动作。"""

import ctypes
import sys

from PySide6.QtCore import QEvent, QObject, QPoint, QRect, QRectF, QThread, Qt, Signal, Slot
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QApplication, QSystemTrayIcon, QWidget

from capture.capture_service import CaptureService
from core.clipboard_utils import deliver_screenshot
from core.last_capture_region import set_last_region
from core.logger import log_debug, log_error
from core.platform_utils import request_trim_working_set
from core.quick_capture_input import QuickCaptureInput
from settings.tool_settings import get_quick_capture_bindings
from ui.quick_capture_overlay import QuickCaptureOverlay, selection_rect


# 截图类动作：(复制到剪贴板, 钉图)
_CAPTURE_ACTIONS = {"copy": (True, False), "pin": (False, True), "copy_pin": (True, True)}
_TEXT_ACTIONS = ("translate", "ocr")


def desktop_bounds():
    """应用关闭了 Qt 缩放，截图坐标都是物理像素。"""
    bounds = QRect()
    for screen in QApplication.screens():
        bounds = bounds.united(screen.geometry())
    return bounds


def flush_desktop():
    # 浮层已在 GUI 线程隐藏；在工作线程等一次合成，选框就不会被截进去
    if sys.platform == "win32":
        ctypes.windll.dwmapi.DwmFlush()


class QuickCaptureWorker(QThread):
    captured = Signal(object, object)  # image, bounds
    failed = Signal(str)

    def __init__(self, region, action, parent=None):
        super().__init__(parent)
        self.region = QRect(region)
        self.action = action
        self.include_cursor = False
        self.cursor = None

    def run(self):
        try:
            flush_desktop()
            if self.isInterruptionRequested():
                return
            if self.include_cursor:
                from capture.system_cursor import SystemCursor
                self.cursor = SystemCursor.grab()
            service = CaptureService()
            if self.action == "edit":
                # 转入普通截图要整个桌面作底图，选区之后还能调
                image, bounds = service.capture_all_screens(self.cursor)
            else:
                image, bounds = service.capture_region(self.region, self.cursor), QRectF(self.region)
            if image.isNull():
                raise RuntimeError("The captured image is empty")
            if not self.isInterruptionRequested():
                self.captured.emit(image, bounds)
        except Exception as exc:
            self.failed.emit(str(exc))


class QuickCaptureController(QObject):
    """持有输入钩子、一个透明浮层和至多一个抓屏线程。"""

    def __init__(self, main_app):
        super().__init__(main_app)
        self.main_app = main_app
        self.input = QuickCaptureInput(self)
        self.input.event.connect(self._on_input, Qt.ConnectionType.QueuedConnection)
        self.input.moved.connect(self._on_moved, Qt.ConnectionType.QueuedConnection)
        self.input.failure.connect(self._on_failure, Qt.ConnectionType.QueuedConnection)
        self.overlay = None
        self._active = None
        self._worker = None
        self._result_valid = False
        self._closed = False
        self._enabled = False
        self._actions = {}
        self._active_action = None
        self._start = QPoint()
        self._bounds = QRect()
        self._capture_pending = False
        QApplication.instance().installEventFilter(self)

    @property
    def busy(self):
        return self._active is not None or self._worker is not None

    def refresh(self):
        """按已保存的设置生效，包括托盘的「暂停全局热键」。"""
        self.cancel()
        config = self.main_app.config_manager
        self._actions = get_quick_capture_bindings(config)
        self._enabled = bool(
            self._actions and not self._closed
            and not config.get_app_setting("global_hotkeys_disabled", False)
        )
        self.sync_input_availability()
        self.input.configure(frozenset(self._actions), self._enabled)
        gestures = ", ".join(f"{'+'.join(sorted(modifiers))}+{button}={action}"
                             for (modifiers, button), action in self._actions.items())
        log_debug(f"Quick capture bindings applied: [{gestures}], enabled={self._enabled}", "QuickCapture")

    def suspend(self):
        self.cancel()
        self._enabled = False
        self.input.configure(frozenset(), False)

    def _blocked(self):
        screenshot = self.main_app.screenshot_window
        return bool(
            QApplication.activeModalWidget() is not None
            or (screenshot and getattr(screenshot, "_session_active", False))
        )

    def set_capture_pending(self, pending):
        """普通截图从排队到窗口建好期间，全局鼠标快捷键不响应。"""
        self._capture_pending = bool(pending)
        self.sync_input_availability()

    def sync_input_availability(self):
        if self._closed:
            return
        # 模态窗口的 Show 事件早于 activeModalWidget() 登记，所以直接看可见的模态窗口；
        # Hide 时同样立即恢复
        modal_visible = any(window.isVisible() and window.isModal()
                            for window in QApplication.topLevelWidgets())
        self.input.set_blocked(self._capture_pending or self._blocked() or modal_visible)

    def eventFilter(self, watched, event):
        if (not self._closed and event.type() in (QEvent.Type.Show, QEvent.Type.Hide)
                and isinstance(watched, QWidget) and watched.isWindow()):
            self.sync_input_availability()
        return False

    @Slot(str, int, int, int)
    def _on_input(self, kind, token, x, y):
        if kind != "cancel" and not self.input.accepts(token):
            return
        if kind == "start":
            action = self._actions.get(self.input.gesture_binding(token))
            if not self._enabled or action is None or self.busy or self._blocked():
                log_debug("Quick capture ignored: disabled, busy, or a capture/modal window is active", "QuickCapture")
                self.input.cancel()
                return
            self._bounds = desktop_bounds()
            self._start = QPoint(x, y)
            if not self._bounds.contains(self._start):
                self.input.cancel()
                return
            self._active = token
            self._active_action = action
            if self.overlay is None:
                self.overlay = QuickCaptureOverlay(self.main_app.config_manager)
            self.overlay.show_selection(self._start, self._start, self._bounds)
        elif token == self._active:
            if kind == "cancel":
                self.cancel()
            elif kind == "finish":
                region = selection_rect(self._start, QPoint(x, y), desktop_bounds())
                self._hide_selection()
                if region.width() >= 2 and region.height() >= 2 and not self._blocked():
                    self._capture(region)
                else:
                    self._schedule_working_set_trim()

    @Slot(int)
    def _on_moved(self, token):
        position = self.input.take_position(token)
        if position is None or token != self._active:
            return
        if self._blocked():
            self.cancel()
            return
        self.overlay.show_selection(self._start, QPoint(*position), self._bounds)

    def _hide_selection(self):
        was_active = self._active is not None
        self._active = None
        if self.overlay is not None:
            self.overlay.dismiss()
        return was_active

    def _schedule_working_set_trim(self):
        # 抓屏线程退出后再请求；之后又开始拖动或转入了普通截图，由共用的忙碌判断顺延
        if not self._closed and not self.busy:
            request_trim_working_set(1500)

    def cancel(self):
        self.input.cancel()
        if self._hide_selection():
            self._schedule_working_set_trim()
        self._result_valid = False
        if self._worker is not None:
            self._worker.requestInterruption()

    def _capture(self, region):
        self._result_valid = True
        action = self._active_action
        worker = QuickCaptureWorker(region, action, self)
        # 识别和翻译要的是字，画上指针只会挡住字
        worker.include_cursor = action not in _TEXT_ACTIONS and bool(
            self.main_app.config_manager.get_app_setting("capture_include_cursor", False))
        self._worker = worker
        worker.captured.connect(self._deliver, Qt.ConnectionType.QueuedConnection)
        worker.failed.connect(self._on_failure, Qt.ConnectionType.QueuedConnection)
        worker.finished.connect(self._worker_finished)
        worker.start()

    @Slot(object, object)
    def _deliver(self, image, bounds):
        worker = self._worker
        if not self._result_valid or self._closed or worker is None or self._blocked():
            return
        region, action = worker.region, worker.action
        config = self.main_app.config_manager
        try:
            if action == "edit":
                region = region.intersected(bounds.toAlignedRect())
                if region.isEmpty():
                    return
                self._open_normal_capture(image, bounds, worker.cursor, region)
            elif action == "translate":
                from translation import TranslationManager
                TranslationManager.instance().translate_from_image(
                    pixmap=QPixmap.fromImage(image), **config.get_translation_request_params())
            elif action == "ocr":
                # 和截图工具栏的「文字识别」一样，按设置弹结果窗口或直接复制
                from text_recognition import copy_text_recognition, show_text_recognition
                if config.get_ocr_copy_directly_enabled():
                    copy_text_recognition(image)
                else:
                    show_text_recognition(image)
            else:
                copy, pin = _CAPTURE_ACTIONS[action]
                # 开着「自动保存截图」时这些动作都存文件
                deliver_screenshot(image, config, copy_to_clipboard=copy)
                if pin:
                    from pin.pin_manager import PinManager
                    PinManager.instance().create_pin(image, region.topLeft(), config)
            set_last_region(region)
        except Exception as exc:
            self._on_failure(str(exc))

    def _open_normal_capture(self, image, bounds, cursor, region):
        self.main_app._on_capture_ready(image, bounds, cursor)
        window = self.main_app.screenshot_window
        if window and getattr(window, "_session_active", False):
            window.scene.preset_selection(QRectF(region))

    @Slot()
    def _worker_finished(self):
        worker = self.sender()
        if worker is self._worker:
            self._worker = None
        worker.deleteLater()
        self._schedule_working_set_trim()

    @Slot(str)
    def _on_failure(self, message):
        log_error(f"Quick capture failed: {message}", module="QuickCapture")
        self.cancel()
        if self._closed:
            return
        tray = getattr(self.main_app, "tray_icon", None)
        if tray is not None:
            tray.showMessage(
                QApplication.translate("SettingsDialog", "Global Mouse Shortcuts"),
                QApplication.translate("SettingsDialog", "Capture failed. Please try again."),
                QSystemTrayIcon.MessageIcon.Warning,
            )

    def close(self):
        if self._closed:
            return
        self._closed = True
        QApplication.instance().removeEventFilter(self)
        self.cancel()
        self.input.close()
        if self._worker is not None:
            self._worker.wait()
        if self.overlay is not None:
            self.overlay.close()
            self.overlay.deleteLater()
            self.overlay = None
