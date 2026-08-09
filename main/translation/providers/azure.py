# -*- coding: utf-8 -*-
"""Azure Cognitive Services Translator provider."""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
import uuid
from typing import Any, Mapping

from core import log_error

from ..models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
    normalize_language_code,
)
from ..provider import TranslationProvider


class AzureTranslateProvider(TranslationProvider):
    """Microsoft Azure Translator (Cognitive Services) REST API v3.0."""

    provider_id = "azure"
    display_name = "Azure Translator"
    API_URL = "https://api.cognitive.microsofttranslator.com/translate"
    API_VERSION = "3.0"
    MAX_TEXT_LENGTH = 50_000

    _LANGUAGE_CODES: dict[str, str] = {
        "zh-Hans": "zh-Hans",
        "zh-Hant": "zh-Hant",
        "en-US": "en",
        "en-GB": "en",
        "pt-BR": "pt",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._api_key = str(config.get("api_key", "") or "").strip()
        self._region = str(config.get("region", "") or "").strip()
        self._endpoint = str(
            config.get("endpoint", "") or ""
        ).strip().rstrip("/")

    def is_configured(self) -> bool:
        return bool(self._api_key)

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if len(request.text) > self.MAX_TEXT_LENGTH:
            return self._error(
                TranslationErrorCode.INVALID_REQUEST,
                "Text exceeds Azure Translator's 50,000-character limit",
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Azure Translator API key is not configured",
            )

        base_url = (
            f"{self._endpoint}/translate"
            if self._endpoint
            else self.API_URL
        )
        params: dict[str, str] = {
            "api-version": self.API_VERSION,
            "to": self._to_azure_code(request.target_lang),
        }
        if request.source_lang:
            params["from"] = self._to_azure_code(request.source_lang)

        url = f"{base_url}?{urllib.parse.urlencode(params)}"
        body = json.dumps(
            [{"Text": request.text}],
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")

        headers: dict[str, str] = {
            "Content-Type": "application/json; charset=utf-8",
            "Ocp-Apim-Subscription-Key": self._api_key,
            "X-ClientTraceId": str(uuid.uuid4()),
        }
        if self._region:
            headers["Ocp-Apim-Subscription-Region"] = self._region

        http_request = urllib.request.Request(
            url=url,
            data=body,
            method="POST",
            headers=headers,
        )

        try:
            with urllib.request.urlopen(
                http_request, timeout=request.timeout
            ) as response:
                raw = json.loads(response.read().decode("utf-8"))

            if not raw or not isinstance(raw, list):
                return self._error(
                    TranslationErrorCode.UNKNOWN,
                    "Invalid Azure Translator response",
                )
            translations = raw[0].get("translations", [])
            if not translations:
                return self._error(
                    TranslationErrorCode.UNKNOWN,
                    "Invalid Azure Translator response",
                )
            translated = str(translations[0].get("text", "") or "")
            if not translated:
                return self._error(
                    TranslationErrorCode.UNKNOWN,
                    "Invalid Azure Translator response",
                )
            detected = ""
            detected_lang = raw[0].get("detectedLanguage", {})
            if detected_lang:
                detected = normalize_language_code(
                    detected_lang.get("language", "")
                ) or ""
            return TranslationResult(
                success=True,
                translated_text=translated,
                detected_source_lang=detected,
            )
        except urllib.error.HTTPError as exc:
            return self._http_error(exc)
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", exc)
            log_error(
                f"Azure Translate network error: {reason}",
                "AzureTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            )
        except (ValueError, UnicodeDecodeError) as exc:
            log_error(
                f"Azure Translate response error: {exc}",
                "AzureTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Azure Translator response",
            )
        except Exception as exc:
            log_error(
                f"Azure Translate request failed: {exc}",
                "AzureTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            )

    @classmethod
    def _to_azure_code(cls, language_code: str) -> str:
        return cls._LANGUAGE_CODES.get(language_code, language_code)

    def _http_error(
        self, error: urllib.error.HTTPError
    ) -> TranslationResult:
        try:
            body = error.read()
            data = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            data = {}
        # Azure returns {"error":{"code":"...", "message":"..."}}
        err_obj = data.get("error", {}) if isinstance(data, dict) else {}
        err_code = str(err_obj.get("code", "") or "")
        message = str(
            err_obj.get("message")
            or error.reason
            or f"HTTP {error.code}"
        )
        code = self._map_error_code(err_code, error.code)
        log_error(
            f"Azure Translate HTTP {error.code}: {err_code or message}",
            "AzureTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_error_code(
        err_code: str, status_code: int
    ) -> TranslationErrorCode:
        code_lower = err_code.lower()
        if status_code in {401, 403} or "unauthorized" in code_lower:
            return TranslationErrorCode.AUTH_FAILED
        if status_code == 429 or "ratelimit" in code_lower:
            return TranslationErrorCode.RATE_LIMITED
        if status_code == 400:
            if "language" in code_lower:
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
