#include "canvas/items/arrow_item.h"
#include <QPainter>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace jietuba {
namespace {
constexpr qreal OPEN_HEAD_ANGLE = 27. * (3.14159265358979323846 / 180.);
// Caps each Bézier handle at a share of its segment so points dragged close together do not
// loop; a typical single bend stays under it, so storing its bend points keeps the shape.
constexpr qreal BEND_HANDLE_LIMIT = .75;
using Cubic = std::array<QPointF, 4>;
QPointF midpoint(const QPointF &a, const QPointF &b) { return {(a.x() + b.x()) / 2., (a.y() + b.y()) / 2.}; }
qreal distance(const QPointF &a, const QPointF &b) { return std::hypot(b.x() - a.x(), b.y() - a.y()); }
std::optional<QPointF> normalized(const QPointF &vec) {
    const auto length = std::hypot(vec.x(), vec.y());
    if (length > 1e-6) return vec / length;
    return std::nullopt;
}
QPointF capped(const QPointF &handle, qreal limit) {
    const auto length = std::hypot(handle.x(), handle.y());
    return length > limit && length > 0 ? handle * (limit / length) : handle;
}
// Cubic pieces through five points at equal parameter steps. The tangents are finite
// differences, exact for points on a quadratic, so a single bend converts without moving.
std::array<Cubic, 4> spline_segments(const std::array<QPointF, 5> &p) {
    std::array<QPointF, 5> d;
    d[0] = (-3 * p[0] + 4 * p[1] - p[2]) / 2;
    for (size_t i = 1; i < 4; ++i) d[i] = (p[i + 1] - p[i - 1]) / 2;
    d[4] = (3 * p[4] - 4 * p[3] + p[2]) / 2;
    std::array<Cubic, 4> segments;
    for (size_t i = 0; i < 4; ++i) {
        const auto limit = distance(p[i], p[i + 1]) * BEND_HANDLE_LIMIT;
        segments[i] = {p[i], p[i] + capped(d[i] / 3, limit), p[i + 1] - capped(d[i + 1] / 3, limit), p[i + 1]};
    }
    return segments;
}
QPointF cubic_at(const Cubic &c, qreal t) {
    const auto s = 1 - t;
    return s * s * s * c[0] + 3 * s * s * t * c[1] + 3 * s * t * t * c[2] + t * t * t * c[3];
}
// mid_index receives the sample at the third point, where the second piece ends.
std::vector<QPointF> sample_segments(const std::array<Cubic, 4> &segments, size_t *mid_index = nullptr) {
    std::vector<QPointF> points{segments[0][0]};
    for (size_t k = 0; k < segments.size(); ++k) {
        const auto &c = segments[k];
        const auto hull = distance(c[0], c[1]) + distance(c[1], c[2]) + distance(c[2], c[3]);
        const int steps = std::clamp(static_cast<int>(std::ceil(hull / 5.)), 4, 48);
        for (int i = 1; i <= steps; ++i) points.push_back(cubic_at(c, static_cast<qreal>(i) / steps));
        if (k == 1 && mid_index) *mid_index = points.size() - 1;
    }
    return points;
}
qreal polyline_length(const std::vector<QPointF> &points) {
    qreal length = 0;
    for (size_t i = 1; i < points.size(); ++i) length += distance(points[i - 1], points[i]);
    return length;
}
// A handle shrinks to nothing when a point sits on its neighbour; the next point along, then the chord, gives the direction.
std::optional<std::pair<QPointF, QPointF>> curve_tangents(const std::array<Cubic, 4> &segments, const QPointF &chord) {
    const auto first = [](std::initializer_list<QPointF> candidates) -> std::optional<QPointF> {
        for (const auto &vec : candidates)
            if (const auto u = normalized(vec)) return u;
        return std::nullopt;
    };
    const auto &head = segments.front(), &tail = segments.back();
    const auto u_start = first({head[1] - head[0], head[2] - head[0], head[3] - head[0], chord});
    const auto u_end = first({tail[3] - tail[2], tail[3] - tail[1], tail[3] - tail[0], chord});
    if (!u_start || !u_end) return std::nullopt;
    return std::make_pair(*u_start, *u_end);
}
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
    else { _control_modified = false; _control_pos = midpoint(start_pos, end_pos); _bend_points.reset(); }
}
void ArrowItem::set_positions(const QPointF &start, const QPointF &end) {
    start_pos = start; end_pos = end;
    if (!_control_modified) _control_pos = midpoint(start, end);
    update_geometry();
}
void ArrowItem::set_control_point(const QPointF &point) { _control_pos = point; _control_modified = true; update_geometry(); }
void ArrowItem::reset_control_point() { _control_modified = false; _control_pos = midpoint(start_pos, end_pos); _bend_points.reset(); update_geometry(); }
std::array<QPointF, 2> ArrowItem::bend_points() const {
    if (_bend_points) return *_bend_points;
    // On the drawn single bend: the quadratic from a through the control point to b (t = 1/2),
    // a and b being the ends moved in along their tangents by the head trims.
    auto a = start_pos, b = end_pos;
    const auto index = STYLES.indexOf(_arrow_style);
    if (const auto f = frame(); f && index >= 0) {
        const auto spec = STYLE_SPECS[static_cast<size_t>(index)];
        const auto m = metrics(f->length, spec.start, spec.end);
        a = f->start + f->u_start * head_trim(spec.start, m);
        b = f->end - f->u_end * head_trim(spec.end, m);
    }
    const QPointF bezier = 2 * control_pos() - (a + b) / 2;
    const auto at = [&](qreal t) { const auto s = 1 - t; return s * s * a + 2 * s * t * bezier + t * t * b; };
    return {{at(.25), at(.75)}};
}
void ArrowItem::set_bend_point(int index, const QPointF &point) {
    if (index < 0 || index > 1) return;
    if (!_bend_points) { _bend_points = bend_points(); _control_pos = control_pos(); _control_modified = true; }
    (*_bend_points)[static_cast<size_t>(index)] = point;
    update_geometry();
}
void ArrowItem::set_bend_points(const std::optional<std::array<QPointF, 2>> &points) {
    if (points && !_control_modified) { _control_pos = control_pos(); _control_modified = true; }
    _bend_points = points;
    update_geometry();
}
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
    auto length = std::max(std::hypot(v_start.x(), v_start.y()) + std::hypot(v_end.x(), v_end.y()), std::hypot(chord.x(), chord.y()));
    if (_bend_points) {
        // The curve through the bend points as stored; update_geometry refines the tangents. The length is
        // never below the single bend's measure, so storing a single bend's points keeps its head size.
        const auto segments = spline_segments({{start, (*_bend_points)[0], mid, (*_bend_points)[1], end}});
        length = std::max(length, polyline_length(sample_segments(segments)));
        if (length < .1) return {};
        const auto tangents = curve_tangents(segments, chord);
        if (!tangents) return {};
        return Frame{start, end, mid, tangents->first, tangents->second, length};
    }
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
std::pair<QPointF, QPointF> ArrowItem::bend_tangents(const Frame &f, qreal trim_start, qreal trim_end) const {
    // The shaft runs through the bend points. The heads follow the curve with the head trims undone the way
    // a single bend applies them, fading to nothing at the control point, so storing a single bend's points
    // keeps its heads. The trims lie along the tangents being solved, so this settles over a few rounds.
    auto tangents = std::make_pair(f.u_start, f.u_end);
    const auto &bends = *_bend_points;
    const QPointF chord(f.end.x() - f.start.x(), f.end.y() - f.start.y());
    for (int round = 0; round < 4; ++round) {
        const auto in_start = tangents.first * trim_start, in_end = tangents.second * -trim_end;
        const auto next = curve_tangents(spline_segments({{f.start, bends[0] - (.375 * in_start - .125 * in_end), f.mid,
            bends[1] - (.375 * in_end - .125 * in_start), f.end}}), chord);
        if (!next) break;
        tangents = *next;
    }
    return tangents;
}
QPainterPath ArrowItem::bend_shaft_path(const QPointF &a, const QPointF &b, const QPointF &mid, const std::array<QPointF, 2> &bends,
    const QPointF &u_a, const QPointF &u_b, qreal w_a, qreal w_b) {
    size_t mid_index = 0;
    const auto line = sample_segments(spline_segments({{a, bends[0], mid, bends[1], b}}), &mid_index);
    std::vector<qreal> along(line.size(), 0.);
    for (size_t i = 1; i < line.size(); ++i) along[i] = along[i - 1] + distance(line[i - 1], line[i]);
    const auto total = along.back(), at_mid = along[mid_index];
    if (total < 1e-6) return {};
    // Widths w_a, w_mid, w_b as on a single bend, linear by length on either side of the control point.
    const QPointF ab(b.x() - a.x(), b.y() - a.y()); const auto len2 = QPointF::dotProduct(ab, ab);
    const auto t = len2 < 1e-6 ? .5 : std::clamp(QPointF::dotProduct(mid - a, ab) / len2, 0., 1.);
    const auto w_mid = w_a + (w_b - w_a) * t;
    const auto width_at = [&](qreal s) {
        if (s <= at_mid) return at_mid > 1e-6 ? w_a + (w_mid - w_a) * s / at_mid : w_mid;
        return total - at_mid > 1e-6 ? w_mid + (w_b - w_mid) * (s - at_mid) / (total - at_mid) : w_mid;
    };
    QPolygonF left, right;
    auto tangent = u_a;
    for (size_t i = 0; i < line.size(); ++i) {
        if (i + 1 == line.size()) tangent = u_b;
        else if (i > 0) tangent = unit(line[i + 1] - line[i - 1]).value_or(tangent);
        const auto offset = perp(tangent) * (width_at(along[i]) / 2);
        left << line[i] + offset;
        right.prepend(line[i] - offset);
    }
    left << right;
    // Winding fill: on a bend tighter than the shaft the inner edge folds over itself.
    QPainterPath path; path.setFillRule(Qt::WindingFill); path.addPolygon(left); path.closeSubpath(); return path;
}
void ArrowItem::update_geometry() {
    const auto value = frame(); if (!value) return;
    const auto &f = *value; const auto index = STYLES.indexOf(_arrow_style);
    if (index < 0) throw std::invalid_argument("Unknown arrow style");
    const auto spec = STYLE_SPECS[static_cast<size_t>(index)]; const auto m = metrics(f.length, spec.start, spec.end);
    const auto trim_start = head_trim(spec.start, m), trim_end = head_trim(spec.end, m);
    const auto [u_start, u_end] = _bend_points ? bend_tangents(f, trim_start, trim_end) : std::make_pair(f.u_start, f.u_end);
    const QPointF a(f.start.x() + u_start.x() * trim_start, f.start.y() + u_start.y() * trim_start);
    const QPointF b(f.end.x() - u_end.x() * trim_end, f.end.y() - u_end.y() * trim_end);
    qreal w_a, w_b;
    if (spec.shaft == Shaft::Line) w_a = w_b = m.line_w;
    else if (spec.shaft == Shaft::Even) w_a = w_b = m.neck_w;
    else { w_a = spec.start != Head::None ? m.neck_w : m.tail_w; w_b = spec.end != Head::None ? m.neck_w : m.tail_w; }
    std::vector<QPainterPath> pieces;
    if (f.length - trim_start - trim_end > .5) {
        if (_bend_points) pieces.push_back(bend_shaft_path(a, b, f.mid, *_bend_points, u_start, u_end, w_a, w_b));
        else pieces.push_back(shaft_path(a, b, f.mid, u_start, u_end, w_a, w_b, QPointF(f.end.x() - f.start.x(), f.end.y() - f.start.y())));
    }
    pieces.push_back(head_path(spec.start, f.start, QPointF(-u_start.x(), -u_start.y()), m));
    pieces.push_back(head_path(spec.end, f.end, u_end, m));
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
