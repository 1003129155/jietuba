import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import threading
import time
import subprocess
from types import MethodType, SimpleNamespace

import pytest
from PySide6.QtCore import QObject, QProcess, QSettings, Signal, Qt

from core import background_tasks
from core.update_controller import UpdateController
from core.updater_process import UpdaterProcess
from main_app import MainApp

from core.constants import PROJECT_RELEASES_LATEST_URL
from core.update_checker import (
    ReleaseInfo,
    GitHubReleaseChecker,
    UpdateCheckError,
    comparable_version,
    fetch_latest_release,
    is_newer_version,
    parse_release_payload,
)


@pytest.mark.parametrize(
    ("value", "expected"),
    (
        ("2.0.3", (2, 0, 3, 0)),
        ("v2.1.0", (2, 1, 0, 0)),
        ("release-2.4", (2, 4, 0, 0)),
        ("release/3", (3, 0, 0, 0)),
    ),
)
def test_comparable_version_accepts_common_release_tags(value, expected):
    assert comparable_version(value) == expected


def test_version_comparison_is_numeric_instead_of_lexicographic():
    assert is_newer_version("v2.0.10", "2.0.9")
    assert not is_newer_version("release-2.0", "2.0.3")
    assert not is_newer_version("v2.0.3", "2.0.3")


def test_parse_release_payload_extracts_notes_and_uses_latest_download_url():
    payload = json.dumps(
        {
            "tag_name": "v2.1.0",
            "name": "Version 2.1",
            "body": "  Added update checking.  ",
            "html_url": "https://example.invalid/untrusted",
        }
    ).encode()

    release = parse_release_payload(payload)

    assert release.tag_name == "v2.1.0"
    assert release.title == "Version 2.1"
    assert release.notes == "Added update checking."
    assert release.url == PROJECT_RELEASES_LATEST_URL


@pytest.mark.parametrize(
    "payload",
    (
        b"not-json",
        b"[]",
        b'{}',
        b'{"tag_name": "latest"}',
    ),
)
def test_parse_release_payload_rejects_unusable_responses(payload):
    with pytest.raises(UpdateCheckError):
        parse_release_payload(payload)


@pytest.fixture
def rust_reply(monkeypatch):
    monkeypatch.setattr("core.update_checker.helper_path", lambda: "updater.exe")
    def reply(tag="v99.0.0"):
        return SimpleNamespace(returncode=0, stdout=json.dumps({"protocol": 1, "event": "available", "data": {
            "tag_name": tag, "title": tag, "notes": "Bug fixes", "url": PROJECT_RELEASES_LATEST_URL,
            "asset_name": "package.zip", "arch": "x64", "artifact": {"urls": ["https://example.com/package.zip"], "size": 123},
        }}).encode())
    return reply


def test_fetch_latest_release_builds_rust_request(monkeypatch, rust_reply):
    calls = []
    def run(command, **kwargs):
        calls.append((command, kwargs))
        return rust_reply("v99.0.0")
    monkeypatch.setattr("core.update_checker.subprocess.run", run)
    release = fetch_latest_release(4321)
    command, kwargs = calls[0]
    assert command[0:2] == ["updater.exe", "check"]
    assert "--install-exe" in command
    assert "--current-version" in command
    assert command[command.index("--variant") + 1] in ("full", "lite")
    assert kwargs["timeout"] == 4.321
    assert release.available is True
    assert release.asset_name == "package.zip"


def test_fetch_latest_release_reports_network_errors(monkeypatch, rust_reply):
    monkeypatch.setattr("core.update_checker.subprocess.run", lambda *a, **k: SimpleNamespace(
        returncode=1, stdout=b'{"protocol":1,"event":"error","data":{"message":"network unavailable"}}'))
    with pytest.raises(UpdateCheckError, match="network unavailable"):
        fetch_latest_release(10000)


def test_checker_runs_request_in_background_and_rejects_overlapping_checks(monkeypatch, qapp, qtbot, rust_reply):
    monkeypatch.setattr("core.update_checker.subprocess.run", lambda *a, **k: rust_reply())
    checker = GitHubReleaseChecker(timeout_ms=4321)
    with qtbot.waitSignal(checker.release_found) as blocker:
        assert checker.check() is True
        assert checker.is_checking is True
        assert checker.check() is False
    assert blocker.args[0].tag_name == "v99.0.0"
    qtbot.waitUntil(lambda: not checker.is_checking)


def test_checker_recovers_from_an_unexpected_worker_error(monkeypatch, qapp, qtbot, rust_reply):
    def run(*a, **k):
        raise RuntimeError("unexpected failure")
    monkeypatch.setattr("core.update_checker.subprocess.run", run)
    checker = GitHubReleaseChecker()
    with qtbot.waitSignal(checker.failed) as blocker:
        assert checker.check() is True
    assert blocker.args == ["Unexpected update-check error"]
    qtbot.waitUntil(lambda: not checker.is_checking)


@pytest.mark.parametrize("reply", [b"not-json", b"{}", b'{"protocol":2}', b'{"protocol":1,"event":"available","data":{}}'])
def test_sync_check_rejects_bad_protocol(monkeypatch, rust_reply, reply):
    monkeypatch.setattr("core.update_checker.subprocess.run", lambda *a, **k: SimpleNamespace(returncode=0, stdout=reply))
    with pytest.raises(UpdateCheckError):
        fetch_latest_release(100)


def test_sync_check_reports_process_timeout(monkeypatch, rust_reply):
    def run(*a, **k):
        raise subprocess.TimeoutExpired("updater", 1)
    monkeypatch.setattr("core.update_checker.subprocess.run", run)
    with pytest.raises(UpdateCheckError):
        fetch_latest_release(100)


class FakeRunner(QObject):
    event_received = Signal(str, str, object)
    failed = Signal(str)
    finished = Signal()
    def __init__(self, parent=None):
        super().__init__(parent)
        self.busy = False
        self.calls = []
        self.sent = []
    def start(self, command, arguments, **kwargs):
        if self.busy:
            return False
        self.calls.append((command, arguments))
        self.busy = True
        return True
    def send(self, value):
        self.sent.append(value)
        return True
    def cancel(self):
        self.sent.append("cancel")
    def finish(self):
        self.busy = False
        self.finished.emit()


class FakeApp(QObject):
    def __init__(self):
        super().__init__()
        self.reason = ""
        self.prepare_result = (True, "")
        self.quit_count = 0
        self.cancel_count = 0
    def update_busy_reason(self):
        return self.reason
    def prepare_for_update(self):
        return self.prepare_result
    def cancel_update_preparation(self):
        self.cancel_count += 1
    def quit_app(self):
        self.quit_count += 1


@pytest.fixture
def controller(monkeypatch, qapp, tmp_path):
    monkeypatch.setattr("core.update_controller.UpdaterProcess", FakeRunner)
    monkeypatch.setattr("core.update_controller.sys.frozen", True, raising=False)
    monkeypatch.setattr("core.update_controller.app_variant", lambda: "lite")
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    app = FakeApp()
    ctl = UpdateController(app)
    ctl.present(ReleaseInfo("release-99.0.0", "new", "notes", "https://example.com", True, "release.zip", "x64", {"urls": ["https://example.com/release.zip"], "size": 100}))
    yield ctl
    ctl.dialog.committed = False
    ctl.dialog.close()
    ctl.dialog.deleteLater()
    ctl.deleteLater()


def downloaded(ctl):
    ctl.runner.event_received.emit("downloaded", "1" * 32, {})
    ctl.runner.finish()


def test_download_progress_duplicate_click_and_handoff(controller):
    ctl = controller
    assert ctl.dialog.windowModality() == Qt.WindowModality.NonModal
    ctl.start()
    ctl.start()
    assert len(ctl.runner.calls) == 1
    ctl.runner.event_received.emit("progress", "1" * 32, {"received": 50, "total": 100})
    assert ctl.dialog.progress.value() == 50
    downloaded(ctl)
    command, arguments = ctl.runner.calls[-1]
    options = dict(zip(arguments[::2], arguments[1::2]))
    assert command == "apply"
    assert options["--transaction"] == "1" * 32
    assert options["--parent-pid"] == str(os.getpid())
    assert options["--variant"] == "lite"
    assert options["--failure-message"].startswith("The update was not completed")
    ctl.runner.event_received.emit("ready", "1" * 32, {})
    assert ctl.runner.sent == ["go"]
    assert ctl.dialog.committed
    assert ctl.dialog.windowModality() == Qt.WindowModality.ApplicationModal
    ctl.cancel()
    assert ctl.runner.sent == ["go"]
    ctl.runner.event_received.emit("handed_off", "1" * 32, {})
    ctl.runner.finish()
    assert ctl.main_app.quit_count == 1
    assert not ctl.dialog.isVisible()


def test_busy_keeps_download_and_retries_install_only(controller):
    ctl = controller
    ctl.start()
    ctl.main_app.reason = "finish work"
    downloaded(ctl)
    assert ctl.downloaded
    assert len(ctl.runner.calls) == 1
    assert ctl.dialog.status.text().endswith("finish work")
    ctl.main_app.reason = ""
    ctl.start()
    assert [c[0] for c in ctl.runner.calls] == ["download", "apply"]


def test_cancel_close_or_settings_failure_does_not_exit(controller):
    ctl = controller
    ctl.start()
    downloaded(ctl)
    ctl.main_app.prepare_result = (False, "not saved")
    ctl.runner.event_received.emit("ready", "1" * 32, {})
    assert ctl.runner.sent == ["cancel"]
    ctl.runner.finish()
    assert ctl.main_app.quit_count == 0
    assert "not saved" in ctl.dialog.status.text()


def test_close_downloading_cancels_without_install(controller):
    ctl = controller
    ctl.start()
    ctl.dialog.close()
    downloaded(ctl)
    assert ctl.runner.sent == ["cancel"]
    assert len(ctl.runner.calls) == 1


def test_source_run_shows_explanation(controller, monkeypatch):
    monkeypatch.setattr("core.update_controller.sys.frozen", False)
    controller.start()
    assert not controller.runner.calls
    assert "packaged" in controller.dialog.status.text()


def test_process_protocol_buffering_rejects_mismatch_and_large_line(qapp):
    runner = UpdaterProcess()
    errors = []
    received = []
    runner.failed.connect(errors.append)
    runner.event_received.connect(lambda *a: received.append(a))
    runner.transaction = "1" * 32
    line = json.dumps({"protocol": 1, "transaction": runner.transaction, "event": "progress", "data": {"received": 1}}).encode() + b"\n"
    chunks = [line[:20], line[20:], b'{"protocol":1,"transaction":"other"}\n']
    killed = []
    runner.process = SimpleNamespace(readAllStandardOutput=lambda: chunks.pop(0), kill=lambda: killed.append(True))
    runner._read()
    assert not received
    runner._read()
    assert received[0][0] == "progress"
    runner._read()
    assert errors and killed
    runner._error = False
    chunks.append(b"x" * 65537)
    runner._read()
    assert len(killed) == 2


@pytest.fixture
def lifecycle(qapp, tmp_path):
    app = SimpleNamespace(app=qapp, screenshot_window=None, _capture_pending=False,
                          settings_window=None, _update_preparing=False,
                          quick_capture=SimpleNamespace(busy=False, suspend=lambda: None, refresh=lambda: None),
                          config_manager=SimpleNamespace(qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)),
                          tr=lambda s: s)
    app._capture_busy = MethodType(MainApp._capture_busy, app)
    app.update_busy_reason = MethodType(MainApp.update_busy_reason, app)
    app.cancel_update_preparation = MethodType(MainApp.cancel_update_preparation, app)
    yield app
    app.cancel_update_preparation()


def test_preparation_respects_window_cancel_and_sync_error(lifecycle):
    app = lifecycle
    app.settings_window = SimpleNamespace(close=lambda: False)
    assert MainApp.prepare_for_update(app)[0] is False
    assert not app._update_preparing
    app.settings_window = SimpleNamespace(close=lambda: True)
    app.config_manager.qsettings = SimpleNamespace(sync=lambda: None, status=lambda: QSettings.Status.AccessError)
    assert MainApp.prepare_for_update(app)[0] is False
    assert not app._update_preparing


def test_preparation_pauses_new_saves_and_restores_on_cancel(lifecycle):
    app = lifecycle
    assert MainApp.prepare_for_update(app) == (True, "")
    with pytest.raises(RuntimeError):
        background_tasks.start_thread(lambda: None)
    app.cancel_update_preparation()
    task = background_tasks.start_thread(lambda: None)
    task.join(2)
    assert not background_tasks.busy()


def test_busy_task_registration_and_release_on_exception(lifecycle):
    finish = threading.Event()
    thread = background_tasks.start_thread(lambda: finish.wait(2))
    try:
        assert background_tasks.busy()
        assert MainApp.prepare_for_update(lifecycle)[0] is False
    finally:
        finish.set()
        thread.join(2)
    assert not background_tasks.busy()


def test_quit_respects_cancel_before_destroying_windows(lifecycle):
    lifecycle.settings_window = SimpleNamespace(close=lambda: False)
    assert MainApp.quit_app(lifecycle) is False


def test_native_clipboard_busy_or_pause_race_aborts_preparation(lifecycle):
    calls = []
    lifecycle.clipboard_manager = SimpleNamespace(pending_writes=lambda: 1, pause_for_update=lambda: False, resume_after_update=lambda: calls.append("resume"))
    assert MainApp.prepare_for_update(lifecycle)[0] is False
    lifecycle.clipboard_manager.pending_writes = lambda: 0
    assert MainApp.prepare_for_update(lifecycle)[0] is False
    assert calls == ["resume"]
    assert not lifecycle._update_preparing


def test_settings_storage_migration_blocks_handoff(lifecycle):
    lifecycle.settings_window = SimpleNamespace(_clipboard_move_thread=SimpleNamespace(isRunning=lambda: True))
    assert MainApp.prepare_for_update(lifecycle)[0] is False
    assert not lifecycle._update_preparing


def test_packaged_helper_is_copied_out_of_unpack_directory(monkeypatch, tmp_path):
    from core.updater_process import helper_path
    embedded = tmp_path / "unpack" / "updater" / "jietuba_updater.exe"
    embedded.parent.mkdir(parents=True)
    embedded.write_bytes(b"CURRENT EMBEDDED HELPER")
    monkeypatch.setattr("core.updater_process.sys.frozen", True, raising=False)
    monkeypatch.setattr("core.updater_process.sys._MEIPASS", str(embedded.parent.parent), raising=False)
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "cache"))
    copied = helper_path()
    embedded.unlink()
    assert copied.read_bytes() == b"CURRENT EMBEDDED HELPER"
    assert copied.is_relative_to(tmp_path / "cache")


def test_packaged_build_reports_its_variant_from_the_bundled_marker(monkeypatch, tmp_path):
    from core.updater_process import app_variant
    marker = tmp_path / "updater" / "app_variant.txt"
    marker.parent.mkdir()
    marker.write_text("lite\n", encoding="utf-8")
    monkeypatch.setattr("core.updater_process.sys.frozen", True, raising=False)
    monkeypatch.setattr("core.updater_process.sys._MEIPASS", str(tmp_path), raising=False)
    assert app_variant() == "lite"


def test_updater_error_codes_are_shown_in_the_ui_language(qapp):
    runner = UpdaterProcess()
    assert runner.error_text({"code": "network", "message": "下载失败"}).startswith("Could not download the update")
    assert runner.error_text({"code": "recovery_failed", "message": "备份损坏"}).startswith("The previous version")
    assert runner.error_text({"code": "mystery", "message": "未知"}) == "The updater stopped with an error (mystery)."
    runner.deleteLater()


def test_background_thread_start_failure_unregisters(monkeypatch):
    monkeypatch.setattr("core.background_tasks.threading.Thread.start", lambda _thread: (_ for _ in ()).throw(OSError("cannot create thread")))
    with pytest.raises(OSError):
        background_tasks.start_thread(lambda: None)
    assert not background_tasks.busy()


def test_failed_process_start_and_timeout_report_once(qapp):
    runner = UpdaterProcess()
    errors = []
    finished = []
    runner.failed.connect(errors.append)
    runner.finished.connect(lambda: finished.append(True))
    runner._busy = True
    runner._process_error(QProcess.ProcessError.FailedToStart)
    assert not runner.busy and len(errors) == 1 and finished
    runner._error = False
    runner._busy = True
    runner._timeout()
    assert len(errors) == 2
    runner._busy = False


def test_dialog_reject_cancels_uncommitted_and_blocks_committed(controller):
    ctl = controller
    ctl.dialog.show()
    ctl.dialog.reject()
    assert ctl._cancelled
    ctl.dialog.show()
    ctl._cancelled = False
    ctl.dialog.committed = True
    ctl.dialog.reject()
    assert ctl.dialog.isVisible()
    assert not ctl._cancelled


def test_clipboard_backend_update_gate_uses_current_built_wheel(tmp_path):
    import pyclipboard
    from clipboard.core.manager import ClipboardManager
    backend = pyclipboard.ClipboardManager(str(tmp_path / "clipboard.db"))
    # Verify the built extension's update gate without accessing the real clipboard.
    assert backend.pending_writes() == 0
    assert backend.pause_for_update()
    backend.resume_after_update()
    wrapper = object.__new__(ClipboardManager)
    wrapper._manager = backend
    wrapper._initialized = True
    assert wrapper.pending_writes() == 0
    assert wrapper.pause_for_update()
    wrapper.resume_after_update()


def test_settings_save_exception_cannot_accept_close_or_handoff(lifecycle):
    from PySide6.QtGui import QCloseEvent
    from ui.settings_ui.dialog import SettingsDialog
    def save(**_kwargs):
        raise OSError("save failed")
    window = SimpleNamespace(_skip_unsaved_close_prompt=False, _has_unsaved_changes=lambda: True,
                             _confirm_close_with_unsaved_changes=lambda: "save", apply_settings=save)
    event = QCloseEvent()
    with pytest.raises(OSError):
        SettingsDialog.closeEvent.__wrapped__(window, event)
    assert not event.isAccepted()
    lifecycle.settings_window = SimpleNamespace(close=lambda: save())
    assert MainApp.prepare_for_update(lifecycle) == (False, "save failed")
    assert not lifecycle._update_preparing

@pytest.mark.parametrize("cancel", [False, True])
def test_async_process_exchanges_jsonl_and_completes(monkeypatch, qtbot, tmp_path, cancel):
    script = tmp_path / "protocol.py"
    script.write_text('''import json, sys
id = sys.argv[sys.argv.index("--transaction") + 1]
def emit(event, data):
    print(json.dumps({"protocol": 1, "transaction": id, "event": event, "data": data}), flush=True)
emit("progress", {"received": 50, "total": 100})
control = json.loads(sys.stdin.readline())
assert control["transaction"] == id and control["protocol"] == 1
if control["command"] == "cancel":
    emit("error", {"code": "cancelled", "message": "下载已取消"})
    sys.exit(1)
else:
    emit("downloaded", {})
''', encoding="utf-8")
    monkeypatch.setattr("core.updater_process.helper_path", lambda: Path(sys.executable))
    runner = UpdaterProcess()
    received, errors = [], []
    runner.event_received.connect(lambda *args: received.append(args))
    runner.failed.connect(errors.append)
    try:
        assert runner.start(str(script), [], timeout_ms=10_000)
        assert not runner.start(str(script), [])
        qtbot.waitUntil(lambda: bool(received), timeout=10_000)
        assert received[0][0] == "progress"
        if cancel:
            runner.cancel()
        else:
            assert runner.send("go")
        qtbot.waitUntil(lambda: not runner.busy, timeout=10_000)
        assert errors == (["The update was cancelled."] if cancel else [])
        assert runner._terminal is (not cancel)
        assert not runner.send("go")
    finally:
        if runner.busy:
            runner.process.kill()
            runner.process.waitForFinished(2000)
        runner.deleteLater()


def test_process_reports_missing_helper_and_incomplete_exit(monkeypatch, qapp):
    runner = UpdaterProcess()
    errors = []
    runner.failed.connect(errors.append)
    def unavailable():
        raise FileNotFoundError("missing helper")
    monkeypatch.setattr("core.updater_process.helper_path", unavailable)
    assert not runner.start("check", [])
    assert errors == ["missing helper"]
    chunks = [b"diagnostic", b"", b""]
    runner.process = SimpleNamespace(readAllStandardError=lambda: chunks.pop(0), readAllStandardOutput=lambda: b"")
    runner._read_stderr()
    runner._busy = True
    runner._finish(1, QProcess.ExitStatus.NormalExit)
    assert errors[-1] == "diagnostic"
    assert not runner.busy
    runner.deleteLater()


@pytest.fixture
def production_updater():
    value = os.environ.get("JIETUBA_UPDATER_EXE")
    if not value:
        pytest.skip("Set JIETUBA_UPDATER_EXE to the current source build")
    executable = Path(value)
    assert executable.is_file()
    return executable


def test_production_updater_version_and_invalid_arguments(production_updater):
    result = subprocess.run([str(production_updater), "--version"], capture_output=True, timeout=10, check=True)
    reply = json.loads(result.stdout)
    assert reply["protocol"] == 1 and reply["event"] == "version"
    assert reply["data"]["test_build"] is False
    result = subprocess.run([str(production_updater), "apply", "--install-exe"], capture_output=True, timeout=10)
    assert result.returncode == 1
    assert json.loads(result.stdout)["data"]["code"] == "arguments"


@pytest.mark.parametrize("phase", ["preparing", "prepared", "replacing", "replaced", "complete"])
def test_production_updater_recovers_interrupted_replacement(production_updater, tmp_path, phase):
    transaction = "a" * 32
    executable = tmp_path / "jietuba_pp.exe"
    old, new = b"old application", b"new application"
    executable.write_bytes(old if phase in ("preparing", "prepared") else new)
    directory = tmp_path / ".jietuba-update" / transaction
    directory.mkdir(parents=True)
    (directory / "backup.exe").write_bytes(old)
    journal = {"transaction": transaction, "executable": "\\\\?\\" + str(executable), "phase": phase,
               "old_hash": hashlib.sha256(old).hexdigest(), "new_hash": hashlib.sha256(new).hexdigest()}
    (directory / "journal.json").write_text(json.dumps(journal), encoding="utf-8")
    untouched = {"config.ini": b"user settings", "clipboard.db": b"user history", "models/custom.bin": b"custom model"}
    for name, content in untouched.items():
        path = tmp_path / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(content)
    result = subprocess.run([str(production_updater), "recover", "--install-exe", str(executable),
                             "--transaction", transaction], capture_output=True, timeout=15)
    assert result.returncode == 0, result.stdout
    assert json.loads(result.stdout)["event"] == "recovered"
    assert executable.read_bytes() == old
    assert (directory / "backup.exe").read_bytes() == old
    assert json.loads((directory / "journal.json").read_text())["phase"] == "recovered"
    for name, content in untouched.items():
        assert (tmp_path / name).read_bytes() == content


@pytest.mark.parametrize("failure", ["corrupt_backup", "external_change"])
def test_production_updater_preserves_failed_recovery(production_updater, tmp_path, failure):
    transaction = "b" * 32
    executable = tmp_path / "jietuba_pp.exe"
    executable.write_bytes(b"external" if failure == "external_change" else b"new")
    directory = tmp_path / ".jietuba-update" / transaction
    directory.mkdir(parents=True)
    backup = directory / "backup.exe"
    backup.write_bytes(b"damaged" if failure == "corrupt_backup" else b"old")
    journal = {"transaction": transaction, "executable": "\\\\?\\" + str(executable), "phase": "replaced",
               "old_hash": hashlib.sha256(b"old").hexdigest(), "new_hash": hashlib.sha256(b"new").hexdigest()}
    record = directory / "journal.json"
    record.write_text(json.dumps(journal), encoding="utf-8")
    before = (executable.read_bytes(), backup.read_bytes(), record.read_bytes())
    result = subprocess.run([str(production_updater), "recover", "--install-exe", str(executable),
                             "--transaction", transaction], capture_output=True, timeout=15)
    assert result.returncode == 1
    assert json.loads(result.stdout)["data"]["code"] == ("conflict" if failure == "external_change" else "recovery_failed")
    assert (executable.read_bytes(), backup.read_bytes(), record.read_bytes()) == before


def test_production_updater_rejects_wrong_architecture_before_network(production_updater, tmp_path):
    executable = tmp_path / "jietuba_pp.exe"
    shutil.copy2(production_updater, executable)
    pe = executable.read_bytes()
    offset = int.from_bytes(pe[60:64], "little")
    machine = int.from_bytes(pe[offset + 4:offset + 6], "little")
    release = {"tag_name": "99.0.0", "title": "new", "notes": "", "url": "https://example.invalid",
               "asset_name": "release.zip", "arch": "arm64" if machine == 0x8664 else "x64",
               "artifact": {"urls": ["https://example.invalid/release.zip"], "size": 100}}
    description = tmp_path / "release.json"
    description.write_text(json.dumps(release), encoding="utf-8")
    result = subprocess.run([str(production_updater), "download", "--install-exe", str(executable),
                             "--current-version", "1.0.0", "--release-file", str(description), "--variant", "full",
                             "--cache-dir", str(tmp_path / "cache")], input=b"", capture_output=True, timeout=15)
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert result.returncode == 1
    assert events[-1]["data"]["code"] == "version"
    assert executable.read_bytes() == pe


def test_production_updater_removes_failed_download_directory(production_updater, tmp_path):
    executable = tmp_path / "jietuba_pp.exe"
    shutil.copy2(production_updater, executable)
    pe = executable.read_bytes()
    offset = int.from_bytes(pe[60:64], "little")
    arch = "x64" if int.from_bytes(pe[offset + 4:offset + 6], "little") == 0x8664 else "arm64"
    release = {"tag_name": "99.0.0", "title": "new", "notes": "", "url": "https://example.invalid",
               "asset_name": f"jietuba_pp-99.0.0-{arch}.zip", "arch": arch,
               "artifact": {"urls": [], "size": None}}
    description = tmp_path / "release.json"
    description.write_text(json.dumps(release), encoding="utf-8")
    transaction = "d" * 32
    cache = tmp_path / "cache"
    result = subprocess.run([str(production_updater), "download", "--install-exe", str(executable),
                             "--current-version", "1.0.0", "--release-file", str(description), "--variant", "full",
                             "--cache-dir", str(cache), "--transaction", transaction],
                            input=b"", capture_output=True, timeout=15)
    assert result.returncode == 1
    assert json.loads(result.stdout.splitlines()[-1])["data"]["code"] == "source"
    assert not (cache / transaction).exists()
    assert executable.read_bytes() == pe


@pytest.mark.parametrize(("variant", "code"), [(None, "arguments"), ("pp", "variant")])
def test_production_updater_requires_the_reported_variant(production_updater, tmp_path, variant, code):
    executable = tmp_path / "jietuba_pp.exe"
    shutil.copy2(production_updater, executable)
    command = [str(production_updater), "check", "--install-exe", str(executable), "--current-version", "1.0.0"]
    if variant:
        command += ["--variant", variant]
    result = subprocess.run(command, input=b"", capture_output=True, timeout=15)
    assert result.returncode == 1
    assert json.loads(result.stdout.splitlines()[-1])["data"]["code"] == code


def test_download_request_is_removed_when_process_finishes(controller):
    controller.start()
    request = controller._request
    assert request.is_file()
    controller.runner.busy = False
    controller._error("download failed")
    controller._finished()
    assert not request.exists()
    assert controller.main_app.quit_count == 0


# A renamed EXE is still updated in place: the build type comes from --variant, not the file name.
@pytest.mark.parametrize("name", ["jietuba_pp.exe", "截图吧.exe"])
def test_production_updater_removes_stage_after_success_and_keeps_backup(production_updater, tmp_path, name):
    executable = tmp_path / name
    shutil.copy2(production_updater, executable)
    pe = executable.read_bytes()
    offset = int.from_bytes(pe[60:64], "little")
    arch = "x64" if int.from_bytes(pe[offset + 4:offset + 6], "little") == 0x8664 else "arm64"
    transaction = "e" * 32
    cache = tmp_path / "cache"
    stage = cache / transaction
    stage.mkdir(parents=True)
    shutil.copy2(production_updater, stage / "app.exe")
    release = {"tag_name": "99.0.0", "title": "new", "notes": "", "url": "https://example.invalid",
               "asset_name": f"jietuba_pp-99.0.0-{arch}.zip", "arch": arch,
               "artifact": {"urls": [], "size": None}}
    (stage / "prepared.json").write_text(json.dumps({
        "executable": "\\\\?\\" + str(executable), "current_version": "1.0.0", "release": release,
        "staged_hash": hashlib.sha256(pe).hexdigest(),
    }), encoding="utf-8")
    control = json.dumps({"protocol": 1, "transaction": transaction, "command": "go"}).encode() + b"\n"
    result = subprocess.run([str(production_updater), "apply", "--install-exe", str(executable),
                             "--current-version", "1.0.0", "--cache-dir", str(cache), "--variant", "full",
                             "--failure-message", "localized failure", "--transaction", transaction],
                            input=control, capture_output=True, timeout=15)
    assert result.returncode == 0, result.stdout
    events = [json.loads(line) for line in result.stdout.splitlines()]
    handoff = next(event for event in events if event["event"] == "handed_off")
    record = Path(handoff["data"]["result"])
    deadline = time.monotonic() + 10
    while not record.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert record.exists()
    assert json.loads(record.read_bytes())["event"] == "complete"
    assert json.loads((record.parent / "request.json").read_bytes())["failure_message"] == "localized failure"
    assert not stage.exists()
    backup = tmp_path / ".jietuba-update" / handoff["data"]["worker"] / "backup.exe"
    assert backup.read_bytes() == pe
    assert executable.read_bytes() == pe
