#pragma once
#include "tools/base.h"

namespace jietuba {
class StrokeItem;

// Hands a finished stroke to the undo stack. It is already in the scene, so pushing the
// AddItemCommand does not add it again.
void commit_stroke(ToolContext &ctx, StrokeItem *item, FreehandStroke &stroke);

// Freehand pen; holding Shift at any point draws a straight line from there (see shift_line_angle).
class PenTool : public Tool {
public:
    QString id() const override { return "pen"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    // Switching tools mid-stroke keeps the stroke.
    void on_deactivate(ToolContext &ctx) override;
    bool drawing = false;
    StrokeItem *current_item = nullptr;
    std::optional<FreehandStroke> stroke;
    QString line_style = "solid";
private:
    void finish(ToolContext &ctx);
};
}
