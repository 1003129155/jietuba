#include "ui/screen_color_picker.h"
#include "capture/capture_service.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "settings/tool_settings.h"
#include "ui/magnifier.h"
#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QTimer>
#include <QWheelEvent>
#include <stdexcept>
#include <tuple>

namespace jietuba {
namespace {
ScreenColorPicker::CaptureFunction &capture_override() {
    static ScreenColorPicker::CaptureFunction capture;
    return capture;
}
}

void ScreenColorPicker::set_capture_for_test(CaptureFunction capture) { capture_override() = std::move(capture); }

ScreenColorPicker::ScreenColorPicker() : QWidget(nullptr) {
    // The capture window's kind: frameless and on top, painted opaque from the frozen image.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::StrongFocus);
    magnifier = new MagnifierOverlay(this, nullptr, nullptr, get_tool_settings_manager());
}

bool ScreenColorPicker::start() {
    try {
        std::tie(image_, rect_) = capture_override() ? capture_override()() : CaptureService().capture_all_screens();
    } catch (const std::exception &e) {
        log_exception(e, "ScreenColorPicker");
        return false;
    }
    if (image_.isNull() || rect_.isEmpty()) return false;
    magnifier->set_sample_image(image_, rect_);
    // Qt's scaling is off, so window coordinates are the capture's physical pixels.
    setGeometry(rect_.toAlignedRect());
    show();
    log_debug(QString("DBG shown geo=%1,%2 %3x%4").arg(x()).arg(y()).arg(width()).arg(height()), "ScreenColorPicker");
    take_focus();
    track(QCursor::pos());
    return true;
}

void ScreenColorPicker::keep_above(QWidget *window) { keep_above_ = window ? window->window() : nullptr; }

QColor ScreenColorPicker::color_at(const QPoint &global) const {
    const QPoint pt = global - rect_.topLeft().toPoint();
    if (!image_.rect().contains(pt)) return {};
    const QColor color = image_.pixelColor(pt);
    return QColor(color.red(), color.green(), color.blue());
}

void ScreenColorPicker::take_focus() {
    raise();
    activateWindow();
    setFocus();
    // Activation puts this window on top; the kept window goes back above it without the keyboard.
    if (keep_above_) set_window_topmost(static_cast<quintptr>(keep_above_->winId()), true);
}

void ScreenColorPicker::track(const QPoint &global) {
    if (finished_) return;
    magnifier->update_cursor(QPointF(global));
    const QColor color = color_at(global);
    if (color.isValid()) emit hovered(color);
}

void ScreenColorPicker::finish(const QColor &color) {
    if (finished_) return;
    if (!color.isValid()) {
        cancel();
        return;
    }
    finished_ = true;
    emit picked(color);
    close();
}

void ScreenColorPicker::cancel() {
    if (finished_) return;
    finished_ = true;
    emit cancelled();
    close();
}

void ScreenColorPicker::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    if (image_.size() == size()) painter.drawImage(event->rect(), image_, event->rect());
    else painter.drawImage(rect(), image_);
}

void ScreenColorPicker::mouseMoveEvent(QMouseEvent *event) { track(event->globalPosition().toPoint()); }

void ScreenColorPicker::mousePressEvent(QMouseEvent *event) {
    log_debug(QString("DBG press %1 at %2,%3").arg(int(event->button())).arg(event->globalPosition().x()).arg(event->globalPosition().y()), "ScreenColorPicker");
    if (event->button() == Qt::LeftButton) finish(color_at(event->globalPosition().toPoint()));
    else if (event->button() == Qt::RightButton) cancel();
}

void ScreenColorPicker::wheelEvent(QWheelEvent *event) {
    const int delta = event->angleDelta().y();
    if (delta) magnifier->adjust_zoom(delta > 0 ? 1 : -1);
}

void ScreenColorPicker::keyPressEvent(QKeyEvent *event) {
    const QPoint pos = QCursor::pos();
    QPoint step;
    switch (event->key()) {
    case Qt::Key_Escape: cancel(); return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space: finish(color_at(pos)); return;
    // The magnifier's own keys, as in the capture window.
    case Qt::Key_Shift: magnifier->cycle_color_format(); return;
    case Qt::Key_C: magnifier->copy_color_info(); return;
    // One physical pixel per press, for spots smaller than the pointer can hit.
    case Qt::Key_Left: step = QPoint(-1, 0); break;
    case Qt::Key_Right: step = QPoint(1, 0); break;
    case Qt::Key_Up: step = QPoint(0, -1); break;
    case Qt::Key_Down: step = QPoint(0, 1); break;
    default: QWidget::keyPressEvent(event); return;
    }
    QCursor::setPos(pos + step);
    track(pos + step);
}

void ScreenColorPicker::enterEvent(QEnterEvent *event) {
    log_debug(QString("DBG enter active=%1").arg(isActiveWindow()), "ScreenColorPicker");
    // Back from the kept window: the keyboard returns here.
    if (!finished_ && !isActiveWindow()) take_focus();
    QWidget::enterEvent(event);
}

void ScreenColorPicker::leaveEvent(QEvent *event) {
    log_debug("DBG leave", "ScreenColorPicker");
    if (!finished_) {
        magnifier->clear_cursor();
        emit left();
    }
    QWidget::leaveEvent(event);
}

void ScreenColorPicker::changeEvent(QEvent *event) {
    if (event->type() == QEvent::ActivationChange) log_debug(QString("DBG activation active=%1 app_active=%2").arg(isActiveWindow()).arg(QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none"), "ScreenColorPicker");
    if (event->type() == QEvent::ActivationChange && !finished_ && !isActiveWindow()) {
        // Another program took the focus. Activation settles after this event, so the
        // new active window is read on the next turn; the kept window does not end it.
        QTimer::singleShot(0, this, [this] {
            if (finished_ || isActiveWindow()) return;
            auto *active = QApplication::activeWindow();
            if (active && active == keep_above_) return;
            cancel();
        });
    }
    QWidget::changeEvent(event);
}
}
