from dataclasses import asdict
import json
import os
from pathlib import Path
import sys
import uuid

from PySide6.QtCore import QObject

from core.updater_process import UpdaterProcess
from ui.update_dialog import UpdateDialog


class UpdateController(QObject):
    def __init__(self, main_app):
        super().__init__(main_app)
        self.main_app = main_app
        self.runner = UpdaterProcess(self)
        self.runner.event_received.connect(self._event)
        self.runner.failed.connect(self._error)
        self.runner.finished.connect(self._finished)
        self.dialog = None
        self.release = None
        self.downloaded = ""
        self.mode = ""
        self._cancelled = False
        self._failed = False
        self._committed = False

    def present(self, release):
        if self.runner.busy:
            if self.dialog:
                self.dialog.show()
                self.dialog.raise_()
            return
        if self.dialog and self.release == release:
            self.dialog.show()
            self.dialog.raise_()
            return
        if self.dialog:
            self.dialog.deleteLater()
        self.release = release
        self.downloaded = ""
        self.dialog = UpdateDialog(release)
        self.dialog.update_requested.connect(self.start)
        self.dialog.cancel_requested.connect(self.cancel)
        self.dialog.show()

    def _arguments(self):
        from main_app import APP_VERSION
        return ["--install-exe", sys.executable, "--current-version", APP_VERSION]

    def start(self):
        if self.runner.busy or not self.dialog or self._committed:
            return
        self._failed = self._cancelled = False
        if not getattr(sys, "frozen", False):
            self._error(self.tr("One-click updates are available in the packaged application."))
            return
        self.dialog.set_working(True)
        if self.downloaded:
            self._install()
            return
        try:
            root = Path(os.environ["LOCALAPPDATA"]) / "jietuba" / "updater" / "requests"
            root.mkdir(parents=True, exist_ok=True)
            path = root / (uuid.uuid4().hex + ".json")
            data = asdict(self.release)
            data.pop("available", None)
            path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
            self.mode = "download"
            self.dialog.status.setText(self.tr("Downloading update..."))
            if not self.runner.start("download", [*self._arguments(), "--release-file", str(path)]):
                self.dialog.set_working(False)
        except (OSError, KeyError) as exc:
            self._error(str(exc))
            self.dialog.set_working(False)

    def _install(self):
        reason = self.main_app.update_busy_reason()
        if reason:
            self._error(reason)
            self.dialog.set_working(False)
            return
        self.mode = "apply"
        self.dialog.progress.setRange(0, 0)
        self.dialog.status.setText(self.tr("Preparing to restart..."))
        # Reuse the Rust download transaction to install the same staged release.
        if not self.runner.start("apply", [*self._arguments(), "--transaction", self.downloaded, "--parent-pid", str(os.getpid())], timeout_ms=105_000):
            self.dialog.set_working(False)

    def _event(self, event, transaction, data):
        if event == "progress":
            total, received = data.get("total"), data.get("received", 0)
            self.dialog.progress.setRange(0, 100 if total else 0)
            if total:
                self.dialog.progress.setValue(min(100, int(received * 100 / total)))
        elif event == "downloaded":
            self.downloaded = transaction
        elif event == "ready":
            ok, reason = self.main_app.prepare_for_update()
            if ok and self.runner.send("go"):
                self.dialog.committed = True
                self.dialog.set_working(True)
            else:
                self._error(reason or self.tr("Update handoff failed."))
                self.runner.cancel()
        elif event == "handed_off":
            self._committed = True

    def _error(self, reason):
        self._failed = True
        self.main_app.cancel_update_preparation()
        if self.dialog:
            self.dialog.committed = False
            self.dialog.status.setText(self.tr("Update failed: %1").replace("%1", reason))

    def _finished(self):
        if self._committed:
            if self.dialog:
                self.dialog.finish_handoff()
            self.main_app.quit_app()
            return
        if self.dialog:
            self.dialog.set_working(False)
        if self.mode == "download" and self.downloaded and not self._failed and not self._cancelled:
            self._install()

    def cancel(self):
        if self._committed or (self.dialog and self.dialog.committed):
            return
        self._cancelled = True
        self.runner.cancel()
        self.main_app.cancel_update_preparation()
