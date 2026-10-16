#pragma once
#include "canvas/undo.h"
#include "tools/base.h"

namespace jietuba {
class CanvasView;
class NumberItem;

// Numbered markers. The counter logic lives in NumberSequence (canvas/undo.h),
// shared with the undo commands; these statics keep the reference's names.
class NumberTool : public Tool {
public:
    static constexpr double RADIUS_SCALE = 2;
    // Below 6 the digits inside the circle become unreadable.
    static constexpr double MIN_WIDTH = 6;
    static constexpr double MAX_WIDTH = 72;
    QString id() const override { return "number"; }
    double min_width() const override { return MIN_WIDTH; }
    double max_width() const override { return MAX_WIDTH; }

    static int get_max_number(QGraphicsScene *scene, NumberItem *override_item = nullptr, int override_number = 0) {
        return NumberSequence::get_max_number(scene, override_item, override_number);
    }
    // The selected style, falling back to solid when unset.
    static QString get_style(const ToolContext &ctx);
    // Shared by capture and pin windows: restyle a selected number (undoable),
    // persist the default unless this is a cross-tool edit, and refresh the
    // cursor when the number tool is active. Returns whether the default was saved.
    static bool apply_style_change(const QString &style, CanvasView *view, QUndoStack *undo_stack);
    static double get_radius_for_width(double stroke_width) { return clamp_width_range(stroke_width, MIN_WIDTH, MAX_WIDTH) * RADIUS_SCALE; }
    static int get_next_number(QGraphicsScene *scene) { return NumberSequence::get_next_number(scene); }
    static int adjust_next_number(QGraphicsScene *scene, int step) { return NumberSequence::adjust_next_number(scene, step); }
    static int set_next_number(QGraphicsScene *scene, int next) { return NumberSequence::set_next_number(scene, next); }
    static int set_next_number_and_refresh(QGraphicsScene *scene, int next, bool force_cursor = true) {
        return NumberSequence::set_next_number_and_refresh(scene, next, force_cursor);
    }
    static int get_next_after_number_edit(QGraphicsScene *scene, NumberItem *item, int old_number, int new_number, std::optional<int> current = {}) {
        return NumberSequence::get_next_after_number_edit(scene, item, old_number, new_number, current);
    }
    static int assign_number_order(QGraphicsScene *scene, NumberItem *item) { return NumberSequence::assign_number_order(scene, item); }

    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
};
}
