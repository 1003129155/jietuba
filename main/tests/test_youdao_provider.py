# -*- coding: utf-8 -*-
"""有道智云翻译 provider 的离线测试：签名固定向量、请求结构、响应与错误映射。

不走网络：monkeypatch 替换 urllib.request.urlopen。签名向量由文档公式
sha256(appKey + input + salt + curtime + appSecret) 独立算出，写死在这里；
C++ 版对照同一组向量。
"""

import io
import json
import types
import urllib.error
import urllib.parse

import pytest

from translation.models import TranslationErrorCode, TranslationRequest
from translation.providers import youdao
from translation.providers.youdao import YoudaoTranslateProvider

_KEY = "test-app-key"
_SECRET = "test-app-secret"
_SALT = "0b1f1b43-6b58-4a0e-9b1c-5b1d2e3f4a5b"
_CURTIME = "1700000000"

_CJK = (
    "今天天气真好，我们一起"
    "去公园散步吧，然后再去"
    "吃饭"
)
_ASTRAL = "\U0001F600abcdefghijklmnopqrstuvwxyz"

# (文本, UTF-16 单元算法的签名, 码点算法的签名)
_SIGN_VECTORS = [
    (
        "Hello",
        "9e5816ad26fca5ee59248f2d3d737e834aaddbf7c5fac1b823e2025a11763ea3",
        "9e5816ad26fca5ee59248f2d3d737e834aaddbf7c5fac1b823e2025a11763ea3",
    ),
    (  # 恰好 20 个字符：input 仍是原文
        "a" * 20,
        "37af65eff9b61414ef122c5b83ac42aee27401f45309385e78fffbaa00996312",
        "37af65eff9b61414ef122c5b83ac42aee27401f45309385e78fffbaa00996312",
    ),
    (  # 21 个字符：开始截取
        "b" * 21,
        "b328682353a1ca7471bf46384557bef8ce8e442081dda61733bfbf983372489d",
        "b328682353a1ca7471bf46384557bef8ce8e442081dda61733bfbf983372489d",
    ),
    (
        "The quick brown fox jumps over the lazy dog",
        "8b70bc9f6a828c91daa729b9ffe89f31614139d2f639b3f0eedb1b16256c2d22",
        "8b70bc9f6a828c91daa729b9ffe89f31614139d2f639b3f0eedb1b16256c2d22",
    ),
    (
        _CJK,
        "3c840e2a6127de27e63f787be1b6c054b5337bbf8c3fb91901f36d83d30f0a2d",
        "3c840e2a6127de27e63f787be1b6c054b5337bbf8c3fb91901f36d83d30f0a2d",
    ),
    (  # 含增补平面字符：两种算法才有差别
        _ASTRAL,
        "f9be267a76fb90b0a8029d2d204a31d8cc4942492aedd1506df9a305e29d7451",
        "b6bbabfbc3988c1fe55d905f4ffef955ceade41a5c14ec5452a7083b8f808cbb",
    ),
]


def _provider(config=None):
    if config is None:
        config = {"app_key": _KEY, "app_secret": _SECRET}
    return YoudaoTranslateProvider(config)


class _Response:
    def __init__(self, data):
        self._data = data

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def read(self):
        return self._data


def _ok(translation=("你好",), l="EN2zh-CHS"):
    return json.dumps(
        {"errorCode": "0", "translation": list(translation), "l": l}
    ).encode("utf-8")


def _error(code):
    return json.dumps({"errorCode": str(code), "l": "auto2zh-CHS"}).encode(
        "utf-8"
    )


@pytest.fixture
def fixed_clock(monkeypatch):
    # 替换模块里的名字而不是全局 time.time，免得影响 pytest 自身计时。
    monkeypatch.setattr(
        youdao, "time", types.SimpleNamespace(time=lambda: int(_CURTIME))
    )
    monkeypatch.setattr(
        youdao, "uuid", types.SimpleNamespace(uuid4=lambda: _SALT)
    )


@pytest.fixture
def urlopen(monkeypatch):
    """依次返回 responses 里的响应（Exception 实例则抛出），记录每次请求。"""

    class _Stub:
        def __init__(self):
            self.responses = []
            self.requests = []

        def __call__(self, request, timeout):
            self.requests.append((request, timeout))
            item = self.responses.pop(0)
            if isinstance(item, Exception):
                raise item
            return _Response(item)

        def form(self, index=-1):
            body = self.requests[index][0].data.decode("utf-8")
            return {
                k: v[0] for k, v in urllib.parse.parse_qs(body).items()
            }

    stub = _Stub()
    monkeypatch.setattr(
        "translation.providers.youdao.urllib.request.urlopen", stub
    )
    return stub


# ============================================================================
# 签名
# ============================================================================

@pytest.mark.parametrize("text, utf16_sign, codepoint_sign", _SIGN_VECTORS)
def test_sign_matches_fixed_vectors(text, utf16_sign, codepoint_sign):
    provider = _provider()

    assert provider._sign(text, _SALT, _CURTIME, utf16_input=True) == utf16_sign
    assert (
        provider._sign(text, _SALT, _CURTIME, utf16_input=False)
        == codepoint_sign
    )


def test_sign_changes_with_secret_and_key():
    base = _provider()._sign("Hello", _SALT, _CURTIME)
    other_secret = _provider(
        {"app_key": _KEY, "app_secret": "other"}
    )._sign("Hello", _SALT, _CURTIME)
    other_key = _provider(
        {"app_key": "other", "app_secret": _SECRET}
    )._sign("Hello", _SALT, _CURTIME)

    assert len({base, other_secret, other_key}) == 3


# ============================================================================
# 请求
# ============================================================================

def test_translate_posts_signed_form(fixed_clock, urlopen):
    urlopen.responses.append(_ok())
    result = _provider().translate(
        TranslationRequest("Hello", "zh-Hans", timeout=7)
    )

    assert result.success
    assert result.translated_text == "你好"
    request, timeout = urlopen.requests[0]
    assert timeout == 7
    assert request.get_method() == "POST"
    assert request.full_url == "https://openapi.youdao.com/api"
    headers = {k.lower(): v for k, v in request.header_items()}
    assert headers["content-type"].startswith(
        "application/x-www-form-urlencoded"
    )
    assert urlopen.form() == {
        "q": "Hello",
        "from": "auto",
        "to": "zh-CHS",
        "appKey": _KEY,
        "salt": _SALT,
        "sign": _SIGN_VECTORS[0][1],
        "signType": "v3",
        "curtime": _CURTIME,
    }


def test_translate_encodes_non_ascii_text_as_utf8(fixed_clock, urlopen):
    urlopen.responses.append(_ok(("Hello",), "zh-CHS2en"))
    result = _provider().translate(TranslationRequest(_CJK, "en-US"))

    assert result.success
    form = urlopen.form()
    assert form["q"] == _CJK
    assert form["to"] == "en"
    assert form["sign"] == _SIGN_VECTORS[4][1]


@pytest.mark.parametrize(
    "app_lang, youdao_code",
    [
        ("zh-Hans", "zh-CHS"),
        ("zh-Hant", "zh-CHT"),
        ("en-US", "en"),
        ("en-GB", "en"),
        ("pt-BR", "pt"),
        ("pt-PT", "pt"),
        ("ja", "ja"),
        ("uk", "uk"),
    ],
)
def test_language_codes_map_to_youdao(urlopen, app_lang, youdao_code):
    urlopen.responses.extend([_ok(), _ok()])
    _provider().translate(TranslationRequest("Hello", app_lang))
    _provider().translate(
        TranslationRequest("Hello", "zh-Hans", source_lang=app_lang)
    )

    assert urlopen.form(0)["to"] == youdao_code
    assert urlopen.form(1)["from"] == youdao_code


@pytest.mark.parametrize(
    "direction, detected",
    [
        ("EN2zh-CHS", "en"),
        ("zh-CHS2en", "zh-Hans"),
        ("zh-CHT2ja", "zh-Hant"),
        ("ja2zh-CHS", "ja"),
        ("auto2zh-CHS", ""),
        ("", ""),
    ],
)
def test_detected_source_is_read_from_direction(urlopen, direction, detected):
    urlopen.responses.append(_ok(l=direction))
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert result.success
    assert result.detected_source_lang == detected


def test_multi_item_translation_is_joined_by_newline(urlopen):
    urlopen.responses.append(_ok(("第一行", "第二行")))
    result = _provider().translate(TranslationRequest("a\nb", "zh-Hans"))

    assert result.translated_text == "第一行\n第二行"


# ============================================================================
# 配置与空输入：不应发请求
# ============================================================================

def test_unconfigured_provider_does_not_call_network(urlopen):
    for config in ({}, {"app_key": _KEY}, {"app_secret": _SECRET}):
        provider = _provider(config)
        assert not provider.is_configured()
        result = provider.translate(TranslationRequest("Hello", "zh-Hans"))
        assert result.error_code is TranslationErrorCode.NOT_CONFIGURED
    assert urlopen.requests == []


def test_blank_text_is_rejected_without_network(urlopen):
    result = _provider().translate(TranslationRequest("  \n", "zh-Hans"))

    assert result.error_code is TranslationErrorCode.INVALID_REQUEST
    assert urlopen.requests == []


def test_credentials_are_stripped():
    assert _provider({"app_key": f" {_KEY} ", "app_secret": f"{_SECRET}\n"}
                     ).is_configured()
    assert not _provider({"app_key": "  ", "app_secret": _SECRET}
                         ).is_configured()


# ============================================================================
# 错误映射
# ============================================================================

@pytest.mark.parametrize(
    "code, expected",
    [
        (108, TranslationErrorCode.AUTH_FAILED),
        (110, TranslationErrorCode.AUTH_FAILED),
        (111, TranslationErrorCode.AUTH_FAILED),
        (112, TranslationErrorCode.AUTH_FAILED),
        (203, TranslationErrorCode.AUTH_FAILED),
        (401, TranslationErrorCode.QUOTA_EXCEEDED),
        (411, TranslationErrorCode.RATE_LIMITED),
        (412, TranslationErrorCode.RATE_LIMITED),
        (102, TranslationErrorCode.UNSUPPORTED_LANGUAGE),
        (101, TranslationErrorCode.INVALID_REQUEST),
        (103, TranslationErrorCode.INVALID_REQUEST),
        (113, TranslationErrorCode.INVALID_REQUEST),
        (206, TranslationErrorCode.INVALID_REQUEST),
        (302, TranslationErrorCode.NETWORK_ERROR),
        (303, TranslationErrorCode.NETWORK_ERROR),
        (304, TranslationErrorCode.NETWORK_ERROR),
        (999, TranslationErrorCode.UNKNOWN),
    ],
)
def test_api_error_codes_are_mapped(urlopen, code, expected):
    urlopen.responses.append(_error(code))
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert not result.success
    assert result.error_code is expected
    assert result.error_message


def test_unknown_error_code_is_named_in_message(urlopen):
    urlopen.responses.append(_error(999))
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert result.error_message == "Youdao error 999"


def test_empty_or_missing_translation_is_an_error(urlopen):
    urlopen.responses.extend(
        [
            json.dumps({"errorCode": "0", "translation": []}).encode(),
            json.dumps({"errorCode": "0"}).encode(),
            json.dumps({"errorCode": "0", "translation": [""]}).encode(),
        ]
    )
    for _ in range(3):
        result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))
        assert result.error_code is TranslationErrorCode.UNKNOWN


def test_non_object_response_is_an_error(urlopen):
    urlopen.responses.append(b"[1, 2]")
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert result.error_code is TranslationErrorCode.UNKNOWN


def test_invalid_json_is_an_error(urlopen):
    urlopen.responses.append(b"<html>gateway</html>")
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert result.error_code is TranslationErrorCode.UNKNOWN


@pytest.mark.parametrize(
    "http_status, expected",
    [
        (401, TranslationErrorCode.AUTH_FAILED),
        (403, TranslationErrorCode.AUTH_FAILED),
        (502, TranslationErrorCode.NETWORK_ERROR),
        (404, TranslationErrorCode.UNKNOWN),
    ],
)
def test_http_errors_are_mapped(urlopen, http_status, expected):
    urlopen.responses.append(
        urllib.error.HTTPError(
            "https://openapi.youdao.com/api",
            http_status,
            "reason",
            {},
            io.BytesIO(b""),
        )
    )
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert result.error_code is expected


def test_timeout_and_network_errors(urlopen):
    urlopen.responses.extend(
        [TimeoutError(), urllib.error.URLError("no route")]
    )
    timed_out = _provider().translate(
        TranslationRequest("Hello", "zh-Hans", timeout=3)
    )
    offline = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert timed_out.error_code is TranslationErrorCode.NETWORK_ERROR
    assert "3s" in timed_out.error_message
    assert offline.error_code is TranslationErrorCode.NETWORK_ERROR
    assert "no route" in offline.error_message


def test_unexpected_exception_is_reported_not_raised(urlopen):
    urlopen.responses.append(RuntimeError("boom"))
    result = _provider().translate(TranslationRequest("Hello", "zh-Hans"))

    assert result.error_code is TranslationErrorCode.UNKNOWN
    assert "boom" in result.error_message


# ============================================================================
# 增补平面字符：签名算法重试
# ============================================================================

def test_astral_text_retries_with_codepoint_sign_after_202(
    fixed_clock, urlopen
):
    urlopen.responses.extend([_error(202), _ok(("你好",))])
    result = _provider().translate(TranslationRequest(_ASTRAL, "zh-Hans"))

    assert result.success
    assert len(urlopen.requests) == 2
    assert urlopen.form(0)["sign"] == _SIGN_VECTORS[5][1]
    assert urlopen.form(1)["sign"] == _SIGN_VECTORS[5][2]


def test_astral_text_accepted_first_time_is_not_retried(
    fixed_clock, urlopen
):
    urlopen.responses.append(_ok(("你好",)))
    result = _provider().translate(TranslationRequest(_ASTRAL, "zh-Hans"))

    assert result.success
    assert len(urlopen.requests) == 1


def test_astral_text_reports_failure_when_both_signs_are_rejected(
    fixed_clock, urlopen
):
    urlopen.responses.extend([_error(202), _error(202)])
    result = _provider().translate(TranslationRequest(_ASTRAL, "zh-Hans"))

    assert result.error_code is TranslationErrorCode.AUTH_FAILED
    assert len(urlopen.requests) == 2


def test_bmp_text_is_never_retried_on_202(fixed_clock, urlopen):
    """两种算法对 BMP 文本结果相同，重试没有意义。"""
    urlopen.responses.append(_error(202))
    result = _provider().translate(TranslationRequest(_CJK, "en-US"))

    assert result.error_code is TranslationErrorCode.AUTH_FAILED
    assert len(urlopen.requests) == 1


def test_other_errors_on_astral_text_are_not_retried(fixed_clock, urlopen):
    urlopen.responses.append(_error(108))
    result = _provider().translate(TranslationRequest(_ASTRAL, "zh-Hans"))

    assert result.error_code is TranslationErrorCode.AUTH_FAILED
    assert len(urlopen.requests) == 1


# ============================================================================
# 接入：注册表与设置
# ============================================================================

def test_youdao_is_registered_with_declared_fields():
    from translation.service import create_default_translation_service

    class _Config:
        def get_translation_provider(self):
            return ""

        def get_translation_provider_config(self, provider_id):
            return {}

    registry = create_default_translation_service(_Config()).registry
    meta = {m.provider_id: m for m in registry.available_providers()}["youdao"]

    assert [f.config_key for f in meta.credentials] == [
        "youdao_translate_app_key",
        "youdao_translate_app_secret",
    ]
    assert meta.credentials[1].secret
    assert meta.help_url.startswith("https://")
    # 没有月度免费额度，不挂本地额度锁。
    assert getattr(YoudaoTranslateProvider, "QUOTA_LIMIT_CHARS", None) is None


def test_credentials_map_to_provider_config():
    from settings.tool_settings import get_tool_settings_manager

    manager = get_tool_settings_manager()
    manager.qsettings.remove("translation/providers/youdao")
    try:
        manager.set_youdao_translate_app_key("  id  ")
        manager.set_youdao_translate_app_secret("secret\n")

        assert manager.get_translation_provider_config("youdao") == {
            "app_key": "id",
            "app_secret": "secret",
        }
    finally:
        manager.qsettings.remove("translation/providers/youdao")
