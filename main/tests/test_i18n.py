# -*- coding: utf-8 -*-
"""
I18n 翻译系统单元测试

测试 XmlTranslator 的 XML 解析和翻译查找逻辑。
"""
import xml.etree.ElementTree as ET

import pytest
from PySide6.QtCore import QTranslator
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


SAMPLE_XML = """\
<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh">
<context>
    <name>MainWindow</name>
    <message>
        <source>Screenshot</source>
        <translation>截图</translation>
    </message>
    <message>
        <source>Settings</source>
        <translation>设置</translation>
    </message>
    <message>
        <source>Unfin</source>
        <translation type="unfinished">未完成</translation>
    </message>
</context>
<context>
    <name>Toolbar</name>
    <message>
        <source>Save</source>
        <translation>保存</translation>
    </message>
</context>
</TS>
"""


@pytest.fixture
def xml_file(tmp_path):
    """创建临时 XML 翻译文件"""
    path = tmp_path / "test_zh.xml"
    path.write_text(SAMPLE_XML, encoding="utf-8")
    return str(path)


class TestXmlTranslator:
    """XmlTranslator 测试"""

    def test_load_success(self, qapp, xml_file):
        """成功加载 XML 文件"""
        from core.i18n import XmlTranslator
        translator = XmlTranslator()
        assert translator.load_from_xml(xml_file) is True

    def test_translate_with_context(self, qapp, xml_file):
        """按上下文翻译"""
        from core.i18n import XmlTranslator
        translator = XmlTranslator()
        translator.load_from_xml(xml_file)
        assert translator.translate("MainWindow", "Screenshot") == "截图"
        assert translator.translate("MainWindow", "Settings") == "设置"
        assert translator.translate("Toolbar", "Save") == "保存"

    def test_translate_fallback_context(self, qapp, xml_file):
        """跨上下文回退查找"""
        from core.i18n import XmlTranslator
        translator = XmlTranslator()
        translator.load_from_xml(xml_file)
        # "Save" 在 Toolbar 上下文中，但用其他上下文也能找到
        result = translator.translate("OtherContext", "Save")
        assert result == "保存"

    def test_translate_missing(self, qapp, xml_file):
        """找不到的翻译返回空字符串"""
        from core.i18n import XmlTranslator
        translator = XmlTranslator()
        translator.load_from_xml(xml_file)
        result = translator.translate("MainWindow", "NonExistent")
        assert result == ""

    def test_unfinished_translation(self, qapp, xml_file):
        """标记为 unfinished 的翻译应使用原文"""
        from core.i18n import XmlTranslator
        translator = XmlTranslator()
        translator.load_from_xml(xml_file)
        # type="unfinished" 的翻译应回退到 source
        result = translator.translate("MainWindow", "Unfin")
        assert result == "Unfin"

    def test_load_nonexistent_file(self, qapp, tmp_path):
        """加载不存在的文件应返回 False"""
        from core.i18n import XmlTranslator
        translator = XmlTranslator()
        result = translator.load_from_xml(str(tmp_path / "nonexistent.xml"))
        assert result is False

    def test_load_invalid_xml(self, qapp, tmp_path):
        """加载无效 XML 文件应返回 False"""
        from core.i18n import XmlTranslator
        bad_file = tmp_path / "bad.xml"
        bad_file.write_text("<notvalid>", encoding="utf-8")
        translator = XmlTranslator()
        result = translator.load_from_xml(str(bad_file))
        assert result is False


class TestI18nManager:
    """I18nManager 单例测试"""

    @pytest.fixture(autouse=True)
    def reset_singleton(self, qapp):
        """重置单例"""
        from core.i18n import I18nManager
        I18nManager._instance = None
        yield

    def test_singleton(self, qapp):
        """应为单例"""
        from core.i18n import I18nManager
        a = I18nManager.instance()
        b = I18nManager.instance()
        assert a is b

    def test_supported_languages(self, qapp):
        """支持的语言列表"""
        from core.i18n import I18nManager
        assert "ja" in I18nManager.LANGUAGES
        assert "en" in I18nManager.LANGUAGES
        assert "ko" in I18nManager.LANGUAGES
        assert "zh" in I18nManager.LANGUAGES

    def test_translations_dir_exists(self, qapp):
        """翻译文件目录应该存在"""
        from core.i18n import I18nManager
        translations_dir = I18nManager.get_translations_dir()
        assert translations_dir.exists(), f"翻译目录不存在: {translations_dir}"


@pytest.mark.parametrize(
    "language",
    ["en", "ja", "ko", "zh"],
)
def test_clipboard_delete_confirmation_is_compiled_in_its_calling_context(
    language,
):
    """QM 翻译必须位于 ClipboardWindow 上下文，不能依赖 XML 的跨上下文回退。"""
    from core.i18n import I18nManager

    translations_dir = I18nManager.get_translations_dir()
    source = "Are you sure you want to delete this item?"
    context = next(
        context
        for context in ET.parse(translations_dir / f"app_{language}.xml")
        .getroot()
        .findall("context")
        if context.findtext("name") == "ClipboardWindow"
    )
    expected = next(
        message.findtext("translation")
        for message in context.findall("message")
        if message.findtext("source") == source
    )

    translator = QTranslator()
    qm_path = translations_dir / f"app_{language}.qm"
    assert translator.load(str(qm_path))
    assert translator.translate("ClipboardWindow", source) == expected


@pytest.mark.parametrize(
    "language",
    ["en", "ja", "ko", "zh"],
)
def test_hotkey_registration_error_is_compiled_in_main_app_context(language):
    """热键注册失败弹窗及备用热键名称必须随界面语言显示。"""
    from core.i18n import I18nManager

    translations_dir = I18nManager.get_translations_dir()
    sources = (
        "Screenshot (2)",
        "Translation (2)",
        "Clipboard (2)",
        "Hotkey Registration Failed",
        "The following hotkeys failed to register:",
        "The hotkey may be occupied by other programs. Please try a different combination.",
    )
    context = next(
        context
        for context in ET.parse(translations_dir / f"app_{language}.xml")
        .getroot()
        .findall("context")
        if context.findtext("name") == "MainApp"
    )
    expected = {
        message.findtext("source"): message.findtext("translation")
        for message in context.findall("message")
    }

    translator = QTranslator()
    qm_path = translations_dir / f"app_{language}.qm"
    assert translator.load(str(qm_path))
    for source in sources:
        assert source in expected
        assert translator.translate("MainApp", source) == expected[source]


@pytest.mark.parametrize(
    "language",
    ["en", "ja", "ko", "zh"],
)
@pytest.mark.parametrize(
    ("context_name", "source"),
    [
        # 逐条冲突提示由录入框组件自己发出，设置窗口和欢迎向导共用这一份
        ("HotkeyEdit", "This hotkey is assigned more than once."),
        (
            "SettingsDialog",
            "Some global hotkeys are duplicated or unavailable. "
            "Please fix them before applying.",
        ),
    ],
)
def test_global_hotkey_conflict_is_compiled_in_its_owning_context(
    language, context_name, source
):
    """全局热键校验提示必须随界面语言显示，且落在真正发出它的上下文里。"""
    from core.i18n import I18nManager

    translations_dir = I18nManager.get_translations_dir()
    # 同名上下文在 xml 里可能分成多块，全部合并后再查
    expected = {
        message.findtext("source"): message.findtext("translation")
        for context in ET.parse(translations_dir / f"app_{language}.xml")
        .getroot()
        .findall("context")
        if context.findtext("name") == context_name
        for message in context.findall("message")
    }

    translator = QTranslator()
    qm_path = translations_dir / f"app_{language}.qm"
    assert translator.load(str(qm_path))
    assert source in expected
    assert translator.translate(context_name, source) == expected[source]


def _compiled_toolbar_customization_sources():
    from ui.toolbar_layout_dialog import BUTTON_NAMES, MODE_NAMES

    dialog_sources = [
        *BUTTON_NAMES.values(),
        *(text for _mode, text in MODE_NAMES),
        "Customize Toolbar",
        "This button can't be hidden",
        "Drag the handle to reorder. Use the drop-down to change visibility.",
        "Restore defaults",
        "OK",
        "Cancel",
    ]
    return [("Toolbar", "More"), ("Toolbar", "Adjust")] + [
        ("ToolbarLayoutDialog", source) for source in dialog_sources
    ]


@pytest.mark.parametrize("language", ["en", "ja", "ko", "zh"])
def test_toolbar_customization_texts_are_compiled_in_their_contexts(language):
    """「…」弹层和排布对话框的文案必须随界面语言显示，且落在各自发出它的上下文里。"""
    from core.i18n import I18nManager

    translations_dir = I18nManager.get_translations_dir()
    root = ET.parse(translations_dir / f"app_{language}.xml").getroot()
    translator = QTranslator()
    assert translator.load(str(translations_dir / f"app_{language}.qm"))

    for context_name, source in _compiled_toolbar_customization_sources():
        expected = {
            message.findtext("source"): message.findtext("translation")
            for context in root.findall("context")
            if context.findtext("name") == context_name
            for message in context.findall("message")
        }
        assert expected.get(source), (context_name, source)
        assert translator.translate(context_name, source) == expected[source]
