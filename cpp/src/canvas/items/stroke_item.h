#pragma once
#include "canvas/items/drawing_items.h"
#include <QAbstractGraphicsShapeItem>
#include <QPainterPath>
#include <optional>
#include <vector>

namespace jietuba {
// A pen or highlighter stroke.
//
// The path is split into chunks of CHUNK_SEGMENTS segments. A repaint joins only the chunks meeting
// the repainted area into one path drawn once, so a translucent pen does not darken at the joints.
// While drawing, extend_path marks only the chunks that changed, so however long the stroke gets,
// each frame repaints only near the tip; when items nearby repaint, only the chunks they touch are
// drawn again.
//
// Dashed pens fill outlines worked out chunk by chunk, each continuing the dash pattern from the
// length of the chunks before it, so a partial repaint matches the whole stroke. Qt's own dashing
// depends on the repainted area and shifts the dashes at its edge.
//
// The pen lives in QAbstractGraphicsShapeItem so code reading pens off shape items reads this one
// too; set it through setPen here, which also drops what the old pen shaped.
class StrokeItem : public QAbstractGraphicsShapeItem, public DrawingItemMixin {
public:
    static constexpr int CHUNK_SEGMENTS = 8;
    static constexpr qreal START_MARGIN = 32;
    static constexpr qreal GROW_MARGIN = 256;
    static constexpr qreal HIT_MARGIN = 10;  // the hit area is this much wider than the stroke on each side

    StrokeItem(const QPainterPath &path, const QPen &pen, bool is_highlighter = false);
    bool is_highlighter;
    bool shows_selection_frame() const override { return false; }

    QPainterPath path() const { return path_; }
    void setPath(const QPainterPath &path);
    // While drawing, catches up with source: elements before dirty_from are as last time, later ones
    // only moved or were appended. Copies just that tail; the pen keeps editing source in place.
    // The bounds start START_MARGIN around the first segment, and each side the stroke runs past moves
    // GROW_MARGIN beyond it. They are repainted whole only when they grow; finish_path tightens them.
    void extend_path(const QPainterPath &source, int dirty_from);
    // Done drawing: drops the slack the bounds kept while growing. Selection handles sit on their corners.
    void finish_path();
    void setPen(const QPen &pen);

    QRectF boundingRect() const override { return bounds_; }
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget = nullptr) override;

    // Elements [first, end) of the path as a path of their own.
    struct Chunk {
        int first = 0, end = 0;
        int segments = 0;
        QPainterPath path;
        QRectF rect;          // what the chunk paints
        qreal travelled = 0;  // path length before the chunk; dashes continue from there
        qreal length = 0;
        mutable std::optional<QPainterPath> outline;  // what a dashed pen fills, worked out when needed
    };
    const std::vector<Chunk> &chunks() const;
    // The chunks meeting exposed joined into one path; the path itself when all of it is exposed.
    QPainterPath visible_path(const QRectF &exposed) const;
    // The dash outlines of the chunks meeting exposed, joined into one path filled by the winding rule
    // so the joints do not darken.
    QPainterPath dash_outline(const QRectF &exposed) const;

    void set_stroke_width(qreal) override;
    bool scale_stroke_width(qreal) override;
    bool set_visual_opacity(qreal) override;
    std::optional<qreal> get_stroke_width() const override;
    std::optional<qreal> get_visual_opacity() const override;
protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *e) override { hover_enter(e); }
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override { hover_move(e); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override { hover_leave(e); }
private:
    bool dashed() const;
    // How far painted pixels reach past the path's control points: half the width, more for square
    // caps and miter joins, plus 2 px of antialiasing.
    qreal paint_reach() const;
    // Hugs the curve itself; painted pixels and the hit area must both fall inside.
    QRectF exact_bounds() const;
    // Cuts from element start to the end; travelled is the path length before start.
    std::vector<Chunk> build_chunks(int start, qreal travelled) const;
    QPainterPath path_;
    QRectF bounds_;
    mutable std::optional<std::vector<Chunk>> chunks_;  // cut on the first partial repaint
    mutable std::optional<QPainterPath> shape_cache_;    // createStroke is expensive and moving asks for it repeatedly
};
}
