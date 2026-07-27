from pathlib import Path

import pytest
from PySide6.QtCore import QSettings

from core.i18n import XmlTranslator
from settings.tool_settings import ToolSettingsManager
from ui.welcome.page3_clipboard import ClipboardHotkeyPage


def _manager(tmp_path):
    settings = QSettings(
        str(tmp_path / "welcome_clipboard.ini"),
        QSettings.Format.IniFormat,
    )
    return ToolSettingsManager(qsettings=settings)


def test_welcome_clipboard_history_limit_uses_saved_value(qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_clipboard_history_limit(3001)

    page = ClipboardHotkeyPage(manager)

    assert page._history_limit_spin.value() == 3001
    assert page._history_limit_spin.minimum() == 0
    assert page._history_limit_spin.maximum() == 10000

    page.close()


def test_welcome_clipboard_history_limit_updates_config(qapp, tmp_path):
    manager = _manager(tmp_path)
    page = ClipboardHotkeyPage(manager)

    page._history_limit_spin.setValue(4321)

    assert manager.get_clipboard_history_limit() == 4321

    page.close()


def test_welcome_clipboard_illustration_does_not_change_toolbar_position(
    qapp, tmp_path
):
    manager = _manager(tmp_path)

    def unexpected_toolbar_position_access(*_args, **_kwargs):
        raise AssertionError("欢迎页动画不应读取或修改工具栏位置")

    manager.get_clipboard_group_bar_position = unexpected_toolbar_position_access
    manager.set_clipboard_group_bar_position = unexpected_toolbar_position_access

    page = ClipboardHotkeyPage(manager)
    animation = page.illus_area.animation
    animation._timer.stop()

    assert animation._type_labels == ["文字", "图片", "文件"]
    assert [name for _kind, name, _metadata in animation._quick_rows] == [
        "Microsoft Edge",
        "Visual Studio Code",
        "Notepad",
    ]

    page.close()


def test_welcome_clipboard_animation_renders_every_stage(qapp, tmp_path):
    manager = _manager(tmp_path)
    page = ClipboardHotkeyPage(manager)
    animation = page.illus_area.animation
    animation._timer.stop()
    animation.resize(260, 320)

    for elapsed_ms in (1000, 5000, 6500, 8000, 9400, 11000):
        animation.set_animation_time(elapsed_ms)
        image = animation.grab().toImage()
        assert not image.isNull()

    page.close()


@pytest.mark.parametrize(
    ("language", "expected"),
    [
        (
            "en",
            {
                "历史记录": "History",
                "文字": "Text",
                "图片": "Image",
                "文件": "File",
                "内容分组": "Content Groups",
                "快速启动": "Quick Launch",
            },
        ),
        (
            "ja",
            {
                "历史记录": "履歴",
                "文字": "テキスト",
                "图片": "画像",
                "文件": "ファイル",
                "内容分组": "コンテンツグループ",
                "快速启动": "クイック起動",
            },
        ),
        (
            "ko",
            {
                "历史记录": "기록",
                "文字": "텍스트",
                "图片": "이미지",
                "文件": "파일",
                "内容分组": "콘텐츠 그룹",
                "快速启动": "빠른 실행",
            },
        ),
    ],
)
def test_welcome_clipboard_animation_has_localized_labels(language, expected):
    translator = XmlTranslator()
    translations = (
        Path(__file__).parents[1] / "translations" / f"app_{language}.xml"
    )

    assert translator.load_from_xml(str(translations))
    for source, translated in expected.items():
        assert translator.translate("WelcomeWizard", source) == translated
