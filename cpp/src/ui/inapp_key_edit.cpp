#include "ui/inapp_key_edit.h"
#include "core/shortcut_manager.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <set>

namespace jietuba {
namespace {
QStringList modifier_parts(Qt::KeyboardModifiers modifiers) {
    QStringList parts;
    if (modifiers & Qt::ControlModifier) parts.append("Ctrl");
    if (modifiers & Qt::ShiftModifier) parts.append("Shift");
    if (modifiers & Qt::AltModifier) parts.append("Alt");
    return parts;
}

bool is_modifier_key(int key) { return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta; }
}

InAppKeyEdit::InAppKeyEdit(QWidget *parent, bool allow_mouse, bool allow_text_keys) : KeyChipLineEdit(parent), allow_mouse_(allow_mouse), allow_text_keys_(allow_text_keys) {
    // The shortcut manager lets in-app mouse presses through to this editor; otherwise a
    // pin under the pointer would consume the middle button first and it could never be
    // recorded. Keys get the same effect from the text-input check.
    setProperty("_captures_inapp_mouse_shortcut", true);
    setReadOnly(true);
    setFocusPolicy(Qt::StrongFocus);
}

bool InAppKeyEdit::event(QEvent *event) {
    // Tab would move focus and never reach keyPressEvent; it is recorded like any key.
    if (event->type() == QEvent::KeyPress) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        if (key_event->key() == Qt::Key_Tab || key_event->key() == Qt::Key_Backtab) {
            keyPressEvent(key_event);
            return true;
        }
    }
    return KeyChipLineEdit::event(event);
}

void InAppKeyEdit::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::MiddleButton || !allow_mouse_) {
        KeyChipLineEdit::mousePressEvent(event);
        return;
    }
    event->accept();
    setFocus(Qt::MouseFocusReason);
    QStringList parts = modifier_parts(event->modifiers());
    parts.append(MOUSE_BUTTON_MIDDLE);
    // The configuration stores this text, so it must be the parser's lower-case form.
    setText(parts.join("+").toLower());
}

void InAppKeyEdit::keyPressEvent(QKeyEvent *event) {
    const int key = event->key();
    event->accept();

    // Escape is the fixed capture cancel key and cannot be rebound.
    if (key == Qt::Key_Escape) {
        setText("");
        return;
    }
    QStringList parts = modifier_parts(event->modifiers());

    // Only a modifier: the intermediate "ctrl+".
    if (is_modifier_key(key)) {
        if (parts.isEmpty()) return;
        if (!text().endsWith("+")) before_prefix_ = text();
        setText(parts.join("+").toLower() + "+");
        return;
    }
    // Backspace without modifiers clears.
    if (parts.isEmpty() && key == Qt::Key_Backspace) {
        setText("");
        return;
    }
    if (!allow_text_keys_ && is_text_editing_key(event)) return;

    const QString key_text = key_to_text(key);
    if (key_text.isEmpty()) return;
    parts.append(key_text);
    setText(parts.join("+").toLower());
}

void InAppKeyEdit::keyReleaseEvent(QKeyEvent *event) {
    // A modifier released before the combination is complete shows what is still held, or the value before.
    if (is_modifier_key(event->key()) && text().endsWith("+")) {
        const auto parts = modifier_parts(event->modifiers());
        setText(parts.isEmpty() ? before_prefix_ : parts.join("+").toLower() + "+");
    }
    KeyChipLineEdit::keyReleaseEvent(event);
}

void InAppKeyEdit::focusOutEvent(QFocusEvent *event) {
    if (text().endsWith("+")) setText(before_prefix_);
    KeyChipLineEdit::focusOutEvent(event);
}

bool InAppKeyEdit::is_text_editing_key(QKeyEvent *event) {
    if (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) return false;
    static const std::set<int> cursor_keys{Qt::Key_Left, Qt::Key_Right, Qt::Key_Up, Qt::Key_Down, Qt::Key_Home,
                                           Qt::Key_End, Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Delete};
    const QString text = event->text();
    bool printable = !text.isEmpty();
    for (const QChar c : text) printable = printable && c.isPrint();
    return printable || cursor_keys.count(event->key());
}

QString InAppKeyEdit::key_to_text(int key) {
    const auto &display_map = get_key_display_map();
    if (display_map.contains(key)) return display_map.value(key);
    if (Qt::Key_F1 <= key && key <= Qt::Key_F24) return QString("F%1").arg(key - Qt::Key_F1 + 1);
    return QKeySequence(key).toString(QKeySequence::PortableText);
}
}
