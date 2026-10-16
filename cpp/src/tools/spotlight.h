#pragma once
#include "tools/base.h"

namespace jietuba {
class SpotlightItem;

// Drags a hole; everything outside every hole is darkened by the shared curtain.
class SpotlightTool : public Tool {
public:
    static constexpr double MIN_SIZE = 10;
    QString id() const override { return "spotlight"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    bool drawing = false;
    QPointF start_pos;
    SpotlightItem *current_item = nullptr;
};
}
