#include "tools/base.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace jietuba {
double Tool::clamp_width_range(const QVariant &width, double min_width, double max_width) {
    bool ok = false;
    double value = width.metaType().id() == QMetaType::QString ? width.toString().trimmed().toDouble(&ok) : width.toDouble(&ok);
    if (!ok || !width.isValid() || width.isNull()) value = min_width;
    return std::max(min_width, std::min(max_width, value));
}

bool Tool::should_append_point(const std::optional<QPointF> &last, const QPointF &pos) {
    return !last || (pos - *last).manhattanLength() >= MIN_POINT_SPACING;
}

bool Tool::on_press(const QPointF &, Qt::MouseButton, ToolContext &) { return true; }
void Tool::on_move(const QPointF &, ToolContext &) {}
void Tool::on_release(const QPointF &, ToolContext &) {}

void Tool::on_activate(ToolContext &ctx) {
    if (ctx.settings_manager) load_settings(ctx);
    // The reference also tries scene.cursor_manager / ctx.canvas_widget, which
    // nothing assigns; the view updates the cursor from the tool-changed signal.
}

void Tool::on_deactivate(ToolContext &ctx) {
    if (ctx.settings_manager) save_settings(ctx);
}

void Tool::load_settings(ToolContext &ctx) {
    if (!ctx.settings_manager) return;
    const auto color = ctx.settings_manager->get_color(id());
    if (color.isValid()) ctx.color = color;
    const auto stroke_width = ctx.settings_manager->get_stroke_width(id());
    if (stroke_width != 0) ctx.stroke_width = clamp_width(stroke_width);
    ctx.opacity = ctx.settings_manager->get_opacity(id());
}

void Tool::save_settings(ToolContext &ctx) {
    if (!ctx.settings_manager) return;
    ctx.settings_manager->update_settings(id(), {{"color", ctx.color.name()}, {"stroke_width", ctx.stroke_width}, {"opacity", ctx.opacity}}, true);
}

QColor color_with_opacity(const QColor &source, std::optional<double> opacity) {
    QColor color(source);
    color.setAlphaF(std::max(0.0, std::min(1.0, opacity.value_or(1.0))));
    return color;
}

QRectF drag_rect(const QPointF &start, const QPointF &end, bool square) {
    QPointF target = end;
    if (square) {
        const double dx = end.x() - start.x(), dy = end.y() - start.y();
        const double side = std::max(std::abs(dx), std::abs(dy));
        target = QPointF(start.x() + std::copysign(side, dx), start.y() + std::copysign(side, dy));
    }
    return QRectF(start, target).normalized();
}

bool shift_held() { return QApplication::keyboardModifiers() & Qt::ShiftModifier; }

int shift_line_angle(const ToolContext &ctx) {
    return ctx.settings_manager ? ctx.settings_manager->get_shift_line_angle() : DEFAULT_SHIFT_LINE_ANGLE;
}

QPointF snap_line_end(const QPointF &anchor, const QPointF &pos, int step_degrees) {
    const double dx = pos.x() - anchor.x(), dy = pos.y() - anchor.y();
    if (!step_degrees || (dx == 0 && dy == 0)) return pos;
    const double step = qDegreesToRadians(double(step_degrees));
    // nearbyint rounds half to even like the reference's round(): an exact diagonal under a
    // 90-degree step goes horizontal in both.
    const double angle = std::nearbyint(std::atan2(dy, dx) / step) * step;
    // cos/sin leave 1e-17 tails on the axes, which would tilt a horizontal line's end off the anchor's y.
    const auto trim = [](double value) { return std::nearbyint(value * 1e12) / 1e12; };
    const double ux = trim(std::cos(angle)), uy = trim(std::sin(angle));
    const double length = dx * ux + dy * uy;
    return {anchor.x() + ux * length, anchor.y() + uy * length};
}

namespace {
// Pulls cur toward (prev + 2·cur + next) / 4, weighted by spacing.
QPointF relax(const QPointF &prev, const QPointF &cur, const QPointF &next) {
    const double gap = std::max(std::hypot(cur.x() - prev.x(), cur.y() - prev.y()), std::hypot(next.x() - cur.x(), next.y() - cur.y()));
    double weight = (SMOOTH_NONE_GAP - gap) / (SMOOTH_NONE_GAP - SMOOTH_FULL_GAP);
    if (weight <= 0) return cur;
    weight = std::min(weight, 1.0) * 0.25;
    return {cur.x() + (prev.x() - 2 * cur.x() + next.x()) * weight, cur.y() + (prev.y() - 2 * cur.y() + next.y()) * weight};
}

// Bezier control points of the centripetal Catmull-Rom segment p1 -> p2.
std::pair<QPointF, QPointF> catmull_rom(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3) {
    const auto knot = [](const QPointF &a, const QPointF &b) { return std::sqrt(std::max(std::hypot(b.x() - a.x(), b.y() - a.y()), 1e-6)); };
    const double d1 = knot(p0, p1), d2 = knot(p1, p2), d3 = knot(p2, p3);
    const QPointF m1 = ((p1 - p0) / d1 - (p2 - p0) / (d1 + d2) + (p2 - p1) / d2) * d2;
    const QPointF m2 = ((p2 - p1) / d2 - (p3 - p1) / (d2 + d3) + (p3 - p2) / d3) * d2;
    return {p1 + m1 / 3, p2 - m2 / 3};
}
}

FreehandStroke::FreehandStroke(const QPointF &start, int angle_step, bool smooth)
    : path(start), angle_step(angle_step), smooth(smooth), last_point(start) {
    begin_run();
}

int FreehandStroke::take_dirty_from() {
    const int count = path.elementCount();
    const auto dirty = dirty_from_;
    dirty_from_.reset();
    return dirty.value_or(count);
}

void FreehandStroke::mark_dirty(int index) {
    if (!dirty_from_ || index < *dirty_from_) dirty_from_ = index;
}

bool FreehandStroke::finish() {
    if (path.elementCount() > 1) return false;
    const auto start = path.elementAt(0);
    mark_dirty(1);
    path.lineTo(start.x + DOT_SEGMENT, start.y);
    return true;
}

bool FreehandStroke::extend(const QPointF &pos, bool straight) {
    if (!straight) {
        if (line_anchor_) {
            line_anchor_.reset();
            line_open_ = false;
            begin_run();
        }
        if (!Tool::should_append_point(last_point, pos)) return false;
        last_point = pos;
        if (smooth) {
            append_smooth(pos);
        } else {
            mark_dirty(path.elementCount());
            path.lineTo(pos);
        }
        return true;
    }
    if (!line_anchor_) line_anchor_ = last_point;
    const QPointF end = snap_line_end(*line_anchor_, pos, angle_step);
    if (end == last_point) return false;
    if (line_open_) {
        mark_dirty(path.elementCount() - 1);
        path.setElementPositionAt(path.elementCount() - 1, end.x(), end.y());
    } else {
        mark_dirty(path.elementCount());
        path.lineTo(end);
        line_open_ = true;
    }
    last_point = end;
    return true;
}

// A new freehand run starts at the path's current last point, which no longer moves.
void FreehandStroke::begin_run() {
    const int last = path.elementCount() - 1;
    const auto element = path.elementAt(last);
    run_base_ = last;
    raw_ = {QPointF(element.x, element.y)};
    once_ = raw_;
    twice_ = raw_;
}

void FreehandStroke::append_smooth(const QPointF &point) {
    raw_.append(point);
    const int n = int(raw_.size()) - 1;
    // The last sample cannot be relaxed (no successor); a new one pulls it, rippling two layers back.
    if (n >= 2) once_[n - 1] = relax(raw_[n - 2], raw_[n - 1], raw_[n]);
    once_.append(point);
    for (int i = std::max(1, n - 2); i < n; ++i) twice_[i] = relax(once_[i - 1], once_[i], once_[i + 1]);
    twice_.append(point);
    // Segment i joins twice[i] and twice[i + 1] and depends on twice[i - 1 .. i + 2].
    mark_dirty(run_base_ + 1 + 3 * std::max(0, n - 4));
    for (int i = std::max(0, n - 4); i < n; ++i) {
        const auto [c1, c2] = catmull_rom(twice_[std::max(i - 1, 0)], twice_[i], twice_[i + 1], twice_[std::min(i + 2, n)]);
        const QPointF end = twice_[i + 1];
        if (i == n - 1) {
            path.cubicTo(c1, c2, end);
        } else {
            const int index = run_base_ + 1 + 3 * i;
            path.setElementPositionAt(index, c1.x(), c1.y());
            path.setElementPositionAt(index + 1, c2.x(), c2.y());
            path.setElementPositionAt(index + 2, end.x(), end.y());
        }
    }
}

void apply_line_style(QPen &pen, const QString &line_style) {
    if (line_style == "dashed") {
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({3, 2});
    } else if (line_style == "dashed_dense") {
        pen.setStyle(Qt::DashLine);
        pen.setDashPattern({1, 2});
    } else {
        pen.setStyle(Qt::SolidLine);
        pen.setDashPattern({});
    }
}
}
