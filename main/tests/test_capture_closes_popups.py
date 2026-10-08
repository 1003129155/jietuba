"""截图窗口出现前关掉本进程的弹出层，否则截图窗口收不到鼠标移动、智能选区不跟手。"""
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPoint, QRect
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QMenu, QWidget

from core.qt_utils import close_active_popups


class _MoveCounter(QWidget):
    def __init__(self):
        super().__init__()
        self.setMouseTracking(True)
        self.moves = 0

    def mouseMoveEvent(self, event):
        self.moves += 1


@pytest.fixture
def open_menu(qapp):
    created = []

    def popup(pos=QPoint(10, 10)):
        menu = QMenu()
        menu.addAction("item")
        created.append(menu)
        menu.popup(pos)
        qapp.processEvents()
        return menu

    yield popup
    for menu in reversed(created):
        menu.hide()
        menu.deleteLater()
    qapp.processEvents()


def test_closes_every_stacked_popup(open_menu):
    outer = open_menu(QPoint(10, 10))
    inner = open_menu(QPoint(150, 10))
    assert QApplication.activePopupWidget() is inner

    close_active_popups()

    assert QApplication.activePopupWidget() is None
    assert not outer.isVisible()
    assert not inner.isVisible()


def test_open_popup_takes_moves_from_other_windows_until_closed(qapp, open_menu):
    counter = _MoveCounter()
    counter.setGeometry(400, 300, 200, 200)
    counter.show()
    qapp.processEvents()
    try:
        open_menu()
        for x in (40, 60, 80):
            QTest.mouseMove(counter.windowHandle(), QPoint(x, 50))
        assert counter.moves == 0

        close_active_popups()
        for x in (40, 60, 80):
            QTest.mouseMove(counter.windowHandle(), QPoint(x, 50))
        assert counter.moves > 0
    finally:
        counter.hide()
        counter.deleteLater()


def test_capture_ready_closes_popups_before_showing_screenshot(open_menu):
    from main_app import MainApp

    open_menu()
    popups_at_show = []
    app = SimpleNamespace(
        screenshot_window=SimpleNamespace(
            prepare_new_session=lambda *_args: popups_at_show.append(QApplication.activePopupWidget()),
        ),
        quick_capture=SimpleNamespace(set_capture_pending=lambda _pending: None),
        _activate_blocking_modal=lambda: False,
    )

    MainApp._on_capture_ready(app, None, QRect(0, 0, 100, 100))

    assert popups_at_show == [None]
