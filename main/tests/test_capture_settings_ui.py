import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings, QTranslator

from settings.tool_settings import ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog
from ui.settings_ui.page_capture import create_capture_page


def _manager(tmp_path):
    qsettings = QSettings(
        str(tmp_path / "capture_settings.ini"),
        QSettings.Format.IniFormat,
    )
    return ToolSettingsManager(qsettings=qsettings)


@pytest.mark.parametrize("enabled", [True, False])
def test_capture_page_reads_double_click_toggle(qapp, tmp_path, enabled):
    manager = _manager(tmp_path)
    manager.set_double_click_copy_close_enabled(enabled)
    dialog = SimpleNamespace(
        config_manager=manager,
        tr=lambda text: text,
        _change_save_dir=lambda: None,
        _open_save_dir=lambda: None,
    )

    page = create_capture_page(dialog)

    try:
        assert dialog.double_click_copy_close_toggle.isChecked() is enabled
    finally:
        page.deleteLater()
        qapp.processEvents()


@pytest.mark.parametrize("enabled", [True, False])
def test_capture_page_reads_annotation_behavior_toggles(qapp, tmp_path, enabled):
    manager = _manager(tmp_path)
    manager.set_cross_tool_selection_enabled(enabled)
    manager.set_text_always_on_top_enabled(enabled)
    dialog = SimpleNamespace(
        config_manager=manager,
        tr=lambda text: text,
        _change_save_dir=lambda: None,
        _open_save_dir=lambda: None,
    )

    page = create_capture_page(dialog)

    try:
        assert dialog.cross_tool_selection_toggle.isChecked() is enabled
        assert dialog.text_always_on_top_toggle.isChecked() is enabled
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_settings_dialog_saves_double_click_toggle(monkeypatch, qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    dialog = SettingsDialog(manager)

    for attr in (
        "log_toggle",
        "autostart_toggle",
        "language_combo",
        "_ui_theme_combo",
        "_appearance_theme_color",
        "_appearance_mask_color",
        "_inapp_edits",
    ):
        if hasattr(dialog, attr):
            delattr(dialog, attr)

    dialog._settings_snapshot = dialog._snapshot_settings()
    dialog.double_click_copy_close_toggle.setChecked(False)

    assert dialog._has_unsaved_changes()
    dialog.accept()
    assert manager.get_double_click_copy_close_enabled() is False

    dialog.deleteLater()
    qapp.processEvents()


def test_settings_dialog_saves_annotation_behavior_toggles(
    monkeypatch,
    qapp,
    tmp_path,
):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    dialog = SettingsDialog(manager)

    for attr in (
        "log_toggle",
        "autostart_toggle",
        "language_combo",
        "_ui_theme_combo",
        "_appearance_theme_color",
        "_appearance_mask_color",
        "_inapp_edits",
    ):
        if hasattr(dialog, attr):
            delattr(dialog, attr)

    dialog._settings_snapshot = dialog._snapshot_settings()
    dialog.cross_tool_selection_toggle.setChecked(False)
    dialog.text_always_on_top_toggle.setChecked(False)

    assert dialog._has_unsaved_changes()
    dialog.accept()
    assert manager.get_cross_tool_selection_enabled() is False
    assert manager.get_text_always_on_top_enabled() is False

    dialog.deleteLater()
    qapp.processEvents()


def test_double_click_toggle_reset_and_refresh(qapp, tmp_path):
    manager = _manager(tmp_path)
    toggle = SimpleNamespace(value=False)
    toggle.isChecked = lambda: toggle.value
    toggle.setChecked = lambda value: setattr(toggle, "value", value)
    dialog = SimpleNamespace(
        config_manager=manager,
        double_click_copy_close_toggle=toggle,
    )

    SettingsDialog._reset_screenshot_settings_page(dialog)
    assert toggle.value is True

    manager.set_double_click_copy_close_enabled(False)
    SettingsDialog.refresh_settings(dialog)
    assert toggle.value is False

    snapshot = SettingsDialog._snapshot_settings(dialog)
    assert snapshot["double_click_copy_close_toggle"] is False


def test_annotation_behavior_toggles_reset_refresh_and_snapshot(qapp, tmp_path):
    manager = _manager(tmp_path)

    def toggle():
        value = SimpleNamespace(value=False)
        value.isChecked = lambda: value.value
        value.setChecked = lambda checked: setattr(value, "value", checked)
        return value

    cross_toggle = toggle()
    text_toggle = toggle()
    dialog = SimpleNamespace(
        config_manager=manager,
        cross_tool_selection_toggle=cross_toggle,
        text_always_on_top_toggle=text_toggle,
    )

    SettingsDialog._reset_screenshot_settings_page(dialog)
    assert cross_toggle.value is True
    assert text_toggle.value is True

    manager.set_cross_tool_selection_enabled(False)
    manager.set_text_always_on_top_enabled(False)
    SettingsDialog.refresh_settings(dialog)
    assert cross_toggle.value is False
    assert text_toggle.value is False

    snapshot = SettingsDialog._snapshot_settings(dialog)
    assert snapshot["cross_tool_selection_toggle"] is False
    assert snapshot["text_always_on_top_toggle"] is False


def test_double_click_setting_translations_exist_and_load(qapp):
    translations = Path(__file__).parents[1] / "translations"
    expected_by_language = {
        "en": {
            "Capture Behavior": "Capture Behavior",
            "Double-click to Copy and Close": "Double-click to Copy and Close",
            "Double-click the selected screenshot to copy it to the clipboard and close the capture.":
                "Double-click the selected screenshot to copy it to the clipboard and close the capture.",
            "Enable Ctrl Cross-Tool Selection": "Enable Ctrl Cross-Tool Selection",
            "Hold Ctrl and click any editable annotation to adjust it without switching tools.":
                "Hold Ctrl and click any editable annotation to adjust it without switching tools.",
            "Keep Text Annotations on Top": "Keep Text Annotations on Top",
            "Keep text above other annotations, including ones drawn later.":
                "Keep text above other annotations, including ones drawn later.",
        },
        "zh": {
            "Capture Behavior": "截图行为",
            "Double-click to Copy and Close": "双击复制并关闭",
            "Double-click the selected screenshot to copy it to the clipboard and close the capture.":
                "双击已选截图时复制到剪贴板并关闭截图。",
            "Enable Ctrl Cross-Tool Selection": "启用 Ctrl 跨工具选择",
            "Hold Ctrl and click any editable annotation to adjust it without switching tools.":
                "按住 Ctrl 点击任意可编辑标注，无需切换工具即可调整。",
            "Keep Text Annotations on Top": "文字标注始终置顶",
            "Keep text above other annotations, including ones drawn later.":
                "让文字保持在其他标注上方，包括之后绘制的标注。",
        },
        "ja": {
            "Capture Behavior": "キャプチャ動作",
            "Double-click to Copy and Close": "ダブルクリックでコピーして閉じる",
            "Double-click the selected screenshot to copy it to the clipboard and close the capture.":
                "選択したスクリーンショットをダブルクリックすると、クリップボードにコピーしてキャプチャを閉じます。",
            "Enable Ctrl Cross-Tool Selection": "Ctrlによるツール横断選択を有効にする",
            "Hold Ctrl and click any editable annotation to adjust it without switching tools.":
                "Ctrlを押しながら編集可能な注釈をクリックすると、ツールを切り替えずに調整できます。",
            "Keep Text Annotations on Top": "テキスト注釈を常に最前面に表示",
            "Keep text above other annotations, including ones drawn later.":
                "後から描画したものを含め、テキストを他の注釈より前面に保ちます。",
        },
        "ko": {
            "Capture Behavior": "캡처 동작",
            "Double-click to Copy and Close": "두 번 클릭하여 복사 후 닫기",
            "Double-click the selected screenshot to copy it to the clipboard and close the capture.":
                "선택한 스크린샷을 두 번 클릭하면 클립보드에 복사하고 캡처를 닫습니다.",
            "Enable Ctrl Cross-Tool Selection": "Ctrl 도구 간 선택 사용",
            "Hold Ctrl and click any editable annotation to adjust it without switching tools.":
                "Ctrl 키를 누른 채 편집 가능한 주석을 클릭하면 도구를 바꾸지 않고 조정할 수 있습니다.",
            "Keep Text Annotations on Top": "텍스트 주석을 항상 위에 표시",
            "Keep text above other annotations, including ones drawn later.":
                "나중에 그린 항목을 포함해 텍스트를 다른 주석보다 위에 유지합니다.",
        },
    }

    for language, expected in expected_by_language.items():
        root = ET.parse(translations / f"app_{language}.xml").getroot()
        settings_messages = {
            message.findtext("source"): message.findtext("translation")
            for context in root.findall("context")
            if context.findtext("name") == "SettingsDialog"
            for message in context.findall("message")
        }
        assert expected.items() <= settings_messages.items()

        translator = QTranslator()
        assert translator.load(str(translations / f"app_{language}.qm"))
        for source, translated in expected.items():
            assert translator.translate("SettingsDialog", source) == translated
