#pragma once
#include <QRectF>
#include <optional>

namespace jietuba {
class SelectionItem;

// Keeps the selection's aspect ratio while resizing by its handles: the dragged
// axis leads and the other follows. Installed as SelectionItem's resize constraint.
class LockRatioLogic {
public:
    explicit LockRatioLogic(SelectionItem *selection_item);
    // Enabling takes the ratio from current_rect; disabling clears it.
    void set_enabled(bool enabled, const std::optional<QRectF> &current_rect = std::nullopt);
    bool enabled() const { return enabled_; }
    std::optional<double> ratio() const { return ratio_; }
    void uninstall();
private:
    void install_hook();
    QRectF constrain(const QRectF &rect, int handle) const;
    SelectionItem *item_;
    bool enabled_ = false;
    std::optional<double> ratio_;  // w / h
    bool installed_ = false;
};
}
