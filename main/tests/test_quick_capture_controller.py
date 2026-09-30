"""Synthetic quick-capture gestures: never install hooks or use the real clipboard."""

from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QObject, QPoint, QRect, QRectF, QSizeF, Signal
from PySide6.QtGui import QImage

from capture import quick_capture_controller as module
from canvas.selection_model import SelectionModel
from settings.tool_settings import QUICK_CAPTURE_ACTIONS, ToolSettingsManager

WIN_LEFT = (frozenset({"win"}), "left")


class FakeInput(QObject):
    event = Signal(str, int, int, int)
    moved = Signal(int)
    failure = Signal(str)
    position = (0, 0)

    def __init__(self, parent):
        super().__init__(parent)
        self.configure = Mock()
        self.set_blocked = Mock()
        self.cancel = Mock()
        self.close = Mock()
        self.accepts = Mock(return_value=True)
        self.gesture_binding = Mock(return_value=WIN_LEFT)
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
            _session_active=True, scene=SimpleNamespace(preset_selection=self.selection.initialize_confirmed_rect),
        )


@pytest.fixture
def capture(qapp, tmp_settings, monkeypatch):
    monkeypatch.setattr(module, "QuickCaptureInput", FakeInput)
    monkeypatch.setattr(module, "QuickCaptureWorker", FakeWorker)
    monkeypatch.setattr(module, "desktop_bounds", lambda: QRect(-100, -100, 500, 500))
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", Mock())
    monkeypatch.setattr(module, "deliver_screenshot", Mock())
    monkeypatch.setattr(module, "set_last_region", Mock())
    monkeypatch.setattr(module, "request_trim_working_set", Mock())
    config = ToolSettingsManager(tmp_settings)
    bind(config, "copy_pin")
    app = FakeApp(config)
    controller = module.QuickCaptureController(app)
    controller.refresh()
    yield controller
    controller.close()


def bind(config, *gestures):
    """bind(config, "pin") 等于只配 Win 左键拖动钉图；也可以传 (手势, 动作) 配多个。"""
    if len(gestures) == 1 and isinstance(gestures[0], str):
        gestures = (("win+dragleft", gestures[0]),)
    bound = {action: gesture for gesture, action in reversed(gestures)}
    for action, *_ in QUICK_CAPTURE_ACTIONS:
        config.set_app_setting(f"quick_capture_{action}", bound.get(action, ""))


def test_default_configuration_binds_win_left_drag_to_pin(qapp, tmp_settings, monkeypatch):
    monkeypatch.setattr(module, "QuickCaptureInput", FakeInput)
    controller = module.QuickCaptureController(FakeApp(ToolSettingsManager(tmp_settings)))
    try:
        controller.refresh()
        assert controller._actions == {WIN_LEFT: "pin"}
        controller.input.configure.assert_called_with(frozenset({WIN_LEFT}), True)
    finally:
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


def test_pending_normal_capture_blocks_input_until_cleared(capture):
    capture.set_capture_pending(True)
    capture.input.set_blocked.assert_called_with(True)
    capture.set_capture_pending(False)
    capture.input.set_blocked.assert_called_with(False)


def image():
    result = QImage(80, 60, QImage.Format.Format_RGB32)
    result.fill(0xff224466)
    return result


@pytest.mark.parametrize("action", ["copy", "pin", "copy_pin", "edit"])
def test_trim_is_requested_once_capture_is_over(capture, qtbot, monkeypatch, action):
    # 转入普通截图时由共用回收的忙碌判断顺延，这里照常请求
    from pin.pin_manager import PinManager
    monkeypatch.setattr(PinManager, "instance", lambda: Mock())
    bind(capture.main_app.config_manager, action)
    capture.refresh()
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    module.request_trim_working_set.assert_not_called()
    worker.captured.emit(image(), QRectF(-100, -100, 500, 500))
    qtbot.waitUntil(lambda: module.set_last_region.called)
    module.request_trim_working_set.assert_not_called()
    worker.finished.emit()
    module.request_trim_working_set.assert_called_once_with(1500)


@pytest.mark.parametrize("ending", ["cancel", "empty", "failure"])
def test_trim_is_requested_after_cancel_empty_selection_and_failure(capture, qtbot, ending):
    start(capture, qtbot)
    if ending == "failure":
        worker = finish(capture, qtbot)
        worker.failed.emit("synthetic capture failure")
        qtbot.waitUntil(lambda: capture.main_app.tray_icon.showMessage.called)
        module.request_trim_working_set.assert_not_called()
        worker.finished.emit()
    elif ending == "empty":
        capture._on_input("finish", 1, -50, -20)
    else:
        capture.cancel()
    module.request_trim_working_set.assert_called_once_with(1500)


def test_closed_controller_requests_no_trim(capture, qtbot):
    start(capture, qtbot)
    capture.close()
    module.request_trim_working_set.assert_not_called()


@pytest.mark.parametrize("action,copy,pin,edit", [
    ("copy", True, False, False), ("pin", False, True, False),
    ("copy_pin", True, True, False), ("edit", False, False, True),
])
def test_actions_use_exact_absolute_selection(capture, qtbot, monkeypatch, action, copy, pin, edit):
    from pin.pin_manager import PinManager
    pin_manager = Mock()
    monkeypatch.setattr(PinManager, "instance", lambda: pin_manager)
    bind(capture.main_app.config_manager, action)
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
    assert module.deliver_screenshot.called == (not edit)
    if not edit:
        assert module.deliver_screenshot.call_args.kwargs["copy_to_clipboard"] == copy
    assert pin_manager.create_pin.called == pin
    assert capture.main_app._on_capture_ready.called == edit
    if pin:
        # 钉图自动识别文字仍按钉图自己的设置，不由快捷键决定
        assert pin_manager.create_pin.call_args.args[1:] == (QPoint(-50, -20), capture.main_app.config_manager)
        assert not pin_manager.create_pin.call_args.kwargs
    if edit:
        assert capture.main_app.selection.rect() == QRectF(-50, -20, 80, 60)
        assert capture.main_app.selection.is_confirmed
    worker.finished.emit()
    qtbot.waitUntil(lambda: not capture.busy)


@pytest.mark.parametrize("copy_directly", [False, True])
def test_recognize_text_follows_the_copy_directly_setting(capture, qtbot, monkeypatch, copy_directly):
    import text_recognition
    show, copy = Mock(), Mock()
    monkeypatch.setattr(text_recognition, "show_text_recognition", show)
    monkeypatch.setattr(text_recognition, "copy_text_recognition", copy)
    config = capture.main_app.config_manager
    config.set_ocr_copy_directly_enabled(copy_directly)
    bind(config, "ocr")
    capture.refresh()
    start(capture, qtbot)
    result = image()
    finish(capture, qtbot).captured.emit(result, QRectF(-50, -20, 80, 60))
    qtbot.waitUntil(lambda: module.set_last_region.called)
    (copy if copy_directly else show).assert_called_once_with(result)
    (show if copy_directly else copy).assert_not_called()
    module.deliver_screenshot.assert_not_called()


def test_translate_opens_translation_with_the_selected_pixels(capture, qtbot, monkeypatch):
    from translation import TranslationManager
    manager = Mock()
    monkeypatch.setattr(TranslationManager, "instance", lambda: manager)
    config = capture.main_app.config_manager
    bind(config, "translate")
    capture.refresh()
    start(capture, qtbot)
    finish(capture, qtbot).captured.emit(image(), QRectF(-50, -20, 80, 60))
    qtbot.waitUntil(lambda: module.set_last_region.called)
    manager.translate_from_image.assert_called_once()
    kwargs = manager.translate_from_image.call_args.kwargs
    assert kwargs["pixmap"].size().toTuple() == (80, 60)
    assert {key: kwargs[key] for key in kwargs if key != "pixmap"} == config.get_translation_request_params()
    module.deliver_screenshot.assert_not_called()


@pytest.mark.parametrize("auto_save", [True, False])
@pytest.mark.parametrize("action,copy", [("copy", True), ("pin", False), ("copy_pin", True)])
def test_auto_save_applies_to_every_quick_capture_action(capture, qtbot, monkeypatch, tmp_path,
                                                        action, copy, auto_save):
    from core import clipboard_utils
    from pin.pin_manager import PinManager
    monkeypatch.setattr(PinManager, "instance", lambda: Mock())
    monkeypatch.setattr(clipboard_utils, "copy_image_to_clipboard", Mock())
    monkeypatch.setattr(module, "deliver_screenshot", clipboard_utils.deliver_screenshot)
    config = capture.main_app.config_manager
    config.set_screenshot_save_enabled(auto_save)
    folder = tmp_path / "captures"
    config.set_screenshot_save_path(str(folder))
    bind(config, action)
    capture.refresh()
    start(capture, qtbot)
    finish(capture, qtbot).captured.emit(image(), QRectF(-100, -100, 500, 500))
    qtbot.waitUntil(lambda: module.set_last_region.called)

    def saved():
        return [QImage(str(path)).size().toTuple() for path in folder.glob("*")] if folder.exists() else []

    if auto_save:
        qtbot.waitUntil(lambda: saved() == [(80, 60)])
    else:
        assert saved() == []
    assert clipboard_utils.copy_image_to_clipboard.called == copy


def test_hotkey_pause_disarms_the_default_binding(capture):
    bind(capture.main_app.config_manager, "pin")
    capture.main_app.config_manager.set_app_setting("global_hotkeys_disabled", True)
    capture.refresh()
    assert not capture._enabled
    assert capture.input.configure.call_args.args[1] is False
    capture._on_input("start", 1, 10, 10)
    assert not capture.busy


@pytest.mark.parametrize("gesture", [
    "", "dragleft", "win+left", "win+dragx3", "unknown+dragleft", "ctrl+alt+win+dragleft", "win+",
])
def test_invalid_bindings_do_not_arm(capture, gesture):
    bind(capture.main_app.config_manager, (gesture, "pin"))
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


def test_stale_or_consumed_motion_cannot_redraw(capture, qtbot, monkeypatch):
    start(capture, qtbot, token=2)
    rendered = Mock()
    monkeypatch.setattr(capture.overlay, "show_selection", rendered)
    move(capture, token=1)
    capture.input.take_position.return_value = None
    capture.input.take_position.side_effect = None
    move(capture, token=2)
    rendered.assert_not_called()


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


def test_every_binding_reaches_input_and_a_repeated_combination_keeps_the_first(capture):
    bind(capture.main_app.config_manager,
         ("win+dragleft", "pin"), ("ctrl+win+dragleft", "copy"), ("win+dragright", "ocr"),
         ("win+dragleft", "edit"))
    capture.refresh()
    capture.input.configure.assert_called_with(frozenset({
        WIN_LEFT, (frozenset({"ctrl", "win"}), "left"), (frozenset({"win"}), "right"),
    }), True)
    assert capture._actions[WIN_LEFT] == "pin"
    assert "edit" not in capture._actions.values()


def test_each_gesture_runs_the_action_of_its_binding(capture, qtbot):
    bind(capture.main_app.config_manager, ("win+dragleft", "pin"), ("win+dragx1", "copy"))
    capture.refresh()
    capture.input.gesture_binding.return_value = (frozenset({"win"}), "x1")
    start(capture, qtbot)
    assert finish(capture, qtbot).action == "copy"


def test_start_matching_no_gesture_is_cancelled(capture):
    capture.input.gesture_binding.return_value = None
    capture._on_input("start", 1, 10, 10)
    assert capture._active is None
    capture.input.cancel.assert_called()


def test_click_without_drag_never_captures(capture, qtbot):
    start(capture, qtbot)
    capture._on_input("finish", 1, -50, -20)
    assert not capture.busy
    assert not capture.overlay.isVisible()


def test_overlay_that_cannot_be_excluded_still_hides_before_capture(capture, qtbot, monkeypatch):
    monkeypatch.setattr("ui.quick_capture_overlay.set_window_exclude_from_capture", lambda *_args: False)
    start(capture, qtbot)
    assert capture.overlay.isVisible()
    worker = finish(capture, qtbot)
    assert worker.region == QRect(-50, -20, 80, 60)
    assert not capture.overlay.isVisible()


def test_small_edit_region_does_not_expand_past_screen_edge(capture, qtbot):
    bind(capture.main_app.config_manager, "edit")
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
    module.deliver_screenshot.assert_not_called()
    module.set_last_region.assert_not_called()


def test_cannot_start_during_other_capture_or_modal(capture, monkeypatch):
    capture.main_app.screenshot_window = SimpleNamespace(_session_active=True)
    capture._on_input("start", 1, 10, 10)
    assert not capture.busy
    capture.main_app.screenshot_window = None
    monkeypatch.setattr(module.QApplication, "activeModalWidget", lambda: object())
    capture._on_input("start", 2, 10, 10)
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


@pytest.mark.parametrize("include_cursor", [False, True])
@pytest.mark.parametrize("action,cursor_allowed", [("copy", True), ("edit", True), ("ocr", False),
                                                    ("translate", False)])
def test_cursor_preference_reaches_capture_worker(capture, qtbot, include_cursor, action, cursor_allowed):
    bind(capture.main_app.config_manager, action)
    capture.main_app.config_manager.set_app_setting("capture_include_cursor", include_cursor)
    capture.refresh()
    start(capture, qtbot)
    worker = finish(capture, qtbot)
    assert worker.include_cursor is (include_cursor and cursor_allowed)


def test_edit_delivery_preserves_captured_cursor_and_selection(capture, qtbot):
    bind(capture.main_app.config_manager, "edit")
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
        service.capture_all_screens.assert_called_once_with(cursor)
    else:
        service.capture_region.assert_called_once_with(region, cursor)
    worker.deleteLater()


@pytest.mark.parametrize("action", ["pin", "copy", "copy_pin", "ocr", "translate", "edit"])
def test_worker_flushes_before_capture_and_keeps_pixels_owned(qapp, monkeypatch, action):
    events = []
    service = Mock()
    service.capture_region.side_effect = lambda rect, cursor: events.append("region") or image()
    service.capture_all_screens.side_effect = lambda cursor: (events.append("all") or image(),
                                                              QRectF(-100, 0, 500, 400))
    monkeypatch.setattr(module, "CaptureService", lambda: service)
    monkeypatch.setattr(module, "flush_desktop", lambda: events.append("flush"))
    worker = module.QuickCaptureWorker(QRect(-50, 10, 80, 60), action)
    received = []
    worker.captured.connect(lambda *args: received.append(args))
    worker.run()
    assert events == ["flush", "all" if action == "edit" else "region"]
    assert received[0][0].pixel(0, 0) == 0xff224466
    if action != "edit":
        service.capture_region.assert_called_once_with(QRect(-50, 10, 80, 60), None)
        assert received[0][1] == QRectF(-50, 10, 80, 60)
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
