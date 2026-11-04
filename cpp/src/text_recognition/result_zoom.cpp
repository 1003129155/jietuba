#include "text_recognition/result_zoom.h"
#include <QAbstractScrollArea>
#include <QKeyEvent>
#include <QWheelEvent>
#include <algorithm>

namespace jietuba {
ResultZoom::ResultZoom(QAbstractScrollArea *view) : QObject(view) {
    view->installEventFilter(this);
    view->viewport()->installEventFilter(this);
}

void ResultZoom::set_percent(int percent) {
    percent = std::clamp(percent, MIN_ZOOM, MAX_ZOOM);
    if (percent == percent_) return;
    percent_ = percent;
    emit changed();
}

bool ResultZoom::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (!(wheel->modifiers() & Qt::ControlModifier)) return false;
        wheel_delta_ += wheel->angleDelta().y();
        const int notches = wheel_delta_ / QWheelEvent::DefaultDeltasPerStep;
        wheel_delta_ -= notches * QWheelEvent::DefaultDeltasPerStep;
        if (notches) set_percent(percent_ + notches * ZOOM_STEP);
        wheel->accept();
        return true;
    }
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (!(key->modifiers() & Qt::ControlModifier)) return false;
        if (key->matches(QKeySequence::ZoomIn) || key->key() == Qt::Key_Equal || key->key() == Qt::Key_Plus)
            set_percent(percent_ + ZOOM_STEP);
        else if (key->matches(QKeySequence::ZoomOut))
            set_percent(percent_ - ZOOM_STEP);
        else if (key->key() == Qt::Key_0)
            set_percent(100);
        else
            return false;
        key->accept();
        return true;
    }
    return QObject::eventFilter(watched, event);
}
}
