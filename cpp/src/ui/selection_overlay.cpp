#include "ui/selection_overlay.h"
#include "canvas/items/selection_item.h"
#include "canvas/selection_model.h"
#include <QPaintEvent>
#include <QPainter>
#include <QRegion>

namespace jietuba {
SelectionOverlayWidget::SelectionOverlayWidget(QWidget *parent, SelectionItem *selection_item, SelectionModel *selection_model)
    : QWidget(parent), item_(selection_item), model_(selection_model) {
    // Mouse events pass through: dragging is still hit-tested by the scene's SelectionItem.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);
    connect_model();
    selection_item->repaint_requested = [this] { refresh(); };
    show();
    raise();
}

SelectionOverlayWidget::~SelectionOverlayWidget() {
    if (auto *item = live_item()) item->repaint_requested = nullptr;
}

void SelectionOverlayWidget::refresh() {
    const auto dirty = chrome_rect().united(painted_);
    if (!dirty.isEmpty()) update(dirty);
}

void SelectionOverlayWidget::connect_model() {
    if (!model_) return;
    connect(model_, &SelectionModel::rectChanged, this, [this](const QRectF &) { refresh(); });
    connect(model_, &SelectionModel::draggingChanged, this, [this](bool) { refresh(); });
    connect(model_, &SelectionModel::confirmed, this, [this](const QRectF &) { refresh(); });
}

QPoint SelectionOverlayWidget::origin() const {
    auto *parent = parentWidget();
    return parent ? QPoint(-parent->x(), -parent->y()) : QPoint(0, 0);
}

SelectionItem *SelectionOverlayWidget::live_item() const { return item_.as<SelectionItem>(); }

QRect SelectionOverlayWidget::chrome_rect() const {
    auto *item = live_item();
    if (!item) return {};
    const auto bounds = item->visual_bounds();
    if (bounds.isNull() || bounds.isEmpty()) return {};
    const auto offset = origin();
    const int pad = EDGE_ALLOWANCE;
    const auto rect = bounds.translated(offset.x(), offset.y()).toAlignedRect();
    return rect.adjusted(-pad, -pad, pad, pad).intersected(this->rect());
}

QRect SelectionOverlayWidget::still_on_screen(const QRegion &repainted, const QRect &drawn) const {
    // Subtraction, not QRegion::contains(QRect), which tests intersection.
    if (QRegion(painted_).subtracted(repainted).isEmpty()) return drawn;
    return drawn.united(painted_);
}

void SelectionOverlayWidget::paintEvent(QPaintEvent *event) {
    auto *item = live_item();
    if (!item || !model_ || model_->is_empty()) {
        painted_ = still_on_screen(event->region(), QRect());
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(origin());
    item->render(&painter);
    painter.end();
    // Passive repaints land here too, so painted_ always reflects the screen.
    painted_ = still_on_screen(event->region(), chrome_rect());
}
}
