from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys
import uuid

from PySide6.QtCore import QObject, QProcess, QTimer, Signal
from core.logger import T, log_warning
from core.update_cache import cache_subdirectory, remove_directory, remove_helper


def helper_path() -> Path:
    if getattr(sys, "frozen", False):
        source = Path(sys._MEIPASS) / "updater" / "jietuba_updater.exe"
        # The onefile unpack directory is removed on exit, so the worker needs a persistent copy.
        root = cache_subdirectory("helpers") / uuid.uuid4().hex
        root.mkdir(parents=True)
        target = root / source.name
        try:
            shutil.copy2(source, target)
        except OSError:
            remove_directory(root)
            raise
        return target
    root = Path(__file__).resolve().parents[2] / "rust_libs" / "target"
    import platform
    arch = "aarch64" if platform.machine().lower() in ("arm64", "aarch64") else "x86_64"
    for candidate in (root / f"{arch}-pc-windows-msvc" / "release" / "jietuba_updater.exe", root / "release" / "jietuba_updater.exe"):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("Build rust_libs/updater before checking for updates from source")


def app_variant() -> str:
    """Release package of this build: "full" bundles PP-OCR, "lite" does not."""
    if getattr(sys, "frozen", False):
        return (Path(sys._MEIPASS) / "updater" / "app_variant.txt").read_text(encoding="utf-8").strip()
    import importlib.util
    return "full" if importlib.util.find_spec("ppocr_rust") else "lite"


class UpdaterProcess(QObject):
    event_received = Signal(str, str, object)
    failed = Signal(str)
    finished = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.process = QProcess(self)
        self.process.readyReadStandardOutput.connect(self._read)
        self.process.readyReadStandardError.connect(self._read_stderr)
        self.process.finished.connect(self._finish)
        self.process.errorOccurred.connect(self._process_error)
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self._timeout)
        self._buffer = b""
        self._stderr = b""
        self.transaction = ""
        self._terminal = False
        self._error = False
        self._busy = False
        self._handed_off = False
        self._helper = None
        self._command = ""
        self._downloaded = False

    @property
    def busy(self):
        return self._busy

    def start(self, command: str, arguments: list[str], timeout_ms=900_000) -> bool:
        if self.busy:
            return False
        self._buffer = self._stderr = b""
        self._terminal = self._error = self._handed_off = False
        self._command = command
        self._downloaded = False
        self.transaction = arguments[arguments.index("--transaction") + 1] if "--transaction" in arguments else uuid.uuid4().hex
        try:
            executable = helper_path()
        except (OSError, KeyError) as exc:
            self.failed.emit(str(exc))
            return False
        self._helper = executable
        self._busy = True
        self.process.setProgram(str(executable))
        self.process.setArguments([command, *arguments] if "--transaction" in arguments else [command, *arguments, "--transaction", self.transaction])
        self.process.start()
        self._timer.start(timeout_ms)
        return True

    def send(self, command):
        if not self.busy:
            return False
        message = {"protocol": 1, "transaction": self.transaction, "command": command}
        return self.process.write((json.dumps(message) + "\n").encode()) >= 0

    def cancel(self):
        if self.busy and not self._handed_off:
            self.send("cancel")
            self.process.closeWriteChannel()

    def _fail(self, reason):
        if not self._error:
            self._error = True
            self.failed.emit(reason)

    def _read(self):
        self._buffer += bytes(self.process.readAllStandardOutput())
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            try:
                if len(line) > 65536:
                    raise ValueError("Updater message too large")
                data = json.loads(line)
                if not isinstance(data, dict) or data.get("protocol") != 1 or data.get("transaction") != self.transaction:
                    raise ValueError("Invalid updater protocol")
                event = data["event"]
                payload = data["data"]
                if not isinstance(event, str) or not isinstance(payload, dict):
                    raise ValueError("Invalid updater event")
                if event == "error":
                    self._fail(self.error_text(payload))
                if event in ("downloaded", "handed_off", "available", "up_to_date", "recovered"):
                    self._terminal = True
                if event == "handed_off":
                    self._handed_off = True
                if event == "downloaded":
                    self._downloaded = True
                self.event_received.emit(event, self.transaction, payload)
            except (ValueError, KeyError, TypeError, UnicodeError) as exc:
                self._fail(str(exc))
                self.process.kill()
                return
        if len(self._buffer) > 65536:
            self._fail("Updater message too large")
            self.process.kill()

    def error_text(self, payload) -> str:
        """The updater reports a stable code; its own message is Chinese and only goes to the log."""
        code = payload.get("code")
        log_warning(T("更新器报错 {code}: {message}", code=code, message=payload.get("message")), "Update")
        if code in ("network", "source"):
            return self.tr("Could not download the update. Check your network connection and try again.")
        if code in ("release", "asset"):
            return self.tr("The latest release has no package for this edition and architecture.")
        if code in ("archive", "size", "architecture"):
            return self.tr("The downloaded package is invalid.")
        if code == "version":
            return self.tr("The downloaded update does not match this installation.")
        if code == "busy":
            return self.tr("Another update or recovery is already running.")
        if code == "pending_recovery":
            return self.tr("An earlier update was not finished. Recover it from the .jietuba-update folder first.")
        if code in ("file_in_use", "process", "exit_timeout"):
            return self.tr("The application did not close or its file is in use. Close it and try again.")
        if code == "permission":
            return self.tr("No permission to write to the installation folder.")
        if code == "conflict":
            return self.tr("The application file was changed by another program. The update was stopped.")
        if code == "recovery_failed":
            return self.tr("The previous version could not be restored automatically. Its backup was kept in the .jietuba-update folder.")
        if code in ("cancelled", "timeout"):
            return self.tr("The update was cancelled.")
        return self.tr("The updater stopped with an error (%1).").replace("%1", str(code or "unknown"))

    def _read_stderr(self):
        self._stderr = (self._stderr + bytes(self.process.readAllStandardError()))[-8192:]

    def _process_error(self, error):
        self._fail(self.process.errorString())
        if error == QProcess.ProcessError.FailedToStart:
            self._busy = False
            self._timer.stop()
            self._cleanup()
            self.finished.emit()

    def _timeout(self):
        self._fail("Update process timed out")
        self.cancel()
        self.process.kill()

    def _finish(self, code, status):
        self._read()
        self._timer.stop()
        self._busy = False
        if not self._error and (code != 0 or status != QProcess.ExitStatus.NormalExit or not self._terminal or self._buffer):
            self._fail(self._stderr.decode("utf-8", "replace") or "Updater exited without a complete response")
        self._cleanup()
        self.finished.emit()

    def _cleanup(self):
        remove_helper(self._helper)
        self._helper = None
        if self._command == "download" and (self._error or not self._downloaded):
            try:
                from core.update_cache import cache_root
                remove_directory(cache_root() / self.transaction)
            except (OSError, KeyError):
                pass
