#pragma once
#include "canvas/items/item_reference.h"
#include "tools/base.h"
#include <QSet>
#include <vector>

namespace jietuba {
// Deletes whole items under the pointer while dragging and commits them as one
// undoable batch on release.
class EraserTool : public Tool {
public:
    QString id() const override { return "eraser"; }
    bool on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) override;
    void on_move(const QPointF &pos, ToolContext &ctx) override;
    void on_release(const QPointF &pos, ToolContext &ctx) override;
    void on_activate(ToolContext &ctx) override;
    void on_deactivate(ToolContext &ctx) override;
    bool erasing = false;
    std::optional<QPointF> last_pos;
    std::optional<int> number_next_before;
private:
    void erase_at_position(const QPointF &pos, ToolContext &ctx);
    void try_erase_item(QGraphicsItem *item);
    // Removed items are owned here until the batch command takes them.
    std::vector<ItemReference> erased_items_;
    QSet<QGraphicsItem *> erased_set_;
};
}
