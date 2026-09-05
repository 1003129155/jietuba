# -*- coding: utf-8 -*-
"""
设置对话框状态逻辑测试（ui/settings_ui/dialog.py）

dialog.py 有 861 条语句、覆盖率约 8.7%，是整个项目最大的单文件覆盖率窟窿。
它的 __init__ 会建九个子页面、装 Fluent 标题栏、读 SVG 图标、连主题信号，
真实构造需要 patch 十几个符号，性价比很低。

但它真正容易出错的部分并不需要窗口：把 APP_DEFAULT_SETTINGS 映射回各控件的
一组 _reset_* 方法、按 stack 下标派发的 _reset_current_page、导航状态机
_on_nav_changed、以及"未保存变更"检测所依赖的 _snapshot_settings 快照。
这些逻辑一旦字段名或下标错位就静默失效——恢复默认会漏掉某项、关闭时不再提示
未保存，而任何自动化测试都不会发现。

隔离方式：以未绑定方式调用真实实现，用 SimpleNamespace 充当 self。
刻意不用 MagicMock 当 self，因为源码大量使用 hasattr 分支判断，
而 MagicMock 的任意属性都存在，会让所有分支恒为真、测出假的通过。
"""
from types import SimpleNamespace

from ui.settings_ui.dialog import SettingsDialog


# ============================================================================
# 假控件：只实现被读写到的方法，并记录收到的入参
# ============================================================================

class _TextWidget:
    def __init__(self, text=""):
        self._text = text
        self.set_texts = []

    def text(self):
        return self._text

    def setText(self, value):
        self._text = value
        self.set_texts.append(value)


class _Toggle:
    def __init__(self, checked=False):
        self._checked = checked
        self.set_checked = []

    def isChecked(self):
        return self._checked

    def setChecked(self, value):
        self._checked = value
        self.set_checked.append(value)


class _Combo:
    def __init__(self, index=0, data_map=None):
        self._index = index
        self._data_map = data_map or {}
        self.set_indexes = []
        self.set_texts = []

    def currentIndex(self):
        return self._index

    def setCurrentIndex(self, value):
        self._index = value
        self.set_indexes.append(value)

    def setCurrentText(self, value):
        self.set_texts.append(value)

    def findData(self, value):
        """找不到时返回 -1，与 Qt 的约定一致"""
        return self._data_map.get(value, -1)


class _Spin:
    def __init__(self, value=0):
        self._value = value
        self.set_values = []

    def value(self):
        return self._value

    def setValue(self, value):
        self._value = value
        self.set_values.append(value)


class _Recorder:
    def __init__(self, result=None):
        self.calls = []
        self._result = result

    def __call__(self, *args):
        self.calls.append(args)
        return self._result

    @property
    def called(self):
        return bool(self.calls)


class _Color:
    def __init__(self, name):
        self._name = name

    def name(self):
        return self._name


# 覆盖各 _reset_* 方法会读到的键
DEFAULTS = {
    "hotkey": "ctrl+shift+a",
    "hotkey_2": "ctrl+alt+a",
    "clipboard_hotkey": "ctrl+shift+v",
    "clipboard_hotkey_2": "ctrl+alt+v",
    "translation_hotkey": "ctrl+shift+t",
    "translation_hotkey_2": "",
    "inapp_cursor_move_mode": "both",
    "inapp_confirm": "ctrl+c",
    "inapp_pin": "ctrl+d",
    "smart_selection": True,
    "screenshot_save_enabled": True,
    "screenshot_save_path": r"D:\shots",
    "screenshot_format": "PNG",
    "ocr_enabled": True,
    "ocr_engine": "windos_ocr",
    "log_enabled": True,
    "log_level": "INFO",
    "log_retention_days": 30,
    "log_dir": r"D:\logs",
    "show_main_window": True,
    "pin_auto_toolbar": False,
    "magnifier_color_copy_format": "rgb_hex",
    "clipboard_enabled": True,
    "clipboard_auto_paste": False,
    "clipboard_history_limit": 100,
}


def _config(defaults=None):
    return SimpleNamespace(APP_DEFAULT_SETTINGS=dict(DEFAULTS if defaults is None else defaults))


# ============================================================================
# 快照与未保存检测
# ============================================================================

class TestSnapshotSettings:

    def test_bare_dialog_snapshots_nothing(self):
        assert SettingsDialog._snapshot_settings(SimpleNamespace()) == {}

    def test_text_widgets_are_captured_by_attribute_name(self):
        fake = SimpleNamespace(
            hotkey_input=_TextWidget("ctrl+shift+a"),
            save_path_lbl=_TextWidget(r"D:\shots"),
        )
        snap = SettingsDialog._snapshot_settings(fake)
        assert snap == {"hotkey_input": "ctrl+shift+a", "save_path_lbl": r"D:\shots"}

    def test_each_widget_family_uses_its_own_getter(self):
        fake = SimpleNamespace(
            hotkey_input=_TextWidget("k"),
            smart_toggle=_Toggle(True),
            log_level_combo=_Combo(index=3),
            cooldown_spinbox=_Spin(0.25),
        )
        snap = SettingsDialog._snapshot_settings(fake)
        assert snap["hotkey_input"] == "k"
        assert snap["smart_toggle"] is True
        assert snap["log_level_combo"] == 3
        assert snap["cooldown_spinbox"] == 0.25

    def test_inapp_shortcuts_are_namespaced_to_avoid_collisions(self):
        fake = SimpleNamespace(_inapp_edits={"inapp_undo": _TextWidget("ctrl+z")})
        snap = SettingsDialog._snapshot_settings(fake)
        assert snap == {"inapp_inapp_undo": "ctrl+z"}

    def test_colours_are_captured_by_name(self):
        fake = SimpleNamespace(
            _appearance_theme_color=_Color("#ff0000"),
            _appearance_mask_color=_Color("#00ff00"),
        )
        snap = SettingsDialog._snapshot_settings(fake)
        assert snap == {"theme_color": "#ff0000", "mask_color": "#00ff00"}

    def test_widget_set_to_none_is_skipped(self):
        """源码用 getattr(..., None) 取控件，None 表示该页尚未构建"""
        fake = SimpleNamespace(hotkey_input=None, smart_toggle=None)
        assert SettingsDialog._snapshot_settings(fake) == {}


class TestHasUnsavedChanges:

    def test_without_a_baseline_snapshot_nothing_is_unsaved(self):
        assert SettingsDialog._has_unsaved_changes(SimpleNamespace()) is False

    def test_matching_snapshot_means_no_change(self):
        fake = SimpleNamespace(
            _settings_snapshot={"smart_toggle": True},
            _snapshot_settings=lambda: {"smart_toggle": True},
        )
        assert SettingsDialog._has_unsaved_changes(fake) is False

    def test_any_difference_counts_as_unsaved(self):
        fake = SimpleNamespace(
            _settings_snapshot={"smart_toggle": True},
            _snapshot_settings=lambda: {"smart_toggle": False},
        )
        assert SettingsDialog._has_unsaved_changes(fake) is True

    def test_a_newly_appearing_key_counts_as_unsaved(self):
        """页面延迟构建后多出一个控件，也应视为有变更而不是静默相等"""
        fake = SimpleNamespace(
            _settings_snapshot={},
            _snapshot_settings=lambda: {"smart_toggle": True},
        )
        assert SettingsDialog._has_unsaved_changes(fake) is True


class TestHotkeyAccessors:

    def test_get_hotkey_strips_surrounding_whitespace(self):
        fake = SimpleNamespace(hotkey_input=_TextWidget("  ctrl+shift+a  "))
        assert SettingsDialog.get_hotkey(fake) == "ctrl+shift+a"

    def test_update_hotkey_writes_into_the_input(self):
        widget = _TextWidget("old")
        SettingsDialog.update_hotkey(SimpleNamespace(hotkey_input=widget), "ctrl+alt+x")
        assert widget.set_texts == ["ctrl+alt+x"]


# ============================================================================
# 恢复默认值的下标派发
# ============================================================================

RESET_METHODS = (
    "_reset_hotkey_page",
    "_reset_screenshot_settings_page",
    "_reset_clipboard_page",
    "_reset_appearance_page",
    "_reset_translation_page",
    "_reset_log_page",
    "_reset_misc_page",
    "_reset_long_screenshot_page",
)

# stack 下标 → 应被调用的方法名
INDEX_TO_METHOD = {
    0: "_reset_hotkey_page",
    1: "_reset_screenshot_settings_page",
    2: "_reset_clipboard_page",
    3: "_reset_appearance_page",
    4: "_reset_translation_page",
    5: "_reset_log_page",
    6: "_reset_misc_page",
    7: "_reset_long_screenshot_page",
}


def _dispatch_fake(index):
    fake = SimpleNamespace(content_stack=_Combo(index=index))
    for name in RESET_METHODS:
        setattr(fake, name, _Recorder())
    return fake


class TestResetCurrentPageDispatch:

    def test_each_page_index_resets_exactly_its_own_page(self):
        for index, expected in INDEX_TO_METHOD.items():
            fake = _dispatch_fake(index)
            SettingsDialog._reset_current_page(fake)
            called = [name for name in RESET_METHODS if getattr(fake, name).called]
            assert called == [expected], (index, called)

    def test_about_page_has_nothing_to_reset(self):
        fake = _dispatch_fake(8)
        SettingsDialog._reset_current_page(fake)
        assert not any(getattr(fake, name).called for name in RESET_METHODS)

    def test_unknown_index_resets_nothing(self):
        for index in (-1, 9, 99):
            fake = _dispatch_fake(index)
            SettingsDialog._reset_current_page(fake)
            assert not any(getattr(fake, name).called for name in RESET_METHODS), index


# ============================================================================
# 各页面的默认值映射
# ============================================================================

class TestResetHotkeyPage:

    def test_primary_hotkey_is_always_restored(self):
        widget = _TextWidget("changed")
        fake = SimpleNamespace(config_manager=_config(), hotkey_input=widget)
        SettingsDialog._reset_hotkey_page(fake)
        assert widget.set_texts == ["ctrl+shift+a"]

    def test_every_attached_secondary_input_is_restored(self):
        fake = SimpleNamespace(
            config_manager=_config(),
            hotkey_input=_TextWidget(),
            hotkey_input_2=_TextWidget(),
            clipboard_hotkey_edit=_TextWidget(),
            clipboard_hotkey_edit_2=_TextWidget(),
            translation_hotkey_edit=_TextWidget(),
            translation_hotkey_edit_2=_TextWidget(),
        )
        SettingsDialog._reset_hotkey_page(fake)
        assert fake.hotkey_input_2.set_texts == ["ctrl+alt+a"]
        assert fake.clipboard_hotkey_edit.set_texts == ["ctrl+shift+v"]
        assert fake.clipboard_hotkey_edit_2.set_texts == ["ctrl+alt+v"]
        assert fake.translation_hotkey_edit.set_texts == ["ctrl+shift+t"]
        assert fake.translation_hotkey_edit_2.set_texts == [""]

    def test_inapp_shortcut_without_a_default_falls_back_to_empty(self):
        fake = SimpleNamespace(
            config_manager=_config(),
            hotkey_input=_TextWidget(),
            _inapp_edits={
                "inapp_confirm": _TextWidget(),
                "inapp_unknown_future_key": _TextWidget(),
            },
        )
        SettingsDialog._reset_hotkey_page(fake)
        assert fake._inapp_edits["inapp_confirm"].set_texts == ["ctrl+c"]
        assert fake._inapp_edits["inapp_unknown_future_key"].set_texts == [""]

    def test_cursor_move_mode_is_selected_by_data(self):
        combo = _Combo(data_map={"both": 2})
        fake = SimpleNamespace(
            config_manager=_config(), hotkey_input=_TextWidget(), cursor_move_combo=combo)
        SettingsDialog._reset_hotkey_page(fake)
        assert combo.set_indexes == [2]

    def test_missing_cursor_move_entry_leaves_the_combo_alone(self):
        combo = _Combo(data_map={})
        fake = SimpleNamespace(
            config_manager=_config(), hotkey_input=_TextWidget(), cursor_move_combo=combo)
        SettingsDialog._reset_hotkey_page(fake)
        assert combo.set_indexes == []


class TestResetScreenshotSettingsPage:

    def test_format_name_maps_to_its_combo_index_case_insensitively(self):
        cases = {
            "PNG": 0, "png": 0, "JPG": 1, "jpg": 1,
            "BMP": 2, "WEBP": 3, "webp": 3, "PDF": 4,
        }
        for fmt, expected in cases.items():
            defaults = dict(DEFAULTS, screenshot_format=fmt)
            combo = _Combo()
            fake = SimpleNamespace(
                config_manager=_config(defaults), screenshot_format_combo=combo)
            SettingsDialog._reset_screenshot_settings_page(fake)
            assert combo.set_indexes == [expected], (fmt, combo.set_indexes)

    def test_unknown_format_falls_back_to_the_first_entry(self):
        for fmt in ("TIFF", "", "avif"):
            defaults = dict(DEFAULTS, screenshot_format=fmt)
            combo = _Combo()
            fake = SimpleNamespace(
                config_manager=_config(defaults), screenshot_format_combo=combo)
            SettingsDialog._reset_screenshot_settings_page(fake)
            assert combo.set_indexes == [0], fmt

    def test_toggles_and_path_follow_the_defaults(self):
        fake = SimpleNamespace(
            config_manager=_config(),
            smart_toggle=_Toggle(False),
            save_toggle=_Toggle(False),
            save_path_lbl=_TextWidget(),
            ocr_enable_toggle=_Toggle(False),
        )
        SettingsDialog._reset_screenshot_settings_page(fake)
        assert fake.smart_toggle.set_checked == [True]
        assert fake.save_toggle.set_checked == [True]
        assert fake.save_path_lbl.set_texts == [r"D:\shots"]
        assert fake.ocr_enable_toggle.set_checked == [True]

    def test_unknown_ocr_engine_leaves_the_combo_alone(self):
        combo = _Combo(data_map={})
        fake = SimpleNamespace(config_manager=_config(), ocr_engine_combo=combo)
        SettingsDialog._reset_screenshot_settings_page(fake)
        assert combo.set_indexes == []


class TestResetLogPage:

    def test_retention_is_selected_by_its_configured_value(self):
        combo = _Combo(data_map={30: 4, 7: 1})
        fake = SimpleNamespace(config_manager=_config(), log_retention_combo=combo)
        SettingsDialog._reset_log_page(fake)
        assert combo.set_indexes == [4]

    def test_unavailable_retention_value_falls_back_to_seven_days(self):
        """下拉项里没有配置的天数时退回 7 天，而不是留在原值"""
        combo = _Combo(data_map={7: 1})
        fake = SimpleNamespace(config_manager=_config(), log_retention_combo=combo)
        SettingsDialog._reset_log_page(fake)
        assert combo.set_indexes == [1]

    def test_combo_without_even_the_seven_day_entry_is_left_alone(self):
        combo = _Combo(data_map={})
        fake = SimpleNamespace(config_manager=_config(), log_retention_combo=combo)
        SettingsDialog._reset_log_page(fake)
        assert combo.set_indexes == []

    def test_level_and_directory_follow_the_defaults(self):
        fake = SimpleNamespace(
            config_manager=_config(),
            log_toggle=_Toggle(False),
            log_level_combo=_Combo(),
            path_lbl=_TextWidget(),
        )
        SettingsDialog._reset_log_page(fake)
        assert fake.log_toggle.set_checked == [True]
        assert fake.log_level_combo.set_texts == ["INFO"]
        assert fake.path_lbl.set_texts == [r"D:\logs"]


class TestResetMiscPage:

    def test_autostart_is_always_cleared_regardless_of_defaults(self):
        """
        开机自启动不从默认值恢复，而是硬编码关闭——它对应的是注册表状态，
        恢复默认应当保守地取消自启动。
        """
        for configured in (True, False):
            defaults = dict(DEFAULTS, autostart=configured)
            toggle = _Toggle(True)
            fake = SimpleNamespace(config_manager=_config(defaults), autostart_toggle=toggle)
            SettingsDialog._reset_misc_page(fake)
            assert toggle.set_checked == [False], configured

    def test_window_and_pin_toggles_follow_the_defaults(self):
        fake = SimpleNamespace(
            config_manager=_config(),
            show_main_window_toggle=_Toggle(False),
            pin_auto_toolbar_toggle=_Toggle(True),
        )
        SettingsDialog._reset_misc_page(fake)
        assert fake.show_main_window_toggle.set_checked == [True]
        assert fake.pin_auto_toolbar_toggle.set_checked == [False]

    def test_colour_format_is_selected_by_data(self):
        combo = _Combo(data_map={"rgb_hex": 1})
        fake = SimpleNamespace(
            config_manager=_config(), magnifier_color_format_combo=combo)
        SettingsDialog._reset_misc_page(fake)
        assert combo.set_indexes == [1]


class TestResetClipboardPage:

    def test_all_three_clipboard_controls_are_restored(self):
        fake = SimpleNamespace(
            config_manager=_config(),
            clipboard_enabled_toggle=_Toggle(False),
            clipboard_auto_paste_toggle=_Toggle(True),
            clipboard_history_limit_spin=_Spin(5),
        )
        SettingsDialog._reset_clipboard_page(fake)
        assert fake.clipboard_enabled_toggle.set_checked == [True]
        assert fake.clipboard_auto_paste_toggle.set_checked == [False]
        assert fake.clipboard_history_limit_spin.set_values == [100]

    def test_page_not_yet_built_is_a_no_op(self):
        SettingsDialog._reset_clipboard_page(SimpleNamespace(config_manager=_config()))


# ============================================================================
# 导航状态机
# ============================================================================

NAV_TITLES = {
    0: "Shortcut Settings",
    1: "Capture Settings",
    2: "Clipboard Settings",
    3: "Appearance Settings",
    4: "Translation Settings",
    5: "Log Settings",
    6: "Other Settings",
    8: "Software Information",
}


def _nav_fake():
    return SimpleNamespace(
        tr=lambda text: text,
        content_title=_TextWidget(),
        content_stack=_Combo(),
        _set_current_nav=_Recorder(),
        _refresh_after_page_change=_Recorder(),
    )


class TestOnNavChanged:

    def test_every_known_index_sets_its_title_and_switches_the_stack(self):
        for index, title in NAV_TITLES.items():
            fake = _nav_fake()
            SettingsDialog._on_nav_changed(fake, index)
            assert fake.content_title.set_texts == [title], index
            assert fake.content_stack.set_indexes == [index]
            assert fake._refresh_after_page_change.called

    def test_route_key_highlights_the_navigation_entry(self):
        fake = _nav_fake()
        SettingsDialog._on_nav_changed(fake, 2, "clipboard")
        assert fake._set_current_nav.calls == [("clipboard",)]

    def test_without_a_route_key_the_navigation_is_left_untouched(self):
        fake = _nav_fake()
        SettingsDialog._on_nav_changed(fake, 2)
        assert fake._set_current_nav.calls == []

    def test_developer_page_is_not_reachable_through_navigation(self):
        """下标 7 是隐藏的开发者页，只能由 _open_developer_page 进入"""
        fake = _nav_fake()
        SettingsDialog._on_nav_changed(fake, 7)
        assert fake.content_stack.set_indexes == []
        assert fake.content_title.set_texts == []
        assert not fake._refresh_after_page_change.called

    def test_unknown_index_changes_nothing(self):
        for index in (-1, 9, 99):
            fake = _nav_fake()
            SettingsDialog._on_nav_changed(fake, index)
            assert fake.content_stack.set_indexes == [], index


class TestNavigationHelpers:

    def test_set_current_nav_forwards_the_route_key(self):
        nav = SimpleNamespace(setCurrentItem=_Recorder())
        SettingsDialog._set_current_nav(SimpleNamespace(nav_list=nav), "logs")
        assert nav.setCurrentItem.calls == [("logs",)]

    def test_set_current_nav_tolerates_a_missing_navigation(self):
        SettingsDialog._set_current_nav(SimpleNamespace(), "logs")
        SettingsDialog._set_current_nav(SimpleNamespace(nav_list=None), "logs")

    def test_developer_page_switches_the_stack_and_clears_the_selection(self):
        fake = SimpleNamespace(
            tr=lambda text: text,
            content_stack=_Combo(),
            content_title=_TextWidget(),
            nav_list=SimpleNamespace(clearCurrentItem=_Recorder()),
            _refresh_after_page_change=_Recorder(),
        )
        SettingsDialog._open_developer_page(fake)
        assert fake.content_stack.set_indexes == [7]
        assert fake.content_title.set_texts == ["Developer Options"]
        assert fake.nav_list.clearCurrentItem.called

    def test_opening_the_wizard_only_emits_the_request(self):
        signal = SimpleNamespace(emit=_Recorder())
        SettingsDialog._open_welcome_wizard(SimpleNamespace(wizard_requested=signal))
        assert signal.emit.calls == [()]


# ============================================================================
# 剪贴板体积标签刷新
# ============================================================================

class TestRefreshClipboardSize:

    def test_missing_label_or_calculator_is_a_no_op(self):
        SettingsDialog._refresh_clipboard_size(SimpleNamespace())
        SettingsDialog._refresh_clipboard_size(
            SimpleNamespace(_clipboard_size_label=_TextWidget()))
        SettingsDialog._refresh_clipboard_size(
            SimpleNamespace(_calc_clipboard_storage_size=lambda: "1 KB"))

    def test_size_string_is_written_into_the_label(self):
        label = _TextWidget()
        fake = SimpleNamespace(
            _clipboard_size_label=label,
            _calc_clipboard_storage_size=lambda: "12.5 MB",
        )
        SettingsDialog._refresh_clipboard_size(fake)
        assert label.set_texts == ["12.5 MB"]

    def test_unknown_size_shows_a_dash_instead_of_an_empty_label(self):
        label = _TextWidget()
        fake = SimpleNamespace(
            _clipboard_size_label=label,
            _calc_clipboard_storage_size=lambda: "",
        )
        SettingsDialog._refresh_clipboard_size(fake)
        assert label.set_texts == ["—"]

    def test_a_delay_defers_the_work_to_a_timer(self, monkeypatch):
        scheduled = []

        class _FakeTimer:
            @staticmethod
            def singleShot(msec, callback):
                scheduled.append((msec, callback))

        monkeypatch.setattr("PySide6.QtCore.QTimer", _FakeTimer)
        import PySide6.QtCore as qtcore
        # 打桩必须生效，否则真的会排一个 Qt 定时器进事件循环
        assert qtcore.QTimer is _FakeTimer

        label = _TextWidget()
        fake = SimpleNamespace(
            _clipboard_size_label=label,
            _calc_clipboard_storage_size=lambda: "1 KB",
        )
        # 源码把 self._refresh_clipboard_size 本身作为定时器回调传出去
        fake._refresh_clipboard_size = _Recorder()
        SettingsDialog._refresh_clipboard_size(fake, delay_ms=200)
        assert len(scheduled) == 1
        assert scheduled[0][0] == 200
        assert scheduled[0][1] is fake._refresh_clipboard_size
        # 延时分支只排定，不应立刻写标签
        assert label.set_texts == []
