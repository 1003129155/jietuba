from types import SimpleNamespace

import pytest

from PySide6.QtCore import QCoreApplication, QTranslator, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QLabel, QWidget

from core.shortcut_manager import load_inapp_bindings
from settings.tool_settings import ToolSettingsManager
from ui.screenshot_window import ScreenshotShortcutHandler


def _key_event(key, modifiers=Qt.KeyboardModifier.NoModifier, *, auto_repeat=False):
    return QKeyEvent(
        QKeyEvent.Type.KeyPress,
        key,
        modifiers,
        "",
        auto_repeat,
        1,
    )


class _Toolbar:
    def __init__(self):
        self.selected = []

    def select_tool(self, tool_id, *, toggle=False):
        self.selected.append((tool_id, toggle))


class _ActionHandler:
    def handle_confirm(self):
        pass

    def handle_pin(self):
        pass


def _window(*, confirmed=True):
    undo_stack = SimpleNamespace(
        canUndo=lambda: False,
        canRedo=lambda: False,
        undo=lambda: None,
        redo=lambda: None,
    )
    scene = SimpleNamespace(
        selection_model=SimpleNamespace(is_confirmed=confirmed),
        undo_stack=undo_stack,
    )
    return SimpleNamespace(
        scene=scene,
        toolbar=_Toolbar(),
        action_handler=_ActionHandler(),
        view=SimpleNamespace(
            smart_edit_controller=SimpleNamespace(delete_selected=lambda: None),
            invalidate_double_click_candidate=lambda: None,
        ),
        magnifier_overlay=None,
        _is_closing=False,
        isVisible=lambda: True,
        _is_text_editing=lambda: False,
        cleanup_and_close=lambda: None,
    )


@pytest.fixture
def isolated_manager(tmp_settings, monkeypatch):
    manager = ToolSettingsManager(qsettings=tmp_settings)
    monkeypatch.setattr("settings.get_tool_settings_manager", lambda: manager)
    return manager


def test_explicit_empty_binding_is_not_loaded(isolated_manager):
    isolated_manager.set_inapp_shortcut("inapp_tool_text", "")

    assert "inapp_tool_text" not in load_inapp_bindings(["inapp_tool_text"])


def test_escape_is_reserved_for_inapp_bindings():
    from core.shortcut_manager import is_reserved_inapp_shortcut

    assert is_reserved_inapp_shortcut("esc") is True
    assert is_reserved_inapp_shortcut("escape") is True
    assert is_reserved_inapp_shortcut("t") is False


def test_default_text_shortcut_selects_text(monkeypatch, isolated_manager):
    monkeypatch.setattr("core.shortcut_manager.load_move_keys", lambda: {})
    window = _window()
    handler = ScreenshotShortcutHandler(window)

    assert handler.handle_key(_key_event(Qt.Key.Key_T)) is True
    assert window.toolbar.selected == [("text", False)]


@pytest.mark.parametrize(
    ("key", "tool_id"),
    [
        (Qt.Key.Key_S, "cursor"),
        (Qt.Key.Key_P, "pen"),
        (Qt.Key.Key_M, "highlighter"),
        (Qt.Key.Key_X, "mosaic"),
        (Qt.Key.Key_A, "arrow"),
        (Qt.Key.Key_N, "number"),
        (Qt.Key.Key_R, "rect"),
        (Qt.Key.Key_O, "ellipse"),
        (Qt.Key.Key_T, "text"),
        (Qt.Key.Key_E, "eraser"),
    ],
)
def test_all_default_tool_shortcuts(monkeypatch, isolated_manager, key, tool_id):
    monkeypatch.setattr("core.shortcut_manager.load_move_keys", lambda: {})
    window = _window()
    handler = ScreenshotShortcutHandler(window)

    assert handler.handle_key(_key_event(key)) is True
    assert window.toolbar.selected == [(tool_id, False)]


def test_residual_text_editing_skips_tool_matching(monkeypatch, isolated_manager):
    monkeypatch.setattr("core.shortcut_manager.load_move_keys", lambda: {})
    window = _window()
    window._is_text_editing = lambda: True
    handler = ScreenshotShortcutHandler(window)

    assert handler.handle_key(_key_event(Qt.Key.Key_T)) is False
    assert window.toolbar.selected == []


def test_tool_shortcut_requires_confirmed_selection(monkeypatch, isolated_manager):
    monkeypatch.setattr("core.shortcut_manager.load_move_keys", lambda: {})
    window = _window(confirmed=False)
    handler = ScreenshotShortcutHandler(window)

    assert handler.handle_key(_key_event(Qt.Key.Key_T)) is False
    assert window.toolbar.selected == []


def test_tool_shortcut_precedes_wasd_and_repeat_is_consumed(monkeypatch, isolated_manager):
    monkeypatch.setattr(
        "core.shortcut_manager.load_move_keys",
        lambda: {Qt.Key.Key_S: (0, 1)},
    )
    window = _window()
    handler = ScreenshotShortcutHandler(window)

    assert handler.handle_key(_key_event(Qt.Key.Key_S)) is True
    assert window.toolbar.selected == [("cursor", False)]
    assert handler.handle_key(_key_event(Qt.Key.Key_S, auto_repeat=True)) is True
    assert window.toolbar.selected == [("cursor", False)]


def test_exact_modifiers_and_custom_binding(monkeypatch, isolated_manager):
    isolated_manager.set_inapp_shortcut("inapp_tool_text", "shift+k")
    monkeypatch.setattr("core.shortcut_manager.load_move_keys", lambda: {})
    window = _window()
    handler = ScreenshotShortcutHandler(window)

    assert handler.handle_key(_key_event(Qt.Key.Key_K)) is False
    assert handler.handle_key(_key_event(Qt.Key.Key_K, Qt.KeyboardModifier.ShiftModifier)) is True
    assert window.toolbar.selected == [("text", False)]


def test_legacy_escape_binding_is_ignored_at_runtime(monkeypatch, isolated_manager):
    isolated_manager.set_inapp_shortcut("inapp_tool_text", "esc")
    monkeypatch.setattr("core.shortcut_manager.load_move_keys", lambda: {})

    assert "inapp_tool_text" not in load_inapp_bindings(["inapp_tool_text"])


def test_inapp_editor_rejects_escape(qapp):
    from ui.inapp_key_edit import InAppKeyEdit

    editor = InAppKeyEdit()
    editor.setText("t")
    QTest.keyClick(editor, Qt.Key.Key_Escape)
    assert editor.text() == ""


def test_settings_page_exposes_tool_tab_shared_conflict_group_and_empty_value(
    qapp, isolated_manager
):
    from ui.settings_ui.page_hotkey import TOOL_KEYS, create_hotkey_page

    isolated_manager.set_inapp_shortcut("inapp_tool_text", "")
    dialog = SimpleNamespace(
        config_manager=isolated_manager,
        current_hotkey=isolated_manager.get_hotkey(),
        _get_input_style=lambda: "",
        tr=lambda text: text,
    )
    page = create_hotkey_page(dialog)
    try:
        assert len(TOOL_KEYS) == 10
        assert dialog._inapp_groups["inapp_confirm"] == "screenshot"
        assert dialog._inapp_groups["inapp_tool_text"] == "screenshot"
        assert dialog._inapp_groups["inapp_copy_pin"] == "pin"
        assert dialog._inapp_edits["inapp_tool_text"].text() == ""
    finally:
        page.close()


def test_settings_page_uses_chinese_annotation_tool_labels(
    qapp, isolated_manager
):
    from pathlib import Path
    from ui.settings_ui.page_hotkey import create_hotkey_page

    translator = QTranslator()
    translations = Path(__file__).parents[1] / "translations"
    assert translator.load(str(translations / "app_zh.qm"))
    qapp.installTranslator(translator)
    dialog = SimpleNamespace(
        config_manager=isolated_manager,
        current_hotkey=isolated_manager.get_hotkey(),
        _get_input_style=lambda: "",
        tr=lambda text: QCoreApplication.translate("SettingsDialog", text),
    )
    page = create_hotkey_page(dialog)
    try:
        labels = {label.text() for label in page.findChildren(QLabel)}
        assert {
            "选择 / 光标", "画笔", "荧光笔", "马赛克", "箭头",
            "序号", "矩形", "椭圆", "文字", "橡皮擦",
        } <= labels
    finally:
        page.close()
        qapp.removeTranslator(translator)


def test_settings_conflict_cancel_restores_explicit_empty(
    monkeypatch, qapp, isolated_manager
):
    import ui.settings_ui.page_hotkey as page_hotkey

    isolated_manager.set_inapp_shortcut("inapp_tool_text", "")
    dialog = SimpleNamespace(
        config_manager=isolated_manager,
        current_hotkey=isolated_manager.get_hotkey(),
        _get_input_style=lambda: "",
        tr=lambda text: text,
    )
    monkeypatch.setattr(page_hotkey, "show_confirm_dialog", lambda *_args: False)
    page = page_hotkey.create_hotkey_page(dialog)
    try:
        dialog._inapp_edits["inapp_tool_text"].setText("p")
        assert dialog._inapp_edits["inapp_tool_text"].text() == ""
        assert dialog._inapp_edits["inapp_tool_pen"].text() == "p"
    finally:
        page.close()


def test_settings_save_persists_empty_custom_and_rejects_escape(
    qapp, isolated_manager
):
    from ui.inapp_key_edit import InAppKeyEdit
    from ui.settings_ui.dialog import save_inapp_shortcut_edits

    edits = {}
    for key, value in (
        ("inapp_tool_text", ""),
        ("inapp_tool_pen", "shift+k"),
        ("inapp_tool_mosaic", "escape"),
    ):
        edit = InAppKeyEdit()
        edit.setText(value)
        edits[key] = edit

    save_inapp_shortcut_edits(isolated_manager, edits)

    assert isolated_manager.get_inapp_shortcut("inapp_tool_text") == ""
    assert isolated_manager.get_inapp_shortcut("inapp_tool_pen") == "shift+k"
    assert isolated_manager.get_inapp_shortcut("inapp_tool_mosaic") == ""
