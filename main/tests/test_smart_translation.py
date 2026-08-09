from types import SimpleNamespace

from PySide6.QtCore import QPoint, Qt
from PySide6.QtWidgets import QApplication

from core.ui_theme import DARK_TOKENS
import translation.smart_translation_controller as smart_translation_controller_mod
from translation.smart_translation_controller import SmartTranslationController
from translation.translation_dialog import LIGHT
from translation.translation_manager import TranslationManager
from translation.translation_popup import TranslationPopup


class _FakeManager:
    def __init__(self):
        self.compact_calls = []
        self.input_calls = []
        self.full_calls = []

    def translate_compact(self, **kwargs):
        self.compact_calls.append(kwargs)

    def translate(self, **kwargs):
        self.full_calls.append(kwargs)

    def open_compact_input(self, **kwargs):
        self.input_calls.append(kwargs)


class _FakeConfig:
    def get_translation_params(self):
        return {
            "api_key": "test-key",
            "target_lang": "ZH",
            "use_pro": False,
            "split_sentences": "nonewlines",
            "preserve_formatting": True,
        }


def _armed_controller(monkeypatch):
    controller = SmartTranslationController()
    manager = _FakeManager()
    controller._config = _FakeConfig()
    controller._probe_active = True
    controller._copy_dispatched = True
    controller._probe_token = 7
    controller._cursor_position = QPoint(120, 80)
    monkeypatch.setattr(controller, "_translation_manager", lambda: manager)
    return controller, manager


def test_text_clipboard_item_routes_to_compact_popup(monkeypatch):
    controller, manager = _armed_controller(monkeypatch)

    controller.on_clipboard_item(
        SimpleNamespace(content_type="text", content="  selected text  ")
    )

    assert not controller.probe_active
    assert manager.full_calls == []
    assert manager.compact_calls[0]["text"] == "selected text"
    assert manager.compact_calls[0]["position"] == QPoint(120, 80)


def test_non_text_clipboard_item_routes_to_compact_input(monkeypatch):
    controller, manager = _armed_controller(monkeypatch)

    controller.on_clipboard_item(
        SimpleNamespace(content_type="image", content="[100x100]")
    )

    assert not controller.probe_active
    assert manager.compact_calls == []
    assert manager.full_calls == []
    assert manager.input_calls[0]["position"] == QPoint(120, 80)


def test_empty_text_routes_to_compact_input(monkeypatch):
    controller, manager = _armed_controller(monkeypatch)

    controller.on_clipboard_item(SimpleNamespace(content_type="text", content=" \n "))

    assert manager.compact_calls == []
    assert len(manager.input_calls) == 1


def test_timeout_routes_only_current_probe_to_compact_input(monkeypatch):
    controller, manager = _armed_controller(monkeypatch)

    controller._on_timeout(6)
    assert manager.input_calls == []
    assert controller.probe_active

    controller._on_timeout(7)
    assert not controller.probe_active
    assert len(manager.input_calls) == 1


def test_copy_probe_uses_ctrl_insert_to_avoid_console_interrupt(monkeypatch):
    class FakeUser32:
        def __init__(self):
            self.events = []

        def keybd_event(self, virtual_key, scan_code, flags, extra_info):
            self.events.append((virtual_key, scan_code, flags, extra_info))

    user32 = FakeUser32()
    monkeypatch.setattr(
        smart_translation_controller_mod.ctypes,
        "windll",
        SimpleNamespace(user32=user32),
    )

    SmartTranslationController()._send_copy_shortcut()

    assert user32.events == [
        (smart_translation_controller_mod.VK_CONTROL, 0, 0, 0),
        (smart_translation_controller_mod.VK_INSERT, 0, 0, 0),
        (
            smart_translation_controller_mod.VK_INSERT,
            0,
            smart_translation_controller_mod.KEYEVENTF_KEYUP,
            0,
        ),
        (
            smart_translation_controller_mod.VK_CONTROL,
            0,
            smart_translation_controller_mod.KEYEVENTF_KEYUP,
            0,
        ),
    ]


def test_disabled_clipboard_monitor_reads_current_text_once(monkeypatch, qapp):
    controller, manager = _armed_controller(monkeypatch)
    QApplication.clipboard().setText("fallback clipboard text")

    controller._read_clipboard_fallback(7)

    assert not controller.probe_active
    assert manager.compact_calls[0]["text"] == "fallback clipboard text"
    assert manager.input_calls == []


def test_clipboard_event_before_copy_dispatch_is_ignored(monkeypatch):
    controller, manager = _armed_controller(monkeypatch)
    controller._copy_dispatched = False

    controller.on_clipboard_item(
        SimpleNamespace(content_type="text", content="stale clipboard event")
    )

    assert controller.probe_active
    assert manager.compact_calls == []
    assert manager.input_calls == []
    assert manager.full_calls == []


def test_compact_popup_reuses_existing_translation_palette(qapp):
    popup = TranslationPopup()
    full_requests = []
    popup.open_full_requested.connect(lambda *args: full_requests.append(args))
    popup.set_backend_ready(True)
    popup.show_popup("hello", QPoint(10, 10))
    qapp.processEvents()
    assert popup.source_edit.toPlainText() == "hello"
    assert not popup.copy_button.isEnabled()

    popup.show_result("你好", "EN")
    qapp.processEvents()
    assert popup.result_edit.toPlainText() == "你好"
    assert popup.copy_button.isEnabled()

    popup.show_error("network error")
    qapp.processEvents()
    assert popup.result_edit.property("error") is True
    assert not popup.copy_button.isEnabled()
    popup._open_full()
    assert full_requests[-1] == ("hello", "", "network error")

    manual_requests = []
    popup.manual_translate_requested.connect(manual_requests.append)
    popup.show_popup("", QPoint(10, 10), activate=True)
    popup.source_edit.setPlainText("manual text")
    popup._request_manual_translation()
    qapp.processEvents()
    assert not popup.source_edit.isReadOnly()
    assert manual_requests[-1] == "manual text"
    popup.close()


def test_compact_popup_uses_current_application_theme_when_created(qapp):
    manager = TranslationManager()
    manager._ui_theme = SimpleNamespace(is_dark=False)

    popup = manager._ensure_popup()

    assert popup._palette is LIGHT
    manager.close_dialog()


def test_existing_translation_surfaces_follow_application_theme_signal(qapp):
    class FakeSurface:
        def __init__(self):
            self.themes = []

        def isVisible(self):
            return True

        def set_theme(self, theme_name):
            self.themes.append(theme_name)

    manager = TranslationManager()
    dialog = FakeSurface()
    popup = FakeSurface()
    manager._dialog = dialog
    manager._popup = popup

    manager._ui_theme.theme_changed.emit(DARK_TOKENS)

    assert dialog.themes == ["dark"]
    assert popup.themes == ["dark"]
    manager._dialog = None
    manager._popup = None


def test_superseding_translation_never_waits_on_network_thread(qapp):
    class FakeRunningThread:
        def __init__(self):
            self.interrupted = False

        def isRunning(self):
            return True

        def requestInterruption(self):
            self.interrupted = True

        def wait(self, *_args):
            raise AssertionError("GUI path must not wait for a network worker")

    manager = TranslationManager()
    thread = FakeRunningThread()
    manager._thread = thread
    old_token = manager._request_token

    manager._stop_current_thread()

    assert thread.interrupted
    assert manager._thread is None
    assert manager._request_token == old_token + 1


def test_manager_reuses_one_editable_popup_for_every_entry_point(qapp):
    manager = TranslationManager()
    popup = manager._ensure_popup()

    manager.open_compact_input(api_key="", position=QPoint(10, 10))
    assert manager._popup is popup
    assert not popup.source_edit.isReadOnly()
    # Manual entry takes focus so the user can start typing straight away.
    assert not popup.testAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)

    manager.translate_compact(
        text="selected text", api_key="", position=QPoint(10, 10)
    )
    assert manager._ensure_popup() is popup
    assert popup.source_edit.toPlainText() == "selected text"
    # Still editable, but selection translation must never steal focus.
    assert not popup.source_edit.isReadOnly()
    assert popup.testAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
    manager.close_dialog()


def test_prefilled_text_does_not_trigger_the_auto_translate_debounce(qapp):
    """Programmatic fills must not fire a second request behind the caller's."""
    popup = TranslationPopup()
    requests = []
    popup.manual_translate_requested.connect(requests.append)

    popup.show_popup("selected text", QPoint(10, 10))
    qapp.processEvents()

    assert not popup._manual_debounce.isActive()
    assert requests == []

    # A real edit still schedules the automatic re-translation.
    popup.source_edit.setPlainText("edited by user")
    assert popup._manual_debounce.isActive()
    popup.close()


def test_full_request_reactivates_dialog_and_binds_result_target(monkeypatch, qapp):
    class FakeSurface:
        def __init__(self):
            self.visible = True
            self.hidden = False

        def isVisible(self):
            return self.visible

        def hide(self):
            self.hidden = True
            self.visible = False

        def set_translation_error(self, _message):
            pass

    manager = TranslationManager()
    manager._api_key = "test-key"
    manager._dialog = FakeSurface()
    manager._popup = FakeSurface()
    manager._active_target = "compact"
    started = []
    monkeypatch.setattr(manager, "_backend_ready", lambda: True)
    monkeypatch.setattr(manager, "_stop_current_thread", lambda: None)
    monkeypatch.setattr(
        manager,
        "_start_translation",
        lambda *args, **kwargs: started.append((args, kwargs)),
    )

    manager._on_translate_requested("hello", "auto", "ZH")

    assert manager._active_target == "dialog"
    assert manager._popup.hidden
    assert started[0][1]["result_target"] == "dialog"


def test_missing_api_is_rendered_inside_all_translation_surfaces(monkeypatch, qapp):
    manager = TranslationManager()
    error_text = manager._api_key_error()
    manager._api_key = "stale-key"
    # 后端就绪状态必须显式打桩：真实实现会读取本机的翻译 Provider 配置，
    # 只传 api_key="" 仅对 DeepL 这个旧调用路径生效，开发机上若启用了
    # 其他 Provider（google/amazon）就仍是"已配置"，测试会误判。
    monkeypatch.setattr(manager, "_backend_ready", lambda: False)

    manager.open_compact_input(api_key="", position=QPoint(10, 10))
    assert manager._popup.result_edit.toPlainText() == error_text
    assert manager._popup.result_edit.property("error") is True

    manager.translate_compact(
        text="selected text", api_key="", position=QPoint(10, 10)
    )
    assert manager._popup.result_edit.toPlainText() == error_text
    assert manager._popup.result_edit.property("error") is True

    manager.translate(text="", api_key="", position=QPoint(10, 10))
    assert error_text in manager._dialog.target_edit.toPlainText()
    assert manager._dialog.target_edit.property("error") is True
    manager.close_dialog()


def test_shutdown_interrupts_and_joins_translation_workers(monkeypatch, qapp):
    class FakeWorker:
        def __init__(self):
            self.running = True
            self.interrupted = False
            self.wait_timeout = None

        def isRunning(self):
            return self.running

        def requestInterruption(self):
            self.interrupted = True

        def wait(self, timeout):
            self.wait_timeout = timeout
            self.running = False
            return True

    manager = TranslationManager()
    worker = FakeWorker()
    manager._threads.add(worker)
    monkeypatch.setattr(manager, "close_dialog", lambda: None)

    manager.shutdown(timeout_ms=50)

    assert worker.interrupted
    assert worker.wait_timeout is not None
    assert not worker.running
