"""MP4 进程协议与界面生命周期，使用合成文件，不采集真实桌面或声音。"""

import json
from concurrent.futures import Future
from types import MethodType, SimpleNamespace
from unittest.mock import Mock

import pytest
from PySide6.QtCore import QObject, QProcess, QRect, Signal
from PySide6.QtWidgets import QCheckBox, QDialogButtonBox, QDoubleSpinBox, QSpinBox, QFileDialog

from gif.video_recorder import MAX_MESSAGE, MAX_STDERR, VideoRecorder, failure_message
from gif.video_settings import RecordingOptions


class FakeProcess(QObject):
    readyReadStandardOutput = Signal()
    readyReadStandardError = Signal()
    errorOccurred = Signal(object)
    finished = Signal(int, object)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.stdout = b""
        self.stderr = b""
        self.commands = []
        self.killed = False
        self.wait_result = True

    def setProcessChannelMode(self, mode):
        self.channel_mode = mode

    def setProcessEnvironment(self, environment):
        self.environment = environment

    def setWorkingDirectory(self, directory):
        self.directory = directory

    def start(self, executable, arguments):
        self.executable = executable
        self.arguments = arguments

    def write(self, line):
        self.commands.append(json.loads(line))
        return len(line)

    def readAllStandardOutput(self):
        chunk, self.stdout = self.stdout, b""
        return chunk

    def readAllStandardError(self):
        chunk, self.stderr = self.stderr, b""
        return chunk

    def emit_event(self, event, **data):
        self.stdout += (json.dumps({"protocol": 1, "event": event, "data": data}) + "\n").encode()
        self.readyReadStandardOutput.emit()

    def exit(self, code=0, status=QProcess.ExitStatus.NormalExit):
        self.finished.emit(code, status)

    def kill(self):
        self.killed = True

    def closeWriteChannel(self):
        self.input_closed = True

    def waitForFinished(self, milliseconds):
        self.wait_time = milliseconds
        return self.wait_result


@pytest.fixture(autouse=True)
def _gdi_capture(monkeypatch):
    """启动录制会查询截图引擎设置与显示器 HDR 状态，测试里固定为 GDI。"""
    monkeypatch.setattr("gif.video_recorder.uses_hdr_engine", lambda: False)


@pytest.fixture
def recorder(qapp, tmp_path):
    helper = tmp_path / "helper.exe"
    helper.write_bytes(b"synthetic helper")
    obj = VideoRecorder(process_factory=FakeProcess, worker_command=[str(helper)])
    completed, failed, finished = [], [], []
    obj.completed.connect(completed.append)
    obj.failed.connect(lambda code, message: failed.append((code, message)))
    obj.finished.connect(lambda: finished.append(True))
    obj._test_results = (completed, failed, finished)
    yield obj
    obj._timer.stop()
    obj._kill_timer.stop()
    obj.deleteLater()


def start(recorder, tmp_path, options=None):
    output = tmp_path / "录像.mp4"
    assert recorder.start(QRect(-100, 40, 303, 205), str(output), options or RecordingOptions())
    return recorder._process, output


def test_new_path_and_even_region_are_passed_without_shell(recorder, tmp_path, monkeypatch):
    monkeypatch.setenv("_PYI_APPLICATION_HOME_DIR", "old extraction")
    process, output = start(recorder, tmp_path, RecordingOptions(fps=17, bitrate=1_500_000, system_audio=False))
    args = dict(zip(process.arguments[::2], process.arguments[1::2]))
    assert args["--output"] == str(output)
    assert args["--left"] == "-99" and args["--top"] == "41"
    assert args["--width"] == "300" and args["--height"] == "202"
    assert args["--fps"] == "17" and args["--bitrate"] == "1500000"
    assert args["--audio"] == "none"
    assert process.environment.value("_PYI_APPLICATION_HOME_DIR") == "old extraction"
    assert recorder.state == "starting"
    assert not recorder.start(QRect(0, 0, 20, 20), str(tmp_path / "other.mp4"), RecordingOptions())


@pytest.mark.parametrize("hdr, capture", [(True, "dxgi"), (False, "gdi")])
def test_capture_backend_follows_the_capture_engine(recorder, tmp_path, monkeypatch, hdr, capture):
    monkeypatch.setattr("gif.video_recorder.uses_hdr_engine", lambda: hdr)
    process, _ = start(recorder, tmp_path)
    args = dict(zip(process.arguments[::2], process.arguments[1::2]))
    assert args["--capture"] == capture


@pytest.mark.parametrize("problem", ["missing_package", "missing_file", "invalid_record"])
def test_worker_failure_reports_existing_error_without_starting(recorder, tmp_path, monkeypatch, problem):
    from gif import video_recorder
    recorder._worker_command = None

    def missing_worker():
        if problem == "missing_package":
            raise ImportError("video_recorder")
        if problem == "invalid_record":
            raise ValueError("invalid module spec")
        return [str(tmp_path / "absent.exe")]

    monkeypatch.setattr(video_recorder, "default_worker_command", missing_worker)
    assert not recorder.start(QRect(0, 0, 20, 20), str(tmp_path / "new.mp4"), RecordingOptions())
    assert recorder._test_results[1][0][0] == "helper_missing"
    assert recorder._process is None and not recorder.active


def test_worker_entry_arguments_precede_recording_options(recorder, tmp_path):
    recorder._worker_command.extend(["--video-recorder-worker"])
    process, _ = start(recorder, tmp_path)
    assert process.arguments[:2] == ["--video-recorder-worker", "--output"]


def test_existing_output_is_never_truncated(recorder, tmp_path):
    output = tmp_path / "existing.mp4"
    output.write_bytes(b"keep")
    assert not recorder.start(QRect(0, 0, 20, 20), str(output), RecordingOptions())
    assert output.read_bytes() == b"keep"
    assert recorder._test_results[1][0][0] == "output_exists"
    assert not recorder.active


def test_complete_waits_for_successful_process_exit(recorder, tmp_path):
    process, output = start(recorder, tmp_path)
    process.emit_event("ready")
    assert recorder.state == "recording"
    recorder.stop()
    output.write_bytes(b"synthetic mp4")
    process.emit_event("complete", output=str(output), finalized=True)
    assert recorder.active and recorder._test_results[0] == []
    process.exit()
    assert recorder._test_results == ([str(output)], [], [True])
    assert not recorder.active


@pytest.mark.parametrize("case", ["exit_only", "wrong_path", "not_finalized", "missing_file", "empty_file", "crash", "missing_output", "legacy_path"])
def test_incomplete_or_crashed_video_never_reports_success(recorder, tmp_path, case):
    process, output = start(recorder, tmp_path)
    process.emit_event("ready")
    if case != "exit_only":
        if case != "missing_file":
            output.write_bytes(b"" if case == "empty_file" else b"mp4")
        data = {"finalized": case != "not_finalized"}
        if case == "legacy_path":
            data["path"] = str(output)
        elif case != "missing_output":
            data["output"] = str(output if case != "wrong_path" else tmp_path / "other.mp4")
        process.emit_event("complete", **data)
    process.exit(1 if case == "crash" else 0)
    assert recorder._test_results[0] == []
    assert len(recorder._test_results[1]) == 1
    assert not recorder.active


def test_pause_resume_state_follows_ack_and_late_ack_during_stop_is_ignored(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    paused = []
    recorder.paused_changed.connect(paused.append)
    process.emit_event("ready")
    assert recorder.pause(True)
    assert recorder.state == "pausing" and paused == []
    assert not recorder.pause(False)
    process.emit_event("paused", elapsed_ms=200)
    assert recorder.state == "paused" and paused == [True]
    assert recorder.pause(False)
    process.emit_event("resumed", elapsed_ms=200)
    assert recorder.state == "recording" and paused == [True, False]
    recorder.pause(True)
    recorder.stop()
    process.emit_event("paused", elapsed_ms=200)
    assert recorder.state == "stopping" and paused == [True, False]
    assert [m["command"] for m in process.commands] == ["pause", "resume", "pause", "stop"]


def test_split_json_lines_and_progress_are_handled_without_frame_buffers(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    progress = []
    recorder.progress.connect(progress.append)
    process.stdout = b'{"protocol":1,"event":"rea'
    process.readyReadStandardOutput.emit()
    assert recorder.state == "starting"
    process.stdout = b'dy","data":{}}\n{"protocol":1,"event":"progress","data":{"elapsed_ms":1500}}\n'
    process.readyReadStandardOutput.emit()
    assert recorder.state == "recording" and progress == [1.5]
    assert not recorder._stdout


@pytest.mark.parametrize("elapsed", [-1, True, "100", float("nan"), float("inf")])
def test_invalid_progress_fails_protocol(recorder, tmp_path, elapsed):
    process, _ = start(recorder, tmp_path)
    process.emit_event("progress", elapsed_ms=elapsed)
    assert process.commands[-1]["command"] == "cancel"
    process.exit()
    assert recorder._test_results[1][0][0] == "protocol"


@pytest.mark.parametrize("payload", [b"bad\n", b'[]\n', b'{"protocol":2,"event":"ready"}\n',
                                      b"x" * (MAX_MESSAGE + 1), b"x" * (MAX_MESSAGE + 1) + b"\n"])
def test_invalid_or_oversized_protocol_is_bounded(recorder, tmp_path, payload):
    process, _ = start(recorder, tmp_path)
    process.stdout = payload
    process.readyReadStandardOutput.emit()
    assert len(recorder._stdout) <= MAX_MESSAGE
    process.exit()
    assert recorder._test_results[1][0][0] == "protocol"


def test_stderr_is_kept_in_a_bounded_ring(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    process.stderr = b"a" * MAX_STDERR + b"tail"
    process.readyReadStandardError.emit()
    assert len(recorder._stderr) == MAX_STDERR and recorder.stderr_tail.endswith("tail")


def test_error_waits_for_child_exit_then_reports_backend_failure(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    process.emit_event("error", code="disk_full", message="disk full")
    assert recorder.active and recorder._test_results[1] == []
    process.exit(1)
    assert recorder._test_results[1] == [("disk_full", "disk full")]


@pytest.mark.parametrize("code,hint", [
    ("audio_device", "关闭系统声音"), ("audio_format", "立体声"),
    ("audio", "播放设备"), ("audio_timestamp", "重新连接"),
    ("encoding", "降低分辨率"), ("capture", "屏幕连接"),
    ("output", "写入权限"), ("disk_full", "空间不足"),
    ("arguments", "重新调整"), ("unrecognized_backend", "查看日志"),
])
def test_backend_errors_offer_translated_recovery_steps(monkeypatch, code, hint):
    translate = Mock(side_effect=lambda text: "translated: " + text)
    monkeypatch.setattr("gif.video_recorder._tr", translate)
    result = failure_message(code, "raw native error 0x80004005")
    assert result.startswith("translated: ") and hint in result
    assert "raw native error" not in result


@pytest.mark.parametrize("paused,event", [(True, "paused"), (False, "resumed")])
def test_pause_ack_restores_elapsed_display(recorder, tmp_path, paused, event):
    process, _ = start(recorder, tmp_path)
    process.emit_event("ready")
    if not paused:
        recorder.pause(True)
        process.emit_event("paused", elapsed_ms=1250)
    elapsed = []
    recorder.progress.connect(elapsed.append)
    assert recorder.pause(paused)
    process.emit_event(event, elapsed_ms=1250)
    assert elapsed == [1.25]


@pytest.mark.parametrize("elapsed", [None, True, -1, float("nan"), "100"])
def test_invalid_pause_ack_does_not_confirm_pause(recorder, tmp_path, elapsed):
    process, _ = start(recorder, tmp_path)
    process.emit_event("ready")
    recorder.pause(True)
    acknowledgements = []
    recorder.paused_changed.connect(acknowledgements.append)
    process.emit_event("paused", elapsed_ms=elapsed)
    assert acknowledgements == [] and recorder.state == "stopping"
    process.exit(1)
    assert recorder._test_results[1][0][0] == "protocol"


def test_pause_timeout_is_reported_as_command_failure(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    process.emit_event("ready")
    recorder.pause(True)
    recorder._on_timeout()
    process.exit(1)
    assert recorder._test_results[1][0][0] == "command_timeout"


def test_start_failure_and_finalize_timeout_cleanup(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    process.errorOccurred.emit(QProcess.ProcessError.FailedToStart)
    assert recorder._test_results[1][0][0] == "start_failed"
    process, _ = start(recorder, tmp_path)
    process.emit_event("ready")
    recorder.stop()
    recorder._on_timeout()
    recorder._kill_unresponsive()
    assert process.killed
    process.exit(-1, QProcess.ExitStatus.CrashExit)
    assert recorder._test_results[1][-1][0] == "finalize_timeout"


def test_stop_during_start_does_not_reenable_recording(recorder, tmp_path):
    process, _ = start(recorder, tmp_path)
    recorder.stop()
    assert process.input_closed
    process.emit_event("ready")
    assert recorder.state == "stopping"


@pytest.mark.parametrize("state", ["recording", "stopping"])
def test_close_record_window_waits_for_video_finalize(qapp, monkeypatch, state):
    from gif.record_window import GifRecordWindow
    from gif.frame_recorder import FrameRecorder
    from gif import record_window
    from main_app import MainApp
    monkeypatch.setattr(FrameRecorder, "prepare", lambda self: None)
    monkeypatch.setattr(record_window, "_request_trim", lambda *args: None)
    monkeypatch.setattr("core.background_tasks.busy", lambda: False)
    window = GifRecordWindow(QRect(40, 40, 320, 200))
    qapp._gif_window = window
    closed = []
    window.closed.connect(lambda: closed.append(True))
    window._video._state = state
    app = SimpleNamespace(app=qapp, _capture_busy=lambda: False, tr=lambda text: text)
    app.update_busy_reason = MethodType(MainApp.update_busy_reason, app)
    allowed, reason = MainApp.prepare_for_update(app)
    assert not allowed and "Finish recording" in reason
    stops = []
    monkeypatch.setattr(window._video, "stop", lambda: stops.append(True))
    window.close_all()
    assert stops == [True] and window.video_busy
    assert window._record_toolbar is not None and closed == []
    assert qapp._gif_window is window
    window._video._state = "idle"
    window._on_video_finished()
    assert closed == [True] and qapp._gif_window is None
    assert window._record_toolbar is None
    assert app.update_busy_reason() == ""


def test_empty_gif_warning_keeps_upstream_translation_context(monkeypatch):
    from gif.frame_recorder import RecordState
    from gif.record_window import GifRecordWindow
    from gif.overlay import OverlayMode
    translate = Mock(side_effect=lambda text, context: f"{context}: {text}")
    warning = Mock()
    monkeypatch.setattr("core.i18n.tr", translate)
    monkeypatch.setattr("ui.dialogs.show_warning_dialog", warning)
    window = SimpleNamespace(
        _video=SimpleNamespace(active=False),
        _recorder=SimpleNamespace(state=RecordState.RECORDING, frames=[],
                                  store=SimpleNamespace(frame_count=0)),
        _show_stop_progress=Mock(), _overlay=Mock(), _record_toolbar=Mock(),
    )
    GifRecordWindow._on_record_stop(window)
    warning.assert_called_once_with(None, "GifRecorder: Recording error",
                                    "GifRecorder: The recording is empty. Please record again.")
    window._overlay.set_recording.assert_called_once_with(False)
    window._overlay.set_mode.assert_called_once_with(OverlayMode.RESIZE)
    window._record_toolbar.reset_state.assert_called_once()


def test_pending_video_waits_for_gif_release_and_close_cancels_start(qapp, monkeypatch, tmp_path):
    from gif.record_window import GifRecordWindow
    from gif.frame_recorder import FrameRecorder
    from gif import record_window
    monkeypatch.setattr(FrameRecorder, "prepare", lambda self: None)
    monkeypatch.setattr(record_window, "_request_trim", lambda *args: None)
    window = GifRecordWindow(QRect(40, 40, 320, 200))
    future = Future()
    window._gif_release = future
    window._pending_video = (str(tmp_path / "new.mp4"), RecordingOptions(), record_window.time.monotonic())
    starts = []
    monkeypatch.setattr(window._video, "start", lambda *args: starts.append(args))
    window._start_pending_video()
    assert window.video_busy and starts == [] and window._release_timer.isActive()
    window.close_all()
    future.set_result(None)
    assert window._pending_video is None and starts == []
    assert not window._release_timer.isActive()


def test_video_toolbar_mode_preserves_gif_fps_and_custom_video_fps(qapp, monkeypatch):
    from gif.record_toolbar import RecordToolbar
    from settings import get_tool_settings_manager
    config = get_tool_settings_manager()
    previous = config.get_app_setting("recording_format", "gif")
    previous_options = RecordingOptions.from_config(config)
    config.set_app_setting("recording_format", "gif")
    toolbar = RecordToolbar()
    try:
        gif_fps = toolbar.get_current_fps()
        toolbar._on_mode_selected("mp4")
        toolbar._on_fps_selected(17)
        assert toolbar.get_current_fps() == 17
        toolbar._on_mode_selected("gif")
        assert toolbar.get_current_fps() == gif_fps
        toolbar._on_mode_selected("mp4")
        assert toolbar.get_video_options().fps == 17
        toolbar.set_busy("saving")
        assert not toolbar._record_btn.isEnabled() and not toolbar._mode_btn.isEnabled()
        toolbar.reset_state()
        assert toolbar._record_btn.isEnabled() and toolbar._mode_btn.isEnabled()
    finally:
        config.set_app_setting("recording_format", previous)
        previous_options.save_to_config(config)
        for panel in toolbar._iter_panels():
            panel.deleteLater()
        toolbar.deleteLater()


@pytest.mark.parametrize("saved_fps", [17, 48, 60])
@pytest.mark.parametrize("initial_mode", ["gif", "mp4"])
def test_old_gif_fps_outside_menu_falls_back_at_start_and_after_mode_switch(qapp, monkeypatch, saved_fps, initial_mode):
    from gif.record_toolbar import RecordToolbar
    from settings import get_tool_settings_manager
    config = get_tool_settings_manager()
    previous = config.get_app_setting("recording_format", "gif")
    config.set_app_setting("recording_format", initial_mode)
    monkeypatch.setattr(config, "get_gif_fps", lambda: saved_fps)
    toolbar = None
    try:
        toolbar = RecordToolbar()
        fallback = config.get_gif_fps_options()[0]
        if initial_mode == "gif":
            assert toolbar.get_current_fps() == fallback
            assert toolbar._fps_btn.current_value() == fallback
        toolbar._mode_btn._on_select(1)
        assert toolbar.get_current_fps() == toolbar.get_video_options().fps
        toolbar._mode_btn._on_select(0)
        assert toolbar.get_current_fps() == fallback
        assert toolbar._fps_btn.current_value() == fallback
    finally:
        config.set_app_setting("recording_format", previous)
        if toolbar is not None:
            for panel in toolbar._iter_panels():
                panel.deleteLater()
            toolbar.deleteLater()


@pytest.fixture
def record_window(qapp, monkeypatch, tmp_path):
    from gif.record_window import GifRecordWindow
    from gif.frame_recorder import FrameRecorder
    from gif import record_window
    from settings import get_tool_settings_manager
    config = get_tool_settings_manager()
    previous = config.get_app_setting("recording_format", "gif")
    previous_options = RecordingOptions.from_config(config)
    config.set_app_setting("recording_format", "gif")
    monkeypatch.setattr(FrameRecorder, "prepare", lambda self: None)
    monkeypatch.setattr(record_window, "_request_trim", lambda *args: None)
    monkeypatch.setattr("ui.dialogs.show_warning_dialog", lambda *args: None)
    monkeypatch.setattr("ui.dialogs.show_info_dialog", lambda *args: None)
    monkeypatch.setattr("ui.dialogs.show_modeless_warning_dialog", lambda *args, **kwargs: None)

    class OffscreenSaveDialog(QFileDialog):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self.setOption(QFileDialog.Option.DontUseNativeDialog, True)

    monkeypatch.setattr(record_window, "QFileDialog", OffscreenSaveDialog)
    window = GifRecordWindow(QRect(40, 40, 320, 200))
    helper = tmp_path / "helper.exe"
    helper.write_bytes(b"helper")
    window._video._worker_command = [str(helper)]
    window._video._process_factory = FakeProcess
    qapp._gif_window = window
    yield window
    if window._video._process is not None:
        window._video._process.exit(1)
    if window._recorder is not None:
        window.close_all()
    config.set_app_setting("recording_format", previous)
    previous_options.save_to_config(config)


def test_real_toolbar_controls_mp4_record_pause_resume_and_finalize(record_window, tmp_path):
    window = record_window
    toolbar = window._record_toolbar
    output = tmp_path / "record.mp4"
    toolbar._mode_btn._on_select(1)
    assert toolbar.get_recording_mode() == "mp4"
    toolbar._record_btn.click()
    assert window.video_busy and window._video._process is None
    window._save_dialog.selectFile(str(output))
    window._save_dialog.accept()
    process = window._video._process
    assert process is not None and window.video_busy
    assert not toolbar._record_btn.isEnabled()
    process.emit_event("ready")
    assert toolbar._record_btn.isEnabled() and toolbar._pause_btn.isEnabled()
    assert window._overlay._color.name() == "#f44336"
    toolbar._pause_btn.click()
    assert window._video.state == "pausing"
    assert not toolbar._paused and window._overlay._color.name() == "#f44336"
    assert toolbar._record_btn.isEnabled() and not toolbar._pause_btn.isEnabled()
    process.emit_event("paused", elapsed_ms=1250)
    assert toolbar._paused and toolbar._pause_btn.isEnabled()
    assert window._overlay._color.name() == "#e5ad32"
    assert toolbar._time_label.text() == "00:01"
    toolbar._pause_btn.click()
    assert toolbar._paused and window._overlay._color.name() == "#e5ad32"
    process.emit_event("resumed", elapsed_ms=1250)
    assert not toolbar._paused
    assert window._overlay._color.name() == "#f44336"
    assert toolbar._time_label.text() == "00:01"
    process.emit_event("progress", elapsed_ms=2500)
    assert toolbar._time_label.text() == "00:02"
    toolbar._record_btn.click()
    assert window._video.state == "stopping" and not toolbar._record_btn.isEnabled()
    label = toolbar._time_label.text()
    process.emit_event("progress", elapsed_ms=3500)
    assert toolbar._time_label.text() == label
    output.write_bytes(b"mp4")
    process.emit_event("complete", output=str(output), finalized=True)
    assert window.video_busy
    process.exit()
    assert not window.video_busy and toolbar._mode_btn.isEnabled()
    assert toolbar._record_btn.isEnabled() and not toolbar._pause_btn.isEnabled()
    assert toolbar._result_container.isVisible() and not toolbar._container.isVisible()


def test_quick_sound_and_cursor_controls_preserve_custom_encoding_options(record_window):
    from settings import get_tool_settings_manager
    toolbar = record_window._record_toolbar
    toolbar._mode_btn._on_select(1)
    original = RecordingOptions(fps=17, bitrate=1_500_000, system_audio=True, cursor=True)
    toolbar._video_options = original
    toolbar._refresh_recording_options()
    toolbar._show_audio_menu()
    toolbar._option_menu.actions()[0].trigger()
    toolbar._close_option_menu()
    toolbar._cursor_btn.click()
    expected = RecordingOptions(fps=17, bitrate=1_500_000, system_audio=False, cursor=False)
    assert toolbar.get_video_options() == expected
    assert RecordingOptions.from_config(get_tool_settings_manager()) == expected
    toolbar.set_busy("starting")
    toolbar._cursor_btn.click()
    toolbar._show_audio_menu()
    assert toolbar._option_menu is None and toolbar.get_video_options() == expected
    assert not toolbar._time_label.isEnabled()
    toolbar.reset_state()
    assert toolbar._time_label.isEnabled()


@pytest.mark.parametrize("mode", ["gif", "mp4"])
@pytest.mark.parametrize("cancel", ["stop", "close"])
def test_countdown_cancel_never_starts_capture_or_leaves_delayed_callback(record_window, monkeypatch, tmp_path, mode, cancel):
    window = record_window
    toolbar = window._record_toolbar
    monkeypatch.setattr(toolbar, "get_start_delay", lambda: 2)
    gif_start = Mock()
    monkeypatch.setattr(window._recorder, "start", gif_start)
    if mode == "mp4":
        toolbar._mode_btn._on_select(1)
    toolbar._record_btn.click()
    output = tmp_path / "countdown.mp4"
    if mode == "mp4":
        window._save_dialog.selectFile(str(output))
        window._save_dialog.accept()
        assert window.video_busy
    assert window._countdown_timer.isActive() and window._video._process is None
    assert not toolbar._pause_btn.isEnabled() and toolbar._record_btn.isEnabled()
    before = QRect(window._rect)
    window._on_rect_changed(QRect(10, 10, 100, 100))
    assert window._rect == before
    window.close_all() if cancel == "close" else toolbar._record_btn.click()
    window._advance_countdown()  # 已排队的 timer 也不能继续启动
    gif_start.assert_not_called()
    assert window._video._process is None and not output.exists()
    assert not window._countdown_timer.isActive() and window._countdown_action is None
    if cancel == "stop":
        assert not window.video_busy and toolbar._mode_btn.isEnabled()


def test_mp4_countdown_runs_after_path_selection_and_starts_once(record_window, monkeypatch, tmp_path):
    window = record_window
    toolbar = window._record_toolbar
    monkeypatch.setattr(toolbar, "get_start_delay", lambda: 2)
    toolbar._mode_btn._on_select(1)
    toolbar._record_btn.click()
    assert window._save_dialog is not None and not window._countdown_timer.isActive()
    window._save_dialog.selectFile(str(tmp_path / "countdown.mp4"))
    window._save_dialog.accept()
    assert window._video._process is None
    window._advance_countdown()
    assert window._video._process is None and window._countdown_remaining == 1
    window._advance_countdown()
    process = window._video._process
    assert process is not None and window._video.state == "starting"
    window._advance_countdown()
    assert window._video._process is process and not window._countdown_timer.isActive()


def test_video_result_waits_for_helper_exit_and_can_open_copy_and_rerecord(record_window, tmp_path, monkeypatch, qapp):
    from PySide6.QtCore import QUrl
    from gif import record_window as module
    window = record_window
    toolbar = window._record_toolbar
    output = tmp_path / "成品.mp4"
    toolbar._mode_btn._on_select(1)
    toolbar._record_btn.click()
    window._save_dialog.selectFile(str(output))
    window._save_dialog.accept()
    process = window._video._process
    process.emit_event("ready")
    toolbar._record_btn.click()
    output.write_bytes(b"mp4")
    process.emit_event("complete", output=str(output), finalized=True)
    assert not toolbar._result_container.isVisible()
    process.exit()
    assert toolbar._result_container.isVisible() and not window.video_busy
    opened = []
    monkeypatch.setattr(module.QDesktopServices, "openUrl", lambda url: opened.append(url) or True)
    toolbar._result_buttons[0].click()
    toolbar._result_buttons[1].click()
    assert opened == [QUrl.fromLocalFile(str(output)), QUrl.fromLocalFile(str(tmp_path))]
    toolbar._result_buttons[2].click()
    assert qapp.clipboard().mimeData().urls() == [QUrl.fromLocalFile(str(output))]
    qapp.clipboard().clear()
    toolbar._result_buttons[3].click()
    assert window._completed_video_path is None and toolbar._container.isVisible()
    assert not toolbar._result_container.isVisible() and output.read_bytes() == b"mp4"


def test_missing_video_result_keeps_actions_available_and_reports_error(record_window, tmp_path, monkeypatch):
    window = record_window
    window._completed_video_path = tmp_path / "moved.mp4"
    opened, warnings = Mock(), Mock()
    monkeypatch.setattr("gif.record_window.QDesktopServices.openUrl", opened)
    monkeypatch.setattr("ui.dialogs.show_modeless_warning_dialog", warnings)
    window._use_video_result("open")
    opened.assert_not_called()
    warnings.assert_called_once()


@pytest.mark.parametrize("mode", ["gif", "mp4"])
@pytest.mark.parametrize("paused", [False, True])
def test_leaving_annotation_keeps_recording_border_and_region_locked(record_window, mode, paused):
    from gif.record_window import AppState
    from gif.frame_recorder import RecordState
    from gif.overlay import OverlayMode
    window = record_window
    if mode == "mp4":
        window._video._state = "paused" if paused else "recording"
    else:
        window._recorder._state = RecordState.PAUSED if paused else RecordState.RECORDING
    try:
        window._enter_state(AppState.DRAWING)
        window._enter_state(AppState.IDLE)
        assert window._overlay._color.name() == ("#e5ad32" if paused else "#f44336")
        assert window._overlay._mode is OverlayMode.PASSTHROUGH
        before = QRect(window._rect)
        window._on_rect_changed(QRect(0, 0, 100, 100))
        assert window._rect == before
        window._enter_state(AppState.PLAYBACK)
        assert window._overlay._color.name() == "#2196f3"
    finally:
        window._video._state = "idle"
        window._recorder._state = RecordState.IDLE


@pytest.mark.parametrize("state,color", [("starting", "#2196f3"), ("pausing", "#f44336"), ("resuming", "#e5ad32")])
def test_leaving_annotation_during_native_transition_keeps_last_confirmed_border(record_window, state, color):
    from gif.record_window import AppState
    from gif.overlay import OverlayMode
    window = record_window
    window._video._state = state
    try:
        window._enter_state(AppState.IDLE)
        assert window._overlay._color.name() == color
        assert window._overlay._mode is OverlayMode.PASSTHROUGH
    finally:
        window._video._state = "idle"


@pytest.mark.parametrize("choice", ["cancel", "existing", "new_without_extension"])
def test_save_dialog_cancellation_and_filename_safety(record_window, tmp_path, choice):
    window = record_window
    window._record_toolbar._mode_btn._on_select(1)
    output = tmp_path / "record.mp4"
    if choice == "existing":
        output.write_bytes(b"keep")
    chosen = "" if choice == "cancel" else str(output.with_suffix("") if choice == "new_without_extension" else output)
    window._record_toolbar._record_btn.click()
    assert window.video_busy and window._video._process is None
    if choice == "cancel":
        window._save_dialog.reject()
    else:
        window._save_dialog.selectFile(chosen)
        window._save_dialog.accept()
    if choice == "new_without_extension":
        assert window._video._output == output
    else:
        assert not window.video_busy and window._record_toolbar._record_btn.isEnabled()
        if choice == "existing":
            assert output.read_bytes() == b"keep"


def test_delayed_gif_release_starts_mp4_only_after_done(record_window, tmp_path):
    import time
    window = record_window
    future = Future()
    window._gif_release = future
    window._pending_video = (str(tmp_path / "new.mp4"), RecordingOptions(), time.monotonic())
    window._start_pending_video()
    assert window._video._process is None
    future.set_result(None)
    window._start_pending_video()
    assert window._video._process is not None
    assert not window._release_timer.isActive()


@pytest.mark.parametrize("failure", ["timeout", "exception"])
def test_gif_release_failure_cannot_start_video(record_window, tmp_path, failure):
    import time
    window = record_window
    future = Future()
    if failure == "exception":
        future.set_exception(RuntimeError("release failed"))
    window._gif_release = future
    window._pending_video = (str(tmp_path / "new.mp4"), RecordingOptions(), time.monotonic() - 31)
    window._start_pending_video()
    assert not window.video_busy and window._video._process is None
    assert window._record_toolbar._record_btn.isEnabled()


def test_switch_back_to_gif_prepares_only_when_selected(record_window, monkeypatch):
    prepares = []
    window = record_window
    monkeypatch.setattr(window._recorder, "prepare", lambda: prepares.append(True))
    window._record_toolbar._mode_btn._on_select(1)
    window._record_toolbar._on_fps_selected(12)
    assert prepares == []
    window._record_toolbar._mode_btn._on_select(0)
    assert prepares == [True]
    window._on_fps_changed(10)
    assert window._recorder.fps == 10


def test_switch_back_to_gif_waits_for_release_before_preparing_or_starting(record_window, monkeypatch):
    window = record_window
    toolbar = window._record_toolbar
    future = Future()
    prepares, starts = [], []
    monkeypatch.setattr(window._recorder, "release", lambda: future)
    monkeypatch.setattr(window._recorder, "prepare", lambda: prepares.append(True))
    monkeypatch.setattr(window._recorder, "start", lambda: starts.append(True))
    toolbar._mode_btn._on_select(1)
    toolbar._mode_btn._on_select(0)
    assert prepares == [] and starts == []
    assert not toolbar._record_btn.isEnabled() and not toolbar._mode_btn.isEnabled()
    assert window._release_timer.isActive()
    toolbar._record_btn.click()
    window._on_record_start()
    assert prepares == [] and starts == []
    future.set_result(None)
    window._start_pending_video()
    window._start_pending_video()
    assert prepares == [True] and starts == []
    assert toolbar._record_btn.isEnabled() and toolbar._mode_btn.isEnabled()
    assert not toolbar._recording and not window._release_timer.isActive()


def test_reselecting_mp4_keeps_original_gif_release_before_switching_back(record_window, monkeypatch):
    window = record_window
    toolbar = window._record_toolbar
    recorder = window._recorder
    future = Future()
    prepares, starts, modes = [], [], []
    # 保留真实 release：第一次归还 HDR 后，重复 release 会走已完成 Future 的分支。
    recorder._hdr_lent = True
    monkeypatch.setattr("gif.frame_recorder.return_hdr_session", lambda after: future)
    monkeypatch.setattr(recorder, "prepare", lambda: prepares.append(True))
    monkeypatch.setattr(recorder, "start", lambda: starts.append(True))
    toolbar.mode_changed.connect(modes.append)

    toolbar._mode_btn._on_select(1)
    assert window._gif_release is future and not recorder._hdr_lent
    assert toolbar._mode_btn.isEnabled()
    toolbar._mode_btn._on_select(1)
    assert window._gif_release is future and not future.done()
    assert modes == ["mp4"]

    toolbar._mode_btn._on_select(0)
    assert modes == ["mp4", "gif"]
    assert prepares == [] and starts == []
    assert not toolbar._record_btn.isEnabled() and not toolbar._mode_btn.isEnabled()
    assert window._release_timer.isActive()
    toolbar._record_btn.click()
    assert starts == []

    future.set_result(None)
    window._start_pending_video()
    assert prepares == [True] and starts == []
    assert window._gif_release is None and not window._release_timer.isActive()
    assert toolbar._record_btn.isEnabled() and toolbar._mode_btn.isEnabled()


def test_close_while_switching_back_to_gif_cancels_release_callback(record_window, monkeypatch, qapp):
    window = record_window
    toolbar = window._record_toolbar
    future = Future()
    prepares = []
    monkeypatch.setattr(window._recorder, "release", lambda: future)
    monkeypatch.setattr(window._recorder, "prepare", lambda: prepares.append(True))
    toolbar._mode_btn._on_select(1)
    toolbar._mode_btn._on_select(0)
    assert window._release_timer.isActive()
    window.close_all()
    assert window._recorder is None and window._record_toolbar is None
    assert not window._release_timer.isActive() and qapp._gif_window is None
    future.set_result(None)
    # 已排队的超时回调也不能访问关闭后的录制器或工具栏。
    window._start_pending_video()
    assert prepares == []


@pytest.mark.parametrize("completed_before_switch", [True, False])
def test_switch_back_to_gif_does_not_prepare_after_release_failure(record_window, monkeypatch, completed_before_switch):
    window = record_window
    toolbar = window._record_toolbar
    future = Future()
    prepares, failures = [], []
    monkeypatch.setattr(window._recorder, "release", lambda: future)
    monkeypatch.setattr(window._recorder, "prepare", lambda: prepares.append(True))
    monkeypatch.setattr("ui.dialogs.show_modeless_warning_dialog", lambda *args: failures.append(args))
    toolbar._mode_btn._on_select(1)
    if completed_before_switch:
        future.set_exception(RuntimeError("release failed"))
    toolbar._mode_btn._on_select(0)
    if not completed_before_switch:
        future.set_exception(RuntimeError("release failed"))
    window._start_pending_video()
    assert prepares == [] and len(failures) == 1
    assert window._gif_release is None and not window._release_timer.isActive()
    assert toolbar._record_btn.isEnabled() and toolbar._mode_btn.isEnabled()


@pytest.mark.parametrize("accepted", [True, False])
def test_custom_video_fps_accept_and_cancel(record_window, accepted):
    toolbar = record_window._record_toolbar
    toolbar._mode_btn._on_select(1)
    original = toolbar.get_current_fps()
    toolbar._on_fps_selected(None)
    toolbar._fps_dialog.setIntValue(19)
    toolbar._fps_dialog.accept() if accepted else toolbar._fps_dialog.reject()
    assert toolbar.get_current_fps() == (19 if accepted else original)


@pytest.mark.parametrize("accepted", [True, False])
def test_video_settings_dialog_saves_target_bitrate_fps_and_muting(record_window, accepted):
    toolbar = record_window._record_toolbar
    toolbar._mode_btn._on_select(1)
    previous = toolbar.get_video_options()

    toolbar._show_video_settings()
    dialog = toolbar._video_dialog
    assert dialog is not None
    dialog.findChild(QSpinBox).setValue(23)
    dialog.findChild(QDoubleSpinBox).setValue(1.5)
    for box in dialog.findChildren(QCheckBox):
        box.setChecked(False)
    buttons = dialog.findChild(QDialogButtonBox)
    buttons.button(QDialogButtonBox.StandardButton.Ok if accepted else QDialogButtonBox.StandardButton.Cancel).click()
    options = toolbar.get_video_options()
    assert options == (RecordingOptions(fps=23, bitrate=1_500_000, system_audio=False, hardware=False, cursor=False)
                       if accepted else previous)


def test_close_while_choosing_output_cancels_dialog_without_starting_helper(record_window, qapp):
    window = record_window
    window._record_toolbar._mode_btn._on_select(1)
    window._record_toolbar._record_btn.click()
    assert window._save_dialog is not None and window.video_busy
    window.close_all()
    assert window._save_dialog is None and window._video._process is None
    assert qapp._gif_window is None and window._record_toolbar is None


@pytest.mark.parametrize("dialog_name", ["_video_dialog", "_fps_dialog"])
def test_close_with_video_option_dialog_does_not_save_or_reenter(record_window, dialog_name):
    window = record_window
    toolbar = window._record_toolbar
    toolbar._mode_btn._on_select(1)
    if dialog_name == "_video_dialog":
        toolbar._show_video_settings()
    else:
        toolbar._on_fps_selected(None)
    assert getattr(toolbar, dialog_name) is not None
    window.close_all()
    assert getattr(toolbar, dialog_name) is None
