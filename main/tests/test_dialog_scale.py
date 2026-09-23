# -*- coding: utf-8 -*-
"""Standalone-window scale uses explicit base metrics, never a post-layout sweep."""

import pytest

from core.ui_scale import dialog_scaled, get_dialog_scale


@pytest.fixture(autouse=True)
def restore_dialog_scale():
    manager = get_dialog_scale()
    before = manager.percent
    before_config = manager._config_manager
    yield
    manager._config_manager = None
    manager.set_percent(before)
    manager._config_manager = before_config


def test_dialog_scale_uses_the_same_safe_pixel_rules_as_toolbar_scale():
    manager = get_dialog_scale()
    manager.set_percent(80)
    assert dialog_scaled(0) == 0
    assert dialog_scaled(1) == 1
    assert dialog_scaled(-1) == -1

    manager.set_percent(150)
    assert dialog_scaled(28) == 42


def test_no_post_layout_dialog_scaler_is_available():
    """Each window must own its metrics so content-driven heights are not double-scaled."""
    import core.ui_scale as ui_scale

    assert not hasattr(ui_scale, "scale_dialog_layouts")


def test_translation_popup_scales_its_own_metrics(qapp):
    from translation.translation_popup import TranslationPopup

    get_dialog_scale().set_percent(150)
    popup = TranslationPopup()
    try:
        margins = popup.layout().contentsMargins()
        assert popup.width() == dialog_scaled(TranslationPopup.WIDTH)
        assert margins.left() == dialog_scaled(12)
        assert margins.top() == dialog_scaled(10)
        assert popup.close_button.size().width() == dialog_scaled(28)
        assert popup.close_button.toolTip() == ""
        assert popup.source_edit.maximumHeight() == dialog_scaled(TranslationPopup.SOURCE_MAX_HEIGHT)
    finally:
        popup.deleteLater()


def test_translation_dashboard_scales_controls_from_their_base_values(qapp):
    from translation.translation_dialog import TranslationDialog
    from ui.fluent_lite import FluentTitleBar

    get_dialog_scale().set_percent(125)
    dialog = TranslationDialog()
    try:
        margins = dialog.layout().contentsMargins()
        assert dialog.minimumWidth() == dialog_scaled(TranslationDialog.MINIMUM_WIDTH)
        assert isinstance(dialog.dashboard_title_bar, FluentTitleBar)
        assert dialog.dashboard_title_bar.height() == dialog_scaled(32)
        assert dialog.dashboard_title_bar.minBtn.size().width() == dialog_scaled(46)
        assert dialog.dashboard_title_bar.minBtn.size().height() == dialog_scaled(32)
        assert dialog.dashboard_title_bar.minBtn.toolTip() == ""
        assert dialog.dashboard_title_bar.maxBtn.toolTip() == ""
        assert dialog.dashboard_title_bar.closeBtn.toolTip() == ""
        assert dialog.source_language.width() == dialog_scaled(150)
        assert dialog.swap_button.size().width() == dialog_scaled(36)
        assert margins.left() == dialog_scaled(18)
        assert margins.top() == dialog_scaled(38)
    finally:
        dialog.deleteLater()


@pytest.mark.parametrize("percent", [80, 100, 125, 150])
def test_translation_caption_buttons_stay_contiguous_and_flush_right(qapp, percent):
    """Caption controls remain a single stable cluster at every UI scale."""
    from translation.translation_dialog import TranslationDialog

    get_dialog_scale().set_percent(percent)
    dialog = TranslationDialog()
    try:
        title_bar = dialog.dashboard_title_bar
        title_bar.resize(dialog.width(), title_bar.height())
        title_bar.layout().activate()

        assert title_bar.layout().contentsMargins().right() == 0
        assert title_bar.maxBtn.geometry().left() == title_bar.minBtn.geometry().right() + 1
        assert title_bar.closeBtn.geometry().left() == title_bar.maxBtn.geometry().right() + 1
        assert title_bar.closeBtn.geometry().right() == title_bar.width() - 1

        for button in (title_bar.minBtn, title_bar.maxBtn, title_bar.closeBtn):
            assert button.width() == dialog_scaled(46)
            assert button.height() == title_bar.height()
    finally:
        dialog.deleteLater()


def test_translation_titlebar_keeps_coloured_child_backgrounds(qapp):
    """The title bar keeps the coloured logo and checked SVG pin visible."""
    from PySide6.QtGui import QColor
    from translation.translation_dialog import LIGHT, TranslationDialog

    get_dialog_scale().set_percent(100)
    dialog = TranslationDialog()
    try:
        dialog.set_theme("light")
        dialog.set_backend_badge("DeepSeek", True)
        dialog.show()
        qapp.processEvents()

        assert dialog.dashboard_title_bar.styleSheet() == (
            "QWidget#fluentTitleBar { background: transparent; }"
        )
        accent = QColor(LIGHT.accent).rgb()
        logo_image = dialog.dashboard_title_bar.logo.grab().toImage()
        logo_accent_pixels = sum(
            logo_image.pixel(x, y) == accent
            for y in range(logo_image.height())
            for x in range(logo_image.width())
        )
        assert logo_accent_pixels > 10

        # The SVG is antialiased onto the title-bar surface, so its sampled
        # edge pixels need not equal the accent byte-for-byte.  It must still
        # contain a clearly blue checked-state mark.
        pin_image = dialog.dashboard_title_bar.pin_button.grab().toImage()
        pin_accent_pixels = sum(
            (color := pin_image.pixelColor(x, y)).blue() > color.red() + 40
            for y in range(pin_image.height())
            for x in range(pin_image.width())
        )
        assert pin_accent_pixels > 10
    finally:
        dialog.hide()
        dialog.deleteLater()


def test_translation_pin_is_an_svg_icon_and_engine_badge_sits_bottom_left(qapp):
    from translation.translation_dialog import PinIconButton, TranslationDialog

    dialog = TranslationDialog()
    try:
        dialog.set_backend_badge("DeepSeek", True)
        dialog.show()
        qapp.processEvents()

        pin = dialog.dashboard_title_bar.pin_button
        assert isinstance(pin, PinIconButton)
        assert pin.text() == ""
        assert pin.icon_path.endswith("钉图.svg")
        assert not hasattr(dialog.dashboard_title_bar, "backend_badge")
        assert dialog.backend_badge.text() == "DeepSeek"
        assert dialog.backend_badge.x() < dialog.translate_button.x()
        assert dialog.backend_badge.y() > dialog.target_pane.geometry().bottom()
    finally:
        dialog.hide()
        dialog.deleteLater()


def test_toolbar_layout_dialog_scales_rows_without_rewriting_the_layout_tree(qapp):
    from PySide6.QtGui import QIcon
    from ui.toolbar_layout import DEFAULT_ORDER, default_layout
    from ui.toolbar_layout_dialog import ToolbarLayoutDialog

    get_dialog_scale().set_percent(150)
    dialog = ToolbarLayoutDialog(default_layout(), {key: QIcon() for key in DEFAULT_ORDER})
    try:
        first_row = next(iter(dialog._rows.values()))
        margins = dialog.layout().contentsMargins()
        assert first_row.grip.size().width() == dialog_scaled(16)
        assert first_row.grip.size().height() == dialog_scaled(28)
        assert margins.left() == dialog_scaled(16)
        assert margins.bottom() == dialog_scaled(14)
    finally:
        dialog.deleteLater()


def test_only_explicitly_opted_fluent_controls_receive_dialog_metrics(qapp):
    from core.ui_scale import configure_dialog_control
    from ui.fluent_lite import ComboBox, PrimaryPushButton

    get_dialog_scale().set_percent(150)
    button = PrimaryPushButton("Apply")
    combo = ComboBox()
    try:
        configure_dialog_control(button)
        configure_dialog_control(combo)
        assert "font: 600 20px" in button.styleSheet()
        assert "min-height: 39px" in combo.styleSheet()
        assert combo.minimumWidth() == dialog_scaled(96)
    finally:
        button.deleteLater()
        combo.deleteLater()


class _DummyClipboardManager:
    """给 ManageDialog 用的最小假管理器，只需要构造期间会被读取的几个属性。"""

    @property
    def is_available(self):
        return True

    def get_groups(self):
        return []


def test_manage_dialog_reads_the_window_scale_at_construction_time(qapp):
    """管理窗口的尺寸来自 layout_scale 里实时求值的函数，不是模块加载时冻结的常量：
    否则用户改了窗口缩放设置后，管理窗口要重启进程才会变。"""
    import clipboard.ui.dialogs.manage_dialog as manage_dialog_mod
    from clipboard.ui.dialogs.manage_dialog import ManageDialog
    from clipboard.ui.layout_scale import (
        manage_dialog_min_height,
        manage_dialog_min_width,
        manage_dialog_width,
    )

    manage_dialog_mod._manage_window_instance = None
    get_dialog_scale().set_percent(80)
    small = ManageDialog(_DummyClipboardManager())
    try:
        assert small.minimumWidth() == manage_dialog_min_width()
        assert small.minimumHeight() == manage_dialog_min_height()
        assert small.width() == manage_dialog_width()
    finally:
        small.hide()
        small.deleteLater()
        manage_dialog_mod._manage_window_instance = None

    get_dialog_scale().set_percent(150)
    large = ManageDialog(_DummyClipboardManager())
    try:
        assert large.minimumWidth() == manage_dialog_min_width()
        assert large.minimumWidth() > small.minimumWidth()
        assert large.width() == manage_dialog_width()
        assert large.width() > small.width()
    finally:
        large.hide()
        large.deleteLater()
        manage_dialog_mod._manage_window_instance = None


def test_manage_dialog_scales_static_and_rebuilt_detail_controls(qapp):
    """The management dialog must not leave its detail form at 100% scale."""
    import clipboard.ui.dialogs.manage_dialog as manage_dialog_mod
    from clipboard.ui.dialogs.manage_dialog import ManageDialog

    manage_dialog_mod._manage_window_instance = None
    get_dialog_scale().set_percent(150)
    dialog = ManageDialog(_DummyClipboardManager())
    try:
        assert dialog.save_btn.property("dialog_scale_factor") == 1.5
        assert "font: 600 20px" in dialog.save_btn.styleSheet()
        assert not hasattr(dialog, "nav_title")
        assert "font-size: 22px" in dialog.list_widget.styleSheet()
        assert "padding: 9px" in dialog.list_widget.styleSheet()

        first_name_input = dialog.group_name_input
        assert first_name_input.property("dialog_scale_factor") == 1.5
        assert "min-height: 39px" in first_name_input.styleSheet()
        assert "font: 20px" in dialog.radio_normal.styleSheet()
        assert "width: 24px" in dialog.radio_normal.styleSheet()

        # Switching/reselecting rebuilds the form from scratch; new controls
        # must receive the marker as well.
        dialog._show_new_group_form()
        assert dialog.group_name_input is not first_name_input
        assert dialog.group_name_input.property("dialog_scale_factor") == 1.5
        assert "min-height: 39px" in dialog.group_name_input.styleSheet()
    finally:
        dialog.hide()
        dialog.deleteLater()
        manage_dialog_mod._manage_window_instance = None


def test_destroy_manage_dialog_clears_the_cached_singleton(monkeypatch):
    """关闭按钮只会隐藏；缩放变化必须显式丢弃管理窗口单例。"""
    import clipboard.ui.dialogs.manage_dialog as manage_dialog_mod

    calls = []
    dialog = type("FakeDialog", (), {
        "isVisible": lambda self: True,
        "hide": lambda self: calls.append("hide"),
        "deleteLater": lambda self: calls.append("deleteLater"),
    })()
    manage_dialog_mod._manage_window_instance = dialog
    monkeypatch.setattr(manage_dialog_mod, "_qt_object_is_valid", lambda obj: obj is dialog)

    assert manage_dialog_mod.destroy_manage_dialog() is True
    assert manage_dialog_mod.get_existing_manage_dialog() is None
    assert calls == ["hide", "deleteLater"]


def test_settings_dialog_scales_its_own_window_and_shared_cards(qapp):
    """设置窗口本身以前不受任何缩放设置影响（resize/setFont 写死），
    现在应跟其它独立窗口一样读取 dialog_scale_percent；卡片尺寸走
    fluent_lite 的 SettingCard，同一个改动应该顺带影响它。"""
    from ui.fluent_lite.cards import SettingCard
    from ui.fluent_lite.icons import FluentIcon
    from ui.settings_ui.dialog import SettingsDialog

    get_dialog_scale().set_percent(80)
    small = SettingsDialog()
    try:
        assert small.width() == dialog_scaled(1050)
        assert small.height() == dialog_scaled(750)
        small_card = SettingCard(FluentIcon.INFO, "t")
        try:
            assert small_card.minimumHeight() == dialog_scaled(62)
        finally:
            small_card.deleteLater()
    finally:
        small.hide()
        small.deleteLater()

    get_dialog_scale().set_percent(150)
    large = SettingsDialog()
    try:
        assert large.width() == dialog_scaled(1050)
        assert large.width() > small.width()
        large_card = SettingCard(FluentIcon.INFO, "t")
        try:
            assert large_card.minimumHeight() == dialog_scaled(62)
            assert large_card.minimumHeight() > small_card.minimumHeight()
        finally:
            large_card.deleteLater()
    finally:
        large.hide()
        large.deleteLater()


@pytest.mark.parametrize("was_visible, expected_opens", [(False, []), (True, [True])])
def test_recreating_settings_preserves_its_previous_visibility(
    was_visible, expected_opens
):
    """语言等外部变化重建设置时，只恢复原本可见的窗口。"""
    from types import SimpleNamespace
    from main_app import MainApp

    calls = []
    window = SimpleNamespace(
        isVisible=lambda: was_visible,
        hide=lambda: calls.append("hide"),
        deleteLater=lambda: calls.append("deleteLater"),
    )
    app = SimpleNamespace(settings_window=window, open_settings=lambda: calls.append("open"))

    class _Preloader:
        def preload_settings(self):
            calls.append("preload")
            app.settings_window = object()

    app._preloader = _Preloader()

    MainApp._recreate_settings_window(app)

    assert calls == ["hide", "deleteLater", "preload"] + [
        "open" for _ in expected_opens
    ]
    assert app.settings_window is not window


@pytest.mark.parametrize("changed, expected_reopens", [(False, []), (True, [True])])
def test_accept_discards_settings_cache_only_when_window_scale_changed(
    changed, expected_reopens
):
    """普通保存保留预加载收益，只有比例真的变化才丢弃缓存。"""
    from types import SimpleNamespace
    from main_app import MainApp

    reopened = []
    accepted_window = SimpleNamespace(_dialog_scale_changed_on_accept=changed)
    app = SimpleNamespace(
        sender=lambda: accepted_window,
        config_manager=SimpleNamespace(get_clipboard_enabled=lambda: True,
                                       get_capture_engine=lambda: "auto"),
        set_clipboard_monitoring_enabled=lambda _enabled: None,
        update_hotkey=lambda **_kwargs: None,
        _recreate_clipboard_manage_dialog=lambda: reopened.append("clipboard"),
        _recreate_settings_window=lambda reopen: reopened.append(reopen),
    )

    MainApp.on_settings_accepted(app)

    expected = ["clipboard", True] if changed else expected_reopens
    assert reopened == expected


def test_recreating_visible_manage_dialog_reuses_manager_and_reconnects(monkeypatch):
    """可见的管理窗口应立即按新比例重建，并恢复与剪贴板窗口的信号连接。"""
    import clipboard
    from types import SimpleNamespace
    from main_app import MainApp

    calls = []
    manager = object()
    old_dialog = SimpleNamespace(manager=manager)
    new_dialog = SimpleNamespace(show_and_activate=lambda: calls.append("show"))
    clipboard_window = SimpleNamespace(
        _connect_manage_dialog=lambda dialog: calls.append(("connect", dialog))
    )

    monkeypatch.setattr(clipboard, "get_existing_manage_dialog", lambda: old_dialog)
    monkeypatch.setattr(clipboard, "destroy_manage_dialog", lambda: True)
    monkeypatch.setattr(
        clipboard,
        "get_manage_dialog",
        lambda received_manager: (
            calls.append(("manager", received_manager)) or new_dialog
        ),
    )

    MainApp._recreate_clipboard_manage_dialog(
        SimpleNamespace(clipboard_window=clipboard_window)
    )

    assert calls == [
        ("manager", manager),
        ("connect", new_dialog),
        "show",
    ]


def test_welcome_wizard_scales_its_own_window_and_shared_widgets(qapp):
    """欢迎向导以前完全不受任何缩放设置影响；现在应跟设置窗口一样接入
    dialog_scale_percent，窗口尺寸和共享控件（步骤条、标签样式）都要跟着变。"""
    from ui.welcome.base_page import _dev_bootstrap
    from ui.welcome.wizard import WelcomeWizard

    mock = _dev_bootstrap()

    get_dialog_scale().set_percent(80)
    small = WelcomeWizard(mock)
    try:
        assert small.width() == dialog_scaled(WelcomeWizard.WINDOW_W)
        assert small.height() == dialog_scaled(WelcomeWizard.WINDOW_H)
        assert small._step_items[0].height() == dialog_scaled(52)
    finally:
        small.hide()
        small.deleteLater()

    get_dialog_scale().set_percent(150)
    large = WelcomeWizard(mock)
    try:
        assert large.width() == dialog_scaled(WelcomeWizard.WINDOW_W)
        assert large.width() > small.width()
        assert large._step_items[0].height() == dialog_scaled(52)
        assert large._step_items[0].height() > small._step_items[0].height()
    finally:
        large.hide()
        large.deleteLater()


# ---------------------------------------------------------------------------
# Metrics and styling set by a caller must survive _apply_theme running again.
# _apply_theme re-derives everything from base values, and it runs on every
# theme change and every configure_dialog_control() pass, so a caller that
# reaches for setIconSize/setFixedSize/setStyleSheet loses its change silently.
# ---------------------------------------------------------------------------


@pytest.fixture
def toggle_app_theme():
    """Drive a real application theme change and restore the previous mode."""
    from core.ui_theme import UIThemeMode, get_ui_theme

    manager = get_ui_theme()
    before = manager.mode

    def toggle():
        target = (
            UIThemeMode.LIGHT
            if manager.effective_mode is UIThemeMode.DARK
            else UIThemeMode.DARK
        )
        manager.set_mode(target, persist=False)

    yield toggle
    manager.set_mode(before, persist=False)


def test_tool_button_keeps_caller_base_metrics(qapp, toggle_app_theme):
    from core.ui_scale import configure_dialog_control, configure_dialog_controls
    from PySide6.QtWidgets import QWidget
    from ui.fluent_lite.buttons import TransparentToolButton

    get_dialog_scale().set_percent(150)
    host = QWidget()
    configure_dialog_control(host)
    button = TransparentToolButton(host)
    configure_dialog_control(button)
    try:
        button.setBaseMetrics(48, 32)
        assert button.width() == dialog_scaled(48)
        assert button.iconSize().width() == dialog_scaled(32)

        toggle_app_theme()
        assert button.width() == dialog_scaled(48)
        assert button.iconSize().width() == dialog_scaled(32)

        configure_dialog_controls(host)
        assert button.width() == dialog_scaled(48)
        assert button.iconSize().width() == dialog_scaled(32)
    finally:
        host.deleteLater()


def test_push_button_keeps_caller_base_icon_size(qapp, toggle_app_theme):
    from core.ui_scale import configure_dialog_control
    from PySide6.QtWidgets import QWidget
    from ui.fluent_lite.buttons import PushButton

    get_dialog_scale().set_percent(150)
    host = QWidget()
    configure_dialog_control(host)
    button = PushButton("ok", host)
    configure_dialog_control(button)
    try:
        button.setBaseIconSize(22)
        assert button.iconSize().width() == dialog_scaled(22)

        toggle_app_theme()
        assert button.iconSize().width() == dialog_scaled(22)
    finally:
        host.deleteLater()


def test_combo_box_minimum_never_exceeds_a_caller_fixed_width(qapp, toggle_app_theme):
    """min > max would make Qt honour the minimum and widen the box."""
    from core.ui_scale import configure_dialog_control
    from PySide6.QtWidgets import QWidget
    from ui.fluent_lite.inputs import ComboBox

    get_dialog_scale().set_percent(150)
    host = QWidget()
    configure_dialog_control(host)
    pinned = ComboBox(host)
    free = ComboBox(host)
    configure_dialog_control(pinned)
    configure_dialog_control(free)
    try:
        pinned.setFixedWidth(dialog_scaled(80))
        toggle_app_theme()
        assert pinned.minimumWidth() == dialog_scaled(80)
        assert pinned.maximumWidth() == dialog_scaled(80)
        # An unconstrained box still gets the scaled default floor.
        assert free.minimumWidth() == dialog_scaled(ComboBox.BASE_MIN_WIDTH)
    finally:
        host.deleteLater()


def test_manage_dialog_content_clears_its_title_bar(qapp):
    """The top margin is derived from titleBar.height(), so the bar has to be
    scaled before _setup_ui() reads it."""
    import clipboard.ui.dialogs.manage_dialog as manage_dialog_mod
    from clipboard.ui.dialogs.manage_dialog import ManageDialog

    manage_dialog_mod._manage_window_instance = None
    get_dialog_scale().set_percent(150)
    dialog = ManageDialog(_DummyClipboardManager())
    try:
        assert dialog.titleBar.height() == dialog_scaled(32)
        assert dialog.layout().contentsMargins().top() >= dialog.titleBar.height()
    finally:
        dialog.hide()
        dialog.deleteLater()
        manage_dialog_mod._manage_window_instance = None


def test_dashboard_caption_colours_survive_an_application_theme_change(
    qapp, toggle_app_theme
):
    """FluentTitleBar._apply_theme is connected to theme_changed, so the
    dashboard's palette has to come from the override, not from a later call."""
    from translation.translation_dialog import DARK, LIGHT, TranslationDialog

    dialog = TranslationDialog()
    try:
        for name, palette in (("dark", DARK), ("light", LIGHT)):
            dialog.set_theme(name)
            bar = dialog.dashboard_title_bar
            assert bar.closeBtn._normalColor.name() == palette.text_2.lower()
            toggle_app_theme()
            assert bar.closeBtn._normalColor.name() == palette.text_2.lower()
    finally:
        dialog.hide()
        dialog.deleteLater()


def test_clipboard_theme_swatches_keep_their_own_styling(qapp):
    """configure_dialog_controls() re-runs PushButton._apply_theme, which
    replaces the stylesheet, so it must run before the swatches are painted."""
    from PySide6.QtWidgets import QMenu
    from ui.settings_ui.dialog import SettingsDialog

    get_dialog_scale().set_percent(150)
    dialog = SettingsDialog()
    try:
        dialog._clip_theme_btn.click()
        menu = dialog.findChild(QMenu)
        assert menu is not None
        swatches = [
            action.defaultWidget()
            for action in menu.actions()
            if hasattr(action, "defaultWidget") and action.defaultWidget() is not None
        ]
        assert len(swatches) > 1
        assert all("qlineargradient" in w.styleSheet() for w in swatches)
        # One swatch is selected, so the styles must not all be identical.
        assert len({w.styleSheet() for w in swatches}) > 1
        menu.close()
    finally:
        dialog.hide()
        dialog.deleteLater()


def test_settings_footer_icons_keep_their_size_across_theme_changes(
    qapp, toggle_app_theme
):
    from ui.settings_ui.dialog import SettingsDialog

    get_dialog_scale().set_percent(150)
    dialog = SettingsDialog()
    try:
        assert dialog._footer_cancel_btn.iconSize().width() == dialog_scaled(22)
        assert dialog._footer_ok_btn.iconSize().width() == dialog_scaled(23)

        toggle_app_theme()
        assert dialog._footer_cancel_btn.iconSize().width() == dialog_scaled(22)
        assert dialog._footer_ok_btn.iconSize().width() == dialog_scaled(23)
    finally:
        dialog.hide()
        dialog.deleteLater()


def test_clipboard_refresh_button_scales_through_the_real_settings_page(
    qapp, toggle_app_theme
):
    """setBaseMetrics runs before the content_stack sweep marks the button, so
    the scaled size only lands if the later _apply_theme re-derives it."""
    from ui.settings_ui.dialog import SettingsDialog

    get_dialog_scale().set_percent(150)
    dialog = SettingsDialog()
    try:
        button = dialog._clipboard_refresh_btn
        assert button.width() == dialog_scaled(48)
        assert button.iconSize().width() == dialog_scaled(32)

        toggle_app_theme()
        assert button.width() == dialog_scaled(48)
        assert button.iconSize().width() == dialog_scaled(32)
    finally:
        dialog.hide()
        dialog.deleteLater()
