#include "clipboard/ui/mixins/frameless_mixin.h"
#include <QEvent>
#include <QMouseEvent>
#include <QWidget>

namespace jietuba::clipboard {
void FramelessMixin::init_frameless(QWidget *window, int edge_margin) {
    window_ = window;
    edge_margin_ = edge_margin;
    reset();
    window_->setMouseTracking(true);
    window_->installEventFilter(window_);
}

QString FramelessMixin::get_edge(const QPoint &local_pos) const {
    const int x = local_pos.x(), y = local_pos.y();
    const int w = window_->width(), h = window_->height();
    const int m = edge_margin_;
    if (x < 0 || y < 0 || x > w || y > h) return {};
    const bool on_left = x < m, on_right = x > w - m, on_top = y < m, on_bottom = y > h - m;
    if (on_top && on_left) return "topleft";
    if (on_top && on_right) return "topright";
    if (on_bottom && on_left) return "bottomleft";
    if (on_bottom && on_right) return "bottomright";
    if (on_left) return "left";
    if (on_right) return "right";
    if (on_top) return "top";
    if (on_bottom) return "bottom";
    return {};
}

void FramelessMixin::update_cursor(const QString &edge) {
    if (edge == "left" || edge == "right") window_->setCursor(Qt::SizeHorCursor);
    else if (edge == "top" || edge == "bottom") window_->setCursor(Qt::SizeVerCursor);
    else if (edge == "topleft" || edge == "bottomright") window_->setCursor(Qt::SizeFDiagCursor);
    else if (edge == "topright" || edge == "bottomleft") window_->setCursor(Qt::SizeBDiagCursor);
    else window_->unsetCursor();
}

void FramelessMixin::do_resize(const QPoint &global_pos) {
    if (!resize_start_geometry_ || !resize_start_pos_) return;
    const int dx = global_pos.x() - resize_start_pos_->x();
    const int dy = global_pos.y() - resize_start_pos_->y();
    const QRect geo = *resize_start_geometry_;
    int new_x = geo.x(), new_y = geo.y(), new_w = geo.width(), new_h = geo.height();
    const int min_w = window_->minimumWidth(), min_h = window_->minimumHeight();
    if (resize_edge_.contains("left")) {
        new_w = std::max(min_w, geo.width() - dx);
        if (new_w > min_w) new_x = geo.x() + dx;
    }
    if (resize_edge_.contains("right")) new_w = std::max(min_w, geo.width() + dx);
    if (resize_edge_.contains("top")) {
        new_h = std::max(min_h, geo.height() - dy);
        if (new_h > min_h) new_y = geo.y() + dy;
    }
    if (resize_edge_.contains("bottom")) new_h = std::max(min_h, geo.height() + dy);
    window_->setGeometry(new_x, new_y, new_w, new_h);
}

void FramelessMixin::reset() {
    is_dragging_ = false;
    drag_pos_.reset();
    resize_edge_.clear();
    resize_start_pos_.reset();
    resize_start_geometry_.reset();
    if (window_) window_->unsetCursor();
}

void FramelessMixin::setup_mouse_tracking_recursive(QWidget *widget) {
    widget->setMouseTracking(true);
    widget->installEventFilter(window_);
    for (QWidget *child : widget->findChildren<QWidget *>()) {
        child->setMouseTracking(true);
        child->installEventFilter(window_);
    }
}

bool FramelessMixin::handle_event(QObject *obj, QEvent *event) {
    const QEvent::Type event_type = event->type();
    // Events carrying a global position (mouse, hover, wheel, enter).
    auto *point_event = dynamic_cast<QSinglePointEvent *>(event);
    std::optional<QPoint> global_pos, local_pos;
    if (point_event) {
        global_pos = point_event->globalPosition().toPoint();
        local_pos = window_->mapFromGlobal(*global_pos);
    }

    if (event_type == QEvent::MouseMove && local_pos) {
        if (!resize_edge_.isEmpty()) {
            do_resize(*global_pos);
            return true;
        }
        if (is_dragging_ && drag_pos_) {
            window_->move(*global_pos - *drag_pos_);
            return true;
        }
        update_cursor(get_edge(*local_pos));
        return false;
    }
    if (event_type == QEvent::MouseButtonPress && local_pos) {
        if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            const QString edge = get_edge(*local_pos);
            if (!edge.isEmpty()) {
                resize_edge_ = edge;
                resize_start_pos_ = global_pos;
                resize_start_geometry_ = window_->geometry();
                return true;
            }
            if (is_draggable_area(obj, *local_pos)) {
                is_dragging_ = true;
                drag_pos_ = *global_pos - window_->pos();
                return true;
            }
        }
    }
    if (event_type == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        if (!resize_edge_.isEmpty() || is_dragging_) {
            reset();
            return true;
        }
    }
    return false;
}
}
