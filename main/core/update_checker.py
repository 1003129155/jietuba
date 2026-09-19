# -*- coding: utf-8 -*-
"""GitHub release lookup and version comparison."""

from __future__ import annotations

from dataclasses import dataclass
import json
import re

from PySide6.QtCore import QObject, QUrl, Signal
from PySide6.QtNetwork import QNetworkAccessManager, QNetworkReply, QNetworkRequest

from core.constants import (
    PROJECT_LATEST_RELEASE_API_URL,
    PROJECT_RELEASES_LATEST_URL,
)


_VERSION_PATTERN = re.compile(r"(?<!\d)\d+(?:\.\d+){0,3}(?![\d.])")


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


class GitHubReleaseChecker(QObject):
    """Fetch the latest public release without blocking the GUI thread."""

    release_found = Signal(object)
    failed = Signal(str)

    def __init__(self, parent: QObject | None = None, timeout_ms: int = 10_000):
        super().__init__(parent)
        self._manager = QNetworkAccessManager(self)
        self._timeout_ms = timeout_ms
        self._reply: QNetworkReply | None = None

    @property
    def is_checking(self) -> bool:
        return self._reply is not None

    def check(self) -> bool:
        if self.is_checking:
            return False

        request = QNetworkRequest(QUrl(PROJECT_LATEST_RELEASE_API_URL))
        request.setRawHeader(b"Accept", b"application/vnd.github+json")
        request.setRawHeader(b"User-Agent", b"jietuba-update-checker")
        request.setTransferTimeout(self._timeout_ms)

        self._reply = self._manager.get(request)
        self._reply.finished.connect(self._on_finished)
        return True

    def _on_finished(self):
        reply = self._reply
        self._reply = None
        if reply is None:
            return

        try:
            if reply.error() != QNetworkReply.NetworkError.NoError:
                self.failed.emit(reply.errorString())
                return

            try:
                release = parse_release_payload(bytes(reply.readAll()))
            except UpdateCheckError as exc:
                self.failed.emit(str(exc))
                return
            self.release_found.emit(release)
        finally:
            reply.deleteLater()
