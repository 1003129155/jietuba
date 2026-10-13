"""长截图的保存：完成和钉图都走截图的统一出口，文件名按界面语言取，横竖方向也一样。"""

from unittest.mock import MagicMock

import pytest
from PIL import Image
from PySide6.QtCore import QRect, QTranslator

from core.i18n import I18nManager
from pin.pin_manager import PinManager
from stitch import scroll_window
from stitch.scroll_window import ScrollCaptureWindow


@pytest.fixture
def window(qapp, monkeypatch):
    monkeypatch.setattr(ScrollCaptureWindow, "_capture_initial_screenshot", lambda self: None)
    win = ScrollCaptureWindow(QRect(100, 100, 300, 200))
    yield win
    if win._stitcher is not None:
        win._cleanup()


@pytest.mark.parametrize("language, direction, prefix, suffix", [
    ("zh", "vertical", "长截图", "竖向"),
    ("zh", "horizontal", "长截图", "横向"),
    ("en", "vertical", "Long Screenshot", "Vertical"),
    ("ja", "horizontal", "長スクショ", "横"),
    ("ko", "vertical", "긴 스크린샷", "세로"),
])
def test_file_name_follows_the_interface_language(qapp, window, language, direction, prefix, suffix):
    window.scroll_direction = direction
    # 只临时叠一层这个语言的翻译，不动全局语言设置，免得影响别的测试
    translator = QTranslator()
    assert translator.load(str(I18nManager.get_translations_dir() / f"app_{language}.qm"))
    qapp.installTranslator(translator)
    try:
        file_prefix = window._file_prefix()
    finally:
        qapp.removeTranslator(translator)
    assert file_prefix == f"{prefix}_{suffix}"


@pytest.mark.parametrize("pin", [False, True])
def test_finish_and_pin_go_through_the_screenshot_exit(qapp, window, monkeypatch, pin):
    delivered = []
    monkeypatch.setattr(scroll_window, "deliver_screenshot",
                        lambda image, config, **kwargs: delivered.append((image.size().toTuple(), config, kwargs)))
    pins = MagicMock()
    monkeypatch.setattr(PinManager, "instance", lambda: pins)
    monkeypatch.setattr(window, "_capture_pending_now", lambda: None)
    monkeypatch.setattr(window, "_export_result", lambda: Image.new("RGB", (30, 500), "white"))
    window.config_manager = MagicMock()
    file_prefix = window._file_prefix()

    window._on_pin() if pin else window._on_finish()

    expected = dict(copy_to_clipboard=False, trigger="pin") if pin else {}
    assert delivered == [((30, 500), window.config_manager, dict(expected, file_prefix=file_prefix))]
    assert pins.create_pin.called is pin
    assert not window.isVisible()
