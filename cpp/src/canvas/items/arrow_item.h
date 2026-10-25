#pragma once
#include "canvas/items/drawing_items.h"
#include <QStringList>
#include <array>
#include <optional>

namespace jietuba {
class ArrowItem : public QGraphicsPathItem, public DrawingItemMixin {
public:
    static constexpr qreal CLICK_MARGIN = 4;
    static constexpr qreal SWEPT_NECK_LEN_RATIO = 1.62, SWEPT_BARB_DEPTH_RATIO = 1.96, SWEPT_NECK_HALF_RATIO = .4;
    static inline const QString STYLE_SINGLE = "single";
    static inline const QString STYLE_DOUBLE = "double";
    static inline const QString STYLE_HOLLOW = "hollow";
    static inline const QString STYLE_LINE = "line";
    static inline const QString STYLE_LINE_DOUBLE = "line_double";
    static inline const QString STYLE_TRIANGLE = "triangle";
    static inline const QString STYLE_TRIANGLE_DOUBLE = "triangle_double";
    static inline const QString STYLE_BAR = "bar";
    static inline const QString STYLE_BAR_ARROW = "bar_arrow";
    static const QStringList STYLES;
    static QString normalize_style(const QString &);
    ArrowItem(const QPointF &start, const QPointF &end, const QPen &, const QString &style = "single");
    QPointF start_pos, end_pos, _control_pos;
    bool _control_modified = false;
    // Points a quarter and three quarters along the curve; stored once either is dragged,
    // after which the curve runs through all five points and can bend both ways.
    std::optional<std::array<QPointF, 2>> _bend_points;
    qreal base_width;
    QColor color;
    QString _arrow_style;
    void setPath(const QPainterPath &);
    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    QString arrow_style() const { return _arrow_style; }
    void set_arrow_style(const QString &);
    QPointF control_pos() const;
    void set_control_pos(std::optional<QPointF>);
    void set_positions(const QPointF &, const QPointF &);
    void set_control_point(const QPointF &);
    void reset_control_point();
    QPointF get_control_point() const { return control_pos(); }
    bool is_curved() const { return _control_modified; }
    // The stored bend points, or where they lie on the current single bend.
    std::array<QPointF, 2> bend_points() const;
    bool has_bend_points() const { return _bend_points.has_value(); }
    void set_bend_point(int index, const QPointF &);
    void set_bend_points(const std::optional<std::array<QPointF, 2>> &);
    void update_geometry();
    void paint(QPainter *, const QStyleOptionGraphicsItem *, QWidget * = nullptr) override;
    void set_stroke_width(qreal) override;
    bool scale_stroke_width(qreal) override;
    bool set_visual_opacity(qreal) override;
    std::optional<qreal> get_stroke_width() const override { return base_width; }
    std::optional<qreal> get_visual_opacity() const override;
protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *e) override { hover_enter(e); }
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override { hover_move(e); }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override { hover_leave(e); }
private:
    enum class Head { None, Solid, Swept, Open, Bar, BarSolid };
    enum class Shaft { Taper, Even, Line };
    struct Spec { Shaft shaft; Head start, end; bool hollow; };
    struct Frame { QPointF start, end, mid, u_start, u_end; qreal length; };
    struct Metrics {
        qreal line_w, head_half, head_len, wing_len, wing_w, open_notch, bar_half;
        qreal sweep_len, sweep_depth, sweep_neck_half, neck_w, tail_w, outline_w;
    };
    static const std::array<Spec, 9> STYLE_SPECS;
    static std::optional<QPointF> unit(const QPointF &, std::optional<QPointF> fallback = {});
    static QPointF perp(const QPointF &);
    std::optional<Frame> frame() const;
    Metrics metrics(qreal length, Head start, Head end) const;
    qreal head_trim(Head, const Metrics &) const;
    QPainterPath head_path(Head, const QPointF &tip, const QPointF &out_dir, const Metrics &) const;
    static QPainterPath solid_head_path(const QPointF &, const QPointF &, qreal head_len, qreal head_half);
    static QPainterPath swept_head_path(const QPointF &, const QPointF &, const Metrics &);
    static QPainterPath open_head_path(const QPointF &, const QPointF &, qreal wing_len, qreal wing_w, qreal notch);
    static QPainterPath bar_path(const QPointF &, const QPointF &, qreal bar_half, qreal thickness);
    static QPainterPath shaft_path(const QPointF &a, const QPointF &b, const QPointF &mid,
        const QPointF &u_a, const QPointF &u_b, qreal w_a, qreal w_b, const QPointF &chord);
    std::pair<QPointF, QPointF> bend_tangents(const Frame &, qreal trim_start, qreal trim_end) const;
    static QPainterPath bend_shaft_path(const QPointF &a, const QPointF &b, const QPointF &mid, const std::array<QPointF, 2> &bends,
        const QPointF &u_a, const QPointF &u_b, qreal w_a, qreal w_b);
    mutable std::optional<QPainterPath> shape_cache_;
    std::optional<QPainterPath> hit_path_;
};
}
