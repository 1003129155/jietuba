# -*- coding: utf-8 -*-
"""Baidu large-model text translation provider (aiTextTranslate)."""

from __future__ import annotations

import hashlib
import json
import random
import urllib.error
import urllib.request
from typing import Any, Mapping

from core import log_error

from ..models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
)
from ..provider import TextField, TranslationProvider


class BaiduTranslateProvider(TranslationProvider):
    """百度大模型文本翻译 API（APPID + 密钥的 MD5 签名鉴权）。

    这个端点同时支持 Bearer Token 和签名两种鉴权，这里只走签名，因为翻译开放
    平台注册后控制台发的就是 APPID + 密钥这一套；Bearer 要的是另外单独申请的
    大模型 API Key，多数用户手上没有。实测拿标准凭据走 Bearer 会被回
    ``54001 invalid token``——和随便填一个字符串的报错一模一样，排查起来毫无
    线索，所以不留这条路。
    """

    provider_id = "baidu"
    display_name = "Baidu Translate"
    API_URL = "https://fanyi-api.baidu.com/ait/api/aiTextTranslate"

    # 百度的语种代码和本应用内部代码（BCP-47 风格）不完全一致，这里只列出
    # 二者不同的那些；没列到的直接透传（百度扩展语种大多和 ISO 码一致）。
    # 两个方向分开写而不是互相推导，是因为好几个应用代码（en-US/en-GB、
    # pt-BR/pt-PT）会收敛到同一个百度代码，推导不出唯一的反向映射。
    #
    # 透传能不能用是逐个打过真实接口验的：应用里 19 个目标语种，只有乌克兰语
    # 需要映射（百度要 ukr，透传 uk 会回 58001 语言方向不支持）；tr/id/th/nl
    # 这些看着像该用三字母码的，实测透传就对，不要想当然补。
    _LANGUAGE_CODES: dict[str, str] = {
        "zh-Hans": "zh",
        "zh-Hant": "cht",
        "ja": "jp",
        "ko": "kor",
        "vi": "vie",
        "fr": "fra",
        "es": "spa",
        "ar": "ara",
        "uk": "ukr",
        "en-US": "en",
        "en-GB": "en",
        "pt-BR": "pt",
        "pt-PT": "pt",
    }
    _REVERSE_LANGUAGE_CODES: dict[str, str] = {
        "zh": "zh-Hans",
        "cht": "zh-Hant",
        "jp": "ja",
        "kor": "ko",
        "vie": "vi",
        "fra": "fr",
        "spa": "es",
        "ara": "ar",
        "ukr": "uk",
    }

    def __init__(self, config: Mapping[str, Any]):
        self._appid = str(config.get("appid", "") or "").strip()
        self._secret_key = str(config.get("secret_key", "") or "").strip()

    CREDENTIAL_FIELDS = (
        TextField("baidu_translate_appid", "Baidu APPID", "APPID"),
        # 标签写「密钥」而不是「API Key」：控制台上这一栏就叫密钥，且百度另有
        # 一个叫 API Key 的东西（大模型单独申请的那个）。叫错名字的直接后果是
        # 用户填了 API Key、撞 54001，而界面上只显示 invalid token，无从判断。
        TextField("baidu_translate_secret_key", "Baidu 密钥",
                  "控制台「开发者信息」里与 APPID 成对的密钥",
                  secret=True),
    )
    HELP_LABEL = "百度翻译开放平台 · 开发者信息"
    HELP_URL = "https://fanyi-api.baidu.com/manage/developer"

    def is_configured(self) -> bool:
        return bool(self._appid and self._secret_key)

    def translate(self, request: TranslationRequest) -> TranslationResult:
        if not request.text or not request.text.strip():
            return self._error(
                TranslationErrorCode.INVALID_REQUEST, "Text is empty"
            )
        if not self.is_configured():
            return self._error(
                TranslationErrorCode.NOT_CONFIGURED,
                "Baidu Translate APPID/密钥 is not configured",
            )

        # salt 必须是数字：服务端 AITextRequest.Salt 是 uint64，传字符串会被回
        # 53001 parse json body error。appid 反过来必须是字符串，传数字同样报
        # 53001。两者都是打真实接口撞出来的，JSON 里别顺手改类型。
        salt = random.randint(100000, 999999)
        body = {
            "appid": self._appid,
            "from": (
                self._to_baidu_code(request.source_lang)
                if request.source_lang
                else "auto"
            ),
            "to": self._to_baidu_code(request.target_lang),
            "q": request.text,
            "salt": salt,
            "sign": self._sign(request.text, salt),
        }
        payload = json.dumps(
            body, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        http_request = urllib.request.Request(
            url=self.API_URL,
            data=payload,
            method="POST",
            headers={"Content-Type": "application/json"},
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
            # 读超时抛的是 TimeoutError，它不是 URLError 的子类，不加这条就会
            # 一路掉进下面的兜底：归类成 UNKNOWN，还把「The read operation
            # timed out」这种英文原文直接甩给用户。
            log_error(
                f"Baidu Translate timed out after {request.timeout}s",
                "BaiduTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Request timed out after {request.timeout}s",
            )
        except urllib.error.URLError as exc:
            reason = getattr(exc, "reason", exc)
            log_error(
                f"Baidu Translate network error: {reason}",
                "BaiduTranslate",
            )
            return self._error(
                TranslationErrorCode.NETWORK_ERROR,
                f"Network error: {reason}",
            )
        except (ValueError, UnicodeDecodeError) as exc:
            log_error(
                f"Baidu Translate response error: {exc}",
                "BaiduTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Failed to parse Baidu Translate response",
            )
        except Exception as exc:
            log_error(
                f"Baidu Translate request failed: {exc}",
                "BaiduTranslate",
            )
            return self._error(
                TranslationErrorCode.UNKNOWN,
                f"Translation failed: {exc}",
            )

    def _sign(self, text: str, salt: int) -> str:
        """MD5(appid + q + salt + 密钥)，小写十六进制。

        拼接用的 salt 是它的十进制字符串形态，放进 JSON 的却必须是数字本身，
        两处形态不同是这版接口的要求，不是笔误。
        """
        raw = f"{self._appid}{text}{salt}{self._secret_key}"
        return hashlib.md5(raw.encode("utf-8")).hexdigest()

    def _parse_response(self, raw: Any) -> TranslationResult:
        # 百度即使鉴权/参数出错也回 HTTP 200，错误信息包在 body 里的
        # error_code/error_msg 中，必须在这里而不是靠 HTTPError 来识别失败。
        if not isinstance(raw, dict):
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Baidu Translate response",
            )
        error_code = raw.get("error_code")
        if error_code and str(error_code) != "0":
            message = str(raw.get("error_msg") or f"error_code={error_code}")
            log_error(
                f"Baidu Translate API error {error_code}: {message}",
                "BaiduTranslate",
            )
            return self._error(self._map_api_error_code(str(error_code)), message)

        results = raw.get("trans_result") or []
        if not results:
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Baidu Translate response",
            )
        translated = "\n".join(str(item.get("dst", "") or "") for item in results)
        if not translated:
            return self._error(
                TranslationErrorCode.UNKNOWN,
                "Invalid Baidu Translate response",
            )
        detected = self._from_baidu_code(str(raw.get("from", "") or ""))
        return TranslationResult(
            success=True,
            translated_text=translated,
            detected_source_lang=detected or "",
        )

    @classmethod
    def _to_baidu_code(cls, language_code: str) -> str:
        return cls._LANGUAGE_CODES.get(language_code, language_code)

    @classmethod
    def _from_baidu_code(cls, baidu_code: str) -> str:
        if not baidu_code:
            return ""
        return cls._REVERSE_LANGUAGE_CODES.get(baidu_code, baidu_code)

    def _http_error(
        self, error: urllib.error.HTTPError
    ) -> TranslationResult:
        try:
            body = error.read()
            data = json.loads(body.decode("utf-8")) if body else {}
        except (ValueError, UnicodeDecodeError):
            data = {}
        if isinstance(data, dict) and data.get("error_code"):
            return self._parse_response(data)
        message = str(error.reason or f"HTTP {error.code}")
        code = (
            TranslationErrorCode.AUTH_FAILED
            if error.code in (401, 403)
            else TranslationErrorCode.NETWORK_ERROR
            if error.code >= 500
            else TranslationErrorCode.UNKNOWN
        )
        log_error(
            f"Baidu Translate HTTP {error.code}: {message}",
            "BaiduTranslate",
        )
        return self._error(code, message)

    @staticmethod
    def _map_api_error_code(error_code: str) -> TranslationErrorCode:
        # 错误码表见 https://fanyi-api.baidu.com/doc/21（大模型版与通用版共用）。
        # 53001 是请求体解析失败，文档里没列，是打真实接口撞出来的——漏了它会
        # 让「字段类型写错」这类问题落进 UNKNOWN，报不出可操作的信息。
        if error_code in {"52003", "54001", "58000"}:
            return TranslationErrorCode.AUTH_FAILED
        if error_code in {"53001", "54000"}:
            return TranslationErrorCode.INVALID_REQUEST
        if error_code in {"54003", "54005"}:
            return TranslationErrorCode.RATE_LIMITED
        if error_code == "54004":
            return TranslationErrorCode.QUOTA_EXCEEDED
        if error_code == "58001":
            return TranslationErrorCode.UNSUPPORTED_LANGUAGE
        if error_code in {"52001", "52002"}:
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
