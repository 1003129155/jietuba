# -*- coding: utf-8 -*-
"""GitHub release lookup and version comparison."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import re
import subprocess
import sys

from core.updater_process import app_variant, helper_path
from core.update_cache import remove_helper

from PySide6.QtCore import QObject, QThread, Signal

from core.constants import (
    PROJECT_RELEASES_LATEST_URL,
)
from core.logger import log_exception


_VERSION_PATTERN = re.compile(r"(?<!\d)\d+(?:\.\d+){0,3}(?![\d.])")
_RUNNING_CHECK_THREADS: set[QThread] = set()


class UpdateCheckError(ValueError):
    """Raised when GitHub does not return a usable release."""


@dataclass(frozen=True)
class ReleaseInfo:
    tag_name: str
    title: str
    notes: str
    url: str
    available: bool | None = None
    asset_name: str = ""
    arch: str = ""
    artifact: dict = field(default_factory=dict)


def comparable_version(value: str) -> tuple[int, int, int, int]:
    """Extract a numeric version from tags such as ``v2.1`` or ``release-2.1``."""
    match = _VERSION_PATTERN.search(value.strip())
    if match is None:
        raise UpdateCheckError(f"Invalid release version: {value!r}")

    parts = [int(part) for part in match.group(0).split(".")]
    parts.extend([0] * (4 - len(parts)))
    return tuple(parts[:4])


def is_newer_version(remote_version: str, current_version: str) -> bool:
    return comparable_version(remote_version) > comparable_version(current_version)


def parse_release_payload(payload: bytes) -> ReleaseInfo:
    try:
        data = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise UpdateCheckError("Invalid response from GitHub") from exc

    if not isinstance(data, dict):
        raise UpdateCheckError("Invalid response from GitHub")

    tag_name = data.get("tag_name")
    if not isinstance(tag_name, str) or not tag_name.strip():
        raise UpdateCheckError("The latest GitHub release has no version tag")
    comparable_version(tag_name)

    title = data.get("name")
    notes = data.get("body")
    return ReleaseInfo(
        tag_name=tag_name.strip(),
        title=title.strip() if isinstance(title, str) and title.strip() else tag_name.strip(),
        notes=notes.strip() if isinstance(notes, str) else "",
        url=PROJECT_RELEASES_LATEST_URL,
    )


def fetch_latest_release(timeout_ms: int) -> ReleaseInfo:
    """Fetch the release matching the installed executable through the Rust helper."""
    from main_app import APP_VERSION
    executable = None
    try:
        executable = helper_path()
        result = subprocess.run(
            [str(executable), "check", "--install-exe", sys.executable,
             "--current-version", APP_VERSION, "--variant", app_variant()],
            capture_output=True, timeout=timeout_ms / 1000,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        events = [json.loads(line) for line in result.stdout.splitlines() if line]
        for event in events:
            if event.get("protocol") != 1:
                raise UpdateCheckError("Invalid updater protocol")
            if event.get("event") == "error":
                raise UpdateCheckError(event["data"].get("message", "Update check failed"))
            if event.get("event") in ("available", "up_to_date") and result.returncode == 0:
                data = event["data"]
                return ReleaseInfo(
                    data["tag_name"], data["title"], data["notes"], data["url"],
                    event["event"] == "available", data["asset_name"], data["arch"], data["artifact"],
                )
        raise UpdateCheckError("Updater returned no release")
    except (OSError, subprocess.TimeoutExpired, ValueError, KeyError, TypeError) as exc:
        raise UpdateCheckError(str(exc)) from exc
    finally:
        remove_helper(executable)


class _ReleaseCheckThread(QThread):
    """Run the blocking stdlib request outside Qt's GUI thread."""

    release_found = Signal(object)
    failed = Signal(str)

    def __init__(self, timeout_ms: int):
        super().__init__()
        self._timeout_ms = timeout_ms

    def run(self) -> None:
        try:
            self.release_found.emit(fetch_latest_release(self._timeout_ms))
        except UpdateCheckError as exc:
            self.failed.emit(str(exc))
        except Exception as exc:
            log_exception(exc, "Update checker worker")
            self.failed.emit("Unexpected update-check error")


def _retire_thread(thread: QThread) -> None:
    """Keep a running QThread alive if its settings dialog is closed early."""
    _RUNNING_CHECK_THREADS.discard(thread)
    thread.deleteLater()


class GitHubReleaseChecker(QObject):
    """Fetch the latest public release without blocking the GUI thread."""

    release_found = Signal(object)
    failed = Signal(str)

    def __init__(self, parent: QObject | None = None, timeout_ms: int = 10_000):
        super().__init__(parent)
        self._timeout_ms = timeout_ms
        self._thread: _ReleaseCheckThread | None = None

    @property
    def is_checking(self) -> bool:
        return self._thread is not None

    def check(self) -> bool:
        if self.is_checking:
            return False

        thread = _ReleaseCheckThread(self._timeout_ms)
        self._thread = thread
        _RUNNING_CHECK_THREADS.add(thread)
        thread.release_found.connect(self.release_found)
        thread.failed.connect(self.failed)
        thread.finished.connect(self._on_finished)
        thread.finished.connect(lambda: _retire_thread(thread))
        thread.start()
        return True

    def _on_finished(self):
        self._thread = None
