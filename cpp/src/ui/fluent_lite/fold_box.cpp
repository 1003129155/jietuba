#include "ui/fluent_lite/fold_box.h"
#include "ui/motion.h"
#include <QCoreApplication>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <algorithm>

namespace jietuba {
namespace {
// A finished animation given new values re-evaluates itself at its end and reports the new end value
// before it has started; nothing listens while it is set up.
void run(QVariantAnimation &animation, const QVariant &from, const QVariant &to, int ms) {
    {
        const QSignalBlocker quiet(&animation);
        animation.setDuration(std::max(1, ms));
        animation.setStartValue(from);
        animation.setEndValue(to);
    }
    animation.start();
}
}

class FoldBox::Veil : public QWidget {
public:
    explicit Veil(FoldBox *box) : QWidget(box), box_(box) { setAttribute(Qt::WA_TransparentForMouseEvents); }
protected:
    void paintEvent(QPaintEvent *) override {
        if (!box_->backdrop_) return;
        QColor color = box_->backdrop_();
        color.setAlphaF(color.alphaF() * box_->cover_);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF r(rect());
        const qreal k = std::min<qreal>(box_->radius_, r.height());
        QPainterPath path;
        path.moveTo(r.topLeft());
        path.lineTo(r.topRight());
        path.lineTo(r.right(), r.bottom() - k);
        path.quadTo(r.bottomRight(), QPointF(r.right() - k, r.bottom()));
        path.lineTo(r.left() + k, r.bottom());
        path.quadTo(r.bottomLeft(), QPointF(r.left(), r.bottom() - k));
        path.closeSubpath();
        painter.fillPath(path, color);
    }
private:
    FoldBox *box_;
};

FoldBox::FoldBox(QWidget *parent) : QFrame(parent), content_(new QWidget(this)), veil_(new Veil(this)) {
    setFrameShape(QFrame::NoFrame);
    content_->installEventFilter(this);
    veil_->hide();
    height_anim_.setEasingCurve(motion::decelerate());
    connect(&height_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { set_shown(value.toInt()); });
    connect(&cover_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { set_cover(value.toReal()); });
    connect(&height_anim_, &QVariantAnimation::finished, this, [this] { next_phase(); });
    connect(&cover_anim_, &QVariantAnimation::finished, this, [this] { next_phase(); });
    setFixedHeight(0);
    hide();
}

void FoldBox::setBackdrop(std::function<QColor()> color, int radius) {
    backdrop_ = std::move(color);
    radius_ = radius;
    veil_->update();
}

int FoldBox::natural_height() const {
    const int width = contentsRect().width();
    return std::max(0, content_->hasHeightForWidth() ? content_->heightForWidth(width) : content_->sizeHint().height());
}

void FoldBox::place() {
    const QRect area = contentsRect();
    content_->setGeometry(area.x(), area.y(), area.width(), natural_height());
    veil_->setGeometry(area.x(), area.y(), area.width(), shown_);
    veil_->raise();
}

bool FoldBox::set_shown(int height) {
    const QMargins frame = contentsMargins();
    const int outer = height + frame.top() + frame.bottom();
    if (height == shown_ && minimumHeight() == outer && maximumHeight() == outer) return false;
    shown_ = height;
    setFixedHeight(outer);
    place();
    // Each step of a fold is laid out through every level before it can be painted: the height climbs one
    // layout per queued request, slower than the steps arrive. Owners sizing themselves on resized()
    // read their layouts once those are up to date, and are then laid out in turn.
    settle();
    emit resized();
    settle();
    return true;
}

void FoldBox::settle() {
    for (QWidget *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        QEvent request(QEvent::LayoutRequest);
        QCoreApplication::sendEvent(ancestor, &request);
        if (ancestor->isWindow()) break;
    }
}

void FoldBox::set_cover(qreal cover) {
    cover_ = cover;
    veil_->setVisible(cover_ > 0);
    veil_->update();
}

void FoldBox::finish() {
    height_anim_.stop();
    cover_anim_.stop();
    phase_ = Phase::Idle;
    set_cover(0);
    const bool was_hidden = isHidden();
    setVisible(open_);
    // Hidden or shown at an unchanged height, the box still leaves or joins its layouts, which are laid
    // out at once like any step of the fold.
    if (!set_shown(open_ ? natural_height() : 0) && was_hidden != isHidden()) {
        settle();
        emit resized();
        settle();
    }
}

void FoldBox::setOpen(bool open, bool animated) {
    if (open == open_ && phase_ == Phase::Idle) {
        refit();
        return;
    }
    open_ = open;
    height_anim_.stop();
    cover_anim_.stop();
    const int grow = motion::duration(motion::MID_MS), fade = motion::duration(motion::FAST_MS);
    const bool on_screen = parentWidget() && parentWidget()->isVisible() && !window()->isMinimized();
    if (!animated || !on_screen || grow <= 0 || fade <= 0) {
        finish();
        return;
    }
    if (open_) {
        phase_ = Phase::Growing;
        // Hidden, it was closed: the content starts covered. Caught while closing, it carries on from where it is.
        if (isHidden()) {
            set_cover(1);
            show();
        }
        const int full = natural_height();
        if (shown_ == full) {
            next_phase();
            return;
        }
        run(height_anim_, shown_, full, grow);
    } else {
        phase_ = Phase::FadingOut;
        run(cover_anim_, cover_, 1.0, qRound(fade * (1 - cover_)));
    }
}

void FoldBox::next_phase() {
    switch (phase_) {
    case Phase::Growing:
        phase_ = Phase::FadingIn;
        run(cover_anim_, cover_, 0.0, qRound(motion::duration(motion::FAST_MS) * cover_));
        break;
    case Phase::FadingOut:
        phase_ = Phase::Shrinking;
        run(height_anim_, shown_, 0, motion::duration(motion::MID_MS));
        break;
    case Phase::FadingIn:
    case Phase::Shrinking:
        finish();
        emit finished();
        break;
    case Phase::Idle: break;
    }
}

void FoldBox::refit() {
    if (!open_ || phase_ != Phase::Idle) return;
    set_shown(natural_height());
}

bool FoldBox::eventFilter(QObject *watched, QEvent *event) {
    if (watched == content_ && event->type() == QEvent::LayoutRequest) refit();
    return QFrame::eventFilter(watched, event);
}

bool FoldBox::event(QEvent *event) {
    const bool result = QFrame::event(event);
    // A style sheet's border arrives with the style and changes how much frame surrounds the content.
    if (event->type() == QEvent::ContentsRectChange) set_shown(shown_);
    return result;
}

void FoldBox::resizeEvent(QResizeEvent *event) {
    QFrame::resizeEvent(event);
    place();
    refit();
}
}
