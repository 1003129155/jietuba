#pragma once
#include "canvas/items/item_reference.h"
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsView>
#include <QPen>
#include <QFont>
#include <optional>

namespace jietuba {
// Explicit dependency contract for the pending SmartEditController/View port.
// Lookup follows the original: scene.views()[0], never QGraphicsItem::isSelected.
class DrawingEditContext {
public:
    virtual ~DrawingEditContext() = default;
    virtual QGraphicsItem *selected_drawing_item() const = 0;
    virtual bool can_show_hover_cursor(QGraphicsItem *) const = 0;
};
class DrawingItemMixin : public TrackedItem {
public:
    explicit DrawingItemMixin(QGraphicsItem *item);
    virtual bool shows_selection_frame() const { return true; }
    virtual bool is_edit_target() const;
    std::optional<Qt::PenStyle> selection_frame_style() const;
    std::optional<QPen> selection_frame_pen() const;
    virtual void set_stroke_width(qreal) {}
    virtual bool scale_stroke_width(qreal) { return false; }
    virtual bool set_visual_opacity(qreal) { return false; }
    virtual std::optional<qreal> get_stroke_width() const { return {}; }
    virtual std::optional<qreal> get_visual_opacity() const;
    void set_hovered(bool);
    void update_hover_cursor(QGraphicsSceneHoverEvent *event = nullptr);
    void hover_enter(QGraphicsSceneHoverEvent *);
    void hover_move(QGraphicsSceneHoverEvent *);
    void hover_leave(QGraphicsSceneHoverEvent *);
protected:
    DrawingEditContext *edit_controller() const;
    bool can_show_hover() const;
    QGraphicsItem *drawing_item_;
    bool hovered_ = false;
};
#define JIETUBA_DRAWING_EVENTS \
    void hoverEnterEvent(QGraphicsSceneHoverEvent *e) override { hover_enter(e); } \
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override { hover_move(e); } \
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override { hover_leave(e); }
#define JIETUBA_PEN_INTERFACE \
    void set_stroke_width(qreal) override; \
    bool scale_stroke_width(qreal) override; \
    bool set_visual_opacity(qreal) override; \
    std::optional<qreal> get_stroke_width() const override; \
    std::optional<qreal> get_visual_opacity() const override;
class RectItem : public QGraphicsRectItem, public DrawingItemMixin {
public:
    static constexpr qreal CLICK_MARGIN = 4;
    RectItem(const QRectF &, const QPen &, qreal corner_radius = 0);
    bool is_highlighter = false, is_highlighter_rect = false;
    void setPen(const QPen &);
    void setRect(const QRectF &);
    qreal get_corner_radius() const { return corner_radius_; }
    void set_corner_radius(qreal);
    QPainterPath shape() const override;
    QRectF boundingRect() const override;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
    JIETUBA_PEN_INTERFACE
protected:
    JIETUBA_DRAWING_EVENTS
private:
    qreal corner_radius_;
    mutable std::optional<QPainterPath> shape_cache_;
};
class EllipseItem : public QGraphicsEllipseItem, public DrawingItemMixin {
public:
    static constexpr qreal CLICK_MARGIN = 4;
    EllipseItem(const QRectF &, const QPen &);
    void setPen(const QPen &);
    void setRect(const QRectF &);
    QPainterPath shape() const override;
    QRectF boundingRect() const override;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
    JIETUBA_PEN_INTERFACE
protected:
    JIETUBA_DRAWING_EVENTS
private:
    mutable std::optional<QPainterPath> shape_cache_;
};
class NumberItem : public QGraphicsItem, public DrawingItemMixin {
public:
    static constexpr qreal FONT_SCALE = .95, CLICK_MARGIN = 6;
    static constexpr int MIN_FONT_SIZE = 10;
    static constexpr qreal RING_WIDTH_RATIO = .12, GLYPH_OUTLINE_RATIO = .09;
    // tools.number.NumberTool.RADIUS_SCALE in the Python version.
    static constexpr qreal RADIUS_SCALE = 2;
    static inline const QString STYLE_SOLID = "solid";            // filled circle, filled digits
    static inline const QString STYLE_HOLLOW_BG = "hollow_bg";    // outlined circle, filled digits
    static inline const QString STYLE_HOLLOW_ALL = "hollow_all";  // outlined circle, outlined digits
    static inline const QString STYLE_NO_CIRCLE = "no_circle";    // digits only
    static inline const QString DEFAULT_STYLE = STYLE_SOLID;
    NumberItem(int number, const QPointF &, qreal radius, const QColor &, const QString &style = {});
    int number;
    std::optional<int> number_order;
    qreal radius;
    QColor color;
    QString style;
    static QString normalize_style(const QString &);
    void set_style(const QString &);
    bool is_hollow() const { return style != "solid"; }
    bool has_ring() const { return style == "hollow_bg" || style == "hollow_all"; }
    QRectF visualRect() const;
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    QRectF sceneVisualRect() const;
    qreal ring_width() const;
    QFont number_font() const;
    QColor solid_text_color() const;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget *) override;
    JIETUBA_PEN_INTERFACE
protected:
    JIETUBA_DRAWING_EVENTS
private:
    void paint_circle(QPainter *, const QRectF &, const QColor &);
    void paint_number(QPainter *, const QRectF &, const QColor &);
    void paint_selection_frame(QPainter *, const QRectF &);
};
#undef JIETUBA_DRAWING_EVENTS
#undef JIETUBA_PEN_INTERFACE
}
