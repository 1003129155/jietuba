#include "ui/wait_bar.h"
#include <algorithm>

namespace jietuba {
WaitBar::WaitBar(QWidget *parent, const QSize &size) : QProgressBar(parent) {
    if (!parent) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground);
    } else {
        // Over a window it only tells; clicks go to what is under it.
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    setFixedSize(size);
    setRange(0, 100);
    setTextVisible(false);
    // Round at both ends whatever the height.
    setStyleSheet(QString("QProgressBar { background: rgba(0, 0, 0, 45); border-radius: %1px; border: none; }"
                          "QProgressBar::chunk { background: #2196F3; border-radius: %1px; }")
                      .arg(size.height() / 2));
    show_timer_.setSingleShot(true);
    connect(&show_timer_, &QTimer::timeout, this, [this] {
        show();
        raise();
    });
}

void WaitBar::set_progress(int done, int total) {
    if (total <= 0) {
        if (maximum() != 0) setRange(0, 0);
        return;
    }
    if (maximum() == 0) setRange(0, 100);
    setValue(static_cast<int>(100.0 * std::clamp(done, 0, total) / total));
}

void WaitBar::show_after(int delay_ms) {
    if (isVisible() || show_timer_.isActive()) return;
    show_timer_.start(delay_ms);
}

void WaitBar::dismiss() {
    show_timer_.stop();
    hide();
}

void WaitBar::center_on(const QPoint &point) { move(point.x() - width() / 2, point.y() - height() / 2); }
}
