#pragma once
#include "canvas/items/drawing_items.h"

namespace jietuba {
// One translucent black curtain for the whole scene with every spotlight cut
// out of it, so overlapping spotlights do not darken each other. Holes are
// looked up at paint time; Qt repaints the curtain wherever a hole repaints.
// Vector paths rather than a full-screen bitmap keep 4K scenes cheap.
class SpotlightCurtain : public QGraphicsItem {
public:
    // Above mosaic (5) so it is darkened too; below highlighter (10) and annotations (20).
    static constexpr qreal Z_VALUE = 6;
    static constexpr qreal DEFAULT_DARKNESS = .7;
    SpotlightCurtain();
    static SpotlightCurtain *find(QGraphicsScene *scene);
    // Created on first use, so capture and pin scenes need no wiring.
    static SpotlightCurtain *of(QGraphicsScene *scene);
    QRectF boundingRect() const override;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
};

// One hole. Moving, resizing, undo, erasing and pin cloning all behave like a
// rectangle, so it is a RectItem with no pen and no brush; the whole hole is
// the hit area, like highlighter rectangles and mosaic boxes.
class SpotlightItem : public RectItem {
public:
    explicit SpotlightItem(const QRectF &rect, qreal corner_radius = 0);
    QPainterPath shape() const override;
    // The hole in scene coordinates, following rotation or scaling.
    QPainterPath hole_path() const;
    // Opacity is the darkness of the shared curtain; holes have no stroke width.
    bool set_visual_opacity(qreal opacity) override;
    std::optional<qreal> get_visual_opacity() const override;
    std::optional<qreal> get_stroke_width() const override { return std::nullopt; }
    void set_stroke_width(qreal) override {}
    bool scale_stroke_width(qreal) override { return false; }
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
};
}
