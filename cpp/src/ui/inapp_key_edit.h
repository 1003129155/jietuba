#pragma once
#include "ui/key_chip.h"

// In-app shortcut editor. Unlike HotkeyEdit it has no system conflict check (no status
// icon), and accepts a bare letter ("C") or Shift plus a letter ("Shift+C").
namespace jietuba {
// Click, then press the combination (or the middle mouse button).
class InAppKeyEdit : public KeyChipLineEdit {
    Q_OBJECT
public:
    // allow_mouse: off when the owning window's handler takes no mouse buttons, since
    // a recorded one would never work. allow_text_keys: off for shortcuts active in text
    // fields, which must not take typing or caret movement keys.
    explicit InAppKeyEdit(QWidget *parent = nullptr, bool allow_mouse = true, bool allow_text_keys = true);
    bool event(QEvent *event) override;
protected:
    // The middle button records a binding; other buttons go to the chip (focus, or × to clear).
    // Only the middle button: side buttons go through the global hook and never reach Qt
    // here; left and right buttons are needed everywhere in the UI.
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *) override {}
private:
    // Keys that type a character or move the caret without Ctrl / Alt.
    static bool is_text_editing_key(QKeyEvent *event);
    static QString key_to_text(int key);
    bool allow_mouse_;
    bool allow_text_keys_;
};
}
