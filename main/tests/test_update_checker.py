import json
import urllib.error

import pytest

from core.constants import PROJECT_RELEASES_LATEST_URL
from core.update_checker import (
    GitHubReleaseChecker,
    UpdateCheckError,
    comparable_version,
    fetch_latest_release,
    is_newer_version,
    parse_release_payload,
)


class _FakeResponse:
    def __init__(self, payload):
        self.payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self):
        return self.payload


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


def test_fetch_latest_release_builds_github_request(monkeypatch):
    calls = []

    def urlopen(request, timeout):
        calls.append((request, timeout))
        return _FakeResponse(b'{"tag_name": "v2.0.5", "body": "Bug fixes"}')

    monkeypatch.setattr("core.update_checker.urllib.request.urlopen", urlopen)

    release = fetch_latest_release(4321)

    request, timeout = calls[0]
    assert release.tag_name == "v2.0.5"
    assert timeout == 4.321
    assert request.get_header("Accept") == "application/vnd.github+json"
    assert request.get_header("User-agent") == "jietuba-update-checker"


def test_fetch_latest_release_reports_network_errors(monkeypatch):
    def urlopen(_request, timeout):
        assert timeout == 10
        raise urllib.error.URLError("network unavailable")

    monkeypatch.setattr("core.update_checker.urllib.request.urlopen", urlopen)

    with pytest.raises(UpdateCheckError, match="network unavailable"):
        fetch_latest_release(10_000)


def test_checker_runs_request_in_background_and_rejects_overlapping_checks(
    monkeypatch, qapp, qtbot
):
    def urlopen(_request, timeout):
        assert timeout == 4.321
        return _FakeResponse(b'{"tag_name": "v2.0.5"}')

    monkeypatch.setattr("core.update_checker.urllib.request.urlopen", urlopen)
    checker = GitHubReleaseChecker(timeout_ms=4321)

    with qtbot.waitSignal(checker.release_found) as blocker:
        assert checker.check() is True
        assert checker.is_checking is True
        assert checker.check() is False

    assert blocker.args[0].tag_name == "v2.0.5"
    qtbot.waitUntil(lambda: not checker.is_checking)


def test_checker_recovers_from_an_unexpected_worker_error(monkeypatch, qapp, qtbot):
    def urlopen(_request, _timeout):
        raise RuntimeError("unexpected failure")

    monkeypatch.setattr("core.update_checker.urllib.request.urlopen", urlopen)
    checker = GitHubReleaseChecker()

    with qtbot.waitSignal(checker.failed) as blocker:
        assert checker.check() is True

    assert blocker.args == ["Unexpected update-check error"]
    qtbot.waitUntil(lambda: not checker.is_checking)
