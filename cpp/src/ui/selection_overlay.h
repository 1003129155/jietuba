#pragma once
#include "canvas/items/item_reference.h"
#include <QMetaObject>
#include <QPointer>
#include <QWidget>

namespace jietuba {
class SelectionItem;
class SelectionModel;

// Transparent full-window layer drawing SelectionItem's frame and handles,
// invalidated locally as needed.
class SelectionOverlayWidget : public QWidget {
    Q_OBJECT
public:
    // Antialiasing and strokes spill slightly past visual_bounds().
    static constexpr int EDGE_ALLOWANCE = 3;
    SelectionOverlayWidget(QWidget *parent, SelectionItem *selection_item, SelectionModel *selection_model);
    ~SelectionOverlayWidget() override;
    // Invalidates new position ∪ old position whenever selection state changes.
    void refresh();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    // Handle visibility depends on dragging and confirmation, not just the rect.
    void connect_model();
    // Scene to local offset: the scene uses physical screen coordinates and this layer fills the window.
    QPoint origin() const;
    // The scene is released when the capture ends, before this layer goes with the window.
    SelectionItem *live_item() const;
    // Decoration bounds in local coordinates; empty when there is nothing to draw.
    QRect chrome_rect() const;
    // Area still showing decoration pixels after this frame. Passive repaints (the mask
    // below invalidating a strip) can happen mid-change, so an old area not fully
    // covered by this repaint is kept for the next invalidation.
    QRect still_on_screen(const QRegion &repainted, const QRect &drawn) const;
    TrackedPointer item_;
    QPointer<SelectionModel> model_;
    QRect painted_;
};
}
