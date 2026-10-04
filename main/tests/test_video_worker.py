"""录制 worker 协议与早分流测试，使用模拟扩展和匿名管道。"""

import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
from types import SimpleNamespace

import pytest

from main import video_worker


def arguments(tmp_path):
    return ["--output", str(tmp_path / "录像.mp4"), "--left", "0", "--top", "0",
            "--width", "320", "--height", "240", "--audio", "none"]


class Recorder:
    instances = []

    def __init__(self, *args, **kwargs):
        self.args, self.kwargs = args, kwargs
        self.commands = []
        self.stopped = threading.Event()
        self.instances.append(self)

    def pause(self):
        self.commands.append("pause")

    def resume(self):
        self.commands.append("resume")

    def stop(self):
        self.commands.append("stop")
        self.stopped.set()

    def cancel(self):
        self.commands.append("cancel")
        self.stopped.set()

    def run(self, callback):
        callback("ready", {"fps": self.kwargs["fps"]})
        assert self.stopped.wait(2)
        callback("cancelled" if "cancel" in self.commands else "complete",
                 {"output": self.args[0], "finalized": True})


@pytest.fixture
def native(monkeypatch):
    Recorder.instances.clear()
    module = SimpleNamespace(Recorder=Recorder)
    monkeypatch.setitem(sys.modules, "video_recorder", module)
    return module


@pytest.fixture
def pipes():
    source, parent = os.pipe()
    output, child = os.pipe()
    yield source, parent, output, child
    for fd in (source, parent, output, child):
        try:
            os.close(fd)
        except OSError:
            pass


def messages(pipes):
    os.close(pipes[3])
    with os.fdopen(os.dup(pipes[2]), "rb") as stream:
        return [json.loads(line) for line in stream]


@pytest.mark.parametrize("command", ["stop", "cancel"])
def test_command_finishes_worker_without_parent_closing_stdin(native, tmp_path, pipes, command):
    os.write(pipes[1], json.dumps({"protocol": 1, "command": command}).encode() + b"\n")
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 0
    events = messages(pipes)
    assert events[-1]["event"] == ("complete" if command == "stop" else "cancelled")
    recorder = Recorder.instances[-1]
    assert recorder.commands == [command]
    assert recorder.kwargs == {"fps": 30, "bitrate": 4000000, "system_audio": False,
                               "hardware": True, "cursor": True}
    # 父写端依然有效，退出不依赖 EOF。
    os.fstat(pipes[1])


@pytest.mark.parametrize("payload", [
    b"invalid\n", b"[]\n", b'{"protocol":true,"command":"stop"}\n',
    b'{"protocol":1.0,"command":"stop"}\n',
    b'{"protocol":1,"protocol":1,"command":"stop"}\n',
    b'{"protocol":1,"command":"stop","extra":1}\n',
    b'{"protocol":1,"command":"unknown"}\n',
    b'{"protocol":1,"command":{}}\n', b"x" * 2049,
])
def test_invalid_protocol_stops_safely_and_never_reports_complete(native, tmp_path, pipes, payload):
    os.write(pipes[1], payload)
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 1
    events = messages(pipes)
    assert events[-1]["event"] == "error" and events[-1]["data"]["code"] == "protocol"
    assert not any(event["event"] in ("complete", "cancelled") for event in events)
    assert Recorder.instances[-1].commands == ["stop"]


@pytest.mark.parametrize("partial", [False, True])
def test_parent_eof_stops_but_partial_json_is_protocol_error(native, tmp_path, pipes, partial):
    if partial:
        os.write(pipes[1], b'{"protocol":1')
    os.close(pipes[1])
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == int(partial)
    last = messages(pipes)[-1]
    assert last["event"] == ("error" if partial else "complete")


def test_controller_thread_close_does_not_wait_on_empty_open_pipe(pipes):
    recorder = Recorder()
    controls = video_worker._Controls(pipes[0], recorder)
    controls.start()
    started = time.monotonic()
    controls.close()
    assert time.monotonic() - started < 1
    assert not controls.thread.is_alive()
    assert recorder.commands == []


def test_pause_and_resume_are_typed_native_calls(pipes):
    recorder = Recorder()
    controls = video_worker._Controls(pipes[0], recorder)
    assert controls._command(b'{"command":"pause","protocol":1}') is False
    assert controls._command(b'{"command":"resume","protocol":1}') is False
    assert controls._command(b'{"command":"stop","protocol":1}') is True
    assert recorder.commands == ["pause", "resume", "stop"]


@pytest.mark.parametrize("option", ["--synthetic", "--synthetic-medium", "--duration", "--fail-hardware-init"])
def test_production_extension_rejects_test_arguments(native, tmp_path, pipes, option):
    assert video_worker.run([*arguments(tmp_path), option], pipes[0], pipes[3]) == 1
    assert messages(pipes)[-1]["data"]["code"] == "arguments"
    assert Recorder.instances == []


def test_test_options_only_call_test_extension_api(native, tmp_path, pipes):
    class TestRecorder(Recorder):
        def _configure_test(self, **options):
            self.test_options = options
    native.Recorder = TestRecorder
    os.write(pipes[1], b'{"protocol":1,"command":"stop"}\n')
    assert video_worker.run([*arguments(tmp_path), "--synthetic-medium", "--duration", "0.1",
                             "--fail-hardware-init"], pipes[0], pipes[3]) == 0
    assert Recorder.instances[-1].test_options == {
        "synthetic": True, "medium": True, "duration": 0.1, "fail_hardware": True,
    }


def test_native_error_code_reaches_protocol_and_reader_is_closed(native, tmp_path, pipes):
    class NativeError(RuntimeError):
        code = "disk_full"
    class FailingRecorder(Recorder):
        def run(self, callback):
            raise NativeError("disk full")
    native.Recorder = FailingRecorder
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 1
    assert messages(pipes) == [{"protocol": 1, "event": "error", "data": {"code": "disk_full", "message": "disk full"}}]
    assert not any(thread.name == "video-control" for thread in threading.enumerate())


def test_control_thread_start_failure_reports_error_without_joining_unstarted_thread(native, tmp_path, pipes, monkeypatch):
    def fail_start(self):
        raise RuntimeError("cannot start thread")
    monkeypatch.setattr(video_worker.threading.Thread, "start", fail_start)
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 1
    assert messages(pipes) == [{"protocol": 1, "event": "error", "data": {
        "code": "backend", "message": "cannot start thread",
    }}]
    assert not any(thread.name == "video-control" for thread in threading.enumerate())


@pytest.mark.parametrize("error_type", [OverflowError, TypeError, ValueError])
def test_pyo3_numeric_conversion_errors_are_argument_errors(native, tmp_path, pipes, error_type):
    class InvalidRecorder(Recorder):
        def __init__(self, *args, **kwargs):
            raise error_type("invalid numeric value")
    native.Recorder = InvalidRecorder
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 1
    assert messages(pipes)[-1]["data"]["code"] == "arguments"


def test_event_size_is_bounded(pipes):
    with pytest.raises(ValueError, match="16384"):
        video_worker._emit(pipes[3], "error", {"message": "x" * video_worker.MAX_MESSAGE})


def test_partial_pipe_writes_preserve_complete_json(monkeypatch):
    chunks = []
    def short_write(fd, payload):
        chunks.append(payload[:3])
        return min(3, len(payload))
    monkeypatch.setattr(video_worker.os, "write", short_write)
    video_worker._emit(1, "ready", {"message": "中文"})
    assert json.loads(b"".join(chunks)) == {"protocol": 1, "event": "ready", "data": {"message": "中文"}}


@pytest.mark.parametrize("entry", ["main_app.py", "video_worker_hook.py"])
def test_worker_dispatch_precedes_gui_settings_and_single_instance_initialization(tmp_path, entry):
    main = Path(__file__).parents[1] / entry
    script = r'''
import builtins, runpy, sys, types
worker = types.ModuleType("video_worker")
def main(argv):
    assert argv == ["--unit-test"]
    return 89
worker.main = main
sys.modules["video_worker"] = worker
original = builtins.__import__
def restricted(name, *args, **kwargs):
    assert not name.startswith(("PySide6", "settings", "core", "ui", "pyclipboard", "inputhub")), name
    return original(name, *args, **kwargs)
builtins.__import__ = restricted
sys.argv = [sys.argv[1], "--video-recorder-worker", "--unit-test"]
runpy.run_path(sys.argv[0], run_name="__main__")
'''
    result = subprocess.run([sys.executable, "-c", script, str(main)], capture_output=True, timeout=15)
    assert result.returncode == 89, result.stderr.decode(errors="replace")


@pytest.mark.skipif(os.name != "nt", reason="Windows windowed EXE standard handles")
def test_windowed_none_stdio_uses_inherited_windows_pipe_handles():
    worker_dir = str(Path(__file__).parents[1])
    script = r'''
import sys
sys.path.insert(0, sys.argv[1])
from video_worker import main
sys.stdin = sys.stdout = sys.stderr = None
raise SystemExit(main(["--unknown"]))
'''
    # 参数错误不启动媒体核心；即使扩展不存在，也必须通过原生输出句柄报告错误。
    result = subprocess.run([sys.executable, "-c", script, worker_dir], input=b"", capture_output=True, timeout=15)
    assert result.returncode == 1
    message = json.loads(result.stdout)
    assert message["event"] == "error" and message["data"]["code"] in ("arguments", "helper_missing")


def test_main_owns_only_duplicated_standard_handles(native, tmp_path, pipes, monkeypatch):
    os.write(pipes[1], b'{"protocol":1,"command":"stop"}\n')
    sources = {"stdin": pipes[0], "stdout": pipes[3]}
    monkeypatch.setattr(video_worker, "_standard_fd", lambda name, number: os.dup(sources[name]))
    assert video_worker.main(arguments(tmp_path)) == 0
    assert messages(pipes)[-1]["event"] == "complete"
    os.fstat(pipes[0])


def test_standard_stream_descriptor_is_duplicated_not_transferred(pipes, monkeypatch):
    monkeypatch.setattr(video_worker, "sys", SimpleNamespace(stdin=SimpleNamespace(fileno=lambda: pipes[0])))
    duplicate = video_worker._standard_fd("stdin", 0)
    os.close(duplicate)
    os.fstat(pipes[0])


@pytest.mark.skipif(os.name != "nt", reason="Windows windowed standard handles")
@pytest.mark.parametrize("stream", [None, SimpleNamespace(fileno=lambda: (_ for _ in ()).throw(ValueError()))])
def test_missing_python_stdout_duplicates_live_windows_handle(monkeypatch, stream):
    monkeypatch.setattr(video_worker, "sys", SimpleNamespace(stdout=stream))
    duplicate = video_worker._standard_fd("stdout", 1)
    try:
        os.fstat(duplicate)
    finally:
        os.close(duplicate)


def test_main_missing_control_handle_reports_process_io(pipes, monkeypatch):
    def standard(name, number):
        if number == 0:
            raise OSError("control pipe missing")
        return os.dup(pipes[3])
    monkeypatch.setattr(video_worker, "_standard_fd", standard)
    assert video_worker.main([]) == 1
    assert messages(pipes)[-1]["data"]["code"] == "process_io"


def test_main_unavailable_status_handle_exits_cleanly(monkeypatch):
    def standard(name, number):
        raise OSError("status pipe missing")
    monkeypatch.setattr(video_worker, "_standard_fd", standard)
    assert video_worker.main([]) == 1


def test_broken_status_pipe_does_not_leave_controller_thread(native, tmp_path, pipes):
    os.close(pipes[2])
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 1
    assert not any(thread.name == "video-control" for thread in threading.enumerate())


def test_invalid_control_handle_is_protocol_error(native, tmp_path, pipes):
    os.close(pipes[0])
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 1
    assert messages(pipes)[-1]["data"]["code"] == "protocol"


def test_multiple_commands_in_one_read_preserve_order(native, tmp_path, pipes):
    os.write(pipes[1], b'{"protocol":1,"command":"pause"}\n'
             b'{"protocol":1,"command":"resume"}\n'
             b'{"protocol":1,"command":"stop"}\n')
    assert video_worker.run(arguments(tmp_path), pipes[0], pipes[3]) == 0
    assert Recorder.instances[-1].commands == ["pause", "resume", "stop"]


def test_normal_application_runtime_hook_does_not_import_worker_or_native(monkeypatch):
    import builtins
    path = Path(__file__).parents[1] / "video_worker_hook.py"
    source = path.read_text(encoding="utf-8-sig")
    original = builtins.__import__
    def guarded(name, *args, **kwargs):
        assert name not in ("video_worker", "video_recorder", "PySide6"), name
        return original(name, *args, **kwargs)
    monkeypatch.setattr(sys, "argv", ["jietuba_pp.exe"])
    monkeypatch.setattr(builtins, "__import__", guarded)
    exec(compile(source, str(path), "exec"), {"__name__": "video_worker_hook"})
