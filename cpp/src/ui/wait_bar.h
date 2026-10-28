#pragma once
#include <QProgressBar>
#include <QTimer>

namespace jietuba {
// The thin rounded bar shown while something takes a while, without text: a window of its own over a
// spot on the screen when it has no parent, otherwise over part of its parent. Until a total is known
// it runs back and forth.
class WaitBar : public QProgressBar {
    Q_OBJECT
public:
    explicit WaitBar(QWidget *parent = nullptr, const QSize &size = QSize(260, 14));
    // done of total; a total of 0 or less while it is not known.
    void set_progress(int done, int total);
    // Shows it after delay_ms unless dismissed first, so a short wait shows nothing.
    void show_after(int delay_ms);
    // Hides it, and cancels a show still waiting.
    void dismiss();
    // Centred on point: on the screen without a parent, in the parent's coordinates with one.
    void center_on(const QPoint &point);
    bool show_pending() const { return show_timer_.isActive(); }

private:
    QTimer show_timer_;
};
}
