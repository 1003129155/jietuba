#pragma once
#include "tools/base.h"

namespace jietuba {
// Creates an empty provisional text item and starts editing. It reaches the
// undo stack only when focus leaves with non-empty text (see TextItem).
class TextTool : public Tool {
public:
    QString id() const override { return "text"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    // Clearing focus lets an empty text item delete itself.
    void on_deactivate(ToolContext &ctx) override;
};
}
