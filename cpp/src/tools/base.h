#pragma once
#include <QColor>
#include <QList>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVariant>
#include <optional>

namespace jietuba {
class CanvasScene;
class CommandUndoStack;
class SelectionModel;
class ToolSettingsManager;

// Everything a tool needs.
struct ToolContext {
    CanvasScene *scene = nullptr;
    SelectionModel *selection = nullptr;
    CommandUndoStack *undo_stack = nullptr;
    QColor color;
    double stroke_width = 0;
    double opacity = 1.0;
    ToolSettingsManager *settings_manager = nullptr;
};

class Tool {
public:
    // The toolbar broadcasts one width to every tool, but each accepts its own
    // range; clamping at the entry keeps ctx.stroke_width valid for the current tool.
    static constexpr double MIN_WIDTH = 1;
    static constexpr double MAX_WIDTH = 99;
    // Minimum spacing between freehand points (scene = physical pixels). High-rate
    // mice report many sub-pixel moves that would make every redraw O(N^2).
    static constexpr double MIN_POINT_SPACING = 1.5;

    virtual ~Tool() = default;
    virtual QString id() const { return "base"; }
    virtual double min_width() const { return MIN_WIDTH; }
    virtual double max_width() const { return MAX_WIDTH; }
    double clamp_width(const QVariant &width) const { return clamp_width_range(width, min_width(), max_width()); }
    static double clamp_width_range(const QVariant &width, double min_width, double max_width);
    // Compared with the last accepted point, not the last event, so slow strokes
    // accumulate distance instead of being dropped entirely.
    static bool should_append_point(const std::optional<QPointF> &last, const QPointF &pos);

    // false refuses the stroke; the reference treats only an explicit False that way.
    virtual bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx);
    virtual void on_move(const QPointF &pos, ToolContext &ctx);
    virtual void on_release(const QPointF &pos, ToolContext &ctx);
    // Loads settings first, then the view's cursor follows the tool.
    virtual void on_activate(ToolContext &ctx);
    virtual void on_deactivate(ToolContext &ctx);
    virtual void load_settings(ToolContext &ctx);
    virtual void save_settings(ToolContext &ctx);
};

QColor color_with_opacity(const QColor &source, std::optional<double> opacity);
// Rectangle dragged from start to end; square takes the longer side toward the cursor.
QRectF drag_rect(const QPointF &start, const QPointF &end, bool square = false);
// Whether Shift is held for the input event being processed.
bool shift_held();
// The line-style setting ("solid", "dashed", "dashed_dense") shared by pen, rect and ellipse.
void apply_line_style(QPen &pen, const QString &line_style);
// Degrees between the directions a Shift-held straight line snaps to; 0 is any angle.
int shift_line_angle(const ToolContext &ctx);
// End of a straight line dragged from anchor toward pos: the direction is the nearest multiple of
// step_degrees and the end is pos projected onto it. A step of 0 keeps pos.
QPointF snap_line_end(const QPointF &anchor, const QPointF &pos, int step_degrees);

// Smoothing only touches dense samples: neighbours up to FULL apart are fully averaged, those
// NONE or more apart stay put. Slow strokes crowd samples, and the integer-pixel steps are the
// jitter between them; fast strokes are sparse, averaging would pull bends in, and the samples
// themselves are the shape, so the curve just passes through them.
inline constexpr double SMOOTH_FULL_GAP = 3.0;
inline constexpr double SMOOTH_NONE_GAP = 8.0;

// Length of the segment a stroke that never moved gets. Qt drops a lineTo onto the current point;
// a segment this short still draws both round caps.
inline constexpr double DOT_SEGMENT = 0.01;

// Freehand stroke whose Shift-held part is a straight line.
//
// When smooth, samples are relaxed twice by spacing and joined by a centripetal Catmull-Rom curve
// through every one. A new sample changes only the last four segments, rewritten in place. The
// start, the end and both ends of straight lines never move.
//
// A straight line starts at the stroke's last point when Shift goes down and stays the path's
// last element, so dragging it around adds no points; releasing Shift fixes it.
class FreehandStroke {
public:
    FreehandStroke(const QPointF &start, int angle_step, bool smooth = false);
    // Extends the stroke to pos, straight while Shift is held; false when the path is unchanged.
    bool extend(const QPointF &pos, bool straight);
    // First element changed since the last call; the element count when nothing changed.
    int take_dirty_from();
    // On release: a stroke that never moved becomes a dot where it was pressed. False when the
    // path is unchanged.
    bool finish();
    QPainterPath path;
    int angle_step;
    bool smooth;
    QPointF last_point;
private:
    void begin_run();
    void append_smooth(const QPointF &point);
    void mark_dirty(int index);
    std::optional<QPointF> line_anchor_;
    bool line_open_ = false;
    std::optional<int> dirty_from_;
    int run_base_ = 0;
    QList<QPointF> raw_, once_, twice_;
};
}
