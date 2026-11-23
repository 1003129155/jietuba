#include "ui/busy_ring.h"
#include "core/ui_scale.h"
#include "ui/base_settings_panel.h"
#include <QAbstractButton>
#include <QEvent>
#include <QPainter>
#include <algorithm>

namespace jietuba {
namespace {
// The ring runs this far outside the icon, this thick; the turning arc is a quarter turn long.
constexpr int RING_GAP = 4;
constexpr double RING_WIDTH = 2;
constexpr int ARC_DEGREES = 100;
// A growing arc is never shorter than this, so it shows from the first paragraph on.
constexpr double MIN_PROGRESS_DEGREES = 12;
// Each frame the drawn arc covers this share of what is left to the real progress.
constexpr double CATCH_UP = 0.2;
}

BusyRing::BusyRing(QAbstractButton *button) : QWidget(button), button_(button) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(button->rect());
    button->installEventFilter(this);
    hide();
    delay_.setSingleShot(true);
    connect(&delay_, &QTimer::timeout, this, [this] {
        clock_.start();
        setGeometry(button_->rect());
        show();
        raise();
        frames_.start();
    });
    frames_.setInterval(16);
    connect(&frames_, &QTimer::timeout, this, [this] {
        if (progress_) drawn_ += (*progress_ - drawn_) * CATCH_UP;
        update(circle().toAlignedRect().adjusted(-2, -2, 2, 2));
    });
}

void BusyRing::set_busy(bool busy) {
    if (busy) {
        if (!isVisible() && !delay_.isActive()) delay_.start(SHOW_DELAY_MS);
        return;
    }
    delay_.stop();
    frames_.stop();
    progress_.reset();
    drawn_ = 0;
    hide();
}

void BusyRing::set_progress(int done, int total) {
    if (total <= 0) {
        progress_.reset();
        drawn_ = 0;
        return;
    }
    progress_ = std::clamp(double(done) / total, 0.0, 1.0);
}

QRectF BusyRing::circle() const {
    const double room = std::min(width(), height()) - scaled(RING_WIDTH) - 2;
    const double diameter = std::min(room, button_->iconSize().width() + 2.0 * scaled(RING_GAP));
    return QRectF(rect().center().x() + 0.5 - diameter / 2, rect().center().y() + 0.5 - diameter / 2, diameter, diameter);
}

void BusyRing::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // A shade darker than the theme colour, which also tints the lit button under it.
    const QColor accent = accent_color().darker(140);
    QColor track = accent;
    track.setAlpha(60);
    const double thickness = scaled(RING_WIDTH);
    painter.setPen(QPen(track, thickness));
    painter.drawEllipse(circle());
    painter.setPen(QPen(accent, thickness, Qt::SolidLine, Qt::RoundCap));
    // Qt counts angles in sixteenths of a degree, anticlockwise: a minus turns it clockwise.
    if (progress_) {
        const double span = std::max(MIN_PROGRESS_DEGREES, 360 * drawn_);
        painter.drawArc(circle(), 90 * 16, -static_cast<int>(span * 16));
        return;
    }
    const int start = 90 - static_cast<int>(360 * (clock_.elapsed() % TURN_MS) / TURN_MS);
    painter.drawArc(circle(), start * 16, -ARC_DEGREES * 16);
}

bool BusyRing::eventFilter(QObject *watched, QEvent *event) {
    if (watched == button_ && event->type() == QEvent::Resize) setGeometry(button_->rect());
    return QWidget::eventFilter(watched, event);
}
}
