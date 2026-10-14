# -*- coding: utf-8 -*-
"""火山 / 腾讯机器翻译 provider 的离线测试（响应解析 + 错误映射 + 签名结构）。

不走网络：monkeypatch 替换 urllib.request.urlopen，与 test_translation_architecture
里其他引擎同一套写法，覆盖：
- 成功翻译 + 请求结构（签名头、URL、body）
- 语源自动检测（source_lang 为空）
- 接口级错误码映射（火山 -415/-500，腾讯 NoFreeAmount / AuthFailure / 不支持语向）
"""

import io
import json
import re
import urllib.error

import pytest

from translation.models import TranslationErrorCode, TranslationRequest
from translation.providers.tencent import TencentTranslateProvider
from translation.providers.volcengine import VolcengineTranslateProvider


def _volcengine(config=None):
    return VolcengineTranslateProvider(
        config
        or {"access_key_id": "AKIDvolc", "secret_access_key": "volc-secret"}
    )


def _tencent(config=None):
    return TencentTranslateProvider(
        config
        or {"secret_id": "AKIDtencent", "secret_key": "tencent-secret"}
    )


class _Response:
    def __init__(self, data):
        self._data = data

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def read(self):
        return self._data


# ============================================================================
# 火山引擎
# ============================================================================

def test_volcengine_translates_and_sends_signed_request(monkeypatch):
    captured = {}

    def _urlopen(request, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return _Response(
            json.dumps(
                {
                    "ResponseMetadata": {"RequestId": "r1"},
                    "TranslationList": [
                        {
                            "Translation": "你好",
                            "DetectedSourceLanguage": "en",
                        }
                    ],
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.volcengine.urllib.request.urlopen", _urlopen
    )
    result = _volcengine().translate(
        TranslationRequest("Hello", "zh-Hans", source_lang="en", timeout=7)
    )

    assert result.success
    assert result.translated_text == "你好"
    assert result.detected_source_lang == "en"
    assert captured["timeout"] == 7
    # URL 带 Action/Version
    assert "Action=TranslateText" in captured["request"].full_url
    # X-Date 必须是 %Y%m%dT%H%M%SZ 的真实格式——YYYYMMDD 字面量是已修掉的致命 bug
    headers = {
        key.lower(): value for key, value in captured["request"].header_items()
    }
    assert re.fullmatch(r"\d{8}T\d{6}Z", headers["x-date"])
    # V4 签名头前缀
    assert headers["authorization"].startswith("HMAC-SHA256 ")
    body = json.loads(captured["request"].data.decode("utf-8"))
    assert body["TargetLanguage"] == "zh"  # zh-Hans -> zh
    assert body["TextList"] == ["Hello"]


def test_volcengine_empty_source_triggers_auto_detection(monkeypatch):
    """source_lang 为空时 SourceLanguage 传空串，交给服务端自动检测。"""
    captured = {}

    def _urlopen(request, timeout):
        captured["body"] = json.loads(request.data.decode("utf-8"))
        return _Response(
            json.dumps(
                {
                    "ResponseMetadata": {},
                    "TranslationList": [
                        {"Translation": "喂，你好吗？", "DetectedSourceLanguage": "zh"}
                    ],
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.volcengine.urllib.request.urlopen", _urlopen
    )
    result = _volcengine().translate(TranslationRequest("你好", "zh-Hans"))

    assert result.success
    assert result.translated_text == "喂，你好吗？"
    assert captured["body"]["SourceLanguage"] == ""


def test_volcengine_maps_unsupported_language_error(monkeypatch):
    def _urlopen(request, timeout):
        return _Response(
            json.dumps(
                {
                    "ResponseMetadata": {
                        "Error": {"Code": "-415", "Message": "unsupported pair"}
                    }
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.volcengine.urllib.request.urlopen", _urlopen
    )
    result = _volcengine().translate(TranslationRequest("Hello", "zh-Hans"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.UNSUPPORTED_LANGUAGE


def test_volcengine_maps_server_error(monkeypatch):
    def _urlopen(request, timeout):
        return _Response(
            json.dumps(
                {
                    "ResponseMetadata": {
                        "Error": {"Code": "-500", "Message": "internal error"}
                    }
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.volcengine.urllib.request.urlopen", _urlopen
    )
    result = _volcengine().translate(TranslationRequest("Hello", "zh-Hans"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.NETWORK_ERROR


def test_volcengine_joins_multiline_result(monkeypatch):
    def _urlopen(request, timeout):
        return _Response(
            json.dumps(
                {
                    "ResponseMetadata": {},
                    "TranslationList": [
                        {"Translation": "Hello"},
                        {"Translation": "World"},
                    ],
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.volcengine.urllib.request.urlopen", _urlopen
    )
    result = _volcengine().translate(TranslationRequest("你好\n世界", "en"))

    assert result.success
    assert result.translated_text == "Hello\nWorld"


def test_volcengine_rejects_when_not_configured():
    result = VolcengineTranslateProvider({}).translate(
        TranslationRequest("Hello", "zh-Hans")
    )
    assert not result.success
    assert result.error_code is TranslationErrorCode.NOT_CONFIGURED


# ============================================================================
# 腾讯云
# ============================================================================

def test_tencent_translates_and_sends_tc3_headers(monkeypatch):
    captured = {}

    def _urlopen(request, timeout):
        captured["request"] = request
        return _Response(
            json.dumps(
                {
                    "Response": {
                        "RequestId": "r1",
                        "Source": "en",
                        "Target": "zh",
                        "TargetText": "你好",
                    }
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.tencent.urllib.request.urlopen", _urlopen
    )
    result = _tencent().translate(
        TranslationRequest("Hello", "en", source_lang="en", timeout=7)
    )

    assert result.success
    assert result.translated_text == "你好"
    assert result.detected_source_lang == "en"
    headers = {
        key.lower(): value for key, value in captured["request"].header_items()
    }
    assert headers["x-tc-action"] == "TextTranslate"
    assert headers["authorization"].startswith("TC3-HMAC-SHA256 ")
    body = json.loads(captured["request"].data.decode("utf-8"))
    assert body["Source"] == "en"
    assert body["Target"] == "en"
    assert body["ProjectId"] == 0


def test_tencent_empty_source_triggers_auto_detection(monkeypatch):
    captured = {}

    def _urlopen(request, timeout):
        captured["body"] = json.loads(request.data.decode("utf-8"))
        return _Response(
            json.dumps(
                {
                    "Response": {
                        "Source": "zh",
                        "Target": "en",
                        "TargetText": "Hello",
                    }
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.tencent.urllib.request.urlopen", _urlopen
    )
    result = _tencent().translate(TranslationRequest("你好", "en"))

    assert result.success
    assert result.translated_text == "Hello"
    assert captured["body"]["Source"] == "auto"


def test_tencent_maps_no_free_amount_to_quota_exceeded(monkeypatch):
    def _urlopen(request, timeout):
        return _Response(
            json.dumps(
                {
                    "Response": {
                        "Error": {
                            "Code": "FailedOperation.NoFreeAmount",
                            "Message": "out of free quota",
                        }
                    }
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.tencent.urllib.request.urlopen", _urlopen
    )
    result = _tencent().translate(TranslationRequest("Hello", "en"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.QUOTA_EXCEEDED


def test_tencent_maps_auth_failure(monkeypatch):
    def _urlopen(request, timeout):
        return _Response(
            json.dumps(
                {
                    "Response": {
                        "Error": {
                            "Code": "AuthFailure.SecretIdNotFound",
                            "Message": "bad credential",
                        }
                    }
                }
            ).encode("utf-8")
        )

    monkeypatch.setattr(
        "translation.providers.tencent.urllib.request.urlopen", _urlopen
    )
    result = _tencent().translate(TranslationRequest("Hello", "en"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.AUTH_FAILED


def test_tencent_rejects_unsupported_target_without_network(monkeypatch):
    def boom(*_a, **_k):
        raise AssertionError("should not hit the network")

    monkeypatch.setattr(
        "translation.providers.tencent.urllib.request.urlopen", boom
    )
    result = _tencent().translate(TranslationRequest("Hello", "nl"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.UNSUPPORTED_LANGUAGE


def test_tencent_rejects_when_not_configured():
    result = TencentTranslateProvider({}).translate(
        TranslationRequest("Hello", "en")
    )
    assert not result.success
    assert result.error_code is TranslationErrorCode.NOT_CONFIGURED


# ============================================================================
# 两家共有的失败路径
# ============================================================================

_PROVIDERS = {
    "volcengine": (_volcengine, "translation.providers.volcengine.urllib.request.urlopen"),
    "tencent": (_tencent, "translation.providers.tencent.urllib.request.urlopen"),
}


def _raise(exc):
    def _urlopen(request, timeout):
        raise exc
    return _urlopen


def _http_error(code, body=b""):
    return urllib.error.HTTPError(
        "https://example.invalid", code, "reason", {}, io.BytesIO(body)
    )


@pytest.mark.parametrize("provider_id", list(_PROVIDERS))
@pytest.mark.parametrize(
    "urlopen, expected",
    [
        (_raise(TimeoutError()), TranslationErrorCode.NETWORK_ERROR),
        (_raise(urllib.error.URLError("offline")), TranslationErrorCode.NETWORK_ERROR),
        (_raise(RuntimeError("boom")), TranslationErrorCode.UNKNOWN),
        (lambda request, timeout: _Response(b"not json"), TranslationErrorCode.UNKNOWN),
        (lambda request, timeout: _Response(b"[]"), TranslationErrorCode.UNKNOWN),
        (lambda request, timeout: _Response(b"{}"), TranslationErrorCode.UNKNOWN),
        (_raise(_http_error(403)), TranslationErrorCode.AUTH_FAILED),
        (_raise(_http_error(503)), TranslationErrorCode.NETWORK_ERROR),
        (_raise(_http_error(400, b"not json")), TranslationErrorCode.UNKNOWN),
    ],
)
def test_failures_map_to_error_codes(monkeypatch, provider_id, urlopen, expected):
    factory, target = _PROVIDERS[provider_id]
    monkeypatch.setattr(target, urlopen)

    result = factory().translate(TranslationRequest("Hello", "ja"))

    assert not result.success
    assert result.error_code is expected


@pytest.mark.parametrize("provider_id", list(_PROVIDERS))
def test_empty_text_is_rejected_without_network(monkeypatch, provider_id):
    factory, target = _PROVIDERS[provider_id]
    monkeypatch.setattr(target, _raise(AssertionError("不该发请求")))

    result = factory().translate(TranslationRequest("  ", "ja"))

    assert result.error_code is TranslationErrorCode.INVALID_REQUEST


def test_volcengine_http_error_body_is_parsed(monkeypatch):
    body = json.dumps(
        {"ResponseMetadata": {"Error": {"Code": "-429", "Message": "slow down"}}}
    ).encode("utf-8")
    monkeypatch.setattr(
        _PROVIDERS["volcengine"][1], _raise(_http_error(429, body))
    )

    result = _volcengine().translate(TranslationRequest("Hello", "ja"))

    assert result.error_code is TranslationErrorCode.RATE_LIMITED
    assert result.error_message == "slow down"


def test_tencent_http_error_body_is_parsed(monkeypatch):
    body = json.dumps(
        {"Response": {"Error": {"Code": "AuthFailure.SignatureFailure", "Message": "bad sig"}}}
    ).encode("utf-8")
    monkeypatch.setattr(_PROVIDERS["tencent"][1], _raise(_http_error(400, body)))

    result = _tencent().translate(TranslationRequest("Hello", "ja"))

    assert result.error_code is TranslationErrorCode.AUTH_FAILED


def test_volcengine_empty_translation_is_invalid(monkeypatch):
    body = json.dumps({"TranslationList": [{"Translation": ""}]}).encode("utf-8")
    monkeypatch.setattr(
        _PROVIDERS["volcengine"][1], lambda request, timeout: _Response(body)
    )

    result = _volcengine().translate(TranslationRequest("Hello", "ja"))

    assert result.error_code is TranslationErrorCode.UNKNOWN


def test_tencent_maps_traditional_chinese_both_ways(monkeypatch):
    captured = {}

    def _urlopen(request, timeout):
        captured["body"] = json.loads(request.data.decode("utf-8"))
        return _Response(json.dumps(
            {"Response": {"TargetText": "你好", "Source": "zh-TW"}}
        ).encode("utf-8"))

    monkeypatch.setattr(_PROVIDERS["tencent"][1], _urlopen)

    result = _tencent().translate(
        TranslationRequest("你好", "zh-Hans", source_lang="zh-Hant")
    )

    assert captured["body"]["Source"] == "zh-TW"
    assert captured["body"]["Target"] == "zh"
    assert result.detected_source_lang == "zh-Hant"


@pytest.mark.parametrize(
    "code, expected",
    [
        ("-400", TranslationErrorCode.INVALID_REQUEST),
        ("-415", TranslationErrorCode.UNSUPPORTED_LANGUAGE),
        ("-429", TranslationErrorCode.RATE_LIMITED),
        ("-500", TranslationErrorCode.NETWORK_ERROR),
        ("100010", TranslationErrorCode.UNKNOWN),
    ],
)
def test_volcengine_api_error_codes(code, expected):
    assert VolcengineTranslateProvider._map_api_error_code(code) is expected


@pytest.mark.parametrize(
    "code, expected",
    [
        ("AuthFailure.SecretIdNotFound", TranslationErrorCode.AUTH_FAILED),
        ("UnauthorizedOperation", TranslationErrorCode.AUTH_FAILED),
        ("FailedOperation.UserNotRegistered", TranslationErrorCode.AUTH_FAILED),
        ("FailedOperation.NoFreeAmount", TranslationErrorCode.QUOTA_EXCEEDED),
        ("FailedOperation.ServiceIsolate", TranslationErrorCode.QUOTA_EXCEEDED),
        ("FailedOperation.StopUsing", TranslationErrorCode.QUOTA_EXCEEDED),
        ("UnsupportedOperation.UnSupportedTargetLanguage", TranslationErrorCode.UNSUPPORTED_LANGUAGE),
        ("UnsupportedOperation.TextTooLong", TranslationErrorCode.INVALID_REQUEST),
        ("RequestLimitExceeded", TranslationErrorCode.RATE_LIMITED),
        ("InvalidParameter", TranslationErrorCode.INVALID_REQUEST),
        ("InternalError", TranslationErrorCode.NETWORK_ERROR),
        ("Something.Else", TranslationErrorCode.UNKNOWN),
    ],
)
def test_tencent_api_error_codes(code, expected):
    assert TencentTranslateProvider._map_api_error_code(code) is expected
