#pragma once
#include "canvas/items/drawing_items.h"
#include "canvas/handle_editor.h"
#include <QGraphicsTextItem>
#include <QUndoStack>
#include <QVariant>
#include <array>
#include <tuple>
#include <vector>

namespace jietuba {
// The laid-out glyphs of a text item as one path, decorations included, for outlines and shadows.
QPainterPath text_glyph_path(const QGraphicsTextItem &item);
// Explicit scene dependency for focus-out commit/delete. CanvasScene owns the
// actual undo stack; this adapter never creates a substitute stack per item.
class TextUndoContext {
public:
    virtual ~TextUndoContext() = default;
    virtual QUndoStack *text_undo_stack() const = 0;
};
class TextItem : public QGraphicsTextItem, public DrawingItemMixin {
public:
    static constexpr qreal TEXT_PADDING = 3, MIN_POINT_SIZE = 6, MAX_POINT_SIZE = 400, CLICK_MARGIN = 2;
    static constexpr int HANDLE_ROTATE = 210, HANDLE_DELETE = 211, HANDLE_SCALE = 212, SCALE_HANDLE_SIZE = 10;
    static constexpr qreal NORMAL_ANNOTATION_Z_VALUE = 20, ANNOTATION_Z_VALUE = 30, BACKGROUND_RADIUS = 6;
    static constexpr qreal MIN_INTERACTION_WIDTH = 30, FRAME_SIDE_GAP = 4, FRAME_INSET = 1;
    static constexpr std::array<qreal, 4> OUTLINE_WIDTH_LEVELS = {.04, .07, .12, .2};
    static constexpr qreal DEFAULT_OUTLINE_WIDTH = .07, SHADOW_OFFSET_RATIO = .08;
    static constexpr auto DEFAULT_OUTLINE_COLOR = "#FFFFFF";
    static constexpr auto DEFAULT_SHADOW_COLOR = "#66000000";
    TextItem(const QString &text, const QPointF &, const QFont &, const QColor &, bool always_on_top = true, bool provisional = false);
    bool has_outline = false, has_shadow = false, has_background = false;
    QColor outline_color = QColor(DEFAULT_OUTLINE_COLOR), shadow_color = QColor(DEFAULT_SHADOW_COLOR), background_color = QColor(Qt::white);
    qreal outline_width = DEFAULT_OUTLINE_WIDTH;
    bool _provisional = false, _is_closed = false;
    // Another window took the focus while typing; the edit goes on when the scene gives it back.
    bool _window_away = false;
    QString _text_before_edit;
    qreal font_point_size() const;
    void set_font_point_size(qreal);
    // units_per_pixel: item units per screen pixel, which spaces the handles on a small or zoomed-out item.
    std::vector<EditHandle> get_edit_handles(double units_per_pixel = 1) const;
    static qreal normalize_outline_width(const QVariant &);
    qreal outline_extent() const;
    qreal shadow_distance() const;
    std::tuple<bool, QColor, qreal> outline_state() const { return {has_outline, outline_color, outline_width}; }
    std::pair<bool, QColor> shadow_state() const { return {has_shadow, shadow_color}; }
    void set_outline(bool, std::optional<QColor> color = {}, std::optional<QVariant> width = {});
    void set_shadow(bool, std::optional<QColor> color = {});
    QRectF content_rect() const;
    QRectF interaction_rect() const;
    QRectF hit_rect() const;
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    bool contains(const QPointF &) const override;
    QPainterPath glyph_path() const;
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
    bool is_editing() const;
    bool is_edit_target() const override;
    void set_background(bool, std::optional<QColor> color = {}, std::optional<qreal> opacity = {});
    bool is_on_edge(const QPointF &) const;
    void apply_edit_cursor(const QPointF &);
    bool scale_stroke_width(qreal) override;
    bool set_visual_opacity(qreal) override;
    std::optional<qreal> get_visual_opacity() const override;
protected:
    void focusInEvent(QFocusEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
    // Pasted text takes the item's font and colour, not the formatting it was copied with.
    void keyPressEvent(QKeyEvent *) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override { hover_leave(event); }
private:
    static void add_decoration_lines(QPainterPath &, const QGlyphRun &, const QPointF &origin);
    friend QPainterPath text_glyph_path(const QGraphicsTextItem &);
    QPen outline_pen() const;
    void paint_shadow(QPainter *, const QPainterPath &, const std::optional<QPen> &);
    static QStyleOptionGraphicsItem text_paint_option(const QStyleOptionGraphicsItem &);
    void paint_interaction_frame(QPainter *);
};
}
