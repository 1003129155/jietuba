"""Synthetic quick-capture gestures: never install hooks or use the real clipboard."""

import threading
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QObject, QPoint, QRect, QRectF, QSizeF, QThread, Qt, Signal
from PySide6.QtGui import QImage

from capture import quick_capture_controller as module
from canvas.selection_model import SelectionModel
from settings.tool_settings import ToolSettingsManager


class FakeInput(QObject):
    event = Signal(str, int, int, int)
    moved = Signal(int)
    failure = Signal(str)
    command = Signal(str)
    position = (0, 0)

    def __init__(self, parent):
        super().__init__(parent)
        self.configure = Mock()
        self.set_blocked = Mock()
        self.cancel = Mock()
        self.close = Mock()
        self.accepts = Mock(return_value=True)
        self.take_position = Mock(side_effect=lambda _token: self.position)


class FakeWorker(QObject):
    captured = Signal(object, object)
    failed = Signal(str)
    finished = Signal()

    def __init__(self, region, action, parent):
        super().__init__(parent)
        self.region, self.action = region, action
        self.include_cursor = False
        self.cursor = None
        self.start = Mock()
        self.requestInterruption = Mock()
        self.wait = Mock()


class FakePreview(QObject):
    failed = Signal(str)
    frame_ready = Signal()
    finished = Signal()

    def __init__(self, source, bounds, parent):
        super().__init__(parent)
        self.source, self.bounds = source, bounds
        self.frame = None
        self.start = Mock()
        self.stop = Mock()
        self.wait = Mock()
        self.request_sample = Mock()

    def take_frame(self):
        frame, self.frame = self.frame, None
        return frame


class FakeApp(QObject):
    def __init__(self, config):
        super().__init__()
        self.config_manager = config
        self.screenshot_window = None
        self.tray_icon = SimpleNamespace(showMessage=Mock())
        self.selection = SelectionModel()
        self._on_capture_ready = Mock(side_effect=self._edit)

    def _edit(self, image, bounds, cursor=None):
        self.screenshot_window = SimpleNamespace(
            _session_active=True, scene=SimpleNamespace(selection_model=self.selection),
        )


@pytest.fixture
def capture(qapp, tmp_settings, monkeypatch):
    monkeypatch.setattr(module, "QuickCaptureInput", FakeInput)
    monkeypatch.setattr(module, "QuickCaptureWorker", FakeWorker)
    monkeypatch.setattr(module, "QuickCapturePreview", FakePreview)
    monkeypatch.setattr(module, "desktop_bounds", lambda: QRect(-100, -100, 500, 500))
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", Mock())
    monkeypatch.setattr(module, "deliver_image_async", Mock())
    monkeypatch.setattr(module, "set_last_region", Mock())
    monkeypatch.setattr(module, "trim_working_set", Mock())
    app = FakeApp(ToolSettingsManager(tmp_settings))
    controller = module.QuickCaptureController(app)
    controller.refresh()
    yield controller
    controller.close()


def start(controller, qtbot, token=1, x=-50, y=-20):
    controller.input.position = (x, y)
    controller.input.event.emit("start", token, x, y)
    qtbot.waitUntil(lambda: controller._active == token)


def finish(controller, qtbot, token=1, x=30, y=40):
    controller.input.event.emit("finish", token, x, y)
    qtbot.waitUntil(lambda: controller._worker is not None)
    return controller._worker


def move(controller, token=1, x=30, y=40):
    controller.input.position = (x, y)
    controller.input.moved.emit(token)
    module.QApplication.processEvents()


def test_native_capture_thread_finishes_availability_update_on_gui_thread(capture, qtbot, monkeypatch):
    threads = []
    capture.set_capture_pending(True)
    original = capture.sync_input_availability

    def record():
        threads.append(QThread.currentThread())
        original()

    monkeypatch.setattr(capture, "sync_input_availability", record)

    class Worker(QThread):
        def run(self):
            pass

    worker = Worker()
    worker.finished.connect(capture.capture_preparation_finished, Qt.ConnectionType.QueuedConnection)
    worker.start()
    assert worker.wait(2000)
    qtbot.waitUntil(lambda: bool(threads))
    assert threads == [module.QApplication.instance().thread()]
    assert not capture._capture_pending
    capture.input.set_blocked.assert_called_with(False)
    worker.deleteLater()


def image():
    result = QImage(80, 60, QImage.Format.Format_RGB32)
    result.fill(0xff224466)
    return result


@pytest.mark.parametrize("action", ["copy", "pin", "copy_pin", "edit"])
def test_working_set_trim_waits_for_capture_and_skips_normal_editor(capture, qtbot, monkeypatch, action):
    from pin.pin_manager import PinManager
    monkeypatch.setattr(PinManager, "instance", lambda: Mock())
    capture.main_app.config_manager.set_app_setting("magnifier_enabled", False)
    capture.main_app.config_manager.set_app_setting("quick_capture_action", action)
    capture.refresh()
    assert capture._trim_timer.interval() == 1500
    capture._trim_timer.setInterval(30)
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    qtbot.wait(70)
    module.trim_working_set.assert_not_called()
    worker.captured.emit(image(), QRectF(-100, -100, 500, 500))
    qtbot.waitUntil(lambda: module.set_last_region.called)
    worker.finished.emit()
    module.trim_working_set.assert_not_called()
    if action == "edit":
        qtbot.wait(70)
        module.trim_working_set.assert_not_called()
    else:
        qtbot.waitUntil(lambda: module.trim_working_set.called)
        module.trim_working_set.assert_called_once_with()


@pytest.mark.parametrize("ending", ["cancel", "empty", "failure"])
def test_working_set_trim_covers_cancel_empty_selection_and_failure(capture, qtbot, ending):
    capture.main_app.config_manager.set_app_setting("magnifier_enabled", False)
    capture._trim_timer.setInterval(30)
    start(capture, qtbot)
    if ending == "failure":
        worker = finish(capture, qtbot)
        worker.failed.emit("synthetic capture failure")
        qtbot.waitUntil(lambda: capture.main_app.tray_icon.showMessage.called)
        worker.finished.emit()
    elif ending == "empty":
        capture._on_input("finish", 1, -50, -20)
    else:
        capture.cancel()
    module.trim_working_set.assert_not_called()
    qtbot.waitUntil(lambda: module.trim_working_set.called)
    module.trim_working_set.assert_called_once_with()


def test_working_set_trim_waits_for_sampler_exit(capture, qtbot):
    capture._trim_timer.setInterval(30)
    start(capture, qtbot)
    preview = capture._preview
    capture.cancel()
    qtbot.wait(70)
    module.trim_working_set.assert_not_called()
    preview.finished.emit()
    qtbot.waitUntil(lambda: module.trim_working_set.called)
    module.trim_working_set.assert_called_once_with()


def test_new_drag_cancels_pending_trim_and_rearms_after_finish(capture, qtbot):
    capture.main_app.config_manager.set_app_setting("magnifier_enabled", False)
    capture._trim_timer.setInterval(100)
    start(capture, qtbot)
    capture.cancel()
    assert capture._trim_timer.isActive()
    start(capture, qtbot, token=2)
    assert not capture._trim_timer.isActive()
    qtbot.wait(150)
    module.trim_working_set.assert_not_called()
    capture.cancel()
    qtbot.waitUntil(lambda: module.trim_working_set.called)
    module.trim_working_set.assert_called_once_with()


@pytest.mark.parametrize("busy_state", ["preparing", "editing", "closed"])
def test_pending_trim_does_not_run_during_normal_capture_or_shutdown(capture, qtbot, busy_state):
    capture.main_app.config_manager.set_app_setting("magnifier_enabled", False)
    capture._trim_timer.setInterval(30)
    start(capture, qtbot)
    capture.cancel()
    if busy_state == "preparing":
        capture.set_capture_pending(True)
    elif busy_state == "editing":
        capture.main_app.screenshot_window = SimpleNamespace(_session_active=True)
    else:
        capture.close()
    qtbot.wait(70)
    module.trim_working_set.assert_not_called()


@pytest.mark.parametrize("action,copy,pin,edit", [
    ("copy", True, False, False), ("pin", False, True, False),
    ("copy_pin", True, True, False), ("edit", False, False, True),
])
def test_actions_use_exact_absolute_selection(capture, qtbot, monkeypatch, action, copy, pin, edit):
    from pin.pin_manager import PinManager
    pin_manager = Mock()
    monkeypatch.setattr(PinManager, "instance", lambda: pin_manager)
    capture.main_app.config_manager.set_app_setting("quick_capture_action", action)
    capture.refresh()
    start(capture, qtbot)
    move(capture)
    assert capture.overlay.model.rect() == QRectF(-50, -20, 80, 60)
    assert capture.overlay.isVisible()
    worker = finish(capture, qtbot)
    assert worker.region == QRect(-50, -20, 80, 60)
    assert not capture.overlay.isVisible()
    worker.captured.emit(image(), QRectF(-100, -100, 500, 500))
    qtbot.waitUntil(lambda: module.set_last_region.called)
    assert module.deliver_image_async.called == copy
    assert pin_manager.create_pin.called == pin
    assert capture.main_app._on_capture_ready.called == edit
    if pin:
        assert pin_manager.create_pin.call_args.args[1] == QPoint(-50, -20)
    if edit:
        assert capture.main_app.selection.rect() == QRectF(-50, -20, 80, 60)
        assert capture.main_app.selection.is_confirmed
    worker.finished.emit()
    qtbot.waitUntil(lambda: not capture.busy)


@pytest.mark.parametrize("setting,value", [
    ("global_hotkeys_disabled", True), ("quick_capture_action", "none"),
    ("quick_capture_modifier_1", ""), ("quick_capture_modifier_1", "unknown"),
    ("quick_capture_action", "unknown"),
])
def test_disabled_or_invalid_configuration_does_not_arm(capture, setting, value):
    capture.main_app.config_manager.set_app_setting(setting, value)
    capture.refresh()
    assert not capture._enabled
    assert capture.input.configure.call_args.args[1] is False
    capture._on_input("start", 1, 10, 10)
    assert not capture.busy


def test_motion_updates_selection_on_gui_event_delivery_without_waiting_for_a_timer(capture, qtbot, monkeypatch):
    start(capture, qtbot)
    rendered = Mock(wraps=capture.overlay.show_selection)
    monkeypatch.setattr(capture.overlay, "show_selection", rendered)
    move(capture, x=61, y=83)
    rendered.assert_called_once_with(QPoint(-50, -20), QPoint(61, 83), capture._bounds)
    assert capture.overlay.model.rect() == QRectF(-50, -20, 111, 103)
    capture._preview.request_sample.assert_called_once_with()


def test_stale_or_consumed_motion_cannot_redraw_or_sample(capture, qtbot, monkeypatch):
    start(capture, qtbot, token=2)
    rendered = Mock()
    monkeypatch.setattr(capture.overlay, "show_selection", rendered)
    move(capture, token=1)
    capture.input.take_position.return_value = None
    capture.input.take_position.side_effect = None
    move(capture, token=2)
    rendered.assert_not_called()
    capture._preview.request_sample.assert_not_called()


def test_queued_motion_cannot_reopen_overlay_after_release(capture, qtbot, monkeypatch):
    start(capture, qtbot)
    rendered = Mock()
    monkeypatch.setattr(capture.overlay, "show_selection", rendered)
    capture.input.moved.emit(1)
    capture._on_input("finish", 1, 30, 40)
    module.QApplication.processEvents()
    rendered.assert_not_called()
    assert not capture.overlay.isVisible()
    assert capture._worker.region == QRect(-50, -20, 80, 60)


def test_two_modifiers_and_duplicate_modifiers(capture):
    capture.main_app.config_manager.set_app_setting("quick_capture_modifier_2", "ctrl")
    capture.refresh()
    capture.input.configure.assert_called_with(frozenset(("win", "ctrl")), True)
    capture.main_app.config_manager.set_app_setting("quick_capture_modifier_2", "win")
    capture.refresh()
    capture.input.configure.assert_called_with(frozenset(("win",)), True)


def test_click_without_drag_never_captures(capture, qtbot):
    start(capture, qtbot)
    capture._on_input("finish", 1, -50, -20)
    assert not capture.busy
    assert not capture.overlay.isVisible()


def test_no_recursive_preview_when_window_cannot_be_excluded(capture, qtbot, monkeypatch):
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", lambda *_args: False)
    start(capture, qtbot)
    assert capture.overlay.isVisible()
    assert capture._preview is None
    assert not capture.overlay.magnifier_overlay.isVisible()
    worker = finish(capture, qtbot)
    assert worker.region == QRect(-50, -20, 80, 60)
    assert not capture.overlay.isVisible()


def test_small_edit_region_does_not_expand_past_screen_edge(capture, qtbot):
    capture.main_app.config_manager.set_app_setting("quick_capture_action", "edit")
    capture.refresh()
    start(capture, qtbot, x=397, y=396)
    worker = finish(capture, qtbot, x=400, y=400)
    worker.captured.emit(image(), QRectF(-100, -100, 500, 500))
    qtbot.waitUntil(lambda: capture.main_app.selection.is_confirmed)
    assert capture.main_app.selection.rect() == QRectF(397, 396, 3, 4)
    assert capture.main_app.selection.min_size == QSizeF(8, 8)


def test_cancel_and_stale_finish_never_captures(capture, qtbot):
    start(capture, qtbot)
    capture._on_input("cancel", 1, 10, 20)
    capture._on_input("finish", 1, 30, 40)
    assert not capture.busy
    assert not capture.overlay.isVisible()


def test_reconfigured_queued_gesture_is_ignored(capture):
    capture.input.accepts.return_value = False
    capture._on_input("start", 9, 10, 10)
    capture._on_input("finish", 9, 50, 50)
    assert not capture.busy


def test_capture_result_is_discarded_when_settings_change(capture, qtbot):
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    capture.refresh()
    worker.requestInterruption.assert_called()
    capture._deliver(image(), QRectF(-100, -100, 500, 500))
    module.deliver_image_async.assert_not_called()
    module.set_last_region.assert_not_called()


def test_cannot_start_during_other_capture_or_modal(capture, monkeypatch):
    capture.main_app.screenshot_window = SimpleNamespace(_session_active=True)
    capture._on_input("start", 1, 10, 10)
    assert not capture.busy
    capture.main_app.screenshot_window = None
    capture.main_app._capture_thread = SimpleNamespace(isRunning=lambda: True)
    capture._on_input("start", 2, 10, 10)
    assert not capture.busy
    capture.main_app._capture_thread = None
    monkeypatch.setattr(module.QApplication, "activeModalWidget", lambda: object())
    capture._on_input("start", 3, 10, 10)
    assert not capture.busy


def test_modal_appearing_during_drag_cancels(capture, qtbot, monkeypatch):
    start(capture, qtbot)
    monkeypatch.setattr(module.QApplication, "activeModalWidget", lambda: object())
    move(capture)
    assert not capture.busy
    assert not capture.overlay.isVisible()


def test_suspend_and_close_clean_up_pending_capture(capture, qtbot):
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    capture.suspend()
    capture.input.configure.assert_called_with(frozenset(), False)
    capture.close()
    capture.close()
    worker.wait.assert_called_once()
    capture.input.close.assert_called_once()


def test_capture_failure_closes_overlay_and_notifies(capture, qtbot):
    start(capture, qtbot)
    capture._on_failure("synthetic failure")
    assert not capture.overlay.isVisible()
    capture.main_app.tray_icon.showMessage.assert_called_once()


def test_live_preview_updates_magnifier_and_stops_before_capture(capture, qtbot, monkeypatch):
    start(capture, qtbot)
    preview = capture._preview
    assert preview.source is capture.input
    preview.start.assert_called_once()
    received = Mock(wraps=capture.overlay.set_sample_image)
    monkeypatch.setattr(capture.overlay, "set_sample_image", received)
    sample = image()
    frame = (sample, QRect(-80, -50, 80, 60))
    preview.frame = frame
    preview.frame_ready.emit()
    qtbot.waitUntil(lambda: received.called)
    received.assert_called_once_with(*frame)
    assert capture.overlay._sample_ready
    assert capture.overlay.config_manager is capture.main_app.config_manager
    move(capture)
    assert received.call_count == 1
    preview.request_sample.assert_called_once_with()

    finish(capture, qtbot)
    preview.stop.assert_called_once()
    assert capture._preview is None
    assert not capture.overlay._sample_ready
    preview.frame = (image(), QRect(-50, -20, 80, 60))
    preview.frame_ready.emit()
    module.QApplication.processEvents()
    assert received.call_count == 1
    preview.finished.emit()
    assert preview not in capture._preview_workers


def test_disabling_magnifier_avoids_sampling_but_keeps_capture(capture, qtbot):
    capture.main_app.config_manager.set_app_setting("magnifier_enabled", False)
    start(capture, qtbot)
    assert capture._preview is None
    assert not capture._preview_workers
    worker = finish(capture, qtbot)
    worker.start.assert_called_once()


@pytest.mark.parametrize("action", ["cancel", "suspend", "failure", "close"])
def test_ending_drag_stops_live_sampling(capture, qtbot, action):
    start(capture, qtbot)
    preview = capture._preview
    if action == "failure":
        preview.failed.emit("Synthetic sampling failure")
        qtbot.waitUntil(lambda: not capture.busy)
    else:
        getattr(capture, action)()
    preview.stop.assert_called()
    assert capture._preview is None
    assert capture._active is None
    if capture.overlay is not None:
        assert not capture.overlay.isVisible()
    if action == "close":
        preview.wait.assert_called_once()


def test_late_preview_finish_cannot_clear_new_drag_preview(capture, qtbot):
    start(capture, qtbot)
    previous = capture._preview
    capture.cancel()
    start(capture, qtbot, token=2)
    current = capture._preview
    assert current is not previous
    previous.finished.emit()
    assert capture._preview is current
    assert previous not in capture._preview_workers
    assert current in capture._preview_workers
    capture.close()
    current.wait.assert_called_once()


def test_queued_failure_from_old_preview_cannot_cancel_a_new_drag(capture, qtbot):
    start(capture, qtbot)
    previous = capture._preview
    previous.failed.emit("A previous preview failed")
    capture.cancel()
    # Start again before the old queued failure reaches the GUI event loop.
    capture._on_input("start", 2, -30, -10)
    current = capture._preview
    module.QApplication.processEvents()
    assert capture._active == 2
    assert capture._preview is current
    assert capture.overlay.isVisible()
    capture.main_app.tray_icon.showMessage.assert_not_called()


def test_queued_frame_from_old_preview_cannot_replace_new_session_sample(capture, qtbot, monkeypatch):
    start(capture, qtbot)
    previous = capture._preview
    previous.frame = (image(), QRect(-50, -20, 80, 60))
    previous.frame_ready.emit()
    capture.cancel()
    capture._on_input("start", 2, -30, -10)
    received = Mock()
    monkeypatch.setattr(capture.overlay, "set_sample_image", received)
    module.QApplication.processEvents()
    received.assert_not_called()
    assert capture._active == 2
    assert capture._preview is not previous


def test_stationary_preview_cancels_when_a_modal_appears(capture, qtbot, monkeypatch):
    start(capture, qtbot)
    preview = capture._preview
    monkeypatch.setattr(module.QApplication, "activeModalWidget", lambda: object())
    preview.frame = (image(), QRect(-80, -50, 80, 60))
    preview.frame_ready.emit()
    module.QApplication.processEvents()
    preview.stop.assert_called_once()
    assert capture._active is None
    assert not capture.overlay.isVisible()


def test_close_waits_for_old_and_current_sampling_workers(capture, qtbot):
    start(capture, qtbot)
    previous = capture._preview
    capture.cancel()
    start(capture, qtbot, token=2)
    current = capture._preview
    capture.close()
    previous.wait.assert_called_once()
    current.wait.assert_called_once()


@pytest.mark.parametrize("command", ["cycle_color", "copy_color", "zoom_in", "zoom_out"])
def test_input_commands_reach_only_an_active_capture(capture, qtbot, monkeypatch, command):
    start(capture, qtbot)
    handle = Mock()
    monkeypatch.setattr(capture.overlay, "handle_command", handle)
    capture.input.command.emit(command)
    qtbot.waitUntil(lambda: handle.called)
    handle.assert_called_once_with(command)
    capture.cancel()
    capture.input.command.emit(command)
    module.QApplication.processEvents()
    assert handle.call_count == 1


@pytest.mark.parametrize("include_cursor", [False, True])
def test_cursor_preference_reaches_capture_worker(capture, qtbot, include_cursor):
    capture.main_app.config_manager.set_app_setting("capture_include_cursor", include_cursor)
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    assert worker.include_cursor is include_cursor


def test_edit_delivery_preserves_captured_cursor_and_selection(capture, qtbot):
    capture.main_app.config_manager.set_app_setting("quick_capture_action", "edit")
    capture.main_app.config_manager.set_app_setting("capture_include_cursor", True)
    capture.refresh()
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    cursor = object()
    worker.cursor = cursor
    result = image()
    bounds = QRectF(-100, -100, 500, 500)
    worker.captured.emit(result, bounds)
    qtbot.waitUntil(lambda: capture.main_app._on_capture_ready.called)
    capture.main_app._on_capture_ready.assert_called_once_with(result, bounds, cursor)
    assert capture.main_app.selection.rect() == QRectF(-50, -20, 80, 60)
    assert capture.main_app.selection.is_confirmed


@pytest.mark.parametrize("position,expected", [
    ((100, 120), QRect(52, 72, 96, 96)),
    ((-98, -98), QRect(-100, -100, 50, 50)),
])
def test_live_preview_samples_only_a_clipped_cursor_patch(qapp, monkeypatch, position, expected):
    preview = module.QuickCapturePreview(SimpleNamespace(position=position), QRect(-100, -100, 500, 500))
    pixels = bytearray(b"\x66\x44\x22\xff" * expected.width() * expected.height())
    desktop = Mock()
    context = Mock()
    context.__enter__ = Mock(return_value=desktop)
    context.__exit__ = Mock(return_value=False)

    def grab(_monitor):
        return SimpleNamespace(width=expected.width(), height=expected.height(), bgra=pixels)

    desktop.grab.side_effect = grab
    monkeypatch.setattr("mss.mss", lambda: context)
    preview.frame_ready.connect(preview.stop)
    preview.run()
    desktop.grab.assert_called_once_with(dict(left=expected.x(), top=expected.y(),
                                             width=expected.width(), height=expected.height()))
    result, region = preview.take_frame()
    assert region == expected
    pixels[:] = b"\x00" * len(pixels)
    assert result.pixel(0, 0) == 0xff224466
    context.__exit__.assert_called_once()
    preview.deleteLater()


def test_preview_coalesces_unconsumed_frames_and_rearms_after_consumption(qapp):
    preview = module.QuickCapturePreview(SimpleNamespace(position=(10, 20)), QRect(0, 0, 500, 500))
    notified = Mock()
    preview.frame_ready.connect(notified)
    sample = image()
    for offset in range(100):
        preview._publish_frame(sample, QRect(offset, offset, 80, 60))
    notified.assert_called_once_with()
    assert preview.take_frame() == (sample, QRect(99, 99, 80, 60))
    assert preview.take_frame() is None
    preview._publish_frame(sample, QRect(101, 102, 80, 60))
    assert notified.call_count == 2
    assert preview.take_frame() == (sample, QRect(101, 102, 80, 60))
    preview.stop()
    preview._publish_frame(sample, QRect(103, 104, 80, 60))
    assert preview.take_frame() is None
    assert notified.call_count == 2
    preview.deleteLater()


def test_movement_during_sampling_wakes_next_sample_with_latest_position(qapp, monkeypatch):
    source = SimpleNamespace(position=(100, 120))
    preview = module.QuickCapturePreview(source, QRect(0, 0, 500, 500))
    waits, regions = [], []

    class RecordingWake(threading.Event):
        def wait(self, timeout=None):
            waits.append(self.is_set())
            if len(waits) == 2:
                preview.stop()
            return super().wait(0)

    preview._sample_requested = RecordingWake()

    def grab(region):
        regions.append(region)
        if len(regions) == 1:
            source.position = (220, 200)
            preview.request_sample()
        return SimpleNamespace(width=region["width"], height=region["height"],
                               bgra=b"\x66\x44\x22\xff" * region["width"] * region["height"])

    context = Mock()
    context.__enter__ = Mock(return_value=SimpleNamespace(grab=grab))
    context.__exit__ = Mock(return_value=False)
    monkeypatch.setattr("mss.mss", lambda: context)
    preview.run()
    assert waits == [True, False]
    assert [(region["left"], region["top"]) for region in regions] == [(52, 72), (172, 152)]
    assert preview.take_frame()[1] == QRect(172, 152, 96, 96)
    preview.deleteLater()


def test_stop_wakes_a_sampler_waiting_for_movement(qapp):
    preview = module.QuickCapturePreview(SimpleNamespace(position=(10, 20)), QRect(0, 0, 500, 500))
    assert not preview._sample_requested.is_set()
    preview.stop()
    assert preview._sample_requested.wait(0)
    preview.deleteLater()


def test_stop_during_wakeup_reset_does_not_start_another_grab(qapp, monkeypatch):
    preview = module.QuickCapturePreview(SimpleNamespace(position=(10, 20)), QRect(0, 0, 500, 500))

    class StopOnClear(threading.Event):
        def clear(self):
            preview.stop()
            super().clear()

    preview._sample_requested = StopOnClear()
    desktop = Mock()
    context = Mock()
    context.__enter__ = Mock(return_value=desktop)
    context.__exit__ = Mock(return_value=False)
    monkeypatch.setattr("mss.mss", lambda: context)
    preview.run()
    desktop.grab.assert_not_called()
    assert preview.take_frame() is None
    preview.deleteLater()


def test_stopped_preview_does_not_sample_desktop(qapp, monkeypatch):
    desktop = Mock()
    context = Mock()
    context.__enter__ = Mock(return_value=desktop)
    context.__exit__ = Mock(return_value=False)
    monkeypatch.setattr("mss.mss", lambda: context)
    preview = module.QuickCapturePreview(SimpleNamespace(position=(10, 20)), QRect(0, 0, 500, 500))
    preview.stop()
    preview.run()
    desktop.grab.assert_not_called()
    assert preview.take_frame() is None
    preview.deleteLater()


@pytest.mark.parametrize("cancelled", [False, True])
def test_preview_reports_sample_failure_unless_already_cancelled(qapp, monkeypatch, cancelled):
    preview = module.QuickCapturePreview(SimpleNamespace(position=(10, 20)), QRect(0, 0, 500, 500))
    desktop = Mock()
    context = Mock()
    context.__enter__ = Mock(return_value=desktop)
    context.__exit__ = Mock(return_value=False)

    def grab(_monitor):
        if cancelled:
            preview.stop()
        raise RuntimeError("Synthetic read failure")

    desktop.grab.side_effect = grab
    monkeypatch.setattr("mss.mss", lambda: context)
    failure = Mock()
    preview.failed.connect(failure)
    preview.run()
    if cancelled:
        failure.assert_not_called()
    else:
        failure.assert_called_once_with("Synthetic read failure")
    assert preview.take_frame() is None
    preview.deleteLater()


@pytest.mark.parametrize("action", ["copy", "edit"])
@pytest.mark.parametrize("cursor_available", [False, True])
def test_worker_passes_captured_cursor_to_capture_service(qapp, monkeypatch, action, cursor_available):
    cursor = object() if cursor_available else None
    grab_cursor = Mock(return_value=cursor)
    monkeypatch.setattr("capture.system_cursor.SystemCursor.grab", grab_cursor)
    monkeypatch.setattr(module, "flush_desktop", Mock())
    service = Mock()
    service.capture_region.return_value = image()
    service.capture_all_screens.return_value = image(), QRectF(-100, 0, 500, 400)
    monkeypatch.setattr(module, "CaptureService", lambda: service)
    region = QRect(-50, 10, 80, 60)
    worker = module.QuickCaptureWorker(region, action)
    worker.include_cursor = True
    success = Mock()
    worker.captured.connect(success)
    worker.run()
    grab_cursor.assert_called_once_with()
    assert worker.cursor is cursor
    success.assert_called_once()
    if action == "edit":
        service.capture_all_screens.assert_called_once_with(*((cursor,) if cursor_available else ()))
    else:
        service.capture_region.assert_called_once_with(region, *((cursor,) if cursor_available else ()))
    worker.deleteLater()


@pytest.mark.parametrize("action", ["pin", "copy", "copy_pin", "edit"])
def test_worker_flushes_before_capture_and_keeps_pixels_owned(qapp, monkeypatch, action):
    events = []
    service = Mock()
    service.capture_region.side_effect = lambda rect: events.append("region") or image()
    service.capture_all_screens.side_effect = lambda: (events.append("all") or image(), QRectF(-100, 0, 500, 400))
    monkeypatch.setattr(module, "CaptureService", lambda: service)
    monkeypatch.setattr(module, "flush_desktop", lambda: events.append("flush"))
    worker = module.QuickCaptureWorker(QRect(-50, 10, 80, 60), action)
    received = []
    worker.captured.connect(lambda *args: received.append(args))
    worker.run()
    assert events == ["flush", "all" if action == "edit" else "region"]
    assert received[0][0].pixel(0, 0) == 0xff224466
    if action != "edit":
        service.capture_region.assert_called_once_with(QRect(-50, 10, 80, 60))
    worker.deleteLater()


def test_worker_reports_errors_without_emitting_image(qapp, monkeypatch):
    monkeypatch.setattr(module, "flush_desktop", Mock(side_effect=RuntimeError("capture failed")))
    worker = module.QuickCaptureWorker(QRect(0, 0, 80, 60), "copy")
    failure, success = Mock(), Mock()
    worker.failed.connect(failure)
    worker.captured.connect(success)
    worker.run()
    failure.assert_called_once_with("capture failed")
    success.assert_not_called()
    worker.deleteLater()
