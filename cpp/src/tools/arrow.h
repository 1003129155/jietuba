#pragma once
#include "tools/base.h"

namespace jietuba {
class ArrowItem;

class ArrowTool : public Tool {
public:
    // Shorter drags are discarded.
    static constexpr double MIN_LENGTH = 10;
    QString id() const override { return "arrow"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    bool drawing = false;
    QPointF start_pos;
    ArrowItem *current_item = nullptr;
    QString arrow_style = "single";
};
}
