"""Lightweight Google Cloud Translation Basic v2 provider."""

from __future__ import annotations

import html
import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Mapping

from core import log_error

from ..models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
    normalize_language_code,
)
from ..provider import TranslationProvider


class GoogleTranslateProvider(TranslationProvider):
    """Google NMT translation through the API-key-compatible v2 REST API."""

    provider_id = "google"
    display_name = "Google Translation"
    API_URL = "https://translation.googleapis.com/language/translate/v2"

    _LANGUAGE_CODES = {
        "zh-Hans": "zh-CN",
        "zh-Hant": "zh-TW",
        "en-US": "en",
        "en-GB": "en",
        "pt-BR": "pt",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._api_key = str(config.get("api_key", "") or "").strip()

    def is_configured(self) -> bool:
        return bool(self._api_key)

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Google Cloud Translation API key is not configured",
            )

        body = {
            "q": request.text,
            "target": self._to_google_code(request.target_lang),
            "format": "text",
        }
        if request.source_lang:
            body["source"] = self._to_google_code(request.source_lang)
        payload = json.dumps(
            body, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        url = f"{self.API_URL}?{urllib.parse.urlencode({'key': self._api_key})}"
        http_request = urllib.request.Request(
            url=url,
            data=payload,
            method="POST",
            headers={"Content-Type": "application/json; charset=utf-8"},
        )

        try:
            with urllib.request.urlopen(
                http_request, timeout=request.timeout
            ) as response:
                raw = json.loads(response.read().decode("utf-8"))
            translations = raw.get("data", {}).get("translations", [])
            if not translations:
                return self._error(
                    TranslationErrorCode.UNKNOWN,
                    "Invalid Google Cloud Translation response",
                )
            translation = translations[0]
            translated = html.unescape(
                str(translation.get("translatedText", "") or "")
            )
            if not translated:
                return self._error(
                    TranslationErrorCode.UNKNOWN,
                    "Invalid Google Cloud Translation response",
                )
            detected = normalize_language_code(
                translation.get("detectedSourceLanguage", "")
            )
            return TranslationResult(
                success=True,
                translated_text=translated,
                detected_source_lang=detected or "",
            )
        except urllib.error.HTTPError as exc:
            return self._http_error(exc)
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", exc)
            log_error(
                f"Google Translate network error: {reason}",
                "GoogleTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            )
        except (ValueError, UnicodeDecodeError) as exc:
            log_error(
                f"Google Translate response error: {exc}",
                "GoogleTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Google Cloud Translation response",
            )
        except Exception as exc:
            log_error(
                f"Google Translate request failed: {exc}",
                "GoogleTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            )

    @classmethod
    def _to_google_code(cls, language_code: str) -> str:
        return cls._LANGUAGE_CODES.get(language_code, language_code)

    def _http_error(
        self, error: urllib.error.HTTPError
    ) -> TranslationResult:
        try:
            body = error.read()
            data = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            data = {}
        details = data.get("error", {}) if isinstance(data, dict) else {}
        status = str(details.get("status", "") or "")
        message = str(
            details.get("message")
            or error.reason
            or f"HTTP {error.code}"
        )
        code = self._map_error_code(status, message, error.code)
        # Never log the request URL because its query contains the API key.
        log_error(
            f"Google Translate HTTP {error.code}: {status or message}",
            "GoogleTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_error_code(
        status: str, message: str, status_code: int
    ) -> TranslationErrorCode:
        lowered = message.lower()
        if (
            status == "RESOURCE_EXHAUSTED"
            or status_code == 429
            or "quota" in lowered
            or "rate limit" in lowered
        ):
            return TranslationErrorCode.RATE_LIMITED
        if status in {
            "UNAUTHENTICATED",
            "PERMISSION_DENIED",
        } or status_code in {401, 403}:
            return TranslationErrorCode.AUTH_FAILED
        if status == "INVALID_ARGUMENT" or status_code == 400:
            if "language" in lowered:
                return TranslationErrorCode.UNSUPPORTED_LANGUAGE
            return TranslationErrorCode.INVALID_REQUEST
        if status_code >= 500:
            return TranslationErrorCode.NETWORK_ERROR
        return TranslationErrorCode.UNKNOWN

    @staticmethod
    def _error(
        code: TranslationErrorCode, message: str
    ) -> TranslationResult:
        return TranslationResult(
            success=False,
            error_code=code,
            error_message=message,
        )
