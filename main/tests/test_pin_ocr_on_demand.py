"""钉图自动 OCR 与按需识别（翻译 / 复制全部文字）的边界测试。"""

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


def test_pending_request_queues_a_callback_and_forces_recognition(monkeypatch):
    manager = _manager(auto_ocr=False)
    start = Mock(return_value=True)
    monkeypatch.setattr(manager, "init_now", start)

    assert manager.recognize_then(Mock()) is True
    assert manager._pending_callbacks != []
    start.assert_called_once_with(force=True)


def test_a_failed_start_leaves_no_callback_behind(monkeypatch):
    """识别没跑起来，回调就永远不会被调用，留在队列里只会拖住下一次。"""
    manager = _manager(auto_ocr=False)
    monkeypatch.setattr(manager, "init_now", Mock(return_value=False))

    assert manager.recognize_then(Mock()) is False
    assert manager._pending_callbacks == []


def test_a_second_request_joins_the_running_recognition(monkeypatch):
    """翻译和复制文字先后发起时，只能识别一遍，两个回调都要收到结果。"""
    manager = _manager(auto_ocr=False)
    monkeypatch.setattr(manager, "init_now", Mock(return_value=True))
    first, second = Mock(), Mock()

    assert manager.recognize_then(first) is True
    manager.ocr_thread = object()
    assert manager.recognize_then(second) is True
    manager.init_now.assert_called_once_with(force=True)

    manager._flush_pending(True, "文字")

    first.assert_called_once_with(True, "文字")
    second.assert_called_once_with(True, "文字")
    assert manager._pending_callbacks == []


def test_flushing_clears_the_queue_even_when_recognition_found_nothing():
    manager = _manager(auto_ocr=False)
    callback = Mock()
    manager._pending_callbacks.append(callback)

    manager._flush_pending(False, "No text was recognized")

    callback.assert_called_once_with(False, "No text was recognized")
    assert manager._pending_callbacks == []


def test_pin_translation_uses_existing_ocr_result():
    layer = object()
    helper = SimpleNamespace(translate=Mock())
    manager = SimpleNamespace(recognize_then=Mock())
    window = SimpleNamespace(
        _translation_helper=helper,
        _ocr_has_result=True,
        ocr_text_layer=layer,
        _ocr_mgr=manager,
    )

    PinWindow._on_translate_clicked(window)

    helper.translate.assert_called_once_with(layer)
    manager.recognize_then.assert_not_called()


def test_pin_translation_shows_dialog_before_scheduling_ocr(monkeypatch):
    events = []
    manager = SimpleNamespace(recognize_then=Mock(return_value=True))
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
        _on_ocr_translation_finished=Mock(),
    )
    window._start_ocr_for_translation = lambda: PinWindow._start_ocr_for_translation(window)
    scheduled = []
    monkeypatch.setattr(
        "pin.pin_window.QTimer.singleShot",
        lambda delay, callback: scheduled.append((delay, callback)),
    )

    PinWindow._on_translate_clicked(window)

    assert events == ["dialog"]
    manager.recognize_then.assert_not_called()
    assert len(scheduled) == 1 and scheduled[0][0] == 0

    scheduled[0][1]()
    manager.recognize_then.assert_called_once_with(
        window._on_ocr_translation_finished
    )
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


# ── 复制全部文字 ─────────────────────────────────────────────

class _FilledLayer:
    def __init__(self, text="识别到的文字"):
        self._text = text

    def has_text(self):
        return bool(self._text)

    def get_all_text(self, separator="\n"):
        return self._text


def _copy_window(layer, has_result, manager):
    return SimpleNamespace(
        ocr_text_layer=layer,
        _ocr_has_result=has_result,
        _ocr_mgr=manager,
        tr=lambda text: text,
        _show_hint_label=Mock(),
        _copy_ocr_text=Mock(),
    )


def test_copying_all_text_uses_an_existing_ocr_result():
    manager = SimpleNamespace(recognize_then=Mock())
    window = _copy_window(_FilledLayer(), True, manager)

    PinWindow.copy_all_text(window)

    window._copy_ocr_text.assert_called_once_with(True, "识别到的文字")
    manager.recognize_then.assert_not_called()


def test_copying_all_text_recognizes_on_demand_when_there_is_no_result():
    manager = SimpleNamespace(recognize_then=Mock(return_value=True))
    window = _copy_window(None, False, manager)

    PinWindow.copy_all_text(window)

    manager.recognize_then.assert_called_once_with(window._copy_ocr_text)
    window._show_hint_label.assert_not_called()


def test_copying_all_text_reports_when_recognition_cannot_start():
    manager = SimpleNamespace(recognize_then=Mock(return_value=False))
    window = _copy_window(None, False, manager)

    PinWindow.copy_all_text(window)

    window._show_hint_label.assert_called_once()


def _callback_window():
    return SimpleNamespace(tr=lambda text: text, _show_hint_label=Mock())


def _fake_clipboard(monkeypatch):
    clipboard = Mock()
    monkeypatch.setattr("pin.pin_window.QApplication",
                        SimpleNamespace(clipboard=lambda: clipboard))
    return clipboard


def test_copy_callback_puts_the_recognized_text_on_the_clipboard(monkeypatch):
    clipboard = _fake_clipboard(monkeypatch)
    window = _callback_window()

    PinWindow._copy_ocr_text(window, True, "第一行\n第二行")

    clipboard.setText.assert_called_once_with("第一行\n第二行")
    window._show_hint_label.assert_called_once_with("Text copied")


def test_copy_callback_reports_failure_without_touching_the_clipboard(monkeypatch):
    clipboard = _fake_clipboard(monkeypatch)
    window = _callback_window()

    PinWindow._copy_ocr_text(window, False, "No text was recognized")

    clipboard.setText.assert_not_called()
    window._show_hint_label.assert_called_once_with("No text was recognized")


def test_copy_callback_treats_a_blank_result_as_nothing_recognized(monkeypatch):
    """识别成功但只认出空白时不该往剪贴板写空串，否则会把原有内容顶掉。"""
    clipboard = _fake_clipboard(monkeypatch)
    window = _callback_window()

    PinWindow._copy_ocr_text(window, True, "   \n  ")

    clipboard.setText.assert_not_called()
    window._show_hint_label.assert_called_once_with("No text was recognized")


class _FinishedThread:
    """假的 OCR 线程：只提供 _on_finished 会读到的结果字段。"""

    def __init__(self, items):
        self.prepared_items = items
        self.prepared_union_rect = QRectF(0, 0, 10, 10)
        self.deleteLater = Mock()


class _LoadableLayer:
    def __init__(self, text="识别到的文字", fails=False):
        self._text = text
        self._fails = fails

    def isVisible(self):
        """源码取这个属性来判断 C++ 对象还在不在，取不到即视为已销毁。"""
        return True

    def load_prepared_ocr_items(self, *args):
        if self._fails:
            raise RuntimeError("文字层已失效")

    def get_all_text(self, separator="\n"):
        return self._text


def _finished_manager(items, layer):
    window = SimpleNamespace(_is_closed=False, tr=lambda text: text,
                             content_rect=lambda: QRectF(0, 0, 100, 50))
    manager = PinOCRManager(window, SimpleNamespace(get_ocr_enabled=lambda: True))
    manager.ocr_text_layer = layer
    manager.ocr_thread = _FinishedThread(items)
    return manager


def test_queued_callbacks_receive_the_recognized_text():
    manager = _finished_manager(["块"], _LoadableLayer())
    callback = Mock()
    manager._pending_callbacks.append(callback)

    manager._on_finished(100, 50)

    callback.assert_called_once_with(True, "识别到的文字")


def test_queued_callbacks_are_told_when_nothing_was_recognized():
    manager = _finished_manager([], _LoadableLayer())
    callback = Mock()
    manager._pending_callbacks.append(callback)

    manager._on_finished(100, 50)

    callback.assert_called_once_with(False, "No text was recognized")


def test_a_failure_while_loading_still_releases_the_queue():
    """加载环节炸了也要回话，否则等在队列里的复制/翻译会永远没有反馈。"""
    manager = _finished_manager(["块"], _LoadableLayer(fails=True))
    callback = Mock()
    manager._pending_callbacks.append(callback)

    manager._on_finished(100, 50)

    assert callback.call_count == 1
    success, message = callback.call_args[0]
    assert success is False
    assert "文字层已失效" in message


def test_closing_the_pin_drops_the_queued_callbacks():
    manager = _finished_manager(["块"], _LoadableLayer())
    manager.ocr_thread = None
    manager.ocr_text_layer = None
    manager._pending_callbacks.append(Mock())

    manager.cleanup()

    assert manager._pending_callbacks == []
