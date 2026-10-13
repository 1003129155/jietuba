# -*- coding: utf-8 -*-

from types import SimpleNamespace

from PySide6.QtCore import QPoint, QRect, QSize
from PySide6.QtWidgets import QApplication

from clipboard.ui.widgets.preview_popup import PreviewPopup, side_position


def _text_item(item_id: int):
    return SimpleNamespace(
        id=item_id,
        content_type="text",
        content=f"preview-{item_id}",
        created_at=None,
    )


def test_disabled_preview_rejects_late_show_request(qapp):
    popup = PreviewPopup()
    popup.set_display_enabled(True)
    popup.show_preview(_text_item(1), QPoint(20, 20), delay_ms=0)
    assert popup.isVisible()

    popup.set_display_enabled(False)
    popup.show_preview(_text_item(2), QPoint(20, 20), delay_ms=0)
    qapp.processEvents()

    assert not popup.isVisible()
    assert popup._pending_item is None
    assert not popup._show_timer.isActive()

    popup.set_display_enabled(True)
    popup.show_preview(_text_item(3), QPoint(20, 20), delay_ms=0)
    qapp.processEvents()

    assert popup.isVisible()
    popup.force_cleanup()
    popup.deleteLater()


def test_disabling_preview_cancels_pending_timer(qapp):
    popup = PreviewPopup()
    popup.set_display_enabled(True)
    popup.show_preview(_text_item(1), QPoint(20, 20), delay_ms=50)
    assert popup._show_timer.isActive()

    popup.set_display_enabled(False)
    qapp.processEvents()

    assert not popup.isVisible()
    assert popup._pending_item is None
    assert not popup._show_timer.isActive()
    popup.deleteLater()


def test_hover_preview_near_right_edge_does_not_cover_cursor(qapp):
    screen = QApplication.primaryScreen().availableGeometry()
    size = QSize(300, 200)
    cursor = QPoint(screen.right() - 50, screen.center().y())

    top_left = side_position(size, cursor)

    assert not QRect(top_left, size).contains(cursor)
    assert top_left.x() + size.width() <= cursor.x()


def test_hover_preview_with_room_stays_right_of_cursor(qapp):
    screen = QApplication.primaryScreen().availableGeometry()
    size = QSize(300, 200)
    cursor = QPoint(screen.left() + 50, screen.center().y())

    top_left = side_position(size, cursor)

    assert top_left.x() > cursor.x()
