#pragma once
#include "canvas/items/item_reference.h"
#include <QGraphicsItem>
#include <QMap>
#include <QMetaObject>
#include <functional>

namespace jietuba {
class SelectionModel;

// Hit area for dragging the selection and its eight handles. It paints nothing
// in the scene (ItemHasNoContents); SelectionOverlayWidget draws the frame
// above the mask through render().
class SelectionItem : public QGraphicsItem, public TrackedItem {
public:
    enum Handle {
        HANDLE_NONE = 0, HANDLE_TOP_LEFT, HANDLE_TOP, HANDLE_TOP_RIGHT, HANDLE_LEFT, HANDLE_RIGHT,
        HANDLE_BOTTOM_LEFT, HANDLE_BOTTOM, HANDLE_BOTTOM_RIGHT, HANDLE_BODY,
    };
    // Handles are hidden once they would occupy more than this share of the shorter side.
    static constexpr qreal HANDLE_CROWDING_RATIO = .8;
    explicit SelectionItem(SelectionModel *model);
    ~SelectionItem() override;
    // Read from the theme on every use, so a changed setting applies to the next capture.
    int border_width() const;
    int handle_size() const;
    int handle_ring_width() const;
    int handle_visual_diameter() const { return handle_size() + handle_ring_width(); }
    QRectF boundingRect() const override;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override {}
    // Set by the overlay once installed; decoration repaints go through it.
    std::function<void()> repaint_requested;
    void request_repaint();
    // Area render() can touch, in scene coordinates.
    QRectF visual_bounds() const;
    void render(QPainter *painter);
    // Replaces render() while it returns true (rounded-corner frame).
    std::function<bool(QPainter *)> render_override;
    // Adjusts the resized rect before it reaches the model (locked aspect ratio);
    // receives (rect, start rect, handle). Moves are never constrained.
    std::function<QRectF(const QRectF &, const QRectF &, int)> resize_constraint;
    // Not while dragging, before confirmation, or when the selection is too small.
    bool handles_visible() const;
    // skip_corners for the rounded-corner preview, where corner handles cannot follow the arc.
    void draw_handles(QPainter *painter, const QRectF &rect, bool skip_corners = false);
    void update_bounds();
    int active_handle = HANDLE_NONE;
    QPointF start_pos;
    QRectF start_rect;
protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
private:
    static QMap<int, QPointF> handle_positions(const QRectF &rect);
    int hit_test(const QPointF &pos) const;
    bool should_delegate_to_text(const QPointF &scene_pos) const;
    SelectionModel *model_;
    QMetaObject::Connection rect_connection_;
};
}
