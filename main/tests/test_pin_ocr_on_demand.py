"""钉图自动 OCR 与按需翻译的边界测试。"""

from types import SimpleNamespace
from unittest.mock import Mock

from PySide6.QtCore import QRectF

from pin.pin_ocr_manager import PinOCRManager
from pin.pin_window import PinWindow
from translation.translation_manager import TranslationManager


class _TextLayer:
    def __init__(self, parent):
        self.parent = parent
        self.geometry = None
        self.enabled = None

    def setGeometry(self, rect):
        self.geometry = rect

    def set_enabled(self, enabled):
        self.enabled = enabled


def _manager(auto_ocr: bool):
    window = SimpleNamespace(content_rect=lambda: QRectF(0, 0, 100, 50))
    config = SimpleNamespace(get_ocr_enabled=lambda: auto_ocr)
    return PinOCRManager(window, config)


def test_automatic_recognition_obeys_the_pin_auto_ocr_setting(monkeypatch):
    manager = _manager(auto_ocr=False)
    initialize = Mock(return_value=True)
    monkeypatch.setattr("ocr.is_ocr_available", lambda: True)
    monkeypatch.setattr("ocr.initialize_ocr", initialize)

    assert manager.init_now() is False
    initialize.assert_not_called()


def test_forced_recognition_ignores_the_pin_auto_ocr_setting(monkeypatch):
    manager = _manager(auto_ocr=False)
    monkeypatch.setattr("ocr.is_ocr_available", lambda: True)
    monkeypatch.setattr("ocr.initialize_ocr", lambda: True)
    monkeypatch.setattr("pin.ocr_text_layer.OCRTextLayer", _TextLayer)
    monkeypatch.setattr(manager, "_start_recognition", Mock(return_value=True))

    assert manager.init_now(force=True) is True
    assert isinstance(manager.ocr_text_layer, _TextLayer)
    manager._start_recognition.assert_called_once_with()


def test_translation_request_marks_pending_and_forces_recognition(monkeypatch):
    manager = _manager(auto_ocr=False)
    start = Mock(return_value=True)
    monkeypatch.setattr(manager, "init_now", start)

    assert manager.recognize_for_translation() is True
    assert manager.translate_pending is True
    start.assert_called_once_with(force=True)


def test_pin_translation_uses_existing_ocr_result():
    layer = object()
    helper = SimpleNamespace(translate=Mock())
    manager = SimpleNamespace(recognize_for_translation=Mock())
    window = SimpleNamespace(
        _translation_helper=helper,
        _ocr_has_result=True,
        ocr_text_layer=layer,
        _ocr_mgr=manager,
    )

    PinWindow._on_translate_clicked(window)

    helper.translate.assert_called_once_with(layer)
    manager.recognize_for_translation.assert_not_called()


def test_pin_translation_shows_dialog_before_scheduling_ocr(monkeypatch):
    events = []
    manager = SimpleNamespace(recognize_for_translation=Mock(return_value=True))
    helper = SimpleNamespace(
        translate=Mock(),
        begin_ocr_translation=Mock(side_effect=lambda: events.append("dialog") or True),
        complete_ocr_translation=Mock(),
    )
    window = SimpleNamespace(
        _translation_helper=helper,
        _ocr_has_result=False,
        ocr_text_layer=None,
        _ocr_mgr=manager,
        _is_closed=False,
    )
    window._start_ocr_for_translation = lambda: PinWindow._start_ocr_for_translation(window)
    scheduled = []
    monkeypatch.setattr(
        "pin.pin_window.QTimer.singleShot",
        lambda delay, callback: scheduled.append((delay, callback)),
    )

    PinWindow._on_translate_clicked(window)

    assert events == ["dialog"]
    manager.recognize_for_translation.assert_not_called()
    assert len(scheduled) == 1 and scheduled[0][0] == 0

    scheduled[0][1]()
    manager.recognize_for_translation.assert_called_once_with()
    helper.translate.assert_not_called()


def test_screenshot_translation_uses_shared_begin_flow_before_its_ocr():
    events = []
    pixmap = object()
    manager = SimpleNamespace(
        begin_ocr_translation=lambda **kwargs: events.append(("dialog", kwargs)),
        _start_ocr_thread=lambda image: events.append(("ocr", image)),
        _pending_pixmap=None,
    )

    TranslationManager.translate_from_image(
        manager, pixmap, target_lang="JA", preserve_formatting=False
    )

    assert events[0] == (
        "dialog",
        {
            "api_key": None,
            "target_lang": "JA",
            "use_pro": None,
            "split_sentences": None,
            "preserve_formatting": False,
        },
    )
    assert events[1] == ("ocr", pixmap)


def test_shared_begin_flow_immediately_sets_recognizing_state():
    source_edit = SimpleNamespace(setPlainText=Mock(), setEnabled=Mock())
    dialog = SimpleNamespace(source_edit=source_edit, tr=lambda text: text)
    manager = SimpleNamespace(
        _resolve_api_key=Mock(),
        _use_pro=False,
        _split_sentences="nonewlines",
        _preserve_formatting=True,
        _stop_current_thread=Mock(),
        _activate_surface=Mock(),
        _ensure_dialog=Mock(),
        _is_dialog_valid=lambda: True,
        _dialog=dialog,
    )

    TranslationManager.begin_ocr_translation(
        manager, target_lang="EN", position=None
    )

    manager._ensure_dialog.assert_called_once_with(
        text="", position=None, source_lang="auto", target_lang="EN"
    )
    source_edit.setPlainText.assert_called_once_with("Recognizing...")
    source_edit.setEnabled.assert_called_once_with(False)
