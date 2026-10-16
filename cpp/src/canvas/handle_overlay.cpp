#include "canvas/handle_overlay.h"
#include "canvas/handle_editor.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/view.h"
#include <QPaintEvent>
#include <QPainter>
#include <QRegion>

namespace jietuba {
HandleOverlayWidget::HandleOverlayWidget(CanvasView *view) : QWidget(view->viewport()), view_(view) {
    // Mouse events must pass through to the view.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);
    setGeometry(view->viewport()->rect());
    show();
    raise();
}

LayerEditor *HandleOverlayWidget::editor() const {
    auto *controller = view_->smart_edit_controller.get();
    return controller ? controller->layer_editor.get() : nullptr;
}

QRect HandleOverlayWidget::handles_viewport_rect() const {
    auto *layer_editor = editor();
    if (!layer_editor) return {};
    const auto bounds = layer_editor->screen_bounds();
    if (bounds.isNull()) return {};
    const int pad = EDGE_ALLOWANCE;
    return bounds.toAlignedRect().adjusted(-pad, -pad, pad, pad).intersected(rect());
}

void HandleOverlayWidget::refresh() {
    const auto viewport_rect = view_->viewport()->rect();
    if (geometry() != viewport_rect) {
        // The viewport was resized; a full invalidation clears old traces.
        setGeometry(viewport_rect);
        painted_ = QRect();
        update();
        return;
    }
    const auto dirty = handles_viewport_rect().united(painted_);
    if (!dirty.isEmpty()) update(dirty);
}

QRect HandleOverlayWidget::still_on_screen(const QRegion &repainted, const QRect &drawn) const {
    // Subtraction, not QRegion::contains(QRect), which only tests intersection.
    if (QRegion(painted_).subtracted(repainted).isEmpty()) return drawn;
    return drawn.united(painted_);
}

void HandleOverlayWidget::paintEvent(QPaintEvent *event) {
    auto *layer_editor = editor();
    if (!layer_editor || !layer_editor->is_editing()) {
        painted_ = still_on_screen(event->region(), QRect());
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Same size and origin as the viewport, so editor screen coordinates apply directly.
    layer_editor->render(&painter);
    painter.end();
    painted_ = still_on_screen(event->region(), handles_viewport_rect());
}
}
