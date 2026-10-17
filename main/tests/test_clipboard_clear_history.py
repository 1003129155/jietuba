# -*- coding: utf-8 -*-
"""剪贴板窗口里清理历史：确认后只清未置顶、未分组的条目，且不做 VACUUM。"""

from unittest.mock import MagicMock

import pytest

from clipboard.controllers import clipboard_controller as controller_module
from clipboard.controllers.clipboard_controller import ClipboardController
from clipboard.ui.panels import setting_panel
from clipboard.ui.windows.clipboard_window import ClipboardWindow


class _Manager:
    def __init__(self, succeeds=True):
        self.succeeds = succeeds
        self.vacuum_args = []

    def clear_unpinned_ungrouped(self, vacuum=True):
        self.vacuum_args.append(vacuum)
        return self.succeeds


@pytest.fixture
def make_controller(monkeypatch):
    monkeypatch.setattr(ClipboardController, "_load_settings", lambda self: None)

    def build(manager, confirmed=True):
        monkeypatch.setattr(controller_module, "show_confirm_dialog", lambda *args: confirmed)
        ctrl = ClipboardController(manager)
        ctrl.reloads = []
        monkeypatch.setattr(ctrl, "load_history", lambda: ctrl.reloads.append(1))
        return ctrl

    return build


class TestClearHistory:
    def test_clearing_skips_vacuum_and_reloads_the_list(self, make_controller):
        manager = _Manager()
        controller = make_controller(manager)

        assert controller.clear_history(parent_widget=object()) is True

        assert manager.vacuum_args == [False]
        assert controller.reloads == [1]

    def test_declining_the_confirmation_clears_nothing(self, make_controller):
        manager = _Manager()
        controller = make_controller(manager, confirmed=False)

        assert controller.clear_history(parent_widget=object()) is False

        assert manager.vacuum_args == []
        assert controller.reloads == []

    def test_a_failed_clear_leaves_the_list_alone(self, make_controller):
        manager = _Manager(succeeds=False)
        controller = make_controller(manager)

        assert controller.clear_history(parent_widget=object()) is False

        assert controller.reloads == []


class TestWindowMenu:
    def test_settings_menu_entry_reaches_the_controller(self, monkeypatch):
        offered = {}

        def fake_menu(parent, **kwargs):
            offered.update(kwargs)
            return MagicMock()

        monkeypatch.setattr(setting_panel, "show_setting_menu", fake_menu)
        window = MagicMock()
        window._settings_menu = None
        window._menu_close_time = float("-inf")

        ClipboardWindow._show_main_menu(window)
        assert offered["on_clear_history"] is window._on_clear_clicked

        ClipboardWindow._on_clear_clicked(window)
        window.controller.clear_history.assert_called_once_with(parent_widget=window)
