# -*- coding: utf-8 -*-
"""欢迎向导快捷键页：回填、组内判重、以及冲突值不落盘"""
from PySide6.QtCore import QSettings

from settings.tool_settings import ToolSettingsManager
from ui.welcome.page_hotkeys import HotkeyPage


def _manager(tmp_path):
    settings = QSettings(
        str(tmp_path / "welcome_hotkeys.ini"),
        QSettings.Format.IniFormat,
    )
    return ToolSettingsManager(qsettings=settings)


def _page(tmp_path):
    manager = _manager(tmp_path)
    return manager, HotkeyPage(manager)


def test_six_fields_are_backfilled_from_config(qapp, tmp_path):
    manager = _manager(tmp_path)
    manager.set_hotkey("ctrl+1")
    manager.set_hotkey_2("ctrl+alt+1")
    manager.set_clipboard_hotkey("ctrl+2")
    manager.set_clipboard_hotkey_2("")
    manager.set_translation_hotkey("ctrl+3")
    manager.set_translation_hotkey_2("mouseback")

    page = HotkeyPage(manager)
    try:
        assert [edit.text() for edit in page._edits] == [
            "ctrl+1", "ctrl+alt+1", "ctrl+2", "", "ctrl+3", "mouseback",
        ]
    finally:
        page.close()


def test_duplicate_across_features_marks_both_fields(qapp, tmp_path):
    """截图和剪贴板抢同一个键时，两格都要报错——否则用户不知道该改哪个。"""
    manager, page = _page(tmp_path)
    try:
        screenshot, _, clipboard, _, translation, _ = page._edits
        screenshot.setText("ctrl+shift+a")
        clipboard.setText("SHIFT+CTRL+A")  # 大小写与修饰键顺序都不影响判重

        assert screenshot.validation_error
        assert clipboard.validation_error
        assert translation.validation_error == ""
    finally:
        page.close()


def test_blank_backups_do_not_collide(qapp, tmp_path):
    """三个备用键都留空是常态，不该互相报重复。"""
    manager, page = _page(tmp_path)
    try:
        for edit in page._edits[1::2]:
            edit.setText("")
        assert all(edit.validation_error == "" for edit in page._edits[1::2])
    finally:
        page.close()


def test_mouse_side_buttons_share_the_conflict_domain(qapp, tmp_path):
    manager, page = _page(tmp_path)
    try:
        screenshot, _, clipboard, _, _, _ = page._edits
        screenshot.setText("mouseback")
        clipboard.setText("mouseback")

        assert screenshot.validation_error
        assert clipboard.validation_error
    finally:
        page.close()


def test_save_writes_every_field_when_clean(qapp, tmp_path):
    manager, page = _page(tmp_path)
    try:
        values = ["ctrl+1", "ctrl+alt+1", "ctrl+2", "mouseback", "ctrl+3", "mouseforward"]
        for edit, value in zip(page._edits, values):
            edit.setText(value)
        page.save()

        assert manager.get_hotkey() == "ctrl+1"
        assert manager.get_hotkey_2() == "ctrl+alt+1"
        assert manager.get_clipboard_hotkey() == "ctrl+2"
        assert manager.get_clipboard_hotkey_2() == "mouseback"
        assert manager.get_translation_hotkey() == "ctrl+3"
        assert manager.get_translation_hotkey_2() == "mouseforward"
    finally:
        page.close()


def test_save_keeps_old_value_for_conflicting_fields(qapp, tmp_path):
    """冲突值落盘后注册必定静默失败，所以宁可保留原值。

    「跳过」和关闭按钮同样会调用 save()，因此拦截必须在这里，而不是在
    「下一步」按钮上。
    """
    manager, page = _page(tmp_path)
    try:
        manager.set_hotkey("ctrl+1")
        manager.set_clipboard_hotkey("ctrl+2")

        screenshot, _, clipboard, _, translation, _ = page._edits
        screenshot.setText("ctrl+shift+x")
        clipboard.setText("ctrl+shift+x")
        translation.setText("ctrl+shift+y")  # 这一格没冲突，照常保存
        page.save()

        assert manager.get_hotkey() == "ctrl+1"
        assert manager.get_clipboard_hotkey() == "ctrl+2"
        assert manager.get_translation_hotkey() == "ctrl+shift+y"
    finally:
        page.close()


def test_save_keeps_old_primary_when_left_blank(qapp, tmp_path):
    """主快捷键清空等于没有任何入口，保留原值；备用键留空是正常选择。"""
    manager, page = _page(tmp_path)
    try:
        manager.set_hotkey("ctrl+1")
        manager.set_hotkey_2("ctrl+alt+1")

        screenshot, screenshot_2 = page._edits[0], page._edits[1]
        screenshot.setText("")
        screenshot_2.setText("")
        page.save()

        assert manager.get_hotkey() == "ctrl+1"
        assert manager.get_hotkey_2() == ""
    finally:
        page.close()


def test_retranslate_refreshes_labels_and_conflict_state(qapp, tmp_path):
    manager, page = _page(tmp_path)
    try:
        screenshot, _, clipboard, _, _, _ = page._edits
        screenshot.setText("ctrl+shift+a")
        clipboard.setText("ctrl+shift+a")

        page.retranslate()

        assert page.title_label.text()
        assert page._hint.text()
        # 语言切换不该把已有的冲突标记抹掉
        assert screenshot.validation_error
        assert clipboard.validation_error
    finally:
        page.close()


# ============================================================================
# 左侧键盘示意图
# ============================================================================

class TestKeyboardMap:
    """示意图上「可用 / 不可用」必须和解析器的实际能力一致。

    这张图是手画的，解析器是另一处代码，两边漂移的后果是界面骗人——标成可用
    的键用户按下去却报红叉。这些用例把两者钉在一起。
    """

    MODIFIER_TOKENS = frozenset({"ctrl", "shift", "alt", "win"})

    def _main_keys(self):
        from ui.welcome.page_hotkeys import _KeyboardMap, _label_token
        for row in _KeyboardMap.ROWS:
            for label, _width, bindable in row:
                token = _label_token(label)
                if token not in self.MODIFIER_TOKENS:
                    yield label, token, bindable

    def test_bindable_flags_match_what_the_parser_accepts(self):
        from core.shortcut_manager import ShortcutManager

        for label, token, bindable in self._main_keys():
            try:
                ShortcutManager._parse_hotkey(f"ctrl+{token}")
                accepted = True
            except ValueError:
                accepted = False
            assert accepted is bindable, (label, token)

    def test_modifiers_are_all_marked_bindable(self):
        from ui.welcome.page_hotkeys import _KeyboardMap, _label_token

        seen = set()
        for row in _KeyboardMap.ROWS:
            for label, _width, bindable in row:
                token = _label_token(label)
                if token in self.MODIFIER_TOKENS:
                    seen.add(token)
                    assert bindable, label
        assert seen == self.MODIFIER_TOKENS

    def test_pressed_tokens_splits_recorded_text(self):
        from ui.welcome.page_hotkeys import pressed_tokens

        assert pressed_tokens("Ctrl+Shift+A") == frozenset({"ctrl", "shift", "a"})
        # 组合键输入到一半时录入框写的是 "ctrl+"，末尾空串要滤掉
        assert pressed_tokens("ctrl+") == frozenset({"ctrl"})
        assert pressed_tokens("mouseback") == frozenset({"mouseback"})
        assert pressed_tokens("") == frozenset()
        assert pressed_tokens(None) == frozenset()


class TestKeyboardHighlight:
    """高亮靠 QApplication.focusChanged 驱动，未显示的控件拿不到焦点事件，
    所以这组用例必须先 show()。"""

    def test_highlight_follows_the_focused_field(self, qapp, tmp_path):
        manager, page = _page(tmp_path)
        page.show()
        qapp.processEvents()
        keyboard = page.illus_area.keyboard_map
        try:
            # 显示后焦点落在第一格上，插画直接显示它当前绑的键
            page._edits[0].setText("ctrl+1")
            assert keyboard._pressed == frozenset({"ctrl", "1"})

            page._edits[0].edit.setFocus()
            page._edits[0].setText("ctrl+shift+f5")
            assert keyboard._pressed == frozenset({"ctrl", "shift", "f5"})

            # 切到另一格应当改为显示那一格的内容
            page._edits[2].edit.setFocus()
            page._edits[2].setText("mouseback")
            assert keyboard._pressed == frozenset({"mouseback"})
        finally:
            page.close()

    def test_editing_an_unfocused_field_does_not_steal_the_highlight(self, qapp, tmp_path):
        manager, page = _page(tmp_path)
        page.show()
        qapp.processEvents()
        keyboard = page.illus_area.keyboard_map
        try:
            page._edits[0].edit.setFocus()
            page._edits[0].setText("ctrl+1")
            page._edits[4].setText("ctrl+9")  # 没有焦点
            assert keyboard._pressed == frozenset({"ctrl", "1"})
        finally:
            page.close()
