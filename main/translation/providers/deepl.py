"""DeepL adapter for the provider-neutral translation API."""

from __future__ import annotations

from typing import Any, Mapping

from ..deepl_service import DeepLService
from ..models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
    normalize_language_code,
)
from ..provider import TextField, ToggleField, TranslationProvider


class DeepLProvider(TranslationProvider):
    provider_id = "deepl"
    display_name = "DeepL API"

    _LANGUAGE_CODES = {
        "zh-Hans": "ZH",
        "zh-Hant": "ZH-HANT",
        "en": "EN",
        "en-US": "EN-US",
        "en-GB": "EN-GB",
        "ja": "JA",
        "ko": "KO",
        "vi": "VI",
        "de": "DE",
        "fr": "FR",
        "es": "ES",
        "it": "IT",
        "pt": "PT",
        "pt-BR": "PT-BR",
        "pt-PT": "PT-PT",
        "ru": "RU",
        "pl": "PL",
        "nl": "NL",
        "tr": "TR",
        "th": "TH",
        "ar": "AR",
        "id": "ID",
        "uk": "UK",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._api_key = str(config.get("api_key", "") or "")
        self._use_pro = bool(config.get("use_pro", False))

    CREDENTIAL_FIELDS = (
        TextField("deepl_api_key", "DeepL API Key",
                  "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx:fx", secret=True),
    )
    OPTION_FIELDS = (
        ToggleField("deepl_use_pro", "Use DeepL Pro API",
                    "Enable if you have a paid DeepL subscription",
                    icon_name="CERTIFICATE"),
    )
    # 只有 DeepL 的接口收这两个参数。声明在这里而不是在界面里判断引擎名，
    # 界面才能「按声明渲染」而不是「按名字特判」。
    # 注意这只影响界面显不显示：请求里这两个参数对每家都照发，别家自己忽略。
    SUPPORTED_REQUEST_OPTIONS = frozenset({
        "split_sentences",
        "preserve_formatting",
    })
    NOTICE = "DeepL free tier: 500,000 chars/month. Get API key at"
    HELP_LABEL = "deepl.com/pro-api"
    HELP_URL = "https://www.deepl.com/pro-api"

    def is_configured(self) -> bool:
        return bool(self._api_key)

    def translate(self, request: TranslationRequest) -> TranslationResult:
        split_sentences = str(
            request.options.get("split_sentences", "nonewlines")
        )
        service = DeepLService(self._api_key, use_pro=self._use_pro)
        raw = service.translate(
            request.text,
            target_lang=self._to_deepl_code(request.target_lang),
            source_lang=(
                self._to_deepl_code(request.source_lang)
                if request.source_lang
                else None
            ),
            split_sentences=split_sentences,
            preserve_formatting=request.preserve_formatting,
            timeout=request.timeout,
        )
        if raw.get("success"):
            detected = normalize_language_code(
                raw.get("detected_source_lang", "")
            )
            return TranslationResult(
                success=True,
                translated_text=raw.get("translated_text", ""),
                detected_source_lang=detected or "",
            )

        message = str(raw.get("error", "") or "Translation failed")
        return TranslationResult(
            success=False,
            error_code=self._error_code(message),
            error_message=message,
        )

    @classmethod
    def _to_deepl_code(cls, language_code: str) -> str:
        return cls._LANGUAGE_CODES.get(language_code, language_code.upper())

    @staticmethod
    def _error_code(message: str) -> TranslationErrorCode:
        lowered = message.lower()
        if "api key" in lowered or "permission" in lowered:
            return TranslationErrorCode.AUTH_FAILED
        if "quota" in lowered:
            return TranslationErrorCode.QUOTA_EXCEEDED
        if "too many requests" in lowered:
            return TranslationErrorCode.RATE_LIMITED
        if "network" in lowered:
            return TranslationErrorCode.NETWORK_ERROR
        if "empty" in lowered or "bad request" in lowered:
            return TranslationErrorCode.INVALID_REQUEST
        return TranslationErrorCode.UNKNOWN

