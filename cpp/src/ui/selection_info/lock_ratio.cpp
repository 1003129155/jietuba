#include "ui/selection_info/lock_ratio.h"
#include "canvas/items/selection_item.h"
#include "core/logger.h"

namespace jietuba {
LockRatioLogic::LockRatioLogic(SelectionItem *selection_item) : item_(selection_item) { install_hook(); }

void LockRatioLogic::set_enabled(bool enabled, const std::optional<QRectF> &current_rect) {
    enabled_ = enabled;
    if (enabled && current_rect && current_rect->height() > 0) {
        ratio_ = current_rect->width() / current_rect->height();
        log_debug(T("锁定纵横比: ON  ratio={ratio:.4f}", {{"ratio", *ratio_}}), "LockRatio");
    } else {
        ratio_.reset();
        if (!enabled) log_debug(T("锁定纵横比: OFF"), "LockRatio");
    }
}

void LockRatioLogic::install_hook() {
    item_->resize_constraint = [this](const QRectF &rect, const QRectF &, int handle) {
        if (enabled_ && ratio_ && *ratio_ > 0) return constrain(rect, handle);
        return rect;
    };
    installed_ = true;
}

QRectF LockRatioLogic::constrain(const QRectF &rect, int handle) const {
    const double ratio = *ratio_;
    QRectF r(rect);
    if (handle == SelectionItem::HANDLE_TOP_LEFT || handle == SelectionItem::HANDLE_TOP_RIGHT || handle == SelectionItem::HANDLE_BOTTOM_LEFT ||
        handle == SelectionItem::HANDLE_BOTTOM_RIGHT) {
        // Corners: width leads; top handles keep the bottom fixed, bottom handles the top.
        const double h = ratio ? r.width() / ratio : r.height();
        if (handle == SelectionItem::HANDLE_TOP_LEFT || handle == SelectionItem::HANDLE_TOP_RIGHT) r.setTop(r.bottom() - h);
        else r.setBottom(r.top() + h);
    } else if (handle == SelectionItem::HANDLE_LEFT || handle == SelectionItem::HANDLE_RIGHT) {
        // Left/right: width leads, height follows around the centre.
        const double h = ratio ? r.width() / ratio : r.height();
        const double cy = r.center().y();
        r.setTop(cy - h / 2);
        r.setBottom(cy + h / 2);
    } else if (handle == SelectionItem::HANDLE_TOP || handle == SelectionItem::HANDLE_BOTTOM) {
        // Top/bottom: height leads, width follows around the centre.
        const double w = ratio ? r.height() * ratio : r.width();
        const double cx = r.center().x();
        r.setLeft(cx - w / 2);
        r.setRight(cx + w / 2);
    }
    return r;
}

void LockRatioLogic::uninstall() {
    if (installed_) {
        item_->resize_constraint = nullptr;
        installed_ = false;
    }
}
}
