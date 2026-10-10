"""设置窗口的整体观感：图标块色相、导航分组、底部按钮与滚动区。"""
import pytest
from PySide6.QtCore import QSettings, Qt
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QScrollArea

from settings.tool_settings import ToolSettingsManager
from ui.fluent_lite import SettingCard
from ui.fluent_lite.navigation import _NavSeparator
from ui.settings_ui.dialog import SettingsDialog

PIN_PAGE, CAPTURE_PAGE, CLIPBOARD_PAGE, TRANSLATION_PAGE = 11, 1, 2, 4


@pytest.fixture
def open_dialog(qapp, tmp_path, monkeypatch):
    manager = ToolSettingsManager(
        qsettings=QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    )
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_a, **_k: None)
    monkeypatch.setattr("core.shortcut_manager.HotkeySystem.check_hotkey_availability", lambda _s, _h: True)
    dialogs = []

    def make():
        dialog = SettingsDialog(manager, manager.get_hotkey())
        dialogs.append(dialog)
        return dialog

    yield make
    for dialog in dialogs:
        dialog._skip_unsaved_close_prompt = True
        dialog.close()
        dialog.deleteLater()
    qapp.processEvents()


@pytest.mark.parametrize(
    ("index", "tone"),
    [(PIN_PAGE, "pin"), (CAPTURE_PAGE, "capture"), (CLIPBOARD_PAGE, "clipboard"), (TRANSLATION_PAGE, "translate")],
)
def test_feature_pages_tint_their_row_icons_with_the_feature_tone(open_dialog, index, tone):
    dialog = open_dialog()
    dialog._ensure_page(index)

    cards = dialog.content_stack.widget(index).findChildren(SettingCard)

    assert cards
    assert {card.iconBadge.tone() for card in cards} == {tone}


def test_navigation_is_split_into_three_groups(open_dialog):
    dialog = open_dialog()

    assert len(dialog.nav_list.findChildren(_NavSeparator)) == 2


def test_disabled_apply_button_keeps_a_visible_check_icon(open_dialog):
    dialog = open_dialog()
    image = dialog._footer_ok_btn.icon().pixmap(16, 16, QIcon.Mode.Disabled).toImage()

    opaque = [
        image.pixelColor(x, y)
        for x in range(image.width()) for y in range(image.height())
        if image.pixelColor(x, y).alpha() > 200
    ]

    # 白色对勾画在置灰的浅底上就看不见了
    assert opaque
    assert all(color.lightness() < 200 for color in opaque)


def test_settings_pages_always_reserve_the_scrollbar_gutter(open_dialog):
    dialog = open_dialog()

    page = dialog.content_stack.widget(0)

    assert isinstance(page, QScrollArea)
    assert page.verticalScrollBarPolicy() == Qt.ScrollBarPolicy.ScrollBarAlwaysOn
