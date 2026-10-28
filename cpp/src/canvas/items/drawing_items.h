#pragma once
#include "canvas/items/item_reference.h"
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsEllipseItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QVariantMap>
#include <QPen>
#include <QFont>
#include <array>
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
// The annotation an item belongs to: the item itself, or for a part of one (a number's label or
// guide line) the top-level item that owns it. Lookups by point or area return the parts as well.
QGraphicsItem *annotation_owner(QGraphicsItem *item);
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
// A highlighter's colour on a layer with no image to multiply onto: enough to tint a line of text,
// little enough to keep the text readable.
constexpr qreal TRANSLUCENT_HIGHLIGHTER_ALPHA = 0.35;
// Sets painter up for the highlighter item: multiplied onto the image below, or laid on at
// TRANSLUCENT_HIGHLIGHTER_ALPHA where the scene has no image (CanvasScene::highlighters_multiply).
// Returns whether it is laid on translucent.
bool begin_highlighter_paint(QPainter *painter, const QGraphicsItem *item);
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
class NumberLabel;
class NumberItem : public QGraphicsItem, public DrawingItemMixin {
public:
    static constexpr qreal FONT_SCALE = .95, CLICK_MARGIN = 6;
    // In radii from the centre: a tip this close draws a bubble tail rather than a line with a head,
    // and a pointer turned on for the first time is this long.
    static constexpr qreal ARROW_TAIL_REACH = 2, ARROW_DEFAULT_LENGTH = 3;
    // In radii: the label's gap from the circle, and its text height at each size level, smallest
    // first. The level is the label's one size setting, so the text follows the number's size.
    static constexpr qreal LABEL_GAP = .3;
    static constexpr std::array<qreal, 5> LABEL_SIZE_LEVELS = {.5, .7, .9, 1.15, 1.45};
    static constexpr int DEFAULT_LABEL_SIZE_LEVEL = 2;
    // The arrow tool's single-headed styles: a pointer starts under the circle, so a second head would be hidden.
    static inline const QStringList ARROW_STYLES = {"single", "hollow", "line", "triangle"};
    // Shaft width of a pointer, in radii; its head follows the arrow style.
    static constexpr qreal ARROW_WIDTH_RATIO = .2;
    // In radii: the guide line under the label, and the label's outline as a share of its text height.
    static constexpr qreal GUIDE_WIDTH_RATIO = .08, LABEL_OUTLINE_RATIO = .07;
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

    // Tip of the pointer drawn out of the circle, relative to the centre; none without a pointer.
    const std::optional<QPointF> &arrow_tip() const { return arrow_tip_; }
    void set_arrow_tip(const std::optional<QPointF> &tip);
    // Turns the pointer off and remembers it, or back on where it was last (pointing away from the label at first).
    void toggle_arrow();
    // Outline of the pointer, before the circle is drawn over its root.
    QPainterPath pointer_path() const;

    NumberLabel *label() const { return label_; }
    QString label_text() const;
    // Shows text beside the number, or hides the label when text is blank.
    void set_label(const QString &text, const std::optional<QPointF> &anchor = {});
    // Shows the label, placed out of the pointer's way when new, and starts typing in it.
    NumberLabel *begin_label_edit();
    // Called when typing in the label ends; records the change for undo.
    void finish_label_edit(const QVariantMap &before);
    QVariantMap label_state() const;
    QFont label_font() const;
    // How the label and the pointer look. Kept on the number, so undo and copies carry them.
    QString label_font_family() const { return label_font_family_; }
    int label_size_level() const { return label_size_level_; }
    bool label_outline() const { return label_outline_; }
    bool label_guide() const { return label_guide_; }
    QString arrow_style() const { return arrow_style_; }
    // White around dark text and black around light text.
    QColor label_outline_color() const { return solid_text_color() == QColor(Qt::black) ? QColor(Qt::black) : QColor(Qt::white); }
    // The line from the circle to the label, behind both; null until first shown.
    QGraphicsPathItem *guide() const { return guide_; }
    void update_guide();

    // Pointer and label, for undo snapshots; apply takes only the keys present.
    QVariantMap extras_state() const;
    void apply_extras_state(const QVariantMap &);
    void copy_extras_from(const NumberItem &);
    bool hover_allowed() const { return can_show_hover(); }
protected:
    JIETUBA_DRAWING_EVENTS
private:
    void set_radius(qreal);
    QPointF default_label_anchor() const;
    void restyle_label();
    void invalidate_pointer();
    void paint_pointer(QPainter *);
    void paint_circle(QPainter *, const QRectF &, const QColor &);
    void paint_number(QPainter *, const QRectF &, const QColor &);
    void paint_selection_frame(QPainter *, const QRectF &);
    std::optional<QPointF> arrow_tip_, parked_arrow_tip_;
    NumberLabel *label_ = nullptr;
    QGraphicsPathItem *guide_ = nullptr;
    QString label_font_family_, arrow_style_ = ARROW_STYLES.front();
    int label_size_level_ = DEFAULT_LABEL_SIZE_LEVEL;
    bool label_outline_ = false, label_guide_ = false;
    mutable std::optional<QPainterPath> pointer_cache_;
};

// Text a number carries beside it. A child of the number, so it moves, hides and goes with it;
// its text and place belong to the number's state and are undone with the number.
class NumberLabel : public QGraphicsTextItem {
public:
    explicit NumberLabel(NumberItem *number);
    // Left end of the text's middle line, in the number's coordinates.
    QPointF anchor() const { return anchor_; }
    void set_anchor(const QPointF &);
    void begin_edit();
    bool is_editing() const { return textInteractionFlags() & Qt::TextEditable; }
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget *) override;
protected:
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override;
private:
    void place();
    void update_cursor();
    NumberItem *number_;
    QPointF anchor_;
    std::optional<QPointF> press_pos_, press_anchor_;
    bool dragged_ = false;
    // Another window took the focus; typing goes on when it comes back.
    bool window_away_ = false;
    QVariantMap state_before_edit_;
};
#undef JIETUBA_DRAWING_EVENTS
#undef JIETUBA_PEN_INTERFACE
}
