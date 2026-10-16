#pragma once
#include "tools/base.h"

namespace jietuba {
class EllipseItem;

// Shift draws a circle.
class EllipseTool : public Tool {
public:
    static constexpr double MIN_SIZE = 10;
    QString id() const override { return "ellipse"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    bool drawing = false;
    QPointF start_pos;
    EllipseItem *current_item = nullptr;
    QString line_style = "solid";
};
}
