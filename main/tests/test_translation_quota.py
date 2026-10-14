# -*- coding: utf-8 -*-
"""月度额度锁：设置存取、service 与 TranslationWorker 两条路径的拦截和记账、设置页用量行。"""
import json
import threading

import pytest

from translation.models import TranslationErrorCode, TranslationRequest
from translation.service import create_default_translation_service
from translation.worker import TranslationWorker


_GROUPS = (
    "translation/providers/volcengine",
    "translation/providers/tencent",
    "translation/usage",
)


@pytest.fixture
def config():
    from settings.tool_settings import get_tool_settings_manager

    manager = get_tool_settings_manager()
    for group in _GROUPS:
        manager.qsettings.remove(group)
    manager.set_volcengine_translate_access_key("ak")
    manager.set_volcengine_translate_secret_key("sk")
    yield manager
    for group in _GROUPS:
        manager.qsettings.remove(group)


class _Response:
    def __init__(self, data):
        self._data = data

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def read(self):
        return self._data


@pytest.fixture
def sent_requests(monkeypatch):
    """记下每次请求所在的线程；回一个成功的火山响应。"""
    calls = []

    def _urlopen(request, timeout):
        calls.append(threading.get_ident())
        return _Response(json.dumps({
            "TranslationList": [{"Translation": "hi", "DetectedSourceLanguage": "zh"}],
        }).encode("utf-8"))

    monkeypatch.setattr(
        "translation.providers.volcengine.urllib.request.urlopen", _urlopen
    )
    return calls


def _run_worker(qtbot, service, text="你好世界"):
    worker = TranslationWorker(
        service, TranslationRequest(text, "EN"), provider_id="volcengine"
    )
    with qtbot.waitSignal(worker.finished_signal, timeout=5000) as blocker:
        worker.start()
    worker.wait(3000)
    # 记账槽排队在 GUI 线程，处理一下事件才会执行。
    qtbot.wait(20)
    return blocker.args[0]


# ============================================================================
# 设置存取
# ============================================================================

def test_credentials_map_to_provider_config(config):
    config.set_tencent_translate_secret_id("  id  ")
    config.set_tencent_translate_secret_key("key")

    assert config.get_translation_provider_config("volcengine") == {
        "access_key_id": "ak", "secret_access_key": "sk",
    }
    assert config.get_translation_provider_config("tencent") == {
        "secret_id": "id", "secret_key": "key",
    }


@pytest.mark.parametrize("provider_id", ["volcengine", "tencent"])
def test_quota_settings_defaults_and_round_trip(config, provider_id):
    assert config.get_translation_quota_enabled(provider_id) is True
    assert config.get_translation_quota_limit(provider_id) > 0

    getattr(config, f"set_{provider_id}_translate_quota_enabled")(False)
    getattr(config, f"set_{provider_id}_translate_quota_limit")(" 1234 ")

    assert config.get_translation_quota_enabled(provider_id) is False
    assert config.get_translation_quota_limit(provider_id) == 1234


def test_invalid_quota_limit_falls_back_to_zero(config):
    config.set_volcengine_translate_quota_limit("abc")
    assert config.get_translation_quota_limit("volcengine") == 0


def test_provider_without_quota_settings(config):
    assert config.get_translation_quota_enabled("deepl") is True
    assert config.get_translation_quota_limit("deepl") == 0


def test_usage_is_bucketed_by_month(config, monkeypatch):
    from settings import tool_settings

    monkeypatch.setattr(tool_settings, "_current_month", lambda: "202601")
    config.add_translation_usage_chars("volcengine", 10)
    config.add_translation_usage_chars("volcengine", 5)
    config.add_translation_usage_chars("volcengine", 0)
    config.add_translation_usage_chars("volcengine", -3)
    assert config.get_translation_usage_chars("volcengine") == 15

    monkeypatch.setattr(tool_settings, "_current_month", lambda: "202602")
    assert config.get_translation_usage_chars("volcengine") == 0


def test_current_month_format():
    from settings.tool_settings import _current_month

    assert len(_current_month()) == 6 and _current_month().isdigit()


# ============================================================================
# TranslationService.translate
# ============================================================================

def test_service_blocks_at_limit_without_request(config, sent_requests):
    config.add_translation_usage_chars("volcengine", 1_990_000)
    service = create_default_translation_service(config)

    result = service.translate(TranslationRequest("你好", "EN"), provider_id="volcengine")

    assert result.error_code == TranslationErrorCode.QUOTA_EXCEEDED
    assert "1990000/1990000" in result.error_message
    assert sent_requests == []


def test_service_records_usage_after_success(config, sent_requests):
    service = create_default_translation_service(config)

    result = service.translate(TranslationRequest("你好世界", "EN"), provider_id="volcengine")

    assert result.success
    assert config.get_translation_usage_chars("volcengine") == 4


def test_service_custom_limit_and_guard_off(config, sent_requests):
    service = create_default_translation_service(config)
    config.set_volcengine_translate_quota_limit("10")
    config.add_translation_usage_chars("volcengine", 10)
    request = TranslationRequest("你好", "EN")

    blocked = service.translate(request, provider_id="volcengine")
    config.set_volcengine_translate_quota_enabled(False)
    allowed = service.translate(request, provider_id="volcengine")

    assert blocked.error_code == TranslationErrorCode.QUOTA_EXCEEDED
    assert allowed.success
    assert len(sent_requests) == 1
    # 关着锁也记账，重新打开时用量是准的
    assert config.get_translation_usage_chars("volcengine") == 12


def test_providers_without_quota_are_not_tracked(config):
    service = create_default_translation_service(config)
    deepl = service.provider("deepl")

    assert service.check_quota(deepl) is None
    service.record_usage(deepl, 100)
    assert config.get_translation_usage_chars("deepl") == 0


# ============================================================================
# TranslationWorker（截图翻译和设置页「测试」走这条）
# ============================================================================

def test_worker_blocks_at_limit_without_request(qtbot, config, sent_requests):
    config.add_translation_usage_chars("volcengine", 1_990_000)

    result = _run_worker(qtbot, create_default_translation_service(config))

    assert result.error_code == TranslationErrorCode.QUOTA_EXCEEDED
    assert sent_requests == []


def test_worker_requests_off_thread_and_records_on_gui_thread(
    qtbot, config, sent_requests, monkeypatch
):
    service = create_default_translation_service(config)
    record_threads = []
    original = service.record_usage

    def _record(provider, chars):
        record_threads.append(threading.get_ident())
        original(provider, chars)

    monkeypatch.setattr(service, "record_usage", _record)

    result = _run_worker(qtbot, service, "你好世界")

    assert result.success
    assert len(sent_requests) == 1 and sent_requests[0] != threading.get_ident()
    assert record_threads == [threading.get_ident()]
    assert config.get_translation_usage_chars("volcengine") == 4


# ============================================================================
# 设置页用量行
# ============================================================================

class _Dialog:
    def __init__(self, config):
        self.config_manager = config
        self.translation_registry = create_default_translation_service(config).registry

    @staticmethod
    def tr(text):
        return text


def test_usage_line_shows_usage_limit_and_guard_state(config):
    from ui.settings_ui.page_translation import _quota_usage_text

    dialog = _Dialog(config)
    config.add_translation_usage_chars("volcengine", 12345)

    assert _quota_usage_text(dialog, "volcengine") == (
        "Used this month: 12,345 / 1,990,000 characters (quota guard on)"
    )
    config.set_volcengine_translate_quota_enabled(False)
    config.set_volcengine_translate_quota_limit("20000")
    assert _quota_usage_text(dialog, "volcengine") == (
        "Used this month: 12,345 / 20,000 characters (quota guard off)"
    )


def test_usage_line_only_for_quota_providers(config):
    from ui.settings_ui.page_translation import _quota_usage_text

    dialog = _Dialog(config)
    shown = {
        meta.provider_id
        for meta in dialog.translation_registry.available_providers()
        if _quota_usage_text(dialog, meta.provider_id)
    }

    assert shown == {"volcengine", "tencent"}
    assert _quota_usage_text(dialog, "") == ""
    assert _quota_usage_text(dialog, "no-such-provider") == ""


def test_settings_page_notice_includes_usage_line(qapp, config):
    from ui.settings_ui.dialog import SettingsDialog
    from ui.settings_ui.page_translation import _update_provider_groups

    dialog = SettingsDialog()
    dialog.build_all_pages()
    combo = dialog.translation_provider_combo
    combo.setCurrentIndex(combo.findData("tencent"))
    _update_provider_groups(dialog)

    text = dialog.translation_notice_label.text()
    assert "<br/>" in text
    assert dialog.tr("Used this month: %1 / %2 characters (quota guard on)").split("%1")[0] in text
    dialog.deleteLater()
