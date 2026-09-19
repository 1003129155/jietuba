# -*- coding: utf-8 -*-
"""翻译服务商设置项的声明 → 界面 → 存取这条链路。

这条链路以前是手写的，断在哪一节都不报错，只会静默丢数据：
azure 断在「建表单」那节（选得到引擎、下面空白），
baidu 断在「保存」那节（填了、存不上、一直说未配置），
而两次出事时全部测试都是绿的。

现在链路由 CREDENTIAL_FIELDS / OPTION_FIELDS 的声明驱动，所以这里守的是
「声明和实现没有脱节」，而不是某一家的具体字段——加一家会被自动覆盖。
"""
import os

import pytest

from translation.provider import TextField, ToggleField
from ui.settings_ui import provider_fields


os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


class _Config:
    """够 create_default_translation_service 用的空壳。"""

    def get_translation_provider(self):
        return ""

    def get_translation_provider_config(self, provider_id):
        return {}


@pytest.fixture(scope="module")
def registry():
    from translation.service import create_default_translation_service

    return create_default_translation_service(_Config()).registry


@pytest.fixture(scope="module")
def declared_fields(registry):
    return provider_fields.all_fields(registry)


# ============================================================================
# 声明本身的一致性
# ============================================================================

def test_every_provider_declares_at_least_one_credential(registry):
    for meta in registry.available_providers():
        assert meta.credentials, meta.provider_id


def test_config_keys_are_unique_across_providers(declared_fields):
    """两家用同一个 config_key 会互相覆盖对方的值，而且不会报错。"""
    keys = [f.config_key for f in declared_fields]
    duplicates = {k for k in keys if keys.count(k) > 1}
    assert not duplicates, duplicates


def test_declared_keys_have_getter_and_setter_on_the_real_config(
    declared_fields,
):
    """config_key 同时是 get_/set_ 的方法名，这个约定断了整条链路就断了。"""
    from settings.tool_settings import ToolSettingsManager

    missing = []
    for f in declared_fields:
        for prefix in ("get_", "set_"):
            if not hasattr(ToolSettingsManager, prefix + f.config_key):
                missing.append(prefix + f.config_key)
    assert not missing, missing


def test_declared_keys_have_defaults(declared_fields):
    """恢复默认按 APP_DEFAULT_SETTINGS 取值，漏登记的键会被静默跳过。"""
    from settings.tool_settings import ToolSettingsManager

    defaults = ToolSettingsManager.APP_DEFAULT_SETTINGS
    missing = [f.config_key for f in declared_fields
               if f.config_key not in defaults]
    assert not missing, missing


_PRE_EXISTING_NON_ASCII = {
    ("amazon", "AWS 区域"),
    ("amazon", "可选，临时凭据使用"),
}


def _declared_ui_strings(meta):
    for f in tuple(meta.credentials) + tuple(meta.options):
        yield "label", f.label
        for attr in ("placeholder", "description"):
            text = getattr(f, attr, "")
            if text:
                yield attr, text
    if meta.notice:
        yield "notice", meta.notice
    if meta.help_label:
        yield "help_label", meta.help_label


def test_declared_ui_strings_are_ascii_source(registry):
    offenders = [
        (meta.provider_id, kind, text)
        for meta in registry.available_providers()
        for kind, text in _declared_ui_strings(meta)
        if any(ord(c) > 127 for c in text)
        and (meta.provider_id, text) not in _PRE_EXISTING_NON_ASCII
    ]
    assert not offenders, offenders


def test_declared_labels_and_notices_have_a_chinese_entry(registry):
    import xml.etree.ElementTree as ET
    from pathlib import Path

    # 相对工作目录取会挂：pytest 可以从仓库根跑，也可以从 main/ 跑
    xml_path = Path(__file__).resolve().parent.parent / "translations" / "app_zh.xml"
    tree = ET.parse(xml_path)
    known = {}
    for context in tree.getroot().iter("context"):
        name = context.findtext("name") or ""
        known.setdefault(name, set()).update(
            (m.findtext("source") or "") for m in context.iter("message")
        )

    def needs_entry(provider_id, text):
        return (provider_id, text) not in _PRE_EXISTING_NON_ASCII

    missing = []
    for meta in registry.available_providers():
        for kind, text in _declared_ui_strings(meta):
            if kind not in ("label", "notice"):
                continue
            if not needs_entry(meta.provider_id, text):
                continue
            if text not in known.get("SettingsDialog", ()):
                missing.append(("SettingsDialog", meta.provider_id, text))
        for f in meta.credentials:
            if not needs_entry(meta.provider_id, f.label):
                continue
            if f.label not in known.get("WelcomeWizard", ()):
                missing.append(("WelcomeWizard", meta.provider_id, f.label))
    assert not missing, missing


def test_supported_request_options_are_real_option_keys(registry):
    """声明支持一个不存在的选项名，界面上就是永远不显示，且不会报错。"""
    known = {"split_sentences", "preserve_formatting"}
    for meta in registry.available_providers():
        unknown = set(meta.supported_request_options) - known
        assert not unknown, (meta.provider_id, unknown)


# ============================================================================
# 声明 → 界面：真实设置页必须为每个声明的字段建出控件
# ============================================================================

def test_settings_page_builds_a_widget_for_every_declared_field(
    qapp, declared_fields
):
    """这是替代「accept() 有没有保存每一家」的那条守卫。

    保存/重载/快照现在都遍历 provider_field_widgets，所以只要每个声明的字段
    都有控件，那几条路径就自动是全的——不必再逐条去测。
    """
    from ui.settings_ui.dialog import SettingsDialog

    dialog = SettingsDialog()
    built = set(dialog.provider_field_widgets)
    declared = {f.config_key for f in declared_fields}
    assert built == declared, {
        "界面漏建": declared - built,
        "界面多出": built - declared,
    }


def test_settings_page_builds_one_section_per_provider(qapp, registry):
    from ui.settings_ui.dialog import SettingsDialog

    dialog = SettingsDialog()
    assert set(dialog.provider_sections) == {
        m.provider_id for m in registry.available_providers()
    }


# ============================================================================
# 界面 → 存取
# ============================================================================

class _Edit:
    def __init__(self, text=""):
        self._text = text

    def text(self):
        return self._text

    def setText(self, value):
        self._text = value


class _Switch:
    def __init__(self, checked=False):
        self._checked = checked

    def isChecked(self):
        return self._checked

    def setChecked(self, value):
        self._checked = value


class _Recorder:
    """只认 get_/set_<key> 的假配置。"""

    def __init__(self, values=None):
        self.values = dict(values or {})

    def __getattr__(self, name):
        if name.startswith("get_"):
            key = name[4:]
            return lambda: self.values.get(key, "")
        if name.startswith("set_"):
            key = name[4:]
            return lambda value: self.values.__setitem__(key, value)
        raise AttributeError(name)


def test_save_from_writes_every_field():
    fields = (TextField("alpha_key", "Alpha"),
              ToggleField("beta_flag", "Beta"))
    widgets = {"alpha_key": _Edit("v1"), "beta_flag": _Switch(True)}
    config = _Recorder()

    provider_fields.save_from(config, fields, widgets)

    assert config.values == {"alpha_key": "v1", "beta_flag": True}


def test_save_from_strips_surrounding_whitespace():
    """凭据里混进空格是高频事故，而且肉眼看不出来。"""
    fields = (TextField("alpha_key", "Alpha"),)
    config = _Recorder()

    provider_fields.save_from(
        config, fields, {"alpha_key": _Edit("  spaced  ")}
    )

    assert config.values["alpha_key"] == "spaced"


def test_load_into_fills_every_field():
    fields = (TextField("alpha_key", "Alpha"),
              ToggleField("beta_flag", "Beta"))
    widgets = {"alpha_key": _Edit(), "beta_flag": _Switch()}

    provider_fields.load_into(
        _Recorder({"alpha_key": "stored", "beta_flag": True}), fields, widgets
    )

    assert widgets["alpha_key"].text() == "stored"
    assert widgets["beta_flag"].isChecked() is True


def test_missing_widget_is_skipped_not_crashed():
    """页面还没建好、或某字段的控件不在时，存取不该炸。"""
    fields = (TextField("alpha_key", "Alpha"),)
    config = _Recorder()

    provider_fields.save_from(config, fields, {})
    provider_fields.load_into(config, fields, {})

    assert config.values == {}


def test_reset_keeps_values_whose_key_has_no_default():
    """默认值表漏登记时宁可不恢复，也不能把用户填好的凭据清空。"""
    fields = (TextField("alpha_key", "Alpha"),)
    widget = _Edit("user-typed")

    provider_fields.reset_to_defaults({}, fields, {"alpha_key": widget})

    assert widget.text() == "user-typed"


def test_reset_applies_declared_defaults():
    fields = (TextField("alpha_key", "Alpha"),
              ToggleField("beta_flag", "Beta"))
    widgets = {"alpha_key": _Edit("dirty"), "beta_flag": _Switch(True)}

    provider_fields.reset_to_defaults(
        {"alpha_key": "", "beta_flag": False}, fields, widgets
    )

    assert widgets["alpha_key"].text() == ""
    assert widgets["beta_flag"].isChecked() is False


def test_unknown_config_key_is_ignored_rather_than_raising():
    """provider 声明了配置管理器没有的键时，不该让整个设置页崩掉。"""
    fields = (TextField("no_such_key", "Nope"),)

    class _Bare:
        pass

    assert provider_fields.read_config(_Bare(), fields[0]) == ""
    provider_fields.write_config(_Bare(), fields[0], "x")  # 不抛异常
