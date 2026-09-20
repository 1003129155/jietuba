# -*- coding: utf-8 -*-
"""GitHub release lookup and version comparison."""

from __future__ import annotations

from dataclasses import dataclass
import json
import re
import urllib.error
import urllib.request

from PySide6.QtCore import QObject, QThread, Signal

from core.constants import (
    PROJECT_LATEST_RELEASE_API_URL,
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
    """Fetch and parse GitHub's latest-release response with the stdlib."""
    request = urllib.request.Request(
        PROJECT_LATEST_RELEASE_API_URL,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "jietuba-update-checker",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_ms / 1000) as response:
            return parse_release_payload(response.read())
    except UpdateCheckError:
        raise
    except urllib.error.HTTPError as exc:
        raise UpdateCheckError(f"HTTP {exc.code}: {exc.reason}") from exc
    except urllib.error.URLError as exc:
        raise UpdateCheckError(str(exc.reason)) from exc
    except (OSError, TimeoutError) as exc:
        raise UpdateCheckError(str(exc)) from exc


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
