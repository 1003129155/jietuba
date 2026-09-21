import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QSettings, QTranslator

from settings.tool_settings import SMART_SELECTION_MODES, ToolSettingsManager
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
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, _hotkey: True,
    )
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
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, _hotkey: True,
    )
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


def test_global_hotkey_duplicates_are_marked_and_never_persisted(
    monkeypatch,
    qapp,
    tmp_path,
):
    manager = _manager(tmp_path)
    manager.set_hotkey("ctrl+shift+a")
    manager.set_hotkey_2("ctrl+alt+a")
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, _hotkey: True,
    )
    warnings = []
    monkeypatch.setattr(
        "ui.settings_ui.dialog.show_warning_dialog",
        lambda _parent, title, message: warnings.append((title, message)),
    )
    dialog = SettingsDialog(manager, manager.get_hotkey())

    # 模拟用户把备用键改成与主键相同：两个输入框都应立即显示冲突。
    dialog.hotkey_input_2.setText("ctrl+shift+a")
    assert dialog.hotkey_input.status_lbl.text() == "❌"
    assert dialog.hotkey_input_2.status_lbl.text() == "❌"

    dialog.accept()

    # 保存被拦截；旧配置不受影响，冲突值不会污染下次打开的界面。
    assert manager.get_hotkey() == "ctrl+shift+a"
    assert manager.get_hotkey_2() == "ctrl+alt+a"
    assert warnings

    # 冲突解除后，两项恢复各自的系统可用性结果。
    dialog.hotkey_input_2.setText("ctrl+alt+b")
    assert dialog.hotkey_input.status_lbl.text() == "✅"
    assert dialog.hotkey_input_2.status_lbl.text() == "✅"

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


def test_refresh_settings_repaints_clipboard_theme_button(monkeypatch, qapp, tmp_path):
    """refresh_settings 的导入写在函数里，只有打开设置窗口才执行，改名漏改时启动不报错。"""
    manager = _manager(tmp_path)
    manager.set_clipboard_theme("pink")
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    styles = []
    dialog = SimpleNamespace(
        config_manager=manager,
        _clip_theme_btn=SimpleNamespace(setStyleSheet=styles.append),
        _clip_theme_name="light",
    )

    SettingsDialog.refresh_settings(dialog)

    assert dialog._clip_theme_name == "pink"
    assert styles and "#E91E63" in styles[-1]


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
            "Automatically Wrap Text": "Automatically Wrap Text",
            "Start new text annotations at about 30 characters wide and keep them inside the selection.":
                "Start new text annotations at about 30 characters wide and keep them inside the selection.",
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
            "Automatically Wrap Text": "文字自动换行",
            "Start new text annotations at about 30 characters wide and keep them inside the selection.":
                "新建文字标注默认约 30 个字符宽，并限制在选区内。",
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
            "Automatically Wrap Text": "テキストを自動で折り返す",
            "Start new text annotations at about 30 characters wide and keep them inside the selection.":
                "新しいテキスト注釈は約30文字幅で開始し、選択範囲内に収めます。",
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
            "Automatically Wrap Text": "텍스트 자동 줄 바꿈",
            "Start new text annotations at about 30 characters wide and keep them inside the selection.":
                "새 텍스트 주석은 약 30자 너비로 시작하고 선택 영역 안에 유지됩니다.",
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


def test_clipboard_file_reference_setting_translations_exist_and_load(qapp):
    translations = Path(__file__).parents[1] / "translations"
    expected_by_language = {
        "en": {
            "Write File Path to Clipboard": "Write File Path to Clipboard",
            "Lets tools that only recognize a file path (e.g. some terminal apps) "
            "paste the screenshot too. Requires Auto-save Screenshots to be enabled.":
                "Lets tools that only recognize a file path (e.g. some terminal apps) "
                "paste the screenshot too. Requires Auto-save Screenshots to be enabled.",
        },
        "zh": {
            "Write File Path to Clipboard": "写入文件路径到剪贴板",
            "Lets tools that only recognize a file path (e.g. some terminal apps) "
            "paste the screenshot too. Requires Auto-save Screenshots to be enabled.":
                "让只认文件路径的工具（如部分终端程序）也能粘贴截图。需要开启\"自动保存截图\"。",
        },
        "ja": {
            "Write File Path to Clipboard": "クリップボードにファイルパスを書き込む",
            "Lets tools that only recognize a file path (e.g. some terminal apps) "
            "paste the screenshot too. Requires Auto-save Screenshots to be enabled.":
                "ファイルパスしか認識しないツール（一部のターミナルアプリなど）でもスクリーンショットを"
                "貼り付けられるようになります。「スクリーンショットの自動保存」を有効にする必要があります。",
        },
        "ko": {
            "Write File Path to Clipboard": "클립보드에 파일 경로 쓰기",
            "Lets tools that only recognize a file path (e.g. some terminal apps) "
            "paste the screenshot too. Requires Auto-save Screenshots to be enabled.":
                "파일 경로만 인식하는 도구(일부 터미널 앱 등)에서도 스크린샷을 붙여넣을 수 있게 합니다. "
                "\"스크린샷 자동 저장\"을 켜야 적용됩니다.",
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


@pytest.mark.parametrize("enabled", [True, False])
def test_capture_page_reads_smart_selection_animation_toggle(qapp, tmp_path, enabled):
    manager = _manager(tmp_path)
    manager.set_smart_selection_animation(enabled)
    dialog = SimpleNamespace(
        config_manager=manager,
        tr=lambda text: text,
        _change_save_dir=lambda: None,
        _open_save_dir=lambda: None,
    )

    page = create_capture_page(dialog)

    try:
        assert dialog.smart_animation_toggle.isChecked() is enabled
    finally:
        page.deleteLater()
        qapp.processEvents()


def test_smart_selection_animation_defaults_off(tmp_path):
    assert _manager(tmp_path).get_smart_selection_animation() is False


def test_detection_mode_preserves_legacy_off_and_remembers_element_preference(tmp_path):
    manager = _manager(tmp_path)
    manager.qsettings.setValue("app/smart_selection", False)
    assert manager.get_smart_selection_mode() == "off"
    manager.set_smart_selection_mode("element")
    manager.set_smart_selection_mode("off")
    manager.qsettings.sync()
    reopened = _manager(tmp_path)
    assert reopened.get_smart_selection_mode() == "off"
    # 选回"不检测"不抹掉粒度偏好：再打开还是控件级，不用重新翻设置。
    reopened.set_smart_selection(True)
    assert reopened.get_smart_selection_mode() == "element"
    reopened.reset_app_settings()
    assert reopened.get_smart_selection_mode() == "window"


@pytest.mark.parametrize("language", ["en", "ja", "ko", "zh"])
def test_smart_selection_card_texts_are_translated_in_every_language(qapp, tmp_path, language):
    """文案取自页面上真正显示的控件，改了标题或描述而漏翻译时这里会红。

    按 source 硬编码的对照表拦不住改文案：旧 source 还留在翻译文件里，
    表照样对得上，界面上漏出来的却是英文原文。
    """
    translations = Path(__file__).parents[1] / "translations"
    translator = QTranslator()
    assert translator.load(str(translations / f"app_{language}.qm"))

    dialog = SimpleNamespace(config_manager=_manager(tmp_path), tr=lambda text: text,
                             _change_save_dir=lambda: None, _open_save_dir=lambda: None)
    page = create_capture_page(dialog)
    try:
        combo = dialog.smart_mode_combo
        sources = [label.text()
                   for card in (combo.parent(), dialog.smart_animation_toggle)
                   for label in (card.titleLabel, card.contentLabel)]
        sources += [combo.itemText(i) for i in range(combo.count())]
        assert len(sources) == 7 and all(sources)
        for source in sources:
            assert translator.translate("SettingsDialog", source), (language, source)
    finally:
        page.deleteLater()


@pytest.mark.parametrize("mode", ["window", "element", "off"])
def test_capture_page_loads_detection_mode(qapp, tmp_path, mode):
    manager = _manager(tmp_path)
    manager.set_smart_selection_mode(mode)
    dialog = SimpleNamespace(config_manager=manager, tr=lambda text: text,
                             _change_save_dir=lambda: None, _open_save_dir=lambda: None)
    page = create_capture_page(dialog)
    try:
        combo = dialog.smart_mode_combo
        assert combo.currentData() == mode
        # 三档是全部合法状态，没有"总开关关着还能选下钻"这种组合要遮。
        assert [combo.itemData(i) for i in range(combo.count())] == list(SMART_SELECTION_MODES)
    finally:
        page.deleteLater()


def test_detection_mode_defaults_to_window_only(tmp_path):
    """默认只到窗口：控件检测的代价由提供方决定，不该是默认承担的；
    而总开关维持开启，老用户升级后不会莫名其妙丢掉智能选区。"""
    assert _manager(tmp_path).get_smart_selection_mode() == "window"


@pytest.mark.parametrize("enabled", [True, False])
def test_capture_page_reads_clipboard_file_reference_toggle(qapp, tmp_path, enabled):
    """两个开关各自独立存储：改自动保存不应该连带改到这个子开关的值。"""
    manager = _manager(tmp_path)
    manager.set_clipboard_file_reference_enabled(enabled)
    dialog = SimpleNamespace(config_manager=manager, tr=lambda text: text,
                             _change_save_dir=lambda: None, _open_save_dir=lambda: None)
    page = create_capture_page(dialog)
    try:
        assert dialog.clipboard_file_reference_toggle.isChecked() is enabled
        assert dialog.clipboard_file_reference_toggle.isEnabled()

        dialog.save_toggle.setChecked(not dialog.save_toggle.isChecked())
        assert dialog.clipboard_file_reference_toggle.isChecked() is enabled
        assert dialog.clipboard_file_reference_toggle.isEnabled()
    finally:
        page.deleteLater()


def test_settings_dialog_saves_clipboard_file_reference_toggle(monkeypatch, qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability",
                        lambda _self, _hotkey: True)
    dialog = SettingsDialog(manager)
    try:
        for attr in ("log_toggle", "autostart_toggle", "language_combo", "_ui_theme_combo",
                     "_appearance_theme_color", "_appearance_mask_color", "_inapp_edits"):
            if hasattr(dialog, attr):
                delattr(dialog, attr)

        # 关闭自动保存不影响这个子开关的独立存储值
        dialog.clipboard_file_reference_toggle.setChecked(True)
        dialog.save_toggle.setChecked(False)
        dialog.accept()
        assert manager.get_screenshot_save_enabled() is False
        assert manager.get_clipboard_file_reference_enabled() is True

        dialog.clipboard_file_reference_toggle.setChecked(False)
        dialog.accept()
        assert manager.get_clipboard_file_reference_enabled() is False

        dialog._reset_screenshot_settings_page()
        assert dialog.save_toggle.isChecked() is True
        assert dialog.clipboard_file_reference_toggle.isChecked() is True
    finally:
        dialog.deleteLater()


def test_detection_mode_changes_are_saved_and_reset_in_settings_dialog(monkeypatch, qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability",
                        lambda _self, _hotkey: True)
    dialog = SettingsDialog(manager)
    try:
        for attr in ("log_toggle", "autostart_toggle", "language_combo", "_ui_theme_combo",
                     "_appearance_theme_color", "_appearance_mask_color", "_inapp_edits"):
            if hasattr(dialog, attr):
                delattr(dialog, attr)
        dialog._settings_snapshot = dialog._snapshot_settings()
        combo = dialog.smart_mode_combo
        combo.setCurrentIndex(SMART_SELECTION_MODES.index("element"))
        assert dialog._has_unsaved_changes()
        dialog.accept()
        assert manager.get_smart_selection_mode() == "element"
        combo.setCurrentIndex(SMART_SELECTION_MODES.index("off"))
        dialog.accept()
        assert manager.get_smart_selection_mode() == "off"
        dialog._reset_screenshot_settings_page()
        assert combo.currentData() == "window"
    finally:
        dialog.deleteLater()


def test_settings_dialog_saves_smart_selection_animation_toggle(
    monkeypatch,
    qapp,
    tmp_path,
):
    """改动这个开关要能被"未保存改动"检测到——快照是白名单，漏加就静默失效。"""
    manager = _manager(tmp_path)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, _hotkey: True,
    )
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
    dialog.smart_animation_toggle.setChecked(True)

    assert dialog._has_unsaved_changes()
    dialog.accept()
    assert manager.get_smart_selection_animation() is True

    dialog.deleteLater()
    qapp.processEvents()
