import datetime as dt
import io
import json
import urllib.error

from translation.models import (
    TranslationErrorCode,
    TranslationRequest,
    TranslationResult,
)
from translation.provider import TranslationProvider
from translation.providers.amazon import AmazonTranslateProvider
from translation.providers.deepl import DeepLProvider
from translation.providers.google import GoogleTranslateProvider
from translation.registry import ProviderRegistry
from translation.service import TranslationService


class _Config:
    def __init__(self, active="fake", provider_config=None):
        self.active = active
        self.provider_config = provider_config or {}

    def get_translation_provider(self):
        return self.active

    def get_translation_provider_config(self, provider_id):
        return self.provider_config.get(provider_id, {})


class _FakeProvider(TranslationProvider):
    provider_id = "fake"
    display_name = "Fake"

    def __init__(self, config):
        self.config = config

    def is_configured(self):
        return bool(self.config.get("token"))

    def translate(self, request):
        return TranslationResult(
            success=True,
            translated_text=f"{request.target_lang}:{request.text}",
        )


def _service(config=None):
    registry = ProviderRegistry()
    registry.register("fake", _FakeProvider, display_name="Fake Provider")
    return TranslationService(
        registry,
        config or _Config(provider_config={"fake": {"token": "ok"}}),
    )


def test_service_selects_registered_provider_from_config():
    service = _service()

    result = service.translate(TranslationRequest("hello", "ZH"))

    assert result.success
    assert result.translated_text == "zh-Hans:hello"
    assert service.provider_name() == "Fake Provider"


def test_service_reports_unconfigured_provider_without_calling_it():
    service = _service(_Config())

    result = service.translate(TranslationRequest("hello", "EN"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.NOT_CONFIGURED


def test_provider_overrides_support_legacy_or_one_off_credentials():
    service = _service(_Config())

    result = service.translate(
        TranslationRequest("hello", "JA"),
        overrides={"token": "temporary"},
    )

    assert result.success
    assert result.translated_text == "ja:hello"


def test_deepl_adapter_owns_language_and_response_mapping(monkeypatch):
    calls = []

    class _DeepLService:
        def __init__(self, api_key, use_pro=False):
            calls.append(("init", api_key, use_pro))

        def translate(self, text, **kwargs):
            calls.append(("translate", text, kwargs))
            return {
                "success": True,
                "translated_text": "你好",
                "detected_source_lang": "EN",
            }

    monkeypatch.setattr(
        "translation.providers.deepl.DeepLService", _DeepLService
    )
    provider = DeepLProvider({"api_key": "key", "use_pro": True})

    result = provider.translate(
        TranslationRequest(
            "hello",
            "zh-Hans",
            source_lang="en",
            options={"split_sentences": "0"},
        )
    )

    assert result.translated_text == "你好"
    assert result.detected_source_lang == "en"
    assert calls[1][2]["target_lang"] == "ZH"
    assert calls[1][2]["source_lang"] == "EN"
    assert calls[1][2]["split_sentences"] == "0"


def _amazon_provider(**overrides):
    config = {
        "region": "us-west-2",
        "access_key_id": "AKIDEXAMPLE",
        "secret_access_key": (
            "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"
        ),
    }
    config.update(overrides)
    return AmazonTranslateProvider(config)


def test_amazon_signature_matches_botocore_reference_vector():
    provider = _amazon_provider()
    payload = (
        b'{"Text":"Hello world","SourceLanguageCode":"en",'
        b'"TargetLanguageCode":"zh"}'
    )

    url, headers = provider._signed_request(
        payload,
        dt.datetime(
            2026, 7, 27, 12, 34, 56, tzinfo=dt.timezone.utc
        ),
    )

    assert url == "https://translate.us-west-2.amazonaws.com/"
    assert headers["authorization"] == (
        "AWS4-HMAC-SHA256 "
        "Credential=AKIDEXAMPLE/20260727/us-west-2/"
        "translate/aws4_request, "
        "SignedHeaders=content-type;host;x-amz-date;x-amz-target, "
        "Signature=d1d7eb0603d298431fa89d8d2f153b7242aad173f2f"
        "875814a9426698e6a48d4"
    )


def test_amazon_provider_translates_through_signed_json_request(monkeypatch):
    captured = {}

    class _Response:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            pass

        def read(self):
            return json.dumps(
                {
                    "TranslatedText": "你好，世界",
                    "SourceLanguageCode": "en",
                    "TargetLanguageCode": "zh",
                }
            ).encode("utf-8")

    def _urlopen(request, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return _Response()

    monkeypatch.setattr(
        "translation.providers.amazon.urllib.request.urlopen", _urlopen
    )
    result = _amazon_provider().translate(
        TranslationRequest("Hello world", "zh-Hans", timeout=7)
    )

    assert result.success
    assert result.translated_text == "你好，世界"
    assert result.detected_source_lang == "en"
    assert captured["timeout"] == 7
    payload = json.loads(captured["request"].data.decode("utf-8"))
    assert payload == {
        "Text": "Hello world",
        "SourceLanguageCode": "auto",
        "TargetLanguageCode": "zh",
    }
    headers = {
        key.lower(): value
        for key, value in captured["request"].header_items()
    }
    assert headers["x-amz-target"].endswith(".TranslateText")
    assert headers["authorization"].startswith("AWS4-HMAC-SHA256 ")


def test_amazon_provider_maps_service_error(monkeypatch):
    body = io.BytesIO(
        json.dumps(
            {
                "__type": "AccessDeniedException",
                "message": "not authorized",
            }
        ).encode("utf-8")
    )

    def _urlopen(*_args, **_kwargs):
        raise urllib.error.HTTPError(
            "https://example.invalid",
            403,
            "Forbidden",
            {},
            body,
        )

    monkeypatch.setattr(
        "translation.providers.amazon.urllib.request.urlopen", _urlopen
    )
    result = _amazon_provider().translate(
        TranslationRequest("Hello", "zh-Hans")
    )

    assert not result.success
    assert result.error_code is TranslationErrorCode.AUTH_FAILED
    assert result.error_message == "not authorized"


def test_amazon_provider_rejects_oversized_utf8_before_network():
    result = _amazon_provider().translate(
        TranslationRequest("中" * 3334, "en")
    )

    assert not result.success
    assert result.error_code is TranslationErrorCode.INVALID_REQUEST


def _google_provider(api_key="google-test-key"):
    return GoogleTranslateProvider({"api_key": api_key})


def test_google_provider_translates_with_api_key_and_auto_detection(
    monkeypatch,
):
    captured = {}

    class _Response:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            pass

        def read(self):
            return json.dumps(
                {
                    "data": {
                        "translations": [
                            {
                                "translatedText": "你好 &amp; 世界",
                                "detectedSourceLanguage": "en",
                            }
                        ]
                    }
                }
            ).encode("utf-8")

    def _urlopen(request, timeout):
        captured["request"] = request
        captured["timeout"] = timeout
        return _Response()

    monkeypatch.setattr(
        "translation.providers.google.urllib.request.urlopen", _urlopen
    )
    result = _google_provider().translate(
        TranslationRequest("Hello & world", "zh-Hans", timeout=8)
    )

    assert result.success
    assert result.translated_text == "你好 & 世界"
    assert result.detected_source_lang == "en"
    assert captured["timeout"] == 8
    assert "key=google-test-key" in captured["request"].full_url
    assert json.loads(captured["request"].data.decode("utf-8")) == {
        "q": "Hello & world",
        "target": "zh-CN",
        "format": "text",
    }


def test_google_provider_sends_explicit_source_language(monkeypatch):
    captured = {}

    class _Response:
        def __enter__(self):
            return self

        def __exit__(self, *_args):
            pass

        def read(self):
            return (
                b'{"data":{"translations":[{"translatedText":"Hello"}]}}'
            )

    def _urlopen(request, timeout):
        captured["body"] = json.loads(request.data.decode("utf-8"))
        return _Response()

    monkeypatch.setattr(
        "translation.providers.google.urllib.request.urlopen", _urlopen
    )
    result = _google_provider().translate(
        TranslationRequest("こんにちは", "en", source_lang="ja")
    )

    assert result.success
    assert captured["body"]["source"] == "ja"


def test_google_provider_maps_quota_error(monkeypatch):
    body = io.BytesIO(
        json.dumps(
            {
                "error": {
                    "code": 429,
                    "message": "Quota exceeded",
                    "status": "RESOURCE_EXHAUSTED",
                }
            }
        ).encode("utf-8")
    )

    def _urlopen(*_args, **_kwargs):
        raise urllib.error.HTTPError(
            "https://example.invalid",
            429,
            "Too Many Requests",
            {},
            body,
        )

    monkeypatch.setattr(
        "translation.providers.google.urllib.request.urlopen", _urlopen
    )
    result = _google_provider().translate(
        TranslationRequest("Hello", "ja")
    )

    assert not result.success
    assert result.error_code is TranslationErrorCode.RATE_LIMITED
