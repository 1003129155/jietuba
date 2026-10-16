"""设置窗口按将要显示的屏幕定尺寸；窗口矮于整列导航时左侧导航滚动。"""

import pytest
from PySide6.QtCore import QRect, QSize

from core.ui_scale import get_dialog_scale
from ui.settings_ui.dialog import SettingsDialog, settings_window_size


@pytest.mark.parametrize("available, factor, expected", [
    ((2560, 1392), 1.0, (1150, 900)),
    ((3840, 2112), 1.0, (1150, 900)),
    ((1920, 1032), 1.0, (1050, 750)),
    # 1080p 开 150% / 1366×768：只有高度缩小，宽度保持快捷键页需要的 1050
    ((1280, 672), 1.0, (1050, 600)),
    ((1366, 720), 1.0, (1050, 620)),
    ((2560, 1392), 1.5, (1575, 1059)),
    ((1920, 1032), 2.0, (1920, 1032)),
    ((1000, 600), 1.0, (1000, 570)),
])
def test_settings_window_size(available, factor, expected):
    assert settings_window_size(*available, factor) == expected


class _Screen:
    def __init__(self, width, height):
        self.area = QRect(0, 0, width, height)

    def availableGeometry(self):
        return self.area


@pytest.fixture
def settings_dialog(qapp, monkeypatch):
    scale = get_dialog_scale()
    before = scale.percent
    scale.set_percent(100)
    current = {"screen": _Screen(1920, 1032)}
    monkeypatch.setattr(SettingsDialog, "_target_screen", lambda self: current["screen"])
    dialog = SettingsDialog()
    try:
        yield dialog, current
    finally:
        dialog.hide()
        dialog.deleteLater()
        scale.set_percent(before)


def test_default_size_follows_the_screen_it_opens_on(settings_dialog):
    dialog, current = settings_dialog
    assert dialog.size() == QSize(1050, 750)

    current["screen"] = _Screen(2560, 1392)
    dialog.fit_to_screen()
    assert dialog.size() == QSize(1150, 900)

    current["screen"] = _Screen(1280, 672)
    dialog.fit_to_screen()
    assert dialog.size() == QSize(1050, 600)


def test_user_size_is_kept_and_only_shrunk_to_fit(settings_dialog):
    dialog, current = settings_dialog
    dialog.resize(1100, 800)

    current["screen"] = _Screen(2560, 1392)
    dialog.fit_to_screen()
    assert dialog.size() == QSize(1100, 800)

    current["screen"] = _Screen(1280, 672)
    dialog.fit_to_screen()
    assert dialog.size() == QSize(1100, 672)


def test_visible_window_is_left_alone(settings_dialog):
    dialog, current = settings_dialog
    dialog.show()
    size = dialog.size()

    current["screen"] = _Screen(1280, 672)
    dialog.fit_to_screen()
    assert dialog.size() == size


def test_navigation_scrolls_instead_of_holding_the_window_tall(qapp, settings_dialog):
    dialog, _current = settings_dialog
    dialog.show()
    qapp.processEvents()
    # 整列导航有六百多像素高；能滚动后窗口高度不再被它撑住
    assert dialog.minimumSizeHint().height() < 400

    dialog.resize(1050, 420)
    qapp.processEvents()
    nav = dialog.nav_list
    bar = nav._scroll.verticalScrollBar()
    assert bar.maximum() > 0
    assert bar.value() == 0

    # 选中被滚出视野的项时滚到它，底部的「关于」不在滚动区里、始终可见
    nav.setCurrentItem("other")
    qapp.processEvents()
    assert bar.value() > 0
    about = nav.widget("about")
    assert not nav._scroll.isAncestorOf(about)
    assert about.isVisible()
