#pragma once
#include "tools/base.h"

namespace jietuba {
class RectItem;
class StrokeItem;

// Translucent pen three times wider, or a filled rectangle in rect mode.
// Holding Shift in a freehand stroke draws a straight line like the pen.
class HighlighterTool : public Tool {
public:
    static inline const QString MODE_FREEHAND = "freehand";
    static inline const QString MODE_RECT = "rect";
    static constexpr double MIN_SIZE = 10;
    QString id() const override { return "highlighter"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    // Switching tools mid-stroke keeps the freehand stroke.
    void on_deactivate(ToolContext &ctx) override;
    bool drawing = false;
    StrokeItem *current_item = nullptr;
    RectItem *rect_item = nullptr;
    std::optional<QPointF> start_pos;  // rect mode
    std::optional<FreehandStroke> stroke;  // freehand mode
    QString draw_mode = MODE_FREEHAND;
private:
    QString load_draw_mode(ToolContext &ctx) const;
    void start_rect_draw(const QPointF &pos, ToolContext &ctx);
    void finish_freehand(ToolContext &ctx);
};
}
