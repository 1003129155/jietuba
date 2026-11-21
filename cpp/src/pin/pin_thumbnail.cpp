#include "pin/pin_thumbnail.h"
#include "canvas/scene.h"
#include "core/logger.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_hover.h"
#include "pin/pin_ocr_manager.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include <QCursor>

namespace jietuba {
void PinThumbnailMode::toggle() {
    if (!active_) enter();
    else exit();
}

void PinThumbnailMode::enter_region(const QRectF &requested) {
    if (active_) return;
    const auto rect = requested.intersected(win_->canvas->scene->sceneRect());
    if (!rect.isEmpty()) enter(rect);
}

void PinThumbnailMode::enter(const std::optional<QRectF> &region) {
    auto *win = win_;
    prev_geometry_ = win->geometry();
    auto cursor_pos = QCursor::pos();
    const auto window_rect = win->geometry();
    double scene_x, scene_y;
    if (window_rect.contains(cursor_pos)) {
        // Through the view's own transform: a turned or mirrored pin is not a plain scale of the image.
        const auto scene_pos = win->view->mapToScene(win->view->viewport()->mapFromGlobal(cursor_pos));
        scene_x = scene_pos.x();
        scene_y = scene_pos.y();
    } else {
        scene_x = win->orig_size().width() / 2.0;
        scene_y = win->orig_size().height() / 2.0;
        cursor_pos = window_rect.center();
    }
    scene_center_ = QPointF(scene_x, scene_y);
    const int new_x = cursor_pos.x() - size_ / 2;
    const int new_y = cursor_pos.y() - size_ / 2;
    region_rect_ = region;
    if (region_rect_) {
        const auto local_rect = win->view->mapFromScene(*region_rect_).boundingRect();
        const auto origin = win->view->viewport()->mapToGlobal(local_rect.topLeft());
        scene_center_ = region_rect_->center();
        win->setGeometry(origin.x(), origin.y(), std::max(3, local_rect.width()), std::max(3, local_rect.height()));
    } else {
        win->setGeometry(new_x, new_y, size_, size_);
    }
    entry_pos_ = win->geometry().topLeft();
    update_view();
    win->ocr_manager()->set_enabled(false);
    win->stop_drawing();
    active_ = true;
    win->hover_controls->sync();
    log_debug(T("进入缩略图模式，场景中心: ({scene_x:.1f}, {scene_y:.1f})", {{"scene_x", scene_x}, {"scene_y", scene_y}}), "PinWindow");
}

void PinThumbnailMode::exit() {
    if (!prev_geometry_ || !scene_center_) {
        active_ = false;
        return;
    }
    auto *win = win_;
    const auto prev_size = prev_geometry_->size();
    // Back where it was, plus however far the thumbnail was dragged. Working back from the thumbnail's
    // centre drifts: QRect::center() rounds even sides up-left, and near an edge the centre is pulled in.
    const auto moved = win->geometry().topLeft() - entry_pos_;
    const int new_x = prev_geometry_->x() + moved.x();
    const int new_y = prev_geometry_->y() + moved.y();
    // Cleared first so resizeEvent handles the resize normally.
    active_ = false;
    scene_center_.reset();
    prev_geometry_.reset();
    region_rect_.reset();
    win->setGeometry(new_x, new_y, prev_size.width(), prev_size.height());
    win->update_view_transform();
    win->ocr_manager()->set_enabled(true);
    win->hover_controls->sync();
    win->update_button_positions();
    log_debug(T("退出缩略图模式"), "PinWindow");
}

void PinThumbnailMode::update_view() {
    auto *win = win_;
    if (!win->view || !scene_center_) return;
    // fitInView keeps the rotation and mirroring it starts from, so the thumbnail is the same way up as the pin.
    QTransform orientation;
    if (const auto *transform = win->image_transform()) {
        orientation.scale(transform->flip_h() ? -1.0 : 1.0, transform->flip_v() ? -1.0 : 1.0);
        orientation.rotate(transform->rotation());
    }
    if (region_rect_) {
        win->view->setTransform(orientation);
        win->view->fitInView(*region_rect_, Qt::KeepAspectRatio);
        return;
    }
    const double half = size_ / 2.0;
    double scene_left = scene_center_->x() - half;
    double scene_top = scene_center_->y() - half;
    const double max_left = std::max(0, win->orig_size().width() - size_);
    const double max_top = std::max(0, win->orig_size().height() - size_);
    scene_left = std::max(0.0, std::min(scene_left, max_left));
    scene_top = std::max(0.0, std::min(scene_top, max_top));
    // The centre may have been clamped.
    scene_center_ = QPointF(scene_left + half, scene_top + half);
    win->view->setTransform(orientation);
    win->view->fitInView(QRectF(scene_left, scene_top, size_, size_), Qt::KeepAspectRatio);
}
}
