"""Shared themed and translated context menus for Qt text editors."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QStyleFactory

from core.i18n import make_tr
from core.ui_theme import UIThemeManager

from .theme import ui_tokens


_tr = make_tr("TextEditContextMenu")

_STANDARD_ACTION_LABELS = {
    "edit-undo": "Undo",
    "edit-redo": "Redo",
    "edit-cut": "Cut",
    "edit-copy": "Copy",
    "edit-paste": "Paste",
    "edit-delete": "Delete",
    "select-all": "Select All",
}


def _translate_standard_actions(menu) -> None:
    """Route Qt's built-in editor labels through the app translator."""
    for action in menu.actions():
        source = _STANDARD_ACTION_LABELS.get(action.objectName())
        if source is None:
            continue
        _old_label, separator, shortcut = action.text().partition("\t")
        translated = _tr(source)
        action.setText(
            f"{translated}{separator}{shortcut}"
            if separator
            else translated
        )


def create_text_context_menu(editor):
    """Create a standard editor menu using the active app theme and language."""
    menu = editor.createStandardContextMenu()
    tokens = ui_tokens(editor)
    _translate_standard_actions(menu)

    # Standard editor menus are transient top-level windows. On Windows they
    # may otherwise retain the OS dark popup renderer while the app is light.
    fusion_style = QStyleFactory.create("Fusion")
    if fusion_style is not None:
        fusion_style.setParent(menu)
        menu.setStyle(fusion_style)
    menu.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
    menu.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
    menu.setAutoFillBackground(True)
    menu.setPalette(UIThemeManager.build_palette(tokens))
    menu.setStyleSheet(f"""
        QMenu {{
            color: {tokens.text};
            background-color: {tokens.popup_background};
            border: 1px solid {tokens.border_hover};
            border-radius: 6px;
            padding: 4px;
        }}
        QMenu::item {{
            color: {tokens.text};
            background-color: transparent;
            padding: 6px 28px 6px 26px;
            margin: 1px 0;
        }}
        QMenu::item:selected {{
            color: {tokens.text};
            background-color: {tokens.popup_hover};
        }}
        QMenu::item:disabled {{
            color: {tokens.text_disabled};
            background-color: transparent;
        }}
        QMenu::separator {{
            height: 1px;
            background-color: {tokens.separator};
            margin: 4px 7px;
        }}
    """)
    return menu


def show_text_context_menu(editor, global_pos) -> None:
    menu = create_text_context_menu(editor)
    menu.exec(global_pos)
    menu.deleteLater()


def install_text_context_menu(editor) -> None:
    """Install the shared menu on an existing editor, such as a spin-box edit."""
    editor.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
    editor.customContextMenuRequested.connect(
        lambda pos, target=editor: show_text_context_menu(
            target, target.mapToGlobal(pos)
        )
    )


class TextContextMenuMixin:
    """Mixin for QLineEdit/QTextEdit subclasses with the shared context menu."""

    def createThemedContextMenu(self):
        return create_text_context_menu(self)

    def contextMenuEvent(self, event):
        show_text_context_menu(self, event.globalPos())


__all__ = [
    "TextContextMenuMixin",
    "create_text_context_menu",
    "install_text_context_menu",
    "show_text_context_menu",
]
