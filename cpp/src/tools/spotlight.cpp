#include "tools/spotlight.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/scene.h"
#include <algorithm>

namespace jietuba {
bool SpotlightTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    drawing = true;
    start_pos = pos;
    // One curtain per scene: a new hole sets the darkness for all existing holes too.
    SpotlightCurtain::of(ctx.scene)->setOpacity(std::max(0.0, std::min(1.0, ctx.opacity)));
    current_item = new SpotlightItem(QRectF(pos, pos));
    ctx.scene->addItem(current_item);
    return true;
}

void SpotlightTool::on_move(const QPointF &pos, ToolContext &) {
    if (drawing && current_item) current_item->setRect(QRectF(start_pos, pos).normalized());
}

void SpotlightTool::on_release(const QPointF &pos, ToolContext &ctx) {
    if (!drawing) return;
    drawing = false;
    auto *item = current_item;
    current_item = nullptr;
    if (!item) return;
    // Out of the scene first: the undo stack decides whether it stays.
    ctx.scene->removeItem(item);
    const auto rect = QRectF(start_pos, pos).normalized();
    if (rect.width() < MIN_SIZE || rect.height() < MIN_SIZE) {
        delete item;
        return;
    }
    ctx.undo_stack->push(new AddItemCommand(ctx.scene, ItemReference(item)));
    emit ctx.scene->item_auto_select_requested(item);
}
}
