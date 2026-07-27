"""Lightweight Amazon Translate provider using AWS Signature Version 4."""

from __future__ import annotations

import datetime as dt
import hashlib
import hmac
import json
import urllib.error
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


class AmazonTranslateProvider(TranslationProvider):
    """Amazon Translate adapter without the boto3/botocore dependency."""

    provider_id = "amazon"
    display_name = "Amazon Translate"

    SERVICE = "translate"
    TARGET = "AWSShineFrontendService_20170701.TranslateText"
    CONTENT_TYPE = "application/x-amz-json-1.1"
    ALGORITHM = "AWS4-HMAC-SHA256"
    MAX_TEXT_BYTES = 10_000

    _LANGUAGE_CODES = {
        "zh-Hans": "zh",
        "zh-Hant": "zh-TW",
        "en-US": "en",
        "en-GB": "en",
        "pt-BR": "pt",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._region = str(config.get("region", "us-west-2") or "").strip()
        self._access_key_id = str(
            config.get("access_key_id", "") or ""
        ).strip()
        self._secret_access_key = str(
            config.get("secret_access_key", "") or ""
        ).strip()
        self._session_token = str(
            config.get("session_token", "") or ""
        ).strip()

    def is_configured(self) -> bool:
        return bool(
            self._region
            and self._access_key_id
            and self._secret_access_key
        )

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if len(request.text.encode("utf-8")) > self.MAX_TEXT_BYTES:
            return self._error(
                TranslationErrorCode.INVALID_REQUEST,
                "Text exceeds Amazon Translate's 10,000-byte limit",
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Amazon Translate credentials are not configured",
            )

        payload = json.dumps(
            {
                "Text": request.text,
                "SourceLanguageCode": self._to_amazon_code(
                    request.source_lang
                ),
                "TargetLanguageCode": self._to_amazon_code(
                    request.target_lang
                ),
            },
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")

        try:
            url, headers = self._signed_request(payload)
            http_request = urllib.request.Request(
                url=url,
                data=payload,
                headers=headers,
                method="POST",
            )
            with urllib.request.urlopen(
                http_request, timeout=request.timeout
            ) as response:
                raw = json.loads(response.read().decode("utf-8"))

            translated = str(raw.get("TranslatedText", "") or "")
            if not translated:
                return self._error(
                    TranslationErrorCode.UNKNOWN,
                    "Invalid Amazon Translate response",
                )
            detected = normalize_language_code(
                raw.get("SourceLanguageCode", "")
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
                f"Amazon Translate network error: {reason}",
                "AmazonTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            )
        except (ValueError, json.JSONDecodeError) as exc:
            log_error(
                f"Amazon Translate response error: {exc}",
                "AmazonTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Amazon Translate response",
            )
        except Exception as exc:
            log_error(
                f"Amazon Translate request failed: {exc}",
                "AmazonTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            )

    def _signed_request(
        self, payload: bytes, now: dt.datetime | None = None
    ) -> tuple[str, dict[str, str]]:
        now = now or dt.datetime.now(dt.timezone.utc)
        if now.tzinfo is None:
            now = now.replace(tzinfo=dt.timezone.utc)
        now = now.astimezone(dt.timezone.utc)
        amz_date = now.strftime("%Y%m%dT%H%M%SZ")
        date_stamp = now.strftime("%Y%m%d")

        host = self._host()
        headers = {
            "content-type": self.CONTENT_TYPE,
            "host": host,
            "x-amz-date": amz_date,
            "x-amz-target": self.TARGET,
        }
        if self._session_token:
            headers["x-amz-security-token"] = self._session_token

        canonical_header_text = "".join(
            f"{name}:{self._normalize_header(value)}\n"
            for name, value in sorted(headers.items())
        )
        signed_headers = ";".join(sorted(headers))
        payload_hash = hashlib.sha256(payload).hexdigest()
        canonical_request = "\n".join(
            (
                "POST",
                "/",
                "",
                canonical_header_text,
                signed_headers,
                payload_hash,
            )
        )
        credential_scope = (
            f"{date_stamp}/{self._region}/{self.SERVICE}/aws4_request"
        )
        string_to_sign = "\n".join(
            (
                self.ALGORITHM,
                amz_date,
                credential_scope,
                hashlib.sha256(canonical_request.encode("utf-8")).hexdigest(),
            )
        )
        signing_key = self._signing_key(date_stamp)
        signature = hmac.new(
            signing_key, string_to_sign.encode("utf-8"), hashlib.sha256
        ).hexdigest()
        headers["authorization"] = (
            f"{self.ALGORITHM} "
            f"Credential={self._access_key_id}/{credential_scope}, "
            f"SignedHeaders={signed_headers}, Signature={signature}"
        )
        return f"https://{host}/", headers

    def _signing_key(self, date_stamp: str) -> bytes:
        key_date = hmac.new(
            f"AWS4{self._secret_access_key}".encode("utf-8"),
            date_stamp.encode("utf-8"),
            hashlib.sha256,
        ).digest()
        key_region = hmac.new(
            key_date, self._region.encode("utf-8"), hashlib.sha256
        ).digest()
        key_service = hmac.new(
            key_region, self.SERVICE.encode("utf-8"), hashlib.sha256
        ).digest()
        return hmac.new(
            key_service, b"aws4_request", hashlib.sha256
        ).digest()

    def _host(self) -> str:
        suffix = (
            "amazonaws.com.cn"
            if self._region.startswith("cn-")
            else "amazonaws.com"
        )
        return f"translate.{self._region}.{suffix}"

    @classmethod
    def _to_amazon_code(cls, language_code: str | None) -> str:
        if not language_code:
            return "auto"
        return cls._LANGUAGE_CODES.get(language_code, language_code)

    @staticmethod
    def _normalize_header(value: str) -> str:
        return " ".join(value.strip().split())

    def _http_error(
        self, error: urllib.error.HTTPError
    ) -> TranslationResult:
        body = b""
        try:
            body = error.read()
            data = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            data = {}
        error_type = str(
            data.get("__type")
            or data.get("code")
            or data.get("Code")
            or ""
        ).split("#")[-1]
        message = str(
            data.get("message")
            or data.get("Message")
            or error.reason
            or f"HTTP {error.code}"
        )
        code = self._map_error_code(error_type, error.code)
        log_error(
            f"Amazon Translate HTTP {error.code}: {error_type or message}",
            "AmazonTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_error_code(
        error_type: str, status_code: int
    ) -> TranslationErrorCode:
        if error_type in {
            "AccessDeniedException",
            "InvalidSignatureException",
            "UnrecognizedClientException",
            "IncompleteSignature",
        } or status_code in {401, 403}:
            return TranslationErrorCode.AUTH_FAILED
        if error_type in {
            "TooManyRequestsException",
            "ThrottlingException",
        } or status_code == 429:
            return TranslationErrorCode.RATE_LIMITED
        if error_type == "UnsupportedLanguagePairException":
            return TranslationErrorCode.UNSUPPORTED_LANGUAGE
        if error_type in {
            "InvalidRequestException",
            "TextSizeLimitExceededException",
            "DetectedLanguageLowConfidenceException",
        } or status_code == 400:
            return TranslationErrorCode.INVALID_REQUEST
        if error_type in {
            "InternalServerException",
            "ServiceUnavailableException",
        } or status_code >= 500:
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
