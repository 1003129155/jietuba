"""新钉图的描边由「自动添加描边」决定，单张钉图仍可在右键菜单里切换；悬停按钮默认关闭。"""

import pytest
from PySide6.QtCore import QPoint
from PySide6.QtGui import QImage

from pin.pin_ocr_manager import PinOCRManager
from pin.pin_window import PinWindow
from settings.tool_settings import ToolSettingsManager


def test_new_pins_get_a_border_and_no_hover_buttons_by_default(tmp_settings):
    config = ToolSettingsManager(tmp_settings)
    assert config.get_app_setting("pin_auto_border") is True
    assert config.get_app_setting("pin_hover_buttons") is False


@pytest.mark.parametrize("enabled", [True, False])
def test_new_pin_border_follows_the_setting_and_still_toggles(qapp, tmp_settings, monkeypatch, enabled):
    monkeypatch.setattr(PinOCRManager, "init_now", lambda self, force=False: False)
    config = ToolSettingsManager(tmp_settings)
    config.set_app_setting("pin_auto_border", enabled)
    image = QImage(200, 120, QImage.Format.Format_RGB32)
    image.fill(0xFF336699)
    pin = PinWindow(image, QPoint(100, 100), config)
    try:
        assert pin.border_enabled is enabled
        assert (pin.border_overlay is not None) is enabled

        pin.toggle_border_effect()
        assert pin.border_enabled is not enabled
        assert (pin.border_overlay is not None and not pin.border_overlay.isHidden()) is not enabled
    finally:
        pin.close_window()
