#pragma once
#include <QLabel>
#include <QPoint>
#include <QWidget>

namespace jietuba {
// One line of feedback next to the cursor: takes no focus and no mouse input, then
// fades out by itself. Reports results of shortcuts that skip a result window; the
// user goes back to the source application to paste, so focus must stay there.
//
// Shown on construction; after finish() it stays HOLD_MS, then fades and deletes
// itself. Callers waiting for a result construct it first and finish() later;
// one-off messages use show_toast.
class Toast : public QWidget {
    Q_OBJECT
public:
    static constexpr int HOLD_MS = 1500;
    static constexpr int FADE_MS = 200;
    static constexpr int MAX_TEXT_WIDTH = 360;
    explicit Toast(const QString &text);
    // Line breaks collapse to spaces and long text is elided: one line only.
    void set_text(const QString &text);
    void finish();
    void finish(const QString &text);
    QLabel *label;
private:
    void fade_out();
    // Below right of the cursor, flipped above it at the screen's bottom edge.
    void place();
    // Follows the cursor at the moment of appearing, not afterwards.
    QPoint anchor_;
    bool finished_ = false;
};

Toast *show_toast(const QString &text);
}
