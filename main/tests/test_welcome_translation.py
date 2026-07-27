from pathlib import Path

import pytest
from PySide6.QtCore import QSettings

from core.i18n import XmlTranslator
from settings.tool_settings import ToolSettingsManager
from ui.welcome.page5_translation import TranslationPage


def _manager(tmp_path):
    settings = QSettings(
        str(tmp_path / "welcome_translation.ini"),
        QSettings.Format.IniFormat,
    )
    return ToolSettingsManager(qsettings=settings)


def test_welcome_translation_defaults_to_google(qapp, tmp_path):
    manager = _manager(tmp_path)
    page = TranslationPage(manager)

    assert manager.get_translation_provider() == "google"
    assert page._provider_combo.currentData() == "google"
    assert page.illus_area.isHidden()
    assert page._settings_card.property("welcomeSettingRow") is True
    assert (
        page._credential_stack.currentWidget()
        is page._provider_pages["google"]
    )

    page.close()


def test_welcome_translation_switches_all_provider_panels(qapp, tmp_path):
    page = TranslationPage(_manager(tmp_path))
    panel_heights = {}

    for provider_id in ("google", "deepl", "amazon"):
        page._provider_combo.setCurrentIndex(
            page._provider_combo.findData(provider_id)
        )
        assert (
            page._credential_stack.currentWidget()
            is page._provider_pages[provider_id]
        )
        panel_heights[provider_id] = page._credential_stack.height()
        assert (
            page._credential_stack.height()
            >= page._provider_pages[provider_id].sizeHint().height() + 10
        )

    assert panel_heights["amazon"] > panel_heights["google"]

    page.close()


def test_welcome_translation_inputs_are_not_clipped(qapp, tmp_path):
    page = TranslationPage(_manager(tmp_path))
    page.show()
    qapp.processEvents()

    controls = [
        page._provider_combo,
        page._lang_combo,
        page._google_key_edit,
        page._deepl_key_edit,
        page._amazon_region_edit,
        page._amazon_access_edit,
        page._amazon_secret_edit,
        page._amazon_token_edit,
    ]
    for control in controls:
        assert control.height() >= control.sizeHint().height()

    page.close()


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("翻译引擎", "翻訳エンジン"),
        ("AWS 区域", "AWS リージョン"),
        ("Session Token", "セッショントークン"),
        ("翻译目标语言", "翻訳先言語"),
    ],
)
def test_welcome_translation_has_japanese_resources(source, expected):
    translator = XmlTranslator()
    translations = (
        Path(__file__).parents[1] / "translations" / "app_ja.xml"
    )

    assert translator.load_from_xml(str(translations))
    assert translator.translate("WelcomeWizard", source) == expected


def test_welcome_translation_saves_all_provider_credentials(
    qapp, tmp_path
):
    manager = _manager(tmp_path)
    page = TranslationPage(manager)
    page._provider_combo.setCurrentIndex(
        page._provider_combo.findData("amazon")
    )
    page._google_key_edit.setText("google-key")
    page._deepl_key_edit.setText("deepl-key")
    page._amazon_region_edit.setText("ap-northeast-1")
    page._amazon_access_edit.setText("amazon-access")
    page._amazon_secret_edit.setText("amazon-secret")
    page._amazon_token_edit.setText("amazon-token")
    page._lang_combo.setCurrentIndex(page._lang_combo.findData("JA"))

    page.save()

    assert manager.get_translation_provider() == "amazon"
    assert manager.get_google_translate_api_key() == "google-key"
    assert manager.get_deepl_api_key() == "deepl-key"
    assert manager.get_translation_provider_config("amazon") == {
        "region": "ap-northeast-1",
        "access_key_id": "amazon-access",
        "secret_access_key": "amazon-secret",
        "session_token": "amazon-token",
    }
    assert manager.get_translation_target_lang() == "JA"

    page.close()
