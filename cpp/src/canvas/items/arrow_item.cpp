#include "canvas/items/arrow_item.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace jietuba {
namespace {
constexpr qreal OPEN_HEAD_ANGLE = 27. * (3.14159265358979323846 / 180.);
QPointF midpoint(const QPointF &a, const QPointF &b) { return {(a.x() + b.x()) / 2., (a.y() + b.y()) / 2.}; }
}
const QStringList ArrowItem::STYLES = {"single", "double", "hollow", "line", "line_double", "triangle", "triangle_double", "bar", "bar_arrow"};
const std::array<ArrowItem::Spec, 9> ArrowItem::STYLE_SPECS = {{
    {Shaft::Taper, Head::None, Head::Swept, false},
    {Shaft::Even, Head::Swept, Head::Swept, false},
    {Shaft::Taper, Head::None, Head::Swept, true},
    {Shaft::Line, Head::None, Head::Open, false},
    {Shaft::Line, Head::Open, Head::Open, false},
    {Shaft::Line, Head::None, Head::Solid, false},
    {Shaft::Line, Head::Solid, Head::Solid, false},
    {Shaft::Line, Head::Bar, Head::Bar, false},
    {Shaft::Line, Head::BarSolid, Head::BarSolid, false},
}};
QString ArrowItem::normalize_style(const QString &value) { return STYLES.contains(value) ? value : QString("single"); }
ArrowItem::ArrowItem(const QPointF &start, const QPointF &end, const QPen &pen, const QString &style)
    : DrawingItemMixin(this), start_pos(start), end_pos(end), _control_pos(midpoint(start, end)),
      base_width(pen.width()), color(pen.color()), _arrow_style(normalize_style(style)) {
    setPen(QPen(Qt::NoPen)); setBrush(pen.color()); setZValue(20); update_geometry();
}
void ArrowItem::setPath(const QPainterPath &path) { QGraphicsPathItem::setPath(path); shape_cache_.reset(); }
QRectF ArrowItem::boundingRect() const { return path().boundingRect().adjusted(-CLICK_MARGIN, -CLICK_MARGIN, CLICK_MARGIN, CLICK_MARGIN); }
QPainterPath ArrowItem::shape() const {
    if (shape_cache_) return *shape_cache_;
    const auto path = hit_path_.value_or(this->path()); if (path.isEmpty()) return path;
    QPainterPathStroker stroker; stroker.setWidth(CLICK_MARGIN * 2); stroker.setCapStyle(Qt::RoundCap); stroker.setJoinStyle(Qt::RoundJoin);
    shape_cache_ = path.united(stroker.createStroke(path)); return *shape_cache_;
}
void ArrowItem::set_arrow_style(const QString &value) { if (STYLES.contains(value)) { _arrow_style = value; update_geometry(); } }
QPointF ArrowItem::control_pos() const { return _control_modified ? _control_pos : midpoint(start_pos, end_pos); }
void ArrowItem::set_control_pos(std::optional<QPointF> value) {
    if (value) _control_pos = *value;
    else { _control_modified = false; _control_pos = midpoint(start_pos, end_pos); }
}
void ArrowItem::set_positions(const QPointF &start, const QPointF &end) {
    start_pos = start; end_pos = end;
    if (!_control_modified) _control_pos = midpoint(start, end);
    update_geometry();
}
void ArrowItem::set_control_point(const QPointF &point) { _control_pos = point; _control_modified = true; update_geometry(); }
void ArrowItem::reset_control_point() { _control_modified = false; _control_pos = midpoint(start_pos, end_pos); update_geometry(); }
std::optional<QPointF> ArrowItem::unit(const QPointF &vec, std::optional<QPointF> fallback) {
    const auto length = std::hypot(vec.x(), vec.y());
    if (length > 1e-6) return QPointF(vec.x() / length, vec.y() / length);
    return fallback ? unit(*fallback) : std::nullopt;
}
QPointF ArrowItem::perp(const QPointF &unit) { return {-unit.y(), unit.x()}; }
std::optional<ArrowItem::Frame> ArrowItem::frame() const {
    const auto start = start_pos, end = end_pos, mid = control_pos();
    const QPointF bezier(2 * mid.x() - .5 * start.x() - .5 * end.x(), 2 * mid.y() - .5 * start.y() - .5 * end.y());
    const QPointF chord(end.x() - start.x(), end.y() - start.y());
    const QPointF v_start(bezier.x() - start.x(), bezier.y() - start.y()), v_end(end.x() - bezier.x(), end.y() - bezier.y());
    const auto length = std::max(std::hypot(v_start.x(), v_start.y()) + std::hypot(v_end.x(), v_end.y()), std::hypot(chord.x(), chord.y()));
    if (length < .1) return {};
    const auto u_start = unit(v_start, chord), u_end = unit(v_end, chord);
    if (!u_start || !u_end) return {};
    return Frame{start, end, mid, *u_start, *u_end, length};
}
ArrowItem::Metrics ArrowItem::metrics(qreal length, Head head_start, Head head_end) const {
    const auto base = std::max(1., base_width);
    auto head_half = std::max(base * 2.2, 8.), head_len = head_half * 1.75;
    auto solid = [](Head h) { return h == Head::Solid || h == Head::BarSolid; };
    const auto solid_heads = static_cast<int>(solid(head_start)) + static_cast<int>(solid(head_end));
    const auto swept_heads = static_cast<int>(head_start == Head::Swept) + static_cast<int>(head_end == Head::Swept);
    if (solid_heads || swept_heads) {
        const auto budget = length * (solid_heads + swept_heads == 1 ? .45 : .34);
        auto reach = head_len; if (swept_heads) reach = std::max(reach, head_half * SWEPT_BARB_DEPTH_RATIO);
        if (reach > budget) { const auto shrink = budget / reach; head_half *= shrink; head_len *= shrink; }
    }
    const auto line_w = std::max(base * .9, 2.);
    auto wing_len = std::max(base * 4.2, 15.);
    const auto open_heads = static_cast<int>(head_start == Head::Open) + static_cast<int>(head_end == Head::Open);
    if (open_heads) wing_len = std::min(wing_len, length * (open_heads == 1 ? .45 : .35));
    const auto wing_w = std::min(line_w, wing_len * std::tan(OPEN_HEAD_ANGLE) * .9);
    const auto neck_half = head_half * SWEPT_NECK_HALF_RATIO;
    return {line_w, head_half, head_len, wing_len, wing_w, wing_w / std::sin(OPEN_HEAD_ANGLE), std::max(base * 2., 8.),
        head_half * SWEPT_NECK_LEN_RATIO, head_half * SWEPT_BARB_DEPTH_RATIO, neck_half, neck_half * 2, 0., std::max(base * .34, 1.6)};
}
qreal ArrowItem::head_trim(Head kind, const Metrics &m) const {
    if (kind == Head::Solid) return m.head_len * .92;
    if (kind == Head::Swept) return m.sweep_len * .92;
    if (kind == Head::BarSolid) return m.line_w * .5 + m.head_len * .8 * .92;
    if (kind == Head::Open) return std::min(m.line_w * 1.4, m.open_notch);
    return 0.;
}
QPainterPath ArrowItem::head_path(Head kind, const QPointF &tip, const QPointF &out, const Metrics &m) const {
    if (kind == Head::Solid) return solid_head_path(tip, out, m.head_len, m.head_half);
    if (kind == Head::Swept) return swept_head_path(tip, out, m);
    if (kind == Head::Open) return open_head_path(tip, out, m.wing_len, m.wing_w, m.open_notch);
    if (kind == Head::Bar) return bar_path(tip, out, m.bar_half, m.line_w);
    if (kind == Head::BarSolid) {
        const auto path = bar_path(tip, out, m.bar_half, m.line_w);
        const QPointF inner_tip(tip.x() - out.x() * m.line_w * .5, tip.y() - out.y() * m.line_w * .5);
        return path.united(solid_head_path(inner_tip, out, m.head_len * .8, m.head_half * .8));
    }
    return {};
}
QPainterPath ArrowItem::solid_head_path(const QPointF &tip, const QPointF &out, qreal length, qreal half) {
    const auto p = perp(out); const auto x = tip.x() - out.x() * length, y = tip.y() - out.y() * length;
    QPainterPath path; path.moveTo(tip); path.lineTo(x + p.x() * half, y + p.y() * half);
    path.lineTo(x - p.x() * half, y - p.y() * half); path.closeSubpath(); return path;
}
QPainterPath ArrowItem::swept_head_path(const QPointF &tip, const QPointF &out, const Metrics &m) {
    const auto p = perp(out);
    const QPointF neck(tip.x() - out.x() * m.sweep_len, tip.y() - out.y() * m.sweep_len);
    const QPointF barb(tip.x() - out.x() * m.sweep_depth, tip.y() - out.y() * m.sweep_depth);
    QPainterPath path; path.moveTo(neck.x() + p.x() * m.sweep_neck_half, neck.y() + p.y() * m.sweep_neck_half);
    path.lineTo(barb.x() + p.x() * m.head_half, barb.y() + p.y() * m.head_half); path.lineTo(tip);
    path.lineTo(barb.x() - p.x() * m.head_half, barb.y() - p.y() * m.head_half);
    path.lineTo(neck.x() - p.x() * m.sweep_neck_half, neck.y() - p.y() * m.sweep_neck_half); path.closeSubpath(); return path;
}
QPainterPath ArrowItem::open_head_path(const QPointF &tip, const QPointF &out, qreal length, qreal width, qreal notch) {
    const auto cos = std::cos(OPEN_HEAD_ANGLE), sin = std::sin(OPEN_HEAD_ANGLE);
    const auto back_x = -out.x(), back_y = -out.y();
    const QPointF left(back_x * cos - back_y * sin, back_x * sin + back_y * cos);
    const QPointF right(back_x * cos + back_y * sin, -back_x * sin + back_y * cos);
    const auto p_left = perp(left); const QPointF in_left(-p_left.x(), -p_left.y()); const auto in_right = perp(right);
    auto wing_end = [&](const QPointF &direction, const QPointF &inward) {
        const QPointF outer(tip.x() + direction.x() * length, tip.y() + direction.y() * length);
        return std::make_pair(outer, QPointF(outer.x() + inward.x() * width, outer.y() + inward.y() * width));
    };
    const auto l = wing_end(left, in_left), r = wing_end(right, in_right);
    QPainterPath path; path.moveTo(tip); path.lineTo(l.first); path.lineTo(l.second);
    path.lineTo(tip.x() + back_x * notch, tip.y() + back_y * notch); path.lineTo(r.second); path.lineTo(r.first); path.closeSubpath(); return path;
}
QPainterPath ArrowItem::bar_path(const QPointF &at, const QPointF &out, qreal half, qreal thickness) {
    const auto p = perp(out); const auto t = thickness / 2.;
    QPainterPath path; path.moveTo(at.x() + p.x() * half + out.x() * t, at.y() + p.y() * half + out.y() * t);
    path.lineTo(at.x() - p.x() * half + out.x() * t, at.y() - p.y() * half + out.y() * t);
    path.lineTo(at.x() - p.x() * half - out.x() * t, at.y() - p.y() * half - out.y() * t);
    path.lineTo(at.x() + p.x() * half - out.x() * t, at.y() + p.y() * half - out.y() * t); path.closeSubpath(); return path;
}
QPainterPath ArrowItem::shaft_path(const QPointF &a, const QPointF &b, const QPointF &mid, const QPointF &u_a, const QPointF &u_b, qreal w_a, qreal w_b, const QPointF &chord) {
    const QPointF ab(b.x() - a.x(), b.y() - a.y()); const auto len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1e-6) return {};
    auto t = ((mid.x() - a.x()) * ab.x() + (mid.y() - a.y()) * ab.y()) / len2; t = std::min(1., std::max(0., t));
    const auto w_mid = w_a + (w_b - w_a) * t;
    const auto u_mid = unit(chord, ab).value_or(u_a); const auto p_a = perp(u_a), p_b = perp(u_b), p_mid = perp(u_mid);
    auto edge = [&](qreal sign) {
        const QPointF pa(a.x() + sign * p_a.x() * w_a / 2, a.y() + sign * p_a.y() * w_a / 2);
        const QPointF pb(b.x() + sign * p_b.x() * w_b / 2, b.y() + sign * p_b.y() * w_b / 2);
        const QPointF pm(mid.x() + sign * p_mid.x() * w_mid / 2, mid.y() + sign * p_mid.y() * w_mid / 2);
        const QPointF ctrl(2 * pm.x() - .5 * pa.x() - .5 * pb.x(), 2 * pm.y() - .5 * pa.y() - .5 * pb.y());
        return std::array<QPointF, 3>{pa, ctrl, pb};
    };
    const auto up = edge(1), dn = edge(-1);
    QPainterPath path; path.moveTo(up[0]); path.quadTo(up[1], up[2]); path.lineTo(dn[2]); path.quadTo(dn[1], dn[0]); path.closeSubpath(); return path;
}
void ArrowItem::update_geometry() {
    const auto value = frame(); if (!value) return;
    const auto &f = *value; const auto index = STYLES.indexOf(_arrow_style);
    if (index < 0) throw std::invalid_argument("Unknown arrow style");
    const auto spec = STYLE_SPECS[static_cast<size_t>(index)]; const auto m = metrics(f.length, spec.start, spec.end);
    const auto trim_start = head_trim(spec.start, m), trim_end = head_trim(spec.end, m);
    const QPointF a(f.start.x() + f.u_start.x() * trim_start, f.start.y() + f.u_start.y() * trim_start);
    const QPointF b(f.end.x() - f.u_end.x() * trim_end, f.end.y() - f.u_end.y() * trim_end);
    qreal w_a, w_b;
    if (spec.shaft == Shaft::Line) w_a = w_b = m.line_w;
    else if (spec.shaft == Shaft::Even) w_a = w_b = m.neck_w;
    else { w_a = spec.start != Head::None ? m.neck_w : m.tail_w; w_b = spec.end != Head::None ? m.neck_w : m.tail_w; }
    std::vector<QPainterPath> pieces;
    if (f.length - trim_start - trim_end > .5) pieces.push_back(shaft_path(a, b, f.mid, f.u_start, f.u_end, w_a, w_b, QPointF(f.end.x() - f.start.x(), f.end.y() - f.start.y())));
    pieces.push_back(head_path(spec.start, f.start, QPointF(-f.u_start.x(), -f.u_start.y()), m));
    pieces.push_back(head_path(spec.end, f.end, f.u_end, m));
    QPainterPath filled;
    for (const auto &piece : pieces) if (!piece.isEmpty()) filled = filled.isEmpty() ? piece : filled.united(piece);
    if (filled.isEmpty()) return;
    hit_path_ = filled;
    if (spec.hollow) {
        QPainterPathStroker stroker; stroker.setWidth(m.outline_w * 2); stroker.setJoinStyle(Qt::MiterJoin); stroker.setMiterLimit(8);
        setPath(stroker.createStroke(filled).intersected(filled).simplified());
    } else setPath(filled);
}
void ArrowItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setRenderHint(QPainter::Antialiasing); p->setPen(Qt::NoPen); p->setBrush(color); p->drawPath(path());
    if (auto selection = selection_frame_pen()) {
        selection->setCapStyle(Qt::RoundCap); selection->setJoinStyle(Qt::RoundJoin);
        p->setBrush(Qt::NoBrush); p->setPen(*selection); p->drawPath(path());
    }
}
void ArrowItem::set_stroke_width(qreal width) { base_width = std::max(1., width); update_geometry(); update(); }
bool ArrowItem::scale_stroke_width(qreal scale) { base_width = std::max(1., base_width * scale); update_geometry(); update(); return true; }
bool ArrowItem::set_visual_opacity(qreal opacity) { color.setAlphaF(std::max(0., std::min(1., opacity))); setOpacity(1.); update(); return true; }
std::optional<qreal> ArrowItem::get_visual_opacity() const { const auto direct = std::max(0., std::min(1., opacity())); return direct < .999 ? direct : color.alphaF(); }
}
