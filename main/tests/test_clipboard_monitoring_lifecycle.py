# -*- coding: utf-8 -*-
"""Tests for the app-level clipboard watcher lifecycle."""

import sys
from types import SimpleNamespace
from unittest.mock import MagicMock

from main_app import MainApp


class _FakeClipboardManager:
    def __init__(self, available=True, monitoring=False):
        self.is_available = available
        self._monitoring = monitoring
        self.start_calls = []
        self.stop_calls = 0

    def is_monitoring(self):
        return self._monitoring

    def start_monitoring(self, callback):
        self.start_calls.append(callback)
        self._monitoring = True

    def stop_monitoring(self):
        self.stop_calls += 1
        self._monitoring = False


def _app_with_manager(manager):
    signal = SimpleNamespace(emit=MagicMock())
    return SimpleNamespace(
        clipboard_manager=manager,
        clipboard_item_received=signal,
    )


def test_disabling_stops_an_active_watcher_once():
    manager = _FakeClipboardManager(monitoring=True)
    app = _app_with_manager(manager)

    assert MainApp.set_clipboard_monitoring_enabled(app, False)
    assert manager.stop_calls == 1
    assert not manager.is_monitoring()

    assert MainApp.set_clipboard_monitoring_enabled(app, False)
    assert manager.stop_calls == 1


def test_enabling_creates_manager_and_starts_watcher(monkeypatch):
    manager = _FakeClipboardManager()
    app = _app_with_manager(None)
    monkeypatch.setitem(
        sys.modules,
        "clipboard",
        SimpleNamespace(ClipboardManager=lambda: manager),
    )

    assert MainApp.set_clipboard_monitoring_enabled(app, True)
    assert app.clipboard_manager is manager
    assert len(manager.start_calls) == 1

    manager.start_calls[0]("new clipboard item")
    app.clipboard_item_received.emit.assert_called_once_with("new clipboard item")

    assert MainApp.set_clipboard_monitoring_enabled(app, True)
    assert len(manager.start_calls) == 1


def test_enabling_does_not_start_an_unavailable_manager():
    manager = _FakeClipboardManager(available=False)
    app = _app_with_manager(manager)

    assert not MainApp.set_clipboard_monitoring_enabled(app, True)
    assert manager.start_calls == []
