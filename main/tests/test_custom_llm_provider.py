# -*- coding: utf-8 -*-
"""自定义 OpenAI 兼容服务，以及为它放开的那些参数。

这家什么都不预设，所以要盯的是「用户填了什么就发什么、没填就一个字段都不多发」：
多发一个服务端不认的参数就可能是 400。
"""
import io
import json
import urllib.error

import pytest

from translation.models import TranslationErrorCode, TranslationRequest
from translation.providers.custom_llm import CustomLLMProvider
from translation.providers.deepseek import DeepSeekProvider

_URLOPEN = "translation.providers.openai_compatible.urllib.request.urlopen"


def _provider(**overrides):
    config = {"base_url": "http://localhost:11434/v1", "model": "qwen3"}
    config.update(overrides)
    return CustomLLMProvider(config)


class _Response:
    def __init__(self, payload):
        self._payload = payload

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass

    def read(self):
        return json.dumps(self._payload).encode("utf-8")


def _reply(content):
    return {"choices": [{"message": {"content": content}, "finish_reason": "stop"}]}


def _http_error(status, payload):
    return urllib.error.HTTPError(
        "http://x", status, "err", {}, io.BytesIO(json.dumps(payload).encode())
    )


@pytest.fixture
def server(monkeypatch):
    """按顺序回放预设回复，并记下每次请求。回复是异常就抛出去。"""
    box = {"requests": [], "replies": []}

    def _urlopen(request, timeout=None):
        box["requests"].append(request)
        box["timeout"] = timeout
        reply = box["replies"].pop(0) if box["replies"] else _reply(
            json.dumps({"translation": "你好", "detected_source_lang": "en"}))
        if isinstance(reply, Exception):
            raise reply
        return _Response(reply)

    monkeypatch.setattr(_URLOPEN, _urlopen)
    return box


def _body(request):
    return json.loads(request.data.decode("utf-8"))


def _headers(request):
    return {k.lower(): v for k, v in request.header_items()}


# ============================================================================
# 什么都不填时
# ============================================================================

def test_local_services_need_no_api_key(server):
    provider = _provider()
    assert provider.is_configured()

    result = provider.translate(TranslationRequest("Hello", "ZH"))
    assert result.success
    assert "authorization" not in _headers(server["requests"][0])


def test_address_and_model_are_required():
    assert not _provider(base_url="").is_configured()
    assert not _provider(model="").is_configured()


def test_nothing_deepseek_specific_is_sent(server):
    """温度不填就不传：有的模型只接受默认温度，传了直接 400。"""
    _provider().translate(TranslationRequest("Hello", "ZH"))
    body = _body(server["requests"][0])

    assert "temperature" not in body
    assert "thinking" not in body
    assert body["response_format"] == {"type": "json_object"}


def test_a_pasted_full_endpoint_is_not_doubled():
    provider = _provider(base_url="https://api.openai.com/v1/chat/completions/")
    assert provider.api_url == "https://api.openai.com/v1/chat/completions"


def test_local_models_get_a_longer_timeout_floor(server):
    _provider().translate(TranslationRequest("Hello", "ZH", timeout=10))
    assert server["timeout"] == CustomLLMProvider.DEFAULT_TIMEOUT


# ============================================================================
# 放开的参数
# ============================================================================

def test_temperature_and_timeout_are_sent_as_configured(server):
    _provider(temperature="0.3", timeout="120").translate(
        TranslationRequest("Hello", "ZH", timeout=10))

    assert _body(server["requests"][0])["temperature"] == 0.3
    assert server["timeout"] == 120


def test_timeout_is_clamped_to_a_sane_range(server):
    _provider(timeout="99999").translate(TranslationRequest("Hello", "ZH"))
    assert server["timeout"] == CustomLLMProvider.TIMEOUT_RANGE[1]


def test_extra_body_is_merged_but_cannot_replace_the_translation(server):
    extra = json.dumps({"enable_thinking": False, "top_p": 0.9,
                        "messages": [], "model": "other", "stream": True})
    _provider(extra_body=extra).translate(TranslationRequest("Hello", "ZH"))
    body = _body(server["requests"][0])

    assert body["enable_thinking"] is False
    assert body["top_p"] == 0.9
    assert body["model"] == "qwen3"
    assert len(body["messages"]) == 2
    assert body["stream"] is False


@pytest.mark.parametrize("overrides,fragment", [
    ({"temperature": "warm"}, "Temperature"),
    ({"timeout": "soon"}, "Timeout"),
    ({"extra_body": "{not json"}, "Extra Body"),
    ({"extra_body": "[1, 2]"}, "Extra Body"),
])
def test_bad_advanced_values_are_reported_not_sent(server, overrides, fragment):
    """填错的高级参数要说清楚是哪一项，而不是发出去换一个服务端的 400。"""
    result = _provider(**overrides).translate(TranslationRequest("Hello", "ZH"))

    assert not result.success
    assert result.error_code is TranslationErrorCode.INVALID_REQUEST
    assert fragment in result.error_message
    assert server["requests"] == []


def test_instructions_go_to_the_system_prompt_not_the_source_text(server):
    _provider(instructions="Keep product names in English").translate(
        TranslationRequest("Hello", "ZH"))
    system, user = _body(server["requests"][0])["messages"]

    assert "Keep product names in English" in system["content"]
    assert user["content"] == "Hello"


def test_without_instructions_the_prompt_is_unchanged(server):
    _provider().translate(TranslationRequest("Hello", "ZH"))
    system = _body(server["requests"][0])["messages"][0]["content"]
    assert "Additional requirements" not in system


# ============================================================================
# JSON 模式与回复解析
# ============================================================================

def test_json_mode_rejected_retries_once_without_it(server):
    server["replies"] = [
        _http_error(400, {"error": {"message": "response_format not supported"}}),
        _reply("你好"),
    ]
    result = _provider().translate(TranslationRequest("Hello", "ZH"))

    assert result.success and result.translated_text == "你好"
    first, second = server["requests"]
    assert "response_format" in _body(first)
    assert "response_format" not in _body(second)


def test_a_400_without_json_mode_is_not_retried(server):
    server["replies"] = [_http_error(400, {"error": {"message": "bad model"}})]
    result = _provider(json_mode=False).translate(TranslationRequest("Hello", "ZH"))

    assert not result.success
    assert result.error_message == "bad model"
    assert len(server["requests"]) == 1
    assert "response_format" not in _body(server["requests"][0])


def test_thinking_block_is_stripped(server):
    server["replies"] = [_reply(
        '<think>The user wants Chinese.</think>\n'
        '{"translation": "你好", "detected_source_lang": "en"}')]
    result = _provider().translate(TranslationRequest("Hello", "ZH"))
    assert result.translated_text == "你好"


def test_thinking_block_is_stripped_from_plain_text_replies_too(server):
    server["replies"] = [_reply("<think>hmm</think>你好")]
    result = _provider().translate(TranslationRequest("Hello", "ZH"))
    assert result.translated_text == "你好"


def test_string_error_bodies_are_read(server):
    """Ollama 的 error 是字符串，不是 OpenAI 那样的对象。"""
    server["replies"] = [_http_error(404, {"error": "model 'qwen9' not found"})]
    result = _provider(json_mode=False).translate(TranslationRequest("Hello", "ZH"))
    assert result.error_message == "model 'qwen9' not found"


# ============================================================================
# 模型列表
# ============================================================================

def test_models_are_listed_sorted_and_deduplicated(server):
    server["replies"] = [{"data": [{"id": "qwen3"}, {"id": "Llama3"},
                                   {"id": "qwen3"}, {"object": "model"}]}]
    models = _provider(api_key="sk-1").list_models()

    assert models == ["Llama3", "qwen3"]
    request = server["requests"][0]
    assert request.full_url == "http://localhost:11434/v1/models"
    assert _headers(request)["authorization"] == "Bearer sk-1"


def test_model_list_errors_carry_the_server_message(server):
    server["replies"] = [_http_error(401, {"error": {"message": "invalid key"}})]
    with pytest.raises(RuntimeError, match="invalid key"):
        _provider().list_models()


def test_model_list_rejects_an_unexpected_shape(server):
    server["replies"] = [{"models": []}]
    with pytest.raises(RuntimeError):
        _provider().list_models()


def test_deepseek_keeps_its_own_defaults(server):
    """基类放开参数后，DeepSeek 的请求不能跟着变。"""
    DeepSeekProvider({"api_key": "sk"}).translate(TranslationRequest("Hello", "ZH"))
    body = _body(server["requests"][0])

    assert body["temperature"] == DeepSeekProvider.TEMPERATURE
    assert body["thinking"] == {"type": "disabled"}
    assert body["response_format"] == {"type": "json_object"}


# ============================================================================
# 配置映射与设置页
# ============================================================================

@pytest.fixture
def settings(tmp_path):
    from PySide6.QtCore import QSettings
    from settings.tool_settings import ToolSettingsManager

    return ToolSettingsManager(
        qsettings=QSettings(str(tmp_path / "s.ini"), QSettings.Format.IniFormat))


def test_saved_settings_reach_the_provider(settings):
    settings.set_custom_llm_base_url("http://localhost:1234/v1")
    settings.set_custom_llm_model("  phi-4 ")
    settings.set_custom_llm_temperature("0.2")
    settings.set_custom_llm_json_mode(False)

    provider = CustomLLMProvider(settings.get_translation_provider_config("custom_llm"))
    assert provider.api_url == "http://localhost:1234/v1/chat/completions"
    assert provider._model == "phi-4"
    assert provider._temperature == 0.2
    assert provider._json_mode is False


def test_json_mode_defaults_on(settings):
    assert settings.get_custom_llm_json_mode() is True


class _Edit:
    def __init__(self, text):
        self._text = text

    def text(self):
        return self._text


def test_pending_config_uses_unsaved_form_values(settings):
    """测试连接要测的是表单上现在的值，而不是上次保存的。"""
    from translation.providers.custom_llm import CustomLLMProvider as P
    from ui.settings_ui.provider_fields import PendingConfig

    settings.set_custom_llm_model("saved-model")
    settings.set_custom_llm_base_url("http://saved/v1")
    pending = PendingConfig(
        settings, P.all_fields(), {"custom_llm_model": _Edit(" typed-model ")}
    )

    config = pending.get_translation_provider_config("custom_llm")
    assert config["model"] == "typed-model"
    assert config["base_url"] == "http://saved/v1"
    # 没被表单覆盖的方法照常转给真实配置
    assert pending.get_translation_provider() == settings.get_translation_provider()


def test_settings_page_offers_test_and_model_list(qapp):
    from translation.provider import ModelField
    from ui.fluent_lite import PushButton
    from ui.settings_ui.dialog import SettingsDialog

    dialog = SettingsDialog()
    assert set(dialog.provider_status_labels) == set(dialog.provider_sections)

    edit = dialog.provider_field_widgets["custom_llm_model"]
    buttons = [b.text() for b in edit.parentWidget().findChildren(PushButton)]
    assert dialog.tr("Fetch Models") in buttons
    assert any(isinstance(f, ModelField) for f in CustomLLMProvider.all_fields())


def test_test_button_reports_the_result_on_the_page(qapp, qtbot, server, settings):
    """走真实的按钮 → 后台线程 → 信号 → 状态行，用的是还没保存的表单值。"""
    from ui.fluent_lite import PushButton
    from ui.settings_ui.dialog import SettingsDialog

    # 不传配置时用的是预览用的 MockConfig，它不做设置键到 provider 参数的映射
    dialog = SettingsDialog(config_manager=settings)
    widgets = dialog.provider_field_widgets
    widgets["custom_llm_base_url"].setText("http://localhost:9/v1")
    widgets["custom_llm_model"].setText("typed-model")

    status = dialog.provider_status_labels["custom_llm"]
    button = next(b for b in status.parentWidget().findChildren(PushButton)
                  if b.text() == dialog.tr("Test Connection"))
    button.click()
    qtbot.waitUntil(lambda: status.text().startswith(("✓", "✗")), timeout=5000)

    assert status.text() == "✓ 你好"
    assert button.isEnabled()
    request = server["requests"][0]
    assert request.full_url == "http://localhost:9/v1/chat/completions"
    assert _body(request)["model"] == "typed-model"


def test_reasoning_that_uses_up_the_reply_says_how_to_turn_it_off(server):
    """思考模型默认开思考，额度花光时正文是空的；只报 finish_reason 用户不知道怎么办。"""
    server["replies"] = [{"choices": [{
        "message": {"content": "", "reasoning": "Let me think about this..."},
        "finish_reason": "length",
    }]}]
    result = _provider().translate(TranslationRequest("Hello", "ZH"))

    assert not result.success
    assert '"reasoning_effort": "none"' in result.error_message


def test_empty_reply_without_reasoning_keeps_the_generic_message(server):
    server["replies"] = [_reply("")]
    server["replies"][0]["choices"][0]["finish_reason"] = "content_filter"
    result = _provider().translate(TranslationRequest("Hello", "ZH"))
    assert "finish_reason=content_filter" in result.error_message


def _fetch_button(dialog, key):
    from ui.fluent_lite import PushButton

    edit = dialog.provider_field_widgets[key]
    return edit, next(b for b in edit.parentWidget().findChildren(PushButton)
                      if b.text() == dialog.tr("Fetch Models"))


def test_fetched_models_are_offered_and_the_pick_is_filled_in(
        qapp, qtbot, server, settings, monkeypatch):
    from PySide6.QtWidgets import QMenu
    from ui.settings_ui import page_translation
    from ui.settings_ui.dialog import SettingsDialog

    offered = []
    backgrounds = []

    class _Menu(QMenu):
        """真 exec 会进模态循环；给 Qt 类打补丁不生效，只能换成子类。"""

        def exec(self, *_args):
            offered.extend(action.text() for action in self.actions())
            self.adjustSize()
            image = self.grab().toImage()
            backgrounds.append(image.pixelColor(image.width() - 8, image.height() - 8).name())
            self.actions()[0].trigger()

    monkeypatch.setattr(page_translation, "QMenu", _Menu)
    server["replies"] = [{"data": [{"id": "qwen3.5:2b"}, {"id": "llama3"}]}]
    dialog = SettingsDialog(config_manager=settings)
    dialog.provider_field_widgets["custom_llm_base_url"].setText("http://localhost:9/v1")
    edit, button = _fetch_button(dialog, "custom_llm_model")

    button.click()
    qtbot.waitUntil(lambda: bool(offered), timeout=5000)

    assert offered == ["llama3", "qwen3.5:2b"]
    assert edit.text() == "llama3"
    # 挂在输入框下的菜单会继承输入框的样式表，不单独上主题色就是黑底
    from ui.fluent_lite.theme import ui_tokens
    assert backgrounds == [ui_tokens(edit).popup_background.lower()]
    assert button.isEnabled()


def test_fetch_failure_is_shown_next_to_the_button(qapp, qtbot, server, settings):
    """状态行在表单最底下，只写那里的话用户会以为按钮没反应。"""
    from PySide6.QtWidgets import QToolTip
    from ui.settings_ui.dialog import SettingsDialog

    server["replies"] = [_http_error(401, {"error": {"message": "invalid key"}})]
    dialog = SettingsDialog(config_manager=settings)
    dialog.provider_field_widgets["custom_llm_base_url"].setText("http://localhost:9/v1")
    _edit, button = _fetch_button(dialog, "custom_llm_model")
    status = dialog.provider_status_labels["custom_llm"]

    button.click()
    qtbot.waitUntil(lambda: status.text().startswith("✗"), timeout=5000)

    assert status.text() == "✗ invalid key"
    assert QToolTip.text() == "✗ invalid key"
    # 套上按钮样式表的提示框是黑底深色字，得用全局主题的配色
    tips = [w for w in qapp.topLevelWidgets()
            if w.metaObject().className() == "QTipLabel" and w.isVisible()]
    assert tips and not tips[0].styleSheet()
