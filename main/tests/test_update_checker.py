import json

import pytest
from PySide6.QtNetwork import QNetworkReply

from core.constants import PROJECT_RELEASES_LATEST_URL
from core.update_checker import (
    GitHubReleaseChecker,
    UpdateCheckError,
    comparable_version,
    is_newer_version,
    parse_release_payload,
)


class _FakeSignal:
    def __init__(self):
        self.callback = None

    def connect(self, callback):
        self.callback = callback


class _FakeReply:
    def __init__(self, payload=b"", error=QNetworkReply.NetworkError.NoError):
        self.finished = _FakeSignal()
        self.payload = payload
        self.network_error = error
        self.deleted = False

    def error(self):
        return self.network_error

    def errorString(self):
        return "network unavailable"

    def readAll(self):
        return self.payload

    def deleteLater(self):
        self.deleted = True


class _FakeManager:
    def __init__(self, reply):
        self.reply = reply
        self.requests = []

    def get(self, request):
        self.requests.append(request)
        return self.reply


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


def test_checker_builds_the_github_request_and_rejects_overlapping_checks(qapp):
    reply = _FakeReply()
    checker = GitHubReleaseChecker(timeout_ms=4321)
    checker._manager = _FakeManager(reply)

    assert checker.check() is True
    assert checker.is_checking is True
    assert checker.check() is False
    assert reply.finished.callback == checker._on_finished

    request = checker._manager.requests[0]
    assert request.rawHeader("Accept") == b"application/vnd.github+json"
    assert request.rawHeader("User-Agent") == b"jietuba-update-checker"
    assert request.transferTimeout() == 4321


def test_checker_emits_a_release_and_cleans_up_the_reply(qapp):
    reply = _FakeReply(b'{"tag_name": "v2.0.5", "body": "Bug fixes"}')
    checker = GitHubReleaseChecker()
    found = []
    checker.release_found.connect(found.append)
    checker._reply = reply

    checker._on_finished()

    assert checker.is_checking is False
    assert [release.tag_name for release in found] == ["v2.0.5"]
    assert reply.deleted is True


@pytest.mark.parametrize(
    ("reply", "message"),
    (
        (
            _FakeReply(error=QNetworkReply.NetworkError.ConnectionRefusedError),
            "network unavailable",
        ),
        (_FakeReply(b"not-json"), "Invalid response from GitHub"),
    ),
)
def test_checker_reports_network_and_payload_errors(qapp, reply, message):
    checker = GitHubReleaseChecker()
    failures = []
    checker.failed.connect(failures.append)
    checker._reply = reply

    checker._on_finished()

    assert failures == [message]
    assert reply.deleted is True


def test_stale_finished_signal_is_ignored(qapp):
    checker = GitHubReleaseChecker()

    checker._on_finished()

    assert checker.is_checking is False
