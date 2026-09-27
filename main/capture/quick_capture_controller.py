"""Global drag capture: keep the live desktop visible until the mouse is released."""

import ctypes
import sys
import threading

from PySide6.QtCore import QEvent, QObject, QPoint, QRect, QRectF, QSizeF, QThread, QTimer, Qt, Signal, Slot
from PySide6.QtWidgets import QApplication, QSystemTrayIcon, QWidget

from capture.capture_service import CaptureService
from core.clipboard_utils import deliver_image_async
from core.last_capture_region import set_last_region
from core.logger import log_debug, log_error
from core.platform_utils import trim_working_set
from core.quick_capture_input import QuickCaptureInput
from settings.tool_settings import QUICK_CAPTURE_ACTIONS, QUICK_CAPTURE_MODIFIERS
from ui.quick_capture_overlay import QuickCaptureOverlay, selection_rect


def desktop_bounds():
    """The application disables Qt scaling: all capture coordinates are physical."""
    bounds = QRect()
    for screen in QApplication.screens():
        bounds = bounds.united(screen.geometry())
    return bounds


def flush_desktop():
    # The overlay is already hidden on the GUI thread. Wait for the compositor
    # on the worker, so the selection border never becomes part of the capture.
    if sys.platform == "win32":
        ctypes.windll.dwmapi.DwmFlush()


class QuickCaptureWorker(QThread):
    captured = Signal(object, object)
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
                image, bounds = service.capture_all_screens(self.cursor) if self.cursor else service.capture_all_screens()
            else:
                image = (service.capture_region(self.region, self.cursor) if self.cursor
                         else service.capture_region(self.region))
                bounds = QRectF(self.region)
            if image.isNull():
                raise RuntimeError("The captured image is empty")
            if not self.isInterruptionRequested():
                self.captured.emit(image, bounds)
        except Exception as exc:
            self.failed.emit(str(exc))


class QuickCapturePreview(QThread):
    """Sample a small live desktop patch; keep only the latest frame for the GUI."""

    failed = Signal(str)
    frame_ready = Signal()
    SAMPLE_SIZE = 96

    def __init__(self, source, bounds, parent=None):
        super().__init__(parent)
        self.source = source
        self.bounds = QRect(bounds)
        self._frame = None
        self._frame_lock = threading.Lock()
        self._frame_pending = False
        self._stop_event = threading.Event()
        self._sample_requested = threading.Event()

    def request_sample(self):
        self._sample_requested.set()

    def take_frame(self):
        """Consume the latest frame and allow one more queued notification."""
        with self._frame_lock:
            frame, self._frame = self._frame, None
            self._frame_pending = False
            return frame

    def _publish_frame(self, image, region):
        with self._frame_lock:
            if self._stop_event.is_set():
                return
            self._frame = (image, region)
            notify = not self._frame_pending
            self._frame_pending = True
        if notify:
            self.frame_ready.emit()

    def stop(self):
        self._stop_event.set()
        self._sample_requested.set()
        self.requestInterruption()

    def run(self):
        import mss
        from PySide6.QtGui import QImage
        try:
            with mss.mss() as desktop:
                while not self._stop_event.is_set():
                    self._sample_requested.clear()
                    if self._stop_event.is_set():
                        break
                    x, y = self.source.position
                    half = self.SAMPLE_SIZE // 2
                    region = QRect(x - half, y - half, self.SAMPLE_SIZE, self.SAMPLE_SIZE).intersected(self.bounds)
                    if not region.isEmpty():
                        shot = desktop.grab(dict(left=region.x(), top=region.y(),
                                                 width=region.width(), height=region.height()))
                        image = QImage(shot.bgra, shot.width, shot.height, shot.width * 4,
                                       QImage.Format.Format_RGB32).copy()
                        # Publish an immutable pair. Neither thread mutates the image.
                        self._publish_frame(image, region)
                    # Movement wakes the sampler immediately. While stationary,
                    # still refresh changing desktop pixels without busy-waiting.
                    if not self._stop_event.is_set():
                        self._sample_requested.wait(0.033)
        except Exception as exc:
            if not self._stop_event.is_set():
                self.failed.emit(str(exc))


class QuickCaptureController(QObject):
    """Own input hooks, one transparent overlay, and at most one capture worker."""

    def __init__(self, main_app):
        super().__init__(main_app)
        self.main_app = main_app
        self.input = QuickCaptureInput(self)
        self.input.event.connect(self._on_input, Qt.ConnectionType.QueuedConnection)
        self.input.moved.connect(self._on_moved, Qt.ConnectionType.QueuedConnection)
        self.input.failure.connect(self._on_failure, Qt.ConnectionType.QueuedConnection)
        self.input.command.connect(self._on_command, Qt.ConnectionType.QueuedConnection)
        self.overlay = None
        self._active = None
        self._worker = None
        self._preview = None
        self._preview_workers = set()
        self._result_valid = False
        self._closed = False
        self._enabled = False
        self._action = "none"
        self._start = QPoint()
        self._bounds = QRect()
        self._capture_pending = False
        # 结束后静置 1.5 秒再收缩工作集；新拖选会取消等待，避免拖动时换页卡顿。
        self._trim_timer = QTimer(self)
        self._trim_timer.setSingleShot(True)
        self._trim_timer.setInterval(1500)
        self._trim_timer.timeout.connect(self._trim_working_set_if_idle)
        QApplication.instance().installEventFilter(self)

    @property
    def busy(self):
        return self._active is not None or self._worker is not None

    def refresh(self):
        """Apply saved settings, including the tray's global-hotkey pause."""
        self.cancel()
        config = self.main_app.config_manager
        first = config.get_app_setting("quick_capture_modifier_1", "win")
        second = config.get_app_setting("quick_capture_modifier_2", "")
        self._action = config.get_app_setting("quick_capture_action", "copy_pin")
        valid = first in QUICK_CAPTURE_MODIFIERS and second in QUICK_CAPTURE_MODIFIERS
        modifiers = frozenset(key for key in (first, second) if key) if valid else frozenset()
        self._enabled = bool(
            modifiers and self._action in dict(QUICK_CAPTURE_ACTIONS) and self._action != "none"
            and not self._closed and not config.get_app_setting("global_hotkeys_disabled", False)
        )
        self.sync_input_availability()
        self.input.configure(modifiers, self._enabled)
        log_debug(f"Quick capture binding applied: modifiers={'+'.join(sorted(modifiers))}, "
                  f"action={self._action}, enabled={self._enabled}", "QuickCapture")

    def suspend(self):
        self.cancel()
        self._enabled = False
        self.input.configure(frozenset(), False)

    def _blocked(self):
        screenshot = self.main_app.screenshot_window
        thread = getattr(self.main_app, "_capture_thread", None)
        return bool(
            QApplication.activeModalWidget() is not None
            or (screenshot and getattr(screenshot, "_session_active", False))
            or (thread and thread.isRunning())
        )

    def set_capture_pending(self, pending):
        """Block before normal capture begins, including its first window build."""
        self._capture_pending = bool(pending)
        self.sync_input_availability()

    @Slot()
    def capture_preparation_finished(self):
        self.set_capture_pending(False)

    def sync_input_availability(self):
        if self._closed:
            return
        # A modal's Show event precedes activeModalWidget() registration. Read
        # its visible window state here, on the GUI thread, before native input
        # can be claimed. Hide similarly restores the shortcut immediately.
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
            if not self._enabled or self.busy or self._blocked():
                log_debug("Quick capture ignored: disabled, busy, or a capture/modal window is active", "QuickCapture")
                self.input.cancel()
                return
            self._bounds = desktop_bounds()
            self._start = QPoint(x, y)
            if not self._bounds.contains(self._start):
                self.input.cancel()
                return
            self._active = token
            self._trim_timer.stop()
            if self.overlay is None:
                self.overlay = QuickCaptureOverlay(self.main_app.config_manager)
            self.overlay.show_selection(self._start, self._start, self._bounds)
            if self.overlay.capture_excluded and self.main_app.config_manager.get_app_setting("magnifier_enabled", True):
                self._start_preview()
            elif not self.overlay.capture_excluded:
                # Capturing a visible overlay would feed its own border and
                # magnifier back into the sampler. The final capture still
                # works through the hide-before-grab fallback.
                log_debug("Live magnifier unavailable: window capture exclusion failed", "QuickCapture")
        elif token == self._active:
            if kind == "cancel":
                self.cancel()
            elif kind == "finish":
                region = selection_rect(self._start, QPoint(x, y), desktop_bounds())
                self._hide_selection()
                if region.width() >= 2 and region.height() >= 2 and not self._blocked():
                    self._capture(region)

    @Slot(int)
    def _on_moved(self, token):
        position = self.input.take_position(token)
        if position is None or token != self._active:
            return
        if self._blocked():
            self.cancel()
            return
        self.overlay.show_selection(self._start, QPoint(*position), self._bounds)
        if self._preview is not None:
            self._preview.request_sample()

    @Slot()
    def _on_preview_frame(self):
        preview = self.sender()
        if preview is not self._preview or self._active is None:
            return
        if self._blocked():
            self.cancel()
            return
        frame = preview.take_frame()
        if frame is not None:
            self.overlay.set_sample_image(*frame)

    def _start_preview(self):
        preview = QuickCapturePreview(self.input, self._bounds, self)
        self._preview = preview
        self._preview_workers.add(preview)
        preview.frame_ready.connect(self._on_preview_frame, Qt.ConnectionType.QueuedConnection)
        preview.failed.connect(self._on_preview_failure, Qt.ConnectionType.QueuedConnection)
        preview.finished.connect(self._preview_finished)
        preview.start()

    @Slot(str)
    def _on_preview_failure(self, message):
        # A stopped sampler may already have queued an error. It must not
        # cancel a later gesture or a capture that has already been released.
        if self.sender() is self._preview:
            self._on_failure(message)

    @Slot()
    def _preview_finished(self):
        preview = self.sender()
        if preview is self._preview:
            self._preview = None
        self._preview_workers.discard(preview)
        preview.deleteLater()
        self._schedule_working_set_trim()

    @Slot(str)
    def _on_command(self, command):
        if self._active is not None and self.overlay is not None:
            self.overlay.handle_command(command)

    def _hide_selection(self):
        was_active = self._active is not None
        self._active = None
        if self._preview is not None:
            self._preview.stop()
            self._preview = None
        if self.overlay is not None:
            self.overlay.hide()
        if was_active:
            self._schedule_working_set_trim()

    def _schedule_working_set_trim(self):
        # 抓屏、采样线程都退出后再计时；重复结束通知只重置同一个定时器。
        if not self._closed and not self.busy and not self._preview_workers:
            self._trim_timer.start()

    @Slot()
    def _trim_working_set_if_idle(self):
        # 转入普通截图后由普通截图负责收尾，不在编辑会话中收缩工作集。
        if (not self._closed and not self.busy and not self._preview_workers
                and not self._capture_pending and not self._blocked()):
            trim_working_set()

    def cancel(self):
        self.input.cancel()
        self._hide_selection()
        self._result_valid = False
        if self._worker is not None:
            self._worker.requestInterruption()

    def _capture(self, region):
        self._result_valid = True
        worker = QuickCaptureWorker(region, self._action, self)
        worker.include_cursor = bool(self.main_app.config_manager.get_app_setting("capture_include_cursor", False))
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
        try:
            if action == "edit":
                region = region.intersected(bounds.toAlignedRect())
                if region.isEmpty():
                    return
                if getattr(worker, "cursor", None) is not None:
                    self.main_app._on_capture_ready(image, bounds, worker.cursor)
                else:
                    self.main_app._on_capture_ready(image, bounds)
                window = self.main_app.screenshot_window
                if window and getattr(window, "_session_active", False):
                    model = window.scene.selection_model
                    # Normal drag selection has an 8px minimum. A precise quick
                    # capture may be smaller, especially at a screen edge.
                    minimum = QSizeF(model.min_size)
                    try:
                        model.min_size = QSizeF(min(minimum.width(), region.width()),
                                                min(minimum.height(), region.height()))
                        model.initialize_confirmed_rect(QRectF(region))
                    finally:
                        model.min_size = minimum
            else:
                if action in ("copy", "copy_pin"):
                    deliver_image_async(image)
                if action in ("pin", "copy_pin"):
                    from pin.pin_manager import PinManager
                    PinManager.instance().create_pin(image, region.topLeft(), self.main_app.config_manager)
            set_last_region(region)
        except Exception as exc:
            self._on_failure(str(exc))

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
                QApplication.translate("SettingsDialog", "Quick Capture"),
                QApplication.translate("SettingsDialog", "Quick capture failed. Please try again."),
                QSystemTrayIcon.MessageIcon.Warning,
            )

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._trim_timer.stop()
        QApplication.instance().removeEventFilter(self)
        self.cancel()
        self.input.close()
        if self._worker is not None:
            self._worker.wait()
        for preview in tuple(self._preview_workers):
            preview.stop()
            preview.wait()
        if self.overlay is not None:
            self.overlay.close()
            self.overlay.deleteLater()
            self.overlay = None
