"""退出等待 MP4 封装与 helper 退出，不先销毁应用资源。"""

from types import SimpleNamespace, MethodType
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QObject, QRectF, Signal

from main_app import MainApp


class Recording(QObject):
    closed = Signal()

    def __init__(self, app):
        super().__init__()
        self.app = app
        self.video_busy = True
        self.close_all = Mock()

    def finish(self):
        self.video_busy = False
        self.app._gif_window = None
        self.closed.emit()


def test_quit_waits_for_recording_once_before_resources_are_closed(qapp):
    events = []
    app = SimpleNamespace(_gif_window=None, quit=lambda: events.append("quit"))
    recording = Recording(app)
    app._gif_window = recording
    settings = SimpleNamespace(close=Mock(side_effect=lambda: events.append("settings") or True))
    harness = SimpleNamespace(
        app=app, quick_capture=SimpleNamespace(close=lambda: events.append("close")),
        screenshot_window=None, clipboard_window=None, clipboard_manager=None,
        settings_window=settings, hotkey_system=Mock(),
    )
    harness.quit_app = MethodType(MainApp.quit_app, harness)
    harness.quit_app()
    harness.quit_app()
    assert events == ["settings"]
    assert harness.settings_window is None
    settings.close.assert_called_once()
    assert recording.close_all.call_count == 2
    recording.finish()
    assert events == ["settings", "close", "quit"]
    harness.hotkey_system.unregister_all.assert_called_once()
    recording.deleteLater()


@pytest.mark.parametrize("close_error", [False, True])
def test_cancelled_settings_close_does_not_stop_recording(qapp, monkeypatch, close_error):
    monkeypatch.setattr("main_app.log_exception", Mock())
    app = SimpleNamespace(_gif_window=None, quit=Mock())
    recording = Recording(app)
    app._gif_window = recording
    settings = SimpleNamespace(close=Mock(return_value=False))
    if close_error:
        settings.close.side_effect = RuntimeError("save failed")
    harness = SimpleNamespace(app=app, settings_window=settings, quick_capture=Mock())
    try:
        assert MainApp.quit_app(harness) is False
        assert harness.settings_window is settings and recording.video_busy
        recording.close_all.assert_not_called()
        harness.quick_capture.close.assert_not_called()
        app.quit.assert_not_called()
        assert not hasattr(harness, "_recording_quit_pending")
    finally:
        recording.deleteLater()


def test_recording_entry_does_not_replace_busy_video(qapp):
    from ui.screenshot_window import ScreenshotWindow
    old = SimpleNamespace(video_busy=True, close_all=Mock())
    previous = getattr(qapp, "_gif_window", None)
    qapp._gif_window = old
    capture = SimpleNamespace(
        scene=SimpleNamespace(selection_model=SimpleNamespace(
            is_confirmed=True, rect=lambda: QRectF(0, 0, 320, 240))),
        cleanup_and_close=Mock(),
    )
    try:
        ScreenshotWindow.start_gif_record_mode(capture)
        assert qapp._gif_window is old
        old.close_all.assert_not_called()
        capture.cleanup_and_close.assert_called_once()
    finally:
        qapp._gif_window = previous
