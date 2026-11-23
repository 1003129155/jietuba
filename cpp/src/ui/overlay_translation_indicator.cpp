#include "ui/overlay_translation_indicator.h"
#include <QEvent>
#include <QGraphicsView>

namespace jietuba {
OverlayTranslationIndicator::OverlayTranslationIndicator(QGraphicsView *view) : TranslationSweep(view->viewport()), view_(view) {
    setGeometry(view->viewport()->rect());
    view->viewport()->installEventFilter(this);
}

void OverlayTranslationIndicator::set_waiting(const OverlayTranslationWait &waiting) {
    if (waiting == waiting_) return;
    if (active()) update(area());
    waiting_ = waiting;
    if (waiting_.empty()) {
        end();
        return;
    }
    begin();
    if (active()) update(area());
}

QRect OverlayTranslationIndicator::area() const { return view_ ? view_->viewportTransform().mapRect(waiting_.rect).toAlignedRect() : QRect(); }

bool OverlayTranslationIndicator::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    return TranslationSweep::eventFilter(watched, event);
}
}
