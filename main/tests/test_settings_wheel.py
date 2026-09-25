from PySide6.QtCore import QPoint, QPointF, QSettings, Qt
from PySide6.QtGui import QWheelEvent
from PySide6.QtWidgets import QApplication, QScrollArea

from settings.tool_settings import ToolSettingsManager
from ui.settings_ui.dialog import SettingsDialog


def _scroll_down(widget):
    center = QPointF(widget.rect().center())
    event = QWheelEvent(
        center, QPointF(widget.mapToGlobal(center.toPoint())),
        QPoint(), QPoint(0, -120),
        Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
        Qt.ScrollPhase.NoScrollPhase, False,
    )
    QApplication.sendEvent(widget, event)


def test_wheel_over_combo_scrolls_page_instead_of_changing_value(monkeypatch, qapp, tmp_path):
    qsettings = QSettings(str(tmp_path / "settings.ini"), QSettings.Format.IniFormat)
    manager = ToolSettingsManager(qsettings=qsettings)
    manager.set_log_dir(str(tmp_path))
    monkeypatch.setattr("ui.settings_ui.dialog.log_info", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(
        "core.shortcut_manager.HotkeySystem.check_hotkey_availability",
        lambda _self, _hotkey: True,
    )
    dialog = SettingsDialog(manager)
    try:
        dialog.resize(900, 420)
        dialog.content_stack.setCurrentIndex(3)
        dialog.show()
        qapp.processEvents()

        page = dialog.content_stack.currentWidget()
        assert isinstance(page, QScrollArea)
        bar = page.verticalScrollBar()
        assert bar.maximum() > 0
        bar.setValue(0)

        combo = dialog._ui_scale_combo
        combo.setCurrentIndex(0)
        _scroll_down(combo)

        assert combo.currentIndex() == 0
        assert bar.value() > 0
    finally:
        dialog.hide()
        dialog.deleteLater()
