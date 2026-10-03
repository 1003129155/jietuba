"""长截图保存的文件名按界面语言取，横竖方向也一样。"""

import pytest
from PIL import Image
from PySide6.QtCore import QRect, QTranslator

from core.i18n import I18nManager
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
def test_file_name_follows_the_interface_language(qapp, window, monkeypatch, language, direction, prefix, suffix):
    calls = []
    monkeypatch.setattr(window.save_service, "save_pil_async", lambda image, **kwargs: calls.append(kwargs) or "saved")
    window.stitched_result = Image.new("RGB", (8, 8))
    window.scroll_direction = direction
    # 只临时叠一层这个语言的翻译，不动全局语言设置，免得影响别的测试
    translator = QTranslator()
    assert translator.load(str(I18nManager.get_translations_dir() / f"app_{language}.qm"))
    qapp.installTranslator(translator)
    try:
        window._save_result()
    finally:
        qapp.removeTranslator(translator)
    assert (calls[0]["prefix"], calls[0]["suffix"]) == (prefix, suffix)
