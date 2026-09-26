"""Clipboard row gestures, with exclusive single/double-click dispatch."""

from PySide6.QtCore import QEvent, QObject, QTimer, Qt
from PySide6.QtGui import QContextMenuEvent
from PySide6.QtWidgets import QApplication

from core.shortcut_manager import mouse_gesture_binding_matches
from settings.tool_settings import CLIPBOARD_MOUSE_ACTIONS, get_clipboard_mouse_binding
from .context_menu_controller import is_quick_editable


class ClipboardMouseController(QObject):
    def __init__(self, window):
        super().__init__(window)
        self.window = window
        self.list_widget = window.list_widget
        self.viewport = self.list_widget.viewport()
        self._pressed = None
        self._pending = None
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.setTimerType(Qt.TimerType.PreciseTimer)
        self._timer.timeout.connect(self._finish_click)
        window.installEventFilter(self)
        self.list_widget.installEventFilter(self)
        self.viewport.installEventFilter(self)
        window.list_widget.verticalScrollBar().valueChanged.connect(self.cancel)
        window.list_widget.model().modelReset.connect(self.cancel)

    def cancel(self, *_args):
        self._timer.stop()
        self._pending = None
        self._pressed = None

    def _available(self, action, item_id):
        item = self.window._get_item_data(item_id)
        if item is None:
            return False
        if action == "pin":
            return item.content_type == "image"
        if action == "quick_edit":
            return is_quick_editable(item)
        return True

    def _match(self, event, gesture, item_id):
        for action, _label, _default, _kind in CLIPBOARD_MOUSE_ACTIONS:
            if (mouse_gesture_binding_matches(
                    get_clipboard_mouse_binding(self.window.config, action), event, gesture)
                    and self._available(action, item_id)):
                return action
        return None

    def _run(self, action, item_id, position):
        if action is None or not self.window.isVisible() or not self._available(action, item_id):
            return
        if action == "paste":
            self.window._on_paste_item(item_id)
        elif action == "pin":
            self.window._create_pin_window(item_id)
        elif action == "quick_edit":
            self.window._quick_edit_item(item_id)
        elif action == "menu":
            self.window._show_item_context_menu(item_id, position)

    def _finish_click(self):
        pending = self._pending
        self.cancel()
        if pending is not None:
            action, item_id, position, _button, _modifiers = pending
            self._run(action, item_id, position)

    def eventFilter(self, obj, event):
        kind = event.type()
        if kind in (QEvent.Type.Hide, QEvent.Type.WindowDeactivate, QEvent.Type.KeyPress, QEvent.Type.Wheel):
            self.cancel()
        # Native mouse context menus must not bypass a remapped right button.
        if kind == QEvent.Type.ContextMenu:
            if event.reason() == QContextMenuEvent.Reason.Mouse:
                return obj in (self.list_widget, self.viewport)
            self.cancel()
        if obj is not self.viewport:
            return False
        if kind == QEvent.Type.MouseMove:
            if self._pressed and (event.globalPosition().toPoint() - self._pressed[3]).manhattanLength() >= QApplication.startDragDistance():
                self.cancel()
            return False
        if kind not in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonRelease, QEvent.Type.MouseButtonDblClick):
            return False
        button = event.button()
        gesture = {Qt.LeftButton: "left", Qt.MiddleButton: "middle", Qt.RightButton: "right"}.get(button)
        if gesture is None:
            return False
        row = self.window.list_widget.itemAt(event.position().toPoint())
        item_id = row.data(Qt.ItemDataRole.UserRole) if row is not None else None
        modifiers = event.modifiers()
        position = event.position().toPoint()
        if kind in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonDblClick):
            pending = self._pending
            self.cancel()
            if item_id is None:
                return True
            self.window.list_widget.setCurrentItem(row)
            self.window.list_widget.setFocus()
            if (kind == QEvent.Type.MouseButtonDblClick and pending is not None
                    and pending[1] == item_id and pending[3:] == (button, modifiers)):
                action = self._match(event, "double" + gesture, item_id)
                if action is not None:
                    self._run(action, item_id, position)
                    return True
            self._pressed = (item_id, button, modifiers, event.globalPosition().toPoint())
        elif self._pressed is not None:
            pressed = self._pressed
            self._pressed = None
            if (pressed[:3] == (item_id, button, modifiers)
                    and (event.globalPosition().toPoint() - pressed[3]).manhattanLength() < QApplication.startDragDistance()):
                action = self._match(event, gesture, item_id)
                if self._match(event, "double" + gesture, item_id) is not None:
                    self._pending = (action, item_id, position, button, modifiers)
                    self._timer.start(QApplication.doubleClickInterval())
                else:
                    self._run(action, item_id, position)
        # Keep QListWidget.itemClicked from also activating the row.
        return True
