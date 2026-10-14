# -*- coding: utf-8 -*-
"""Volcengine machine translation provider (matx_translate / TranslateText).

火山引擎文本机器翻译，走 OpenAPI 的 V4 签名（HMAC-SHA256）。这是独立的
机器翻译产品，不是火山方舟的豆包大模型——免费额度不同、鉴权方式也不同：
方舟是 ARK API Key（Bearer），这里要的是火山「访问控制-密钥管理」里的
AccessKey ID + Secret Access Key，两种不要混填。
"""

from __future__ import annotations

import hashlib
import hmac
import json
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from typing import Any, Mapping

from core import log_error

from ..models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
)
from ..provider import TextField, ToggleField, TranslationProvider

try:
    import zoneinfo
except ImportError:  # pragma: no cover - 3.9 以前没有 zoneinfo，用自带时区兜底
    zoneinfo = None


class VolcengineTranslateProvider(TranslationProvider):
    """火山引擎文本机器翻译（文本翻译 API，Action=TranslateText）。

    每月免费额度约 200 万字符。软件侧默认把额度锁在 199 万（QUOTA_LIMIT_CHARS），
    到点后由 service 层拦截、不再发起请求，避免误超免费额度产生费用；跨月自动
    重置。这一保护只对这个 provider 生效，不碰其他引擎。
    """

    provider_id = "volcengine"
    display_name = "Volcengine"
    API_URL = "https://translate.volcengineapi.com"
    REGION = "cn-north-1"
    SERVICE = "translate"
    ACTION = "TranslateText"
    VERSION = "2020-06-01"

    # 本地额度锁：199 万字符/月，到点自动停用，跨月重置。
    QUOTA_LIMIT_CHARS: int | None = 1_990_000

    # 应用内部语种（BCP-47 风格）到火山代码的映射。火山基本用 ISO 码，绝大多数
    # 语种直接透传即可；需要改的只有中文简/繁。反向（检测结果回填）同理。
    _LANGUAGE_CODES: dict[str, str] = {
        "zh-Hans": "zh",
        "zh-Hant": "zh-Hant",
    }
    _REVERSE_LANGUAGE_CODES: dict[str, str] = {
        "zh": "zh-Hans",
        "zh-Hant": "zh-Hant",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._access_key_id = str(
            config.get("access_key_id") or ""
        ).strip()
        self._secret_access_key = str(
            config.get("secret_access_key") or ""
        ).strip()

    CREDENTIAL_FIELDS = (
        TextField(
            "volcengine_translate_access_key",
            "Access Key ID",
            "Volcengine AccessKey AK (Console > Access Control > Key Management)",
        ),
        TextField(
            "volcengine_translate_secret_key",
            "Secret Access Key",
            "Volcengine AccessKey SK, paired with the AK",
            secret=True,
        ),
    )
    # 额度锁选项：开关 + 自定义上限（字符）。OPTION_FIELDS 自动渲染进设置页，
    # config_key 驱动读写，不需要改界面代码。
    OPTION_FIELDS = (
        ToggleField(
            "volcengine_translate_quota_enabled",
            "Enable monthly quota guard",
            "Stop translation once the monthly free quota is reached",
        ),
        TextField(
            "volcengine_translate_quota_limit",
            "Monthly quota (characters)",
            placeholder="1990000",
        ),
    )
    # 火山文本翻译免费额度约每月 200 万字符，超出后直接进入付费段、按量扣费
    # （服务端不会拒绝）。所以本地额度锁对火山是真防线，默认开启、可在设置里
    # 关掉或调上限，跨月自动重置。
    NOTICE = (
        "Volcengine machine translation: about 2M free characters/month. "
        "Overuse is billed at pay-as-you-go rates, so a local quota guard "
        "(default on) prevents surprise charges."
    )
    HELP_LABEL = "Volcengine Console"
    HELP_URL = "https://console.volcengine.com/iam/keymanage/"
    MIN_TIMEOUT = 5

    def is_configured(self) -> bool:
        return bool(self._access_key_id and self._secret_access_key)

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Volcengine AccessKey/SecretKey is not configured",
            )

        body = {
            "SourceLanguage": (
                self._to_code(request.source_lang)
                if request.source_lang
                else ""
            ),
            "TargetLanguage": self._to_code(request.target_lang),
            "TextList": [request.text],
        }
        payload = json.dumps(
            body, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        x_date = _utcnow("%Y%m%dT%H%M%SZ")
        headers = self._signed_headers(payload, x_date)

        http_request = urllib.request.Request(
            url=(
                f"{self.API_URL}/?Action={self.ACTION}"
                f"&Version={urllib.parse.quote(self.VERSION)}"
            ),
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
                f"Volcengine MT timed out after {request.timeout}s",
                "VolcengineTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Request timed out after {request.timeout}s",
            )
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", exc)
            log_error(
                f"Volcengine MT network error: {reason}",
                "VolcengineTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            )
        except (ValueError, UnicodeDecodeError) as exc:
            log_error(
                f"Volcengine MT response error: {exc}",
                "VolcengineTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Volcengine MT response",
            )
        except Exception as exc:
            log_error(
                f"Volcengine MT request failed: {exc}",
                "VolcengineTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            )

    # ---------------------------------------------------------------- 签名
    def _signed_headers(
        self, payload: bytes, x_date: str
    ) -> dict[str, str]:
        """构造 V4 签名后的请求头。

        与 AWS Signature V4 同构：canonical request -> string_to_sign ->
        派生签名密钥 -> signature。participating headers 固定为
        content-type / host / x-content-sha256 / x-date 四个。
        """
        hashed_payload = hashlib.sha256(payload).hexdigest()
        query = (
            f"Action={self.ACTION}&Version={urllib.parse.quote(self.VERSION)}"
        )
        short_date = x_date[:8]

        signed_headers_list = [
            "content-type",
            "host",
            "x-content-sha256",
            "x-date",
        ]
        canonical_headers = "".join(
            f"{k}:{v.strip()}\n"
            for k, v in {
                "content-type": "application/json",
                "host": urllib.parse.urlparse(self.API_URL).netloc,
                "x-content-sha256": hashed_payload,
                "x-date": x_date,
            }.items()
        )
        canonical_request = "\n".join(
            [
                "POST",
                "/",
                query,
                canonical_headers,
                ";".join(signed_headers_list),
                hashed_payload,
            ]
        )

        credential_scope = (
            f"{short_date}/{self.REGION}/{self.SERVICE}/request"
        )
        string_to_sign = "\n".join(
            [
                "HMAC-SHA256",
                x_date,
                credential_scope,
                hashlib.sha256(
                    canonical_request.encode("utf-8")
                ).hexdigest(),
            ]
        )

        k_date = _hmac(self._secret_access_key, short_date)
        k_region = _hmac(k_date, self.REGION)
        k_service = _hmac(k_region, self.SERVICE)
        signing_key = _hmac(k_service, "request")
        signature = hmac.new(
            signing_key, string_to_sign.encode("utf-8"), hashlib.sha256
        ).hexdigest()

        authorization = (
            f"HMAC-SHA256 Credential={self._access_key_id}/{credential_scope}, "
            f"SignedHeaders={';'.join(signed_headers_list)}, "
            f"Signature={signature}"
        )
        return {
            "Authorization": authorization,
            "Content-Type": "application/json",
            "Host": urllib.parse.urlparse(self.API_URL).netloc,
            "X-Content-Sha256": hashed_payload,
            "X-Date": x_date,
        }

    # ---------------------------------------------------------------- 解析
    def _parse_response(self, raw: Any) -> TranslationResult:
        if not isinstance(raw, dict):
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Volcengine MT response",
            )
        meta = raw.get("ResponseMetadata")
        if isinstance(meta, dict) and meta.get("Error"):
            error = meta["Error"]
            return self._error(
                self._map_api_error_code(str(error.get("Code") or "")),
                str(error.get("Message") or "Volcengine MT API error"),
            )

        results = raw.get("TranslationList") or []
        if not results:
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Volcengine MT response",
            )
        translated = "\n".join(
            str(item.get("Translation", "") or "")
            for item in results
            if isinstance(item, dict)
        )
        if not translated:
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Volcengine MT response",
            )
        first = results[0]
        detected = ""
        if isinstance(first, dict):
            detected = self._from_code(
                str(first.get("DetectedSourceLanguage", "") or "")
            )
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
            f"Volcengine MT HTTP {error.code}: {message}",
            "VolcengineTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_api_error_code(code: str) -> TranslationErrorCode:
        # 错误码见文本翻译 API 文档：-400 参数错、-415 不支持语向、
        # -429 过频、-500 服务内部错误、1000XX 接口错误。
        if code == "-415":
            return TranslationErrorCode.UNSUPPORTED_LANGUAGE
        if code == "-429":
            return TranslationErrorCode.RATE_LIMITED
        if code == "-400":
            return TranslationErrorCode.INVALID_REQUEST
        if code == "-500":
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


def _utcnow(fmt: str) -> str:
    """UTC 当前时间，按给定格式输出。3.9 起可带时区；低版本退化为 naive utc。"""
    if zoneinfo is not None:
        now = datetime.now(timezone.utc)
    else:  # pragma: no cover
        now = datetime.utcnow()
    return now.strftime(fmt)
