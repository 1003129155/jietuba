# -*- coding: utf-8 -*-
"""
I18n 翻译系统单元测试

测试 XmlTranslator 的 XML 解析和翻译查找逻辑。
"""
import pytest
import os
import tempfile
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
        assert "zh" in I18nManager.LANGUAGES

    def test_translations_dir_exists(self, qapp):
        """翻译文件目录应该存在"""
        from core.i18n import I18nManager
        translations_dir = I18nManager.get_translations_dir()
        assert translations_dir.exists(), f"翻译目录不存在: {translations_dir}"
 