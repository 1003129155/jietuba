"""Provider-neutral translation request and result models."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Mapping


class TranslationErrorCode(str, Enum):
    NOT_CONFIGURED = "not_configured"
    AUTH_FAILED = "auth_failed"
    QUOTA_EXCEEDED = "quota_exceeded"
    RATE_LIMITED = "rate_limited"
    NETWORK_ERROR = "network_error"
    UNSUPPORTED_LANGUAGE = "unsupported_language"
    INVALID_REQUEST = "invalid_request"
    UNKNOWN = "unknown"


_LEGACY_LANGUAGE_CODES = {
    "ZH": "zh-Hans",
    "ZH-HANS": "zh-Hans",
    "ZH-HANT": "zh-Hant",
    "EN": "en",
    "EN-US": "en-US",
    "EN-GB": "en-GB",
    "JA": "ja",
    "KO": "ko",
    "VI": "vi",
    "DE": "de",
    "FR": "fr",
    "ES": "es",
    "IT": "it",
    "PT": "pt",
    "PT-BR": "pt-BR",
    "PT-PT": "pt-PT",
    "RU": "ru",
    "PL": "pl",
    "NL": "nl",
    "TR": "tr",
    "TH": "th",
    "AR": "ar",
    "ID": "id",
    "UK": "uk",
}


def normalize_language_code(code: str | None) -> str | None:
    """Convert legacy/provider codes to the application's BCP-47-style codes."""
    if not code or code.lower() == "auto":
        return None
    return _LEGACY_LANGUAGE_CODES.get(code.upper(), code)


@dataclass(frozen=True)
class TranslationRequest:
    text: str
    target_lang: str
    source_lang: str | None = None
    preserve_formatting: bool = True
    timeout: int = 10
    options: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "target_lang", normalize_language_code(self.target_lang) or "en"
        )
        object.__setattr__(
            self, "source_lang", normalize_language_code(self.source_lang)
        )


@dataclass(frozen=True)
class TranslationResult:
    success: bool
    translated_text: str = ""
    detected_source_lang: str = ""
    error_code: TranslationErrorCode | None = None
    error_message: str = ""

