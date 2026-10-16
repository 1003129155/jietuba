#pragma once
#include "tools/base.h"

namespace jietuba {
class RectItem;

// Shift draws a square.
class RectTool : public Tool {
public:
    // Drags smaller than this are discarded.
    static constexpr double MIN_SIZE = 10;
    QString id() const override { return "rect"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    bool drawing = false;
    QPointF start_pos;
    RectItem *current_item = nullptr;
    QString line_style = "solid";
    double corner_radius = 0;
};
}
