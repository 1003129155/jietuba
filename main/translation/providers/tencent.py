# -*- coding: utf-8 -*-
"""Tencent machine translation provider (TMT / TextTranslate).

腾讯云机器翻译 TMT，走 API 3.0 的 TC3-HMAC-SHA256 签名。免费额度每月约
500 万字符，软件侧默认锁到 499 万（QUOTA_LIMIT_CHARS），跨月重置。
"""

from __future__ import annotations

import hashlib
import hmac
import json
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from typing import Any, Mapping

from core import log_error

from ..models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
)
from ..provider import TextField, TranslationProvider


class TencentTranslateProvider(TranslationProvider):
    """腾讯云机器翻译（文本翻译 TextTranslate）。

    免费额度每月约 500 万字符，这里锁到 499 万防误超；到点由 service 层拦截，
    跨月重置。
    """

    provider_id = "tencent"
    display_name = "Tencent"
    API_URL = "https://tmt.tencentcloudapi.com"
    SERVICE = "tmt"
    ACTION = "TextTranslate"
    VERSION = "2018-03-21"
    REGION = "ap-guangzhou"
    PROJECT_ID = 0

    # 本地额度锁：499 万字符/月，到点自动停用，跨月重置。
    QUOTA_LIMIT_CHARS: int | None = 4_990_000

    # 应用内部语种 -> 腾讯代码。腾讯不支持 nl/pl/uk，所以这三个不映射，
    # 并通过 supported_target_languages() 明确告知界面不可选。
    _LANGUAGE_CODES: dict[str, str] = {
        "zh-Hans": "zh",
        "zh-Hant": "zh-TW",
        "en-US": "en",
        "en-GB": "en",
        "pt-BR": "pt",
        "pt-PT": "pt",
    }
    _REVERSE_LANGUAGE_CODES: dict[str, str] = {
        "zh": "zh-Hans",
        "zh-TW": "zh-Hant",
    }
    _UNSUPPORTED_TARGET = {"nl", "pl", "uk"}

    def __init__(self, config: Mapping[str, Any]):
        self._secret_id = str(config.get("secret_id") or "").strip()
        self._secret_key = str(config.get("secret_key") or "").strip()

    CREDENTIAL_FIELDS = (
        TextField(
            "tencent_translate_secret_id",
            "SecretId",
            "腾讯云 API 密钥 SecretId（控制台-访问管理-API密钥管理）",
        ),
        TextField(
            "tencent_translate_secret_key",
            "SecretKey",
            "腾讯云 API 密钥 SecretKey，与 SecretId 配对",
            secret=True,
        ),
    )
    NOTICE = "每月免费约 500 万字符；软件默认锁到 499 万，跨月重置"
    HELP_LABEL = "腾讯云-机器翻译-文本翻译"
    HELP_URL = "https://console.cloud.tencent.com/cam/capi"
    MIN_TIMEOUT = 5

    def is_configured(self) -> bool:
        return bool(self._secret_id and self._secret_key)

    def supported_target_languages(self) -> set[str] | None:
        # 腾讯只支持表内语言；把应用里它不支持的语种挡掉，避免提交后被服务端
        # 以 UnsupportedOperation.* 打回。基于 _LEGACY_LANGUAGE_CODES 的取值
        # （应用内部 BCP-47 风格代码）构造，再去掉 nl/pl/uk。
        from ..models import _LEGACY_LANGUAGE_CODES  # 避免循环导入风险

        supported = {
            code for code in _LEGACY_LANGUAGE_CODES.values() if code
        }
        for unsupported in ("nl", "pl", "uk"):
            supported.discard(unsupported)
        return supported

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Tencent SecretId/SecretKey is not configured",
            )
        if request.target_lang in self._UNSUPPORTED_TARGET:
            return self._error(
                TranslationErrorCode.UNSUPPORTED_LANGUAGE,
                f"Tencent does not support target language {request.target_lang}",
            )

        body = {
            "SourceText": request.text,
            "Source": (
                self._to_code(request.source_lang)
                if request.source_lang
                else "auto"
            ),
            "Target": self._to_code(request.target_lang),
            "ProjectId": self.PROJECT_ID,
        }
        payload = json.dumps(
            body, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        timestamp = str(int(time.time()))
        headers = self._signed_headers(payload, timestamp)

        http_request = urllib.request.Request(
            url=self.API_URL + "/",
            data=payload,
            method="POST",
            headers=headers,
        )
        try:
            with urllib.request.urlopen(
                http_request, timeout=request.timeout
            ) as response:
                raw = json.loads(response.read().decode("utf-8"))
            return self._parse_response(raw)
        except urllib.error.HTTPError as exc:
            return self._http_error(exc)
        except TimeoutError:
            log_error(
                f"Tencent MT timed out after {request.timeout}s",
                "TencentTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Request timed out after {request.timeout}s",
            )
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", exc)
            log_error(
                f"Tencent MT network error: {reason}",
                "TencentTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            )
        except (ValueError, UnicodeDecodeError) as exc:
            log_error(
                f"Tencent MT response error: {exc}",
                "TencentTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Tencent MT response",
            )
        except Exception as exc:
            log_error(
                f"Tencent MT request failed: {exc}",
                "TencentTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            )

    # ---------------------------------------------------------------- 签名
    def _signed_headers(
        self, payload: bytes, timestamp: str
    ) -> dict[str, str]:
        """构造 TC3-HMAC-SHA256 签名后的请求头。

        流程：canonical request -> string_to_sign -> 派生密钥 -> signature。
        content-type 必须与实际发送完全一致（含 charset），host 与
        x-tc-action 一并参与签名。
        """
        host = _parse_host(self.API_URL)
        content_type = "application/json; charset=utf-8"
        hashed_payload = hashlib.sha256(payload).hexdigest()

        canonical_headers = (
            f"content-type:{content_type}\n"
            f"host:{host}\n"
            f"x-tc-action:{self.ACTION.lower()}\n"
        )
        signed_headers = "content-type;host;x-tc-action"
        canonical_request = "\n".join(
            ["POST", "/", "", canonical_headers, signed_headers, hashed_payload]
        )

        # Date 必须由时间戳按 UTC 换算（东八区凌晨会差一天导致签名失败）。
        date = datetime.fromtimestamp(
            int(timestamp), tz=timezone.utc
        ).strftime("%Y-%m-%d")
        credential_scope = f"{date}/{self.SERVICE}/tc3_request"
        string_to_sign = "\n".join(
            [
                "TC3-HMAC-SHA256",
                timestamp,
                credential_scope,
                hashlib.sha256(
                    canonical_request.encode("utf-8")
                ).hexdigest(),
            ]
        )

        secret_date = _hmac("TC3" + self._secret_key, date)
        secret_service = _hmac(secret_date, self.SERVICE)
        secret_signing = _hmac(secret_service, "tc3_request")
        signature = hmac.new(
            secret_signing, string_to_sign.encode("utf-8"), hashlib.sha256
        ).hexdigest()

        authorization = (
            f"TC3-HMAC-SHA256 Credential={self._secret_id}/{credential_scope}, "
            f"SignedHeaders={signed_headers}, Signature={signature}"
        )
        return {
            "Authorization": authorization,
            "Content-Type": content_type,
            "Host": host,
            "X-TC-Action": self.ACTION,
            "X-TC-Region": self.REGION,
            "X-TC-Timestamp": timestamp,
            "X-TC-Version": self.VERSION,
        }

    # ---------------------------------------------------------------- 解析
    def _parse_response(self, raw: Any) -> TranslationResult:
        if not isinstance(raw, dict):
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Tencent MT response",
            )
        response = raw.get("Response")
        if not isinstance(response, dict):
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Tencent MT response",
            )
        error = response.get("Error")
        if isinstance(error, dict):
            code = str(error.get("Code") or "")
            return self._error(
                self._map_api_error_code(code),
                str(error.get("Message") or code),
            )

        translated = str(response.get("TargetText") or "")
        if not translated:
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Tencent MT response",
            )
        detected = self._from_code(str(response.get("Source") or ""))
        return TranslationResult(
            success=True,
            translated_text=translated,
            detected_source_lang=detected or "",
        )

    @classmethod
    def _to_code(cls, language_code: str) -> str:
        return cls._LANGUAGE_CODES.get(language_code, language_code)

    @classmethod
    def _from_code(cls, code: str) -> str:
        if not code:
            return ""
        return cls._REVERSE_LANGUAGE_CODES.get(code, code)

    def _http_error(
        self, error: urllib.error.HTTPError
    ) -> TranslationResult:
        try:
            body = error.read()
            data = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            data = {}
        if isinstance(data, dict):
            parsed = self._parse_response(data)
            if not parsed.success:
                return parsed
        message = str(error.reason or f"HTTP {error.code}")
        code = (
            TranslationErrorCode.AUTH_FAILED
            if error.code in (401, 403)
            else TranslationErrorCode.NETWORK_ERROR
            if error.code >= 500
            else TranslationErrorCode.UNKNOWN
        )
        log_error(
            f"Tencent MT HTTP {error.code}: {message}",
            "TencentTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_api_error_code(code: str) -> TranslationErrorCode:
        # 错误码见「文本翻译」接口文档及腾讯云公共错误码。
        if "AuthFailure" in code or "UnauthorizedOperation" in code:
            return TranslationErrorCode.AUTH_FAILED
        if "NoFreeAmount" in code:
            return TranslationErrorCode.QUOTA_EXCEEDED
        if code in {"FailedOperation.ServiceIsolate", "FailedOperation.StopUsing"}:
            return TranslationErrorCode.QUOTA_EXCEEDED
        if code == "FailedOperation.UserNotRegistered":
            return TranslationErrorCode.AUTH_FAILED
        if any(
            s in code
            for s in (
                "UnSupportedTargetLanguage",
                "UnsupportedLanguage",
                "UnsupportedSourceLanguage",
            )
        ):
            return TranslationErrorCode.UNSUPPORTED_LANGUAGE
        if "TextTooLong" in code:
            return TranslationErrorCode.INVALID_REQUEST
        if any(
            s in code
            for s in ("LimitedAccessFrequency", "RequestLimitExceeded", "LimitExceeded")
        ):
            return TranslationErrorCode.RATE_LIMITED
        if any(s in code for s in ("InvalidParameter", "MissingParameter")):
            return TranslationErrorCode.INVALID_REQUEST
        if "InternalError" in code:
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


# ---------------------------------------------------------------- 工具
def _hmac(key: bytes | str, msg: str) -> bytes:
    if isinstance(key, str):
        key = key.encode("utf-8")
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def _parse_host(url: str) -> str:
    import urllib.parse

    return urllib.parse.urlparse(url).netloc
