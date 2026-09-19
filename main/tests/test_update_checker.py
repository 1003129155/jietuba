import json

import pytest

from core.constants import PROJECT_RELEASES_LATEST_URL
from core.update_checker import (
    UpdateCheckError,
    comparable_version,
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
