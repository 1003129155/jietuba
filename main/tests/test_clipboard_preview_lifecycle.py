# -*- coding: utf-8 -*-

from types import SimpleNamespace

from PySide6.QtCore import QPoint

from clipboard.ui.widgets.preview_popup import PreviewPopup


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
