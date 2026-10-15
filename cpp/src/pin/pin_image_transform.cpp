#include "pin/pin_image_transform.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include "pin/ocr_text_layer.h"
#include "pin/pin_border_overlay.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include <algorithm>

namespace jietuba {
QSize PinImageTransform::display_size(const QSize &orig_size) const {
    if (is_rotated_90_or_270()) return QSize(orig_size.height(), orig_size.width());
    return orig_size;
}

QTransform PinImageTransform::build_view_transform(double content_w, double content_h, double scene_w, double scene_h) const {
    const double cx = scene_w / 2.0, cy = scene_h / 2.0;
    QTransform geo;
    geo.translate(cx, cy);
    geo.scale(flip_h_ ? -1.0 : 1.0, flip_v_ ? -1.0 : 1.0);
    if (rotation_) geo.rotate(rotation_);
    geo.translate(-cx, -cy);
    // After 90/270 the content spans scene_h x scene_w.
    double sx, sy;
    if (is_rotated_90_or_270()) {
        sx = content_w / scene_h;
        sy = content_h / scene_w;
    } else {
        sx = content_w / scene_w;
        sy = content_h / scene_h;
    }
    // A * B applies A first, so geo -> offset -> scale is geo * offset * scale.
    QTransform scale_t;
    scale_t.scale(sx, sy);
    if (!is_rotated_90_or_270()) return geo * scale_t;
    // The rotated corners no longer start at (0, 0); shift the content's top-left there.
    const QPointF corners[] = {geo.map(QPointF(0, 0)), geo.map(QPointF(scene_w, 0)), geo.map(QPointF(scene_w, scene_h)), geo.map(QPointF(0, scene_h))};
    double min_x = corners[0].x(), min_y = corners[0].y();
    for (const auto &p : corners) {
        min_x = std::min(min_x, p.x());
        min_y = std::min(min_y, p.y());
    }
    QTransform offset;
    offset.translate(-min_x, -min_y);
    return geo * offset * scale_t;
}

void PinImageTransform::apply_to_window(PinWindow *pin_window) const {
    const double cur_scale = pin_window->scale_factor;
    // The new rotation's 100% size times the current scale.
    const auto new_size = display_size(pin_window->orig_size());
    const int new_w = std::max(1, static_cast<int>(python_round(new_size.width() * cur_scale)));
    const int new_h = std::max(1, static_cast<int>(python_round(new_size.height() * cur_scale)));
    // The centre stays put.
    const int cx = pin_window->x() + pin_window->width() / 2;
    const int cy = pin_window->y() + pin_window->height() / 2;
    pin_window->setGeometry(cx - new_w / 2, cy - new_h / 2, new_w, new_h);
    if (pin_window->canvas) pin_window->canvas->invalidate_cache();
    pin_window->update_view_transform();
    pin_window->refresh_background_for_scale();
    pin_window->update_button_positions();
    if (pin_window->toolbar && pin_window->toolbar->isVisible()) pin_window->toolbar->sync_with_pin_window();
    update_ocr_layer(pin_window);
    refresh_border(pin_window);
    pin_window->update();
    log_debug(T("变换已应用: rotation={rotation}, flip_h={flip_h}, flip_v={flip_v}", {{"rotation", rotation_}, {"flip_h", flip_h_}, {"flip_v", flip_v_}}),
              "ImageTransform");
}

void PinImageTransform::update_ocr_layer(PinWindow *pin_window) const {
    if (auto *ocr_layer = pin_window->ocr_text_layer()) ocr_layer->set_image_transform(this);
}

void PinImageTransform::refresh_border(PinWindow *pin_window) const {
    const bool locked = pin_window->locked();
    const auto color = locked ? LOCKED_BORDER_COLOR : pin_window->border_color;
    if (pin_window->border_enabled || locked) {
        if (!pin_window->border_overlay) pin_window->border_overlay = new PinBorderOverlay(pin_window, pin_window->corner, color);
        else pin_window->border_overlay->set_border_color(color);
        pin_window->border_overlay->setGeometry(0, 0, pin_window->width(), pin_window->height());
        pin_window->border_overlay->show();
        pin_window->border_overlay->raise();
    } else if (pin_window->border_overlay) {
        pin_window->border_overlay->hide();
    }
    if (pin_window->view) pin_window->view->set_corner_radius(pin_window->border_enabled ? pin_window->corner : 0);
}

QImage PinImageTransform::transform_image(const QImage &image) const {
    if (!has_transform()) return image;
    const double cw = image.width() / 2.0, ch = image.height() / 2.0;
    QTransform t;
    t.translate(cw, ch);
    t.scale(flip_h_ ? -1.0 : 1.0, flip_v_ ? -1.0 : 1.0);
    if (rotation_) t.rotate(rotation_);
    t.translate(-cw, -ch);
    auto result = image.transformed(t, Qt::SmoothTransformation);
    result.setDevicePixelRatio(image.devicePixelRatio());
    return result;
}

std::pair<double, double> PinImageTransform::map_ocr_point(double x, double y, double orig_w, double orig_h) const {
    const double cx = orig_w / 2.0, cy = orig_h / 2.0;
    double px = x - cx, py = y - cy;
    // 1) rotate
    if (rotation_ == 90) {
        std::tie(px, py) = std::make_pair(-py, px);
    } else if (rotation_ == 180) {
        std::tie(px, py) = std::make_pair(-px, -py);
    } else if (rotation_ == 270) {
        std::tie(px, py) = std::make_pair(py, -px);
    }
    // 2) flip
    if (flip_h_) px = -px;
    if (flip_v_) py = -py;
    // 3) back to the transformed system, whose centre swaps after 90/270
    if (is_rotated_90_or_270()) {
        px += orig_h / 2.0;
        py += orig_w / 2.0;
    } else {
        px += cx;
        py += cy;
    }
    return {px, py};
}

QRectF PinImageTransform::map_ocr_rect(const QRectF &rect, double orig_w, double orig_h) const {
    const std::pair<double, double> corners[] = {{rect.left(), rect.top()}, {rect.right(), rect.top()}, {rect.right(), rect.bottom()}, {rect.left(), rect.bottom()}};
    double min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool first = true;
    for (const auto &[cx, cy] : corners) {
        const auto [mx, my] = map_ocr_point(cx, cy, orig_w, orig_h);
        if (first) {
            min_x = max_x = mx;
            min_y = max_y = my;
            first = false;
        } else {
            min_x = std::min(min_x, mx);
            max_x = std::max(max_x, mx);
            min_y = std::min(min_y, my);
            max_y = std::max(max_y, my);
        }
    }
    return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
}

std::pair<double, double> PinImageTransform::mapped_image_size(double orig_w, double orig_h) const {
    if (is_rotated_90_or_270()) return {orig_h, orig_w};
    return {orig_w, orig_h};
}
}
