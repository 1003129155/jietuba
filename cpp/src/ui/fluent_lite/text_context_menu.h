#pragma once
#include <QPoint>

class QMenu;
class QWidget;

namespace jietuba {
struct UIThemeTokens;

// The editor's standard menu with translated labels and the app theme.
// editor is a QLineEdit or QTextEdit (including a spin box's line edit).
QMenu *create_text_context_menu(QWidget *editor);
// A menu parented to a styled widget inherits that sheet instead of the global
// QMenu rules and renders black; this gives it the theme explicitly.
void style_popup_menu(QMenu *menu, const UIThemeTokens &tokens);
void show_text_context_menu(QWidget *editor, const QPoint &global_pos);
// For existing editors such as a spin box's line edit.
void install_text_context_menu(QWidget *editor);
}
