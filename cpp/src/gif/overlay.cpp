#include "gif/overlay.h"
#include <QCursor>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <cstdlib>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef PASSTHROUGH

namespace jietuba {
namespace {
// Drawn border width (px).
constexpr int BORDER_W = 3;
// Clickable border width (px), centred on the border line; the outer margin is for hit testing only.
constexpr int HIT_MARGIN = 8;
const QColor COLOR_IDLE("#2196F3");
const QColor COLOR_RECORD("#F44336");
const QColor COLOR_PAUSED("#E5AD32");
}

CaptureOverlay::CaptureOverlay(const QRect &rect, QWidget *parent) : QWidget(parent), rect_(rect), color_(COLOR_IDLE) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setGeometry(outer_geom());
    set_passthrough(true);
}

void CaptureOverlay::set_mode(OverlayMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    set_passthrough(mode == OverlayMode::PASSTHROUGH);
    update();
}

void CaptureOverlay::set_recording(bool recording, bool paused) {
    color_ = recording ? (paused ? COLOR_PAUSED : COLOR_RECORD) : COLOR_IDLE;
    update();
}

void CaptureOverlay::update_rect(const QRect &rect) {
    rect_ = rect;
    setGeometry(outer_geom());
    update();
}

void CaptureOverlay::set_passthrough(bool enable) {
    const auto hwnd = reinterpret_cast<HWND>(winId());
    LONG style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (enable) {
        style |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    } else {
        style &= ~WS_EX_TRANSPARENT;
        style |= WS_EX_LAYERED;
    }
    SetWindowLongW(hwnd, GWL_EXSTYLE, style);
}

void CaptureOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int m = HIT_MARGIN / 2;
    const QRect inner(m, m, width() - m * 2, height() - m * 2);
    p.setPen(QPen(color_, BORDER_W));
    p.setBrush(Qt::NoBrush);
    p.drawRect(inner);
    p.end();
}

void CaptureOverlay::mousePressEvent(QMouseEvent *ev) {
    if (mode_ != OverlayMode::RESIZE) {
        QWidget::mousePressEvent(ev);
        return;
    }
    const auto edge = hit_edge(ev->pos());
    if (!edge.isEmpty()) {
        drag_edge_ = edge;
        drag_origin_ = ev->globalPosition().toPoint();
        drag_rect_origin_ = rect_;
    }
}

void CaptureOverlay::mouseMoveEvent(QMouseEvent *ev) {
    if (mode_ != OverlayMode::RESIZE) {
        QWidget::mouseMoveEvent(ev);
        return;
    }
    if (!drag_edge_.isEmpty()) {
        apply_resize(ev->globalPosition().toPoint() - drag_origin_);
        return;
    }
    // Hover: update the cursor.
    static const QHash<QString, Qt::CursorShape> cursor_map = {
        {"t", Qt::SizeVerCursor}, {"b", Qt::SizeVerCursor}, {"l", Qt::SizeHorCursor}, {"r", Qt::SizeHorCursor},
        {"tl", Qt::SizeFDiagCursor}, {"br", Qt::SizeFDiagCursor}, {"tr", Qt::SizeBDiagCursor}, {"bl", Qt::SizeBDiagCursor},
    };
    setCursor(QCursor(cursor_map.value(hit_edge(ev->pos()), Qt::ArrowCursor)));
}

void CaptureOverlay::mouseReleaseEvent(QMouseEvent *ev) {
    if (!drag_edge_.isEmpty()) {
        drag_edge_.clear();
        emit rect_changed(rect_);
    }
    QWidget::mouseReleaseEvent(ev);
}

QString CaptureOverlay::hit_edge(const QPoint &pos) const {
    const int x = pos.x(), y = pos.y();
    const int w = width(), h = height();
    const int half = HIT_MARGIN / 2;
    const bool on_left = std::abs(x - half) <= half;
    const bool on_right = std::abs(x - (w - half)) <= half;
    const bool on_top = std::abs(y - half) <= half;
    const bool on_bottom = std::abs(y - (h - half)) <= half;
    // Corners first.
    if (on_top && on_left) return "tl";
    if (on_top && on_right) return "tr";
    if (on_bottom && on_left) return "bl";
    if (on_bottom && on_right) return "br";
    const bool in_lr_range = half <= y && y <= h - half;
    const bool in_tb_range = half <= x && x <= w - half;
    if (on_left && in_lr_range) return "l";
    if (on_right && in_lr_range) return "r";
    if (on_top && in_tb_range) return "t";
    if (on_bottom && in_tb_range) return "b";
    return {};
}

void CaptureOverlay::apply_resize(const QPoint &delta) {
    QRect r = drag_rect_origin_;
    const int dx = delta.x(), dy = delta.y();
    const auto &e = drag_edge_;
    if (e.contains('l')) r.setLeft(r.left() + dx);
    if (e.contains('r')) r.setRight(r.right() + dx);
    if (e.contains('t')) r.setTop(r.top() + dy);
    if (e.contains('b')) r.setBottom(r.bottom() + dy);
    if (r.width() < 64) r.setWidth(64);
    if (r.height() < 64) r.setHeight(64);
    rect_ = r.normalized();
    setGeometry(outer_geom());
    update();
}

QRect CaptureOverlay::outer_geom() const {
    const int m = HIT_MARGIN / 2;
    return QRect(rect_.x() - m, rect_.y() - m, rect_.width() + m * 2, rect_.height() + m * 2);
}
}
