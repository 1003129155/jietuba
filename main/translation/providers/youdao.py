# -*- coding: utf-8 -*-
"""Youdao AI Cloud text translation provider (openapi.youdao.com/api)."""

from __future__ import annotations

import hashlib
import json
import time
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
)
from ..provider import TextField, TranslationProvider


class YoudaoTranslateProvider(TranslationProvider):
    """有道智云文本翻译（应用 ID + 应用密钥，signType=v3 的 SHA-256 签名）。

    没有月度免费额度：新账号送体验金，用完按字符计费，所以不挂本地额度锁。
    """

    provider_id = "youdao"
    display_name = "Youdao"
    API_URL = "https://openapi.youdao.com/api"

    # 应用内部语种 -> 有道代码，只列二者不同的；没列到的透传。
    # 英式/美式、巴西/葡萄牙都收敛到同一个有道代码，所以反向表单独写。
    _LANGUAGE_CODES: dict[str, str] = {
        "zh-Hans": "zh-CHS",
        "zh-Hant": "zh-CHT",
        "en-US": "en",
        "en-GB": "en",
        "pt-BR": "pt",
        "pt-PT": "pt",
    }
    # 响应里的语言方向（如 "EN2zh-CHS"）大小写不固定，按小写查。
    _REVERSE_LANGUAGE_CODES: dict[str, str] = {
        "zh-chs": "zh-Hans",
        "zh-cht": "zh-Hant",
    }

    # 服务端只回错误码不回说明，说明由这里补，直接展示给用户。
    _ERROR_MESSAGES: dict[str, str] = {
        "101": "Missing required parameter",
        "102": "Unsupported language",
        "103": "Text is too long (up to 5000 characters per request)",
        "108": "Invalid application ID",
        "110": "The application is not bound to the text translation service",
        "111": "Invalid developer account",
        "112": "Invalid service requested",
        "113": "Text is empty",
        "202": "Signature verification failed, check the application ID and secret",
        "203": "Request IP is not in the application's allowed IP list",
        "206": "Invalid timestamp, check the system clock",
        "302": "Translation query failed",
        "303": "Youdao server error",
        "304": "Youdao session timed out",
        "401": "Youdao account is in arrears, please top up",
        "411": "Request rate limited",
        "412": "Long requests are too frequent",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._app_key = str(config.get("app_key") or "").strip()
        self._app_secret = str(config.get("app_secret") or "").strip()

    CREDENTIAL_FIELDS = (
        TextField(
            "youdao_translate_app_key",
            "Youdao App ID",
            "ID of an application bound to Text Translation (Application Management)",
        ),
        TextField(
            "youdao_translate_app_secret",
            "Youdao App Secret",
            "Application secret, paired with the application ID",
            secret=True,
        ),
    )
    NOTICE = (
        "Youdao AI Cloud: new accounts get a trial credit, after that "
        "translation is billed by character count."
    )
    HELP_LABEL = "Youdao AI Cloud Console"
    HELP_URL = "https://ai.youdao.com/console/"
    MIN_TIMEOUT = 5

    def is_configured(self) -> bool:
        return bool(self._app_key and self._app_secret)

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Youdao App ID/App Secret is not configured",
            )

        result, api_code = self._post(request, utf16_input=True)
        # 签名里的长度/截取按 UTF-16 单元还是码点算，文档的各语言示例并不一致，
        # 服务端取哪种无法从文档确定；仅在文本含增补平面字符（两种算法才有
        # 差别）且被拒签（202）时，换另一种算法重试一次。
        if api_code == "202" and _has_astral(request.text):
            result, _ = self._post(request, utf16_input=False)
        return result

    def _post(
        self, request: TranslationRequest, *, utf16_input: bool
    ) -> tuple[TranslationResult, str]:
        """发一次请求，返回结果和接口错误码（非接口层失败时为空串）。"""
        salt = str(uuid.uuid4())
        curtime = str(int(time.time()))
        form = {
            "q": request.text,
            "from": (
                self._to_code(request.source_lang)
                if request.source_lang
                else "auto"
            ),
            "to": self._to_code(request.target_lang),
            "appKey": self._app_key,
            "salt": salt,
            "sign": self._sign(
                request.text, salt, curtime, utf16_input=utf16_input
            ),
            "signType": "v3",
            "curtime": curtime,
        }
        http_request = urllib.request.Request(
            url=self.API_URL,
            data=urllib.parse.urlencode(form).encode("utf-8"),
            method="POST",
            headers={
                "Content-Type": "application/x-www-form-urlencoded; charset=UTF-8"
            },
        )
        try:
            with urllib.request.urlopen(
                http_request, timeout=request.timeout
            ) as response:
                raw = json.loads(response.read().decode("utf-8"))
            api_code = (
                str(raw.get("errorCode", "") or "")
                if isinstance(raw, dict)
                else ""
            )
            return self._parse_response(raw), api_code
        except urllib.error.HTTPError as exc:
            return self._http_error(exc), ""
        except TimeoutError:
            log_error(
                f"Youdao Translate timed out after {request.timeout}s",
                "YoudaoTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Request timed out after {request.timeout}s",
            ), ""
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", exc)
            log_error(
                f"Youdao Translate network error: {reason}",
                "YoudaoTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            ), ""
        except (ValueError, UnicodeDecodeError) as exc:
            log_error(
                f"Youdao Translate response error: {exc}",
                "YoudaoTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Youdao Translate response",
            ), ""
        except Exception as exc:
            log_error(
                f"Youdao Translate request failed: {exc}",
                "YoudaoTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            ), ""

    def _sign(
        self, text: str, salt: str, curtime: str, *, utf16_input: bool = True
    ) -> str:
        """SHA-256(应用 ID + input + salt + curtime + 应用密钥)，小写十六进制。

        input 在文本不超过 20 个字符时就是文本本身，否则是「前 10 个字符 +
        总长度 + 后 10 个字符」。
        """
        raw = (
            f"{self._app_key}{_sign_input(text, utf16_input)}"
            f"{salt}{curtime}{self._app_secret}"
        )
        # UTF-16 模式下截取可能劈开代理对，孤立代理项按 "?" 编码。
        return hashlib.sha256(raw.encode("utf-8", errors="replace")).hexdigest()

    def _parse_response(self, raw: Any) -> TranslationResult:
        # 有道鉴权、参数出错也回 HTTP 200，错误码在 body 的 errorCode 里
        # （字符串，"0" 为成功）。
        if not isinstance(raw, dict):
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Youdao Translate response",
            )
        error_code = str(raw.get("errorCode", "") or "")
        if error_code != "0":
            message = self._ERROR_MESSAGES.get(
                error_code, f"Youdao error {error_code or 'unknown'}"
            )
            log_error(
                f"Youdao Translate API error {error_code}: {message}",
                "YoudaoTranslate",
            )
            return self._error(self._map_api_error_code(error_code), message)

        translation = raw.get("translation")
        if not isinstance(translation, list):
            translation = []
        translated = "\n".join(str(item or "") for item in translation)
        if not translated.strip():
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Youdao Translate response",
            )
        # l 形如 "EN2zh-CHS" / "auto2zh-CHS"，语言代码本身不含数字 2。
        source = str(raw.get("l", "") or "").split("2", 1)[0]
        return TranslationResult(
            success=True,
            translated_text=translated,
            detected_source_lang=self._from_code(source),
        )

    @classmethod
    def _to_code(cls, language_code: str) -> str:
        return cls._LANGUAGE_CODES.get(language_code, language_code)

    @classmethod
    def _from_code(cls, code: str) -> str:
        if not code or code.lower() == "auto":
            return ""
        return cls._REVERSE_LANGUAGE_CODES.get(code.lower(), code.lower())

    def _http_error(
        self, error: urllib.error.HTTPError
    ) -> TranslationResult:
        message = str(error.reason or f"HTTP {error.code}")
        code = (
            TranslationErrorCode.AUTH_FAILED
            if error.code in (401, 403)
            else TranslationErrorCode.NETWORK_ERROR
            if error.code >= 500
            else TranslationErrorCode.UNKNOWN
        )
        log_error(
            f"Youdao Translate HTTP {error.code}: {message}",
            "YoudaoTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_api_error_code(error_code: str) -> TranslationErrorCode:
        # 错误码表见有道智云「文本翻译」文档的「错误代码列表」。
        if error_code in {"108", "110", "111", "112", "202", "203"}:
            return TranslationErrorCode.AUTH_FAILED
        if error_code == "401":
            return TranslationErrorCode.QUOTA_EXCEEDED
        if error_code in {"411", "412"}:
            return TranslationErrorCode.RATE_LIMITED
        if error_code == "102":
            return TranslationErrorCode.UNSUPPORTED_LANGUAGE
        if error_code in {"101", "103", "113", "206"}:
            return TranslationErrorCode.INVALID_REQUEST
        if error_code in {"302", "303", "304"}:
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


def _has_astral(text: str) -> bool:
    return any(ord(ch) > 0xFFFF for ch in text)


def _sign_input(text: str, utf16: bool) -> str:
    """签名用的 input。utf16=True 按 UTF-16 单元数长度和截取，False 按码点。"""
    if utf16:
        units = text.encode("utf-16-le")
        length = len(units) // 2
        if length <= 20:
            return text
        head = units[:20].decode("utf-16-le", errors="surrogatepass")
        tail = units[-20:].decode("utf-16-le", errors="surrogatepass")
        return f"{head}{length}{tail}"
    if len(text) <= 20:
        return text
    return f"{text[:10]}{len(text)}{text[-10:]}"
