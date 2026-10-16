#include "canvas/selection_model.h"

namespace jietuba {
void SelectionModel::set_rect(QRectF &r) {
    if (r == rect_) return;
    // Python mutates the caller's QRectF before copying it into the model.
    if (r.width() < min_size.width()) r.setWidth(min_size.width());
    if (r.height() < min_size.height()) r.setHeight(min_size.height());
    rect_ = r;
    emit rectChanged(rect_);
}
void SelectionModel::start_dragging() {
    if (!dragging_) { dragging_ = true; emit draggingChanged(true); }
}
void SelectionModel::stop_dragging() {
    if (dragging_) { dragging_ = false; emit draggingChanged(false); }
}
void SelectionModel::deactivate() {
    active_ = false;
    rect_ = {};
    // The reference does NOT reset confirmed/dragging here.
    emit rectChanged({});
}
void SelectionModel::confirm() {
    if (is_active()) { confirmed_ = true; emit confirmed(rect_); }
}
void SelectionModel::initialize_confirmed_rect(const QRectF &rect) {
    rect_ = rect;
    emit rectChanged(rect_);
    active_ = false;
    confirmed_ = true;
    emit confirmed(rect_);
}
void SelectionModel::normalize() {
    if (!rect_.isNull()) { rect_ = rect_.normalized(); emit rectChanged(rect_); }
}
}
